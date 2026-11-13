/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * The system services' wire calls (see <nxu/services.h>): the client side
 * every caller links, and the request loop the services themselves run.
 */
#include <nxu/services.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>

/* How long a caller waits for an answer. Usage walks a whole volume. */
#define SERVICE_REPLY_TIMEOUT_US 4000000U
#define SERVICE_SLOW_REPLY_TIMEOUT_US 30000000U

#define SERVICE_CACHE_MAX 4U

static struct {
	const char *label;
	uint32_t port;
} g_ports[SERVICE_CACHE_MAX];

void
nxu_service_copy(char *destination, uint64_t capacity, const char *source)
{
	if (!destination || capacity == 0U) return;

	uint64_t length = source ? nxu_strlen(source) : 0U;

	if (length >= capacity) length = capacity - 1U;
	if (length) memcpy(destination, source, length);
	memset(destination + length, 0, capacity - length);
}

/* The service's port, looked up once (or again after `forget`). */
static uint32_t
service_port(const char *label, bool forget)
{
	for (uint32_t index = 0; index < SERVICE_CACHE_MAX; index++) {
		if (g_ports[index].label == label) {
			if (!forget) return g_ports[index].port;
			g_ports[index].label = 0;
			g_ports[index].port = 0;
		}
	}

	uint32_t port = 0;

	if (!bootstrap_client_lookup(label, &port) || port == 0) return 0;

	for (uint32_t index = 0; index < SERVICE_CACHE_MAX; index++) {
		if (g_ports[index].label == 0) {
			g_ports[index].label = label;
			g_ports[index].port = port;
			break;
		}
	}
	return port;
}

/* Send `message` to `label` and replace it with the answer. */
static int64_t
service_call(const char *label, nxu_service_message_t *message, uint32_t timeout_us)
{
	message->version = NXU_SERVICES_VERSION;
	message->status = 0;

	for (uint32_t attempt = 0; attempt < 2U; attempt++) {
		uint32_t port = service_port(label, attempt > 0U);

		if (port == 0) return -NXU_SYS_E_NOT_FOUND;

		int64_t reply = nxu_ipc_port_allocate();

		if (reply <= 0) return -NXU_SYS_E_NO_MEMORY;

		if (nxu_ipc_send(port, message, sizeof(*message), (uint32_t)reply) != 0) {
			/* The service restarted since it was looked up: look again. */
			(void)nxu_ipc_port_deallocate((uint32_t)reply);
			continue;
		}

		uint32_t transfer = 0, type = 0;
		int64_t received;

		do {
			received = nxu_ipc_receive_timed((uint32_t)reply, message, sizeof(*message), &transfer, &type, timeout_us);
		} while (received == -NXU_SYS_E_INTERRUPTED);
		(void)nxu_ipc_port_deallocate((uint32_t)reply);

		if (received < 0) return received;
		if ((uint64_t)received != sizeof(*message) || message->version != NXU_SERVICES_VERSION) return -NXU_SYS_E_IO;

		return message->status;
	}
	return -NXU_SYS_E_NOT_FOUND;
}

static int64_t
service_result(int64_t status, const nxu_service_message_t *message, void *out, uint64_t size)
{
	if (status < 0) return status;
	if (message->length < size) return -NXU_SYS_E_IO;
	memcpy(out, message->data, size);
	return 0;
}

/* ----- preferencesd ----- */

int64_t
nxu_preferences_read(const char *domain, char *buffer, uint64_t capacity)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_PREFERENCES_READ;
	nxu_service_copy(message.name, sizeof(message.name), domain);

	int64_t status = service_call(NXU_PREFERENCES_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US);

	if (status < 0) return status;
	if (message.length > NXU_SERVICE_DATA_MAX) return -NXU_SYS_E_IO;
	if ((uint64_t)message.length + 1U > capacity) return -NXU_SYS_E_NO_SPACE;

	memcpy(buffer, message.data, message.length);
	buffer[message.length] = '\0';
	return message.length;
}

