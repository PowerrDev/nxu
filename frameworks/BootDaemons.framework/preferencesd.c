/*
 * preferencesd: the one writer of /Library/Preferences.
 *
 * Apps and tools ask it to read a domain, replace one, or set or remove a
 * single key (see <nxu/services.h>); it checks the domain's name, keeps the
 * file a well-formed property list and writes it. Anyone may read; only root
 * may change preferences, as with macOS's /Library/Preferences. Readers that
 * only watch a file (the desktop, the Dock) read it directly.
 */
#include <nxu/services.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <frameworks/CoreFoundation.framework/lib/plist/plist.h>

#define PREFERENCES_DIRECTORY "/Volumes/mainOS/Library/Preferences/"
#define ENTRY_MAX 48U
#define VALUE_MAX 256U

typedef struct {
	char key[NXU_SERVICE_KEY_MAX];
	uint32_t type; /* NXU_PREFERENCE_* */
	char value[VALUE_MAX];
} entry_t;

#define WATCHER_MAX 16U

/* Ports to tell when a domain changes ("" watches them all). */
static struct {
	uint32_t port;
	char domain[97];
} g_watchers[WATCHER_MAX];

/* Big: off the stack. */
static entry_t g_entries[ENTRY_MAX];
static char g_file[NXU_SERVICE_DATA_MAX + 1U];
static char g_text[NXU_SERVICE_DATA_MAX + 1U];

static void
log_line(const char *first, const char *second)
{
	(void)nxu_write(1, "preferencesd: ", 14);
	(void)nxu_write(1, first, nxu_strlen(first));
	if (second) (void)nxu_write(1, second, nxu_strlen(second));
	(void)nxu_write(1, "\n", 1);
}

static bool
domain_valid(const char *domain)
{
	uint64_t length = nxu_strlen(domain);

	if (length == 0 || length > 96U || domain[0] == '.') return false;
	for (uint64_t index = 0; index < length; index++) {
		char c = domain[index];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
				  c == '-' || c == '_';

		if (!ok || (c == '.' && domain[index + 1] == '.')) return false;
	}
	return true;
}

static bool
domain_path(const char *domain, char *path, uint64_t capacity)
{
	uint64_t prefix = sizeof(PREFERENCES_DIRECTORY) - 1U, length = nxu_strlen(domain);

	if (prefix + length + 7U > capacity) return false;
	memcpy(path, PREFERENCES_DIRECTORY, prefix);
	memcpy(path + prefix, domain, length);
	memcpy(path + prefix + length, ".plist", 7U);
	return true;
}

/* The domain's file into g_file; its length, 0 if there is none. */
static int64_t
read_domain(const char *path)
{
	int64_t fd = nxu_open(path, NXU_O_READ);

	if (fd < 0) return 0;

	uint64_t total = 0;

	while (total < NXU_SERVICE_DATA_MAX) {
		int64_t count = nxu_read((uint64_t)fd, g_file + total, NXU_SERVICE_DATA_MAX - total);

		if (count <= 0) break;
		total += (uint64_t)count;
	}
	(void)nxu_close((uint64_t)fd);
	g_file[total] = '\0';
	return (int64_t)total;
}

static int64_t
write_file(const char *path, const char *text, uint64_t length)
{
	int64_t fd = nxu_open(path, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);

	if (fd < 0) return fd;

	uint64_t total = 0;

	while (total < length) {
		int64_t count = nxu_write((uint64_t)fd, text + total, length - total);

		if (count <= 0) break;
		total += (uint64_t)count;
	}
	(void)nxu_close((uint64_t)fd);
	(void)nxu_sync();
	return total == length ? 0 : -NXU_SYS_E_IO;
}

/* Whether `text` is a whole, well-formed property list. */
static bool
plist_valid(const char *text, uint64_t length)
{
	plist_parser_t parser;
	plist_event_t event;

	if (plist_parser_init(&parser, text, length) != PLIST_STATUS_OK) return false;
	for (;;) {
		plist_status_t status = plist_parser_next(&parser, &event);

		if (status == PLIST_STATUS_END) return true;
		if (status != PLIST_STATUS_OK) return false;
	}
}

static void
format_integer(int64_t value, char out[VALUE_MAX])
{
	char reversed[24];
	uint32_t length = 0, at = 0;
	uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1U : (uint64_t)value;

	do {
		reversed[length++] = (char)('0' + magnitude % 10U);
		magnitude /= 10U;
	} while (magnitude);
	if (value < 0) out[at++] = '-';
	while (length) out[at++] = reversed[--length];
	out[at] = '\0';
}