int64_t
nxu_preferences_write(const char *domain, const char *plist, uint64_t length)
{
	nxu_service_message_t message = {0};

	if (length > NXU_SERVICE_DATA_MAX) return -NXU_SYS_E_NO_SPACE;

	message.operation = NXU_PREFERENCES_WRITE;
	nxu_service_copy(message.name, sizeof(message.name), domain);
	memcpy(message.data, plist, length);
	message.length = (uint32_t)length;
	return service_call(NXU_PREFERENCES_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US);
}

static int64_t
preferences_set(const char *domain, const char *key, uint32_t type, const char *value)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_PREFERENCES_SET;
	message.arguments[0] = type;
	nxu_service_copy(message.name, sizeof(message.name), domain);
	nxu_service_copy(message.key, sizeof(message.key), key);
	nxu_service_copy((char *)message.data, sizeof(message.data), value);
	message.length = (uint32_t)nxu_strlen((const char *)message.data);
	return service_call(NXU_PREFERENCES_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US);
}

int64_t
nxu_preferences_set_string(const char *domain, const char *key, const char *value)
{
	return preferences_set(domain, key, NXU_PREFERENCE_STRING, value);
}

int64_t
nxu_preferences_set_integer(const char *domain, const char *key, int64_t value)
{
	char digits[24];
	uint32_t length = 0;
	uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1U : (uint64_t)value;
	char reversed[24];

	do {
		reversed[length++] = (char)('0' + magnitude % 10U);
		magnitude /= 10U;
	} while (magnitude && length < sizeof(reversed));

	uint32_t at = 0;

	if (value < 0) digits[at++] = '-';
	while (length) digits[at++] = reversed[--length];
	digits[at] = '\0';
	return preferences_set(domain, key, NXU_PREFERENCE_INTEGER, digits);
}

int64_t
nxu_preferences_set_boolean(const char *domain, const char *key, bool value)
{
	return preferences_set(domain, key, NXU_PREFERENCE_BOOLEAN, value ? "true" : "false");
}

int64_t
nxu_preferences_remove(const char *domain, const char *key)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_PREFERENCES_REMOVE;
	nxu_service_copy(message.name, sizeof(message.name), domain);
	nxu_service_copy(message.key, sizeof(message.key), key);
	return service_call(NXU_PREFERENCES_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US);
}

int64_t
nxu_preferences_watch(const char *domain, uint32_t port)
{
	uint32_t service = service_port(NXU_PREFERENCES_SERVICE, false);
	nxu_service_message_t message = {0};

	if (service == 0) return -NXU_SYS_E_NOT_FOUND;

	/* The port itself is the reply port: the answer is the first notice. */
	message.version = NXU_SERVICES_VERSION;
	message.operation = NXU_PREFERENCES_WATCH;
	nxu_service_copy(message.name, sizeof(message.name), domain);
	if (nxu_ipc_send(service, &message, sizeof(message), port) != 0) {
		service = service_port(NXU_PREFERENCES_SERVICE, true);
		if (service == 0 || nxu_ipc_send(service, &message, sizeof(message), port) != 0) return -NXU_SYS_E_NOT_FOUND;
	}
	return 0;
}

/* ----- timed ----- */

int64_t
nxu_timed_now(nxu_time_info_t *info)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_TIMED_NOW;
	return service_result(service_call(NXU_TIMED_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US), &message, info,
						  sizeof(*info));
}

int64_t
nxu_timed_zone(uint32_t index, nxu_time_zone_t *zone)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_TIMED_ZONE;
	message.arguments[0] = index;
	return service_result(service_call(NXU_TIMED_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US), &message, zone,
						  sizeof(*zone));
}

int64_t
nxu_timed_set_zone(const char *zone)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_TIMED_SET_ZONE;
	nxu_service_copy(message.name, sizeof(message.name), zone);
	return service_call(NXU_TIMED_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US);
}

/* ----- storaged ----- */

int64_t
nxu_storaged_volume(uint32_t index, nxu_volume_info_t *info)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_STORAGED_VOLUME;
	message.arguments[0] = index;
	return service_result(service_call(NXU_STORAGED_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US), &message, info,
						  sizeof(*info));
}

int64_t
nxu_storaged_usage(const char *mount_point, nxu_storage_usage_t *usage)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_STORAGED_USAGE;
	nxu_service_copy(message.name, sizeof(message.name), mount_point);
	return service_result(service_call(NXU_STORAGED_SERVICE, &message, SERVICE_SLOW_REPLY_TIMEOUT_US), &message,
						  usage, sizeof(*usage));
}

/* ----- diskd ----- */

int64_t
nxu_diskd_disk(uint32_t index, nxu_disk_info_t *info)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_DISKD_DISK;
	message.arguments[0] = index;
	return service_result(service_call(NXU_DISKD_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US), &message, info,
						  sizeof(*info));
}

int64_t
nxu_diskd_partition(uint32_t disk, uint32_t partition, nxu_partition_info_t *info)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_DISKD_PARTITION;
	message.arguments[0] = disk;
	message.arguments[1] = partition;
	return service_result(service_call(NXU_DISKD_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US), &message, info,
						  sizeof(*info));
}

int64_t
nxu_diskd_mount(uint32_t device, const char *filesystem, const char *path)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_DISKD_MOUNT;
	message.arguments[0] = device;
	nxu_service_copy(message.key, sizeof(message.key), filesystem);
	nxu_service_copy(message.name, sizeof(message.name), path);
	return service_call(NXU_DISKD_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US);
}

int64_t
nxu_diskd_unmount(const char *path)
{
	nxu_service_message_t message = {0};

	message.operation = NXU_DISKD_UNMOUNT;
	nxu_service_copy(message.name, sizeof(message.name), path);
	return service_call(NXU_DISKD_SERVICE, &message, SERVICE_REPLY_TIMEOUT_US);
}

/* ----- the services' side ----- */

static uint32_t g_reply_port;
static bool g_keep_reply_port;

uint32_t
nxu_service_reply_port(void)
{
	return g_reply_port;
}

void
nxu_service_keep_reply_port(void)
{
	g_keep_reply_port = true;
}

int
nxu_service_run(const char *label, nxu_service_handler_t handler, void (*idle)(void), uint32_t idle_us)
{
	int64_t port = nxu_ipc_port_allocate();

	if (port <= 0 || !bootstrap_client_register(label, (uint32_t)port)) return 1;

	/* Large: keep it off the stack. */
	static nxu_service_message_t message;

	for (;;) {
		uint32_t sequence = (uint32_t)nxu_event_wait(0, 0);
		uint32_t reply = 0, type = 0;
		nxu_ipc_peer_t peer;
		int64_t received = nxu_ipc_receive_peer((uint32_t)port, &message, sizeof(message), &reply, &type, &peer);

		if (received < 0) {
			if (idle) idle();
			(void)nxu_event_wait(idle_us, sequence);
			continue;
		}
		if (reply == 0 || type != NXU_IPC_XFER_PORT) continue;

		if ((uint64_t)received != sizeof(message) || message.version != NXU_SERVICES_VERSION) {
			memset(&message, 0, sizeof(message));
			message.status = (int32_t)-NXU_SYS_E_INVALID_ARGUMENT;
		} else {
			message.name[sizeof(message.name) - 1U] = '\0';
			message.key[sizeof(message.key) - 1U] = '\0';
			if (message.length > sizeof(message.data)) message.length = sizeof(message.data);
			message.status = 0;
			g_reply_port = reply;
			g_keep_reply_port = false;
			handler(&message, &peer);
		}
		message.version = NXU_SERVICES_VERSION;
		(void)nxu_ipc_send(reply, &message, sizeof(message), 0);
		if (!g_keep_reply_port) (void)nxu_ipc_port_deallocate(reply);
		g_keep_reply_port = false;
	}
}