/* Read a flat dictionary into g_entries. -1 if it is not one. */
static int32_t
parse_flat(const char *text, uint64_t length)
{
	plist_parser_t parser;
	plist_event_t event;
	int32_t count = 0;
	uint32_t depth = 0;

	if (length == 0) return 0;
	if (plist_parser_init(&parser, text, length) != PLIST_STATUS_OK) return -1;

	for (;;) {
		plist_status_t status = plist_parser_next(&parser, &event);

		if (status == PLIST_STATUS_END) return count;
		if (status != PLIST_STATUS_OK) return -1;

		switch (event.type) {
		case PLIST_EVENT_DICT_BEGIN:
			if (++depth > 1U) return -1;
			break;
		case PLIST_EVENT_DICT_END:
			depth--;
			break;
		case PLIST_EVENT_ARRAY_BEGIN:
			return -1;
		case PLIST_EVENT_KEY:
			if (count == (int32_t)ENTRY_MAX) return -1;
			nxu_service_copy(g_entries[count].key, sizeof(g_entries[count].key), event.text);
			break;
		case PLIST_EVENT_STRING:
		case PLIST_EVENT_INTEGER:
		case PLIST_EVENT_BOOLEAN:
			if (count == (int32_t)ENTRY_MAX) return -1;
			if (event.type == PLIST_EVENT_STRING) {
				g_entries[count].type = NXU_PREFERENCE_STRING;
				nxu_service_copy(g_entries[count].value, VALUE_MAX, event.text);
			} else if (event.type == PLIST_EVENT_INTEGER) {
				g_entries[count].type = NXU_PREFERENCE_INTEGER;
				format_integer(event.integer, g_entries[count].value);
			} else {
				g_entries[count].type = NXU_PREFERENCE_BOOLEAN;
				nxu_service_copy(g_entries[count].value, VALUE_MAX, event.boolean ? "true" : "false");
			}
			count++;
			break;
		default:
			break;
		}
	}
}

typedef struct {
	char *text;
	uint64_t length, capacity;
	bool overflow;
} builder_t;

static void
put(builder_t *builder, const char *text)
{
	uint64_t length = nxu_strlen(text);

	if (builder->length + length >= builder->capacity) {
		builder->overflow = true;
		return;
	}
	memcpy(builder->text + builder->length, text, length);
	builder->length += length;
	builder->text[builder->length] = '\0';
}

static void
put_escaped(builder_t *builder, const char *text)
{
	char one[2] = {0, 0};

	for (; *text; text++) {
		if (*text == '&') put(builder, "&amp;");
		else if (*text == '<') put(builder, "&lt;");
		else if (*text == '>') put(builder, "&gt;");
		else {
			one[0] = *text;
			put(builder, one);
		}
	}
}

static int64_t
serialize(int32_t count, uint64_t *length)
{
	builder_t builder = {g_text, 0, sizeof(g_text), false};

	put(&builder, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				  "<!DOCTYPE plist PUBLIC \"-//NXU//DTD PLIST 1.0//EN\" "
				  "\"http://www.nxu.local/DTDs/PropertyList-1.0.dtd\">\n"
				  "<plist version=\"1.0\">\n<dict>\n");
	for (int32_t index = 0; index < count; index++) {
		put(&builder, "\t<key>");
		put_escaped(&builder, g_entries[index].key);
		put(&builder, "</key>\n\t");
		if (g_entries[index].type == NXU_PREFERENCE_BOOLEAN) {
			put(&builder, nxu_streq(g_entries[index].value, "true") ? "<true/>" : "<false/>");
		} else {
			bool integer = g_entries[index].type == NXU_PREFERENCE_INTEGER;

			put(&builder, integer ? "<integer>" : "<string>");
			put_escaped(&builder, g_entries[index].value);
			put(&builder, integer ? "</integer>" : "</string>");
		}
		put(&builder, "\n");
	}
	put(&builder, "</dict>\n</plist>\n");
	*length = builder.length;
	return builder.overflow ? -NXU_SYS_E_NO_SPACE : 0;
}

static bool
value_valid(uint32_t type, const char *value)
{
	if (type == NXU_PREFERENCE_BOOLEAN) return nxu_streq(value, "true") || nxu_streq(value, "false");
	if (type == NXU_PREFERENCE_INTEGER) {
		if (*value == '-') value++;
		if (!*value) return false;
		for (; *value; value++)
			if (*value < '0' || *value > '9') return false;
		return true;
	}
	return type == NXU_PREFERENCE_STRING;
}

static int64_t
change(nxu_service_message_t *message, const char *path)
{
	int64_t length = read_domain(path);
	int32_t count = parse_flat(g_file, (uint64_t)length);

	if (count < 0) return -NXU_SYS_E_NOT_SUPPORTED;

	int32_t found = -1;

	for (int32_t index = 0; index < count; index++)
		if (nxu_streq(g_entries[index].key, message->key)) found = index;

	if (message->operation == NXU_PREFERENCES_REMOVE) {
		if (found < 0) return 0;
		for (int32_t index = found; index + 1 < count; index++) g_entries[index] = g_entries[index + 1];
		count--;
	} else {
		char value[VALUE_MAX];
		uint64_t size = message->length < VALUE_MAX - 1U ? message->length : VALUE_MAX - 1U;

		memcpy(value, message->data, size);
		value[size] = '\0';
		if (!message->key[0] || !value_valid((uint32_t)message->arguments[0], value))
			return -NXU_SYS_E_INVALID_ARGUMENT;
		if (found < 0) {
			if (count == (int32_t)ENTRY_MAX) return -NXU_SYS_E_NO_SPACE;
			found = count++;
			nxu_service_copy(g_entries[found].key, sizeof(g_entries[found].key), message->key);
		}
		g_entries[found].type = (uint32_t)message->arguments[0];
		nxu_service_copy(g_entries[found].value, VALUE_MAX, value);
	}

	uint64_t text_length = 0;
	int64_t status = serialize(count, &text_length);

	return status < 0 ? status : write_file(path, g_text, text_length);
}

/* Tell everyone watching `domain` that it changed. A watcher whose port is
 * gone (it quit) is dropped. */
static void
notify(const char *domain)
{
	nxu_preferences_notice_t notice = {NXU_SERVICES_VERSION, NXU_PREFERENCES_CHANGED, {0}};

	nxu_service_copy(notice.domain, sizeof(notice.domain), domain);
	for (uint32_t index = 0; index < WATCHER_MAX; index++) {
		if (g_watchers[index].port == 0) continue;
		if (g_watchers[index].domain[0] && !nxu_streq(g_watchers[index].domain, domain)) continue;
		if (nxu_ipc_send(g_watchers[index].port, &notice, sizeof(notice), 0) != 0) {
			(void)nxu_ipc_port_deallocate(g_watchers[index].port);
			g_watchers[index].port = 0;
		}
	}
}

static void
watch(nxu_service_message_t *message)
{
	for (uint32_t index = 0; index < WATCHER_MAX; index++) {
		if (g_watchers[index].port != 0) continue;
		g_watchers[index].port = nxu_service_reply_port();
		nxu_service_copy(g_watchers[index].domain, sizeof(g_watchers[index].domain), message->name);
		nxu_service_keep_reply_port();
		message->status = 0;
		return;
	}
	message->status = (int32_t)-NXU_SYS_E_NO_SPACE;
}

static void
handle(nxu_service_message_t *message, const nxu_ipc_peer_t *peer)
{
	char path[256];

	/* `length` is the request's payload until the request is handled. */
	if (message->operation == NXU_PREFERENCES_WATCH) {
		message->length = 0;
		if (message->name[0] && !domain_valid(message->name)) message->status = (int32_t)-NXU_SYS_E_INVALID_ARGUMENT;
		else watch(message);
		return;
	}

	if (!domain_valid(message->name) || !domain_path(message->name, path, sizeof(path))) {
		message->status = (int32_t)-NXU_SYS_E_INVALID_ARGUMENT;
		message->length = 0;
		return;
	}

	if (message->operation == NXU_PREFERENCES_READ) {
		int64_t length = read_domain(path);

		message->status = length > 0 ? 0 : (int32_t)-NXU_SYS_E_NOT_FOUND;
		message->length = (uint32_t)length;
		memcpy(message->data, g_file, (uint64_t)length);
		return;
	}

	if (peer->uid != 0U) {
		message->status = (int32_t)-NXU_SYS_E_DENIED;
		message->length = 0;
		return;
	}

	int64_t status;

	switch (message->operation) {
	case NXU_PREFERENCES_WRITE:
		status = plist_valid((const char *)message->data, message->length)
					 ? write_file(path, (const char *)message->data, message->length)
					 : -NXU_SYS_E_INVALID_ARGUMENT;
		break;
	case NXU_PREFERENCES_SET:
		status = change(message, path);
		break;
	case NXU_PREFERENCES_REMOVE:
		status = message->key[0] ? change(message, path) : nxu_unlink(path);
		if (status == -NXU_SYS_E_NOT_FOUND) status = 0;
		break;
	default:
		status = -NXU_SYS_E_NOT_SUPPORTED;
		break;
	}
	if (status == 0) {
		log_line("updated ", message->name);
		notify(message->name);
	} else {
		log_line("refused a change to ", message->name);
	}
	message->status = (int32_t)status;
	message->length = 0;
}

int
main(void)
{
	(void)nxu_mkdir("/Volumes/mainOS/Library/Preferences");
	log_line("ready", 0);
	return nxu_service_run(NXU_PREFERENCES_SERVICE, handle, 0, 1000000U);
}
