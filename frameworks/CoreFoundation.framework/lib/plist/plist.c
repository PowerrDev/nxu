/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/CoreFoundation.framework/lib/plist/plist.c
 *
 * Allocation-free XML property-list tokenizer for root userspace. The parser
 * deliberately implements the small plist subset NXU consumes today while
 * preserving a generic event stream for future configuration clients.
 */

#include <frameworks/CoreFoundation.framework/lib/plist/plist.h>

#include <stdbool.h>
#include <stdint.h>

static bool
plist_is_space(char character)
{
	return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

static bool
plist_match_at(const plist_parser_t *parser, uint64_t offset, const char *text)
{
	uint64_t index = 0ULL;
	while (text[index] != '\0') {
		if (offset + index >= parser->length || parser->data[offset + index] != text[index]) return false;
		index++;
	}
	return true;
}

static void
plist_skip_space(plist_parser_t *parser)
{
	while (parser->offset < parser->length && plist_is_space(parser->data[parser->offset])) parser->offset++;
}

static plist_status_t
plist_skip_until(plist_parser_t *parser, const char *terminator)
{
	while (parser->offset < parser->length) {
		if (plist_match_at(parser, parser->offset, terminator)) {
			uint64_t length = 0ULL;
			while (terminator[length] != '\0') length++;
			parser->offset += length;
			return PLIST_STATUS_OK;
		}
		parser->offset++;
	}

	return PLIST_STATUS_MALFORMED;
}

static plist_status_t
plist_skip_markup(plist_parser_t *parser)
{
	for (;;) {
		plist_skip_space(parser);

		if (plist_match_at(parser, parser->offset, "<?")) {
			parser->offset += 2ULL;
			plist_status_t status = plist_skip_until(parser, "?>");
			if (status != PLIST_STATUS_OK) return status;
			continue;
		}

		if (plist_match_at(parser, parser->offset, "<!--")) {
			parser->offset += 4ULL;
			plist_status_t status = plist_skip_until(parser, "-->");
			if (status != PLIST_STATUS_OK) return status;
			continue;
		}

		if (plist_match_at(parser, parser->offset, "<!DOCTYPE")) {
			parser->offset += 9ULL;
			plist_status_t status = plist_skip_until(parser, ">");
			if (status != PLIST_STATUS_OK) return status;
			continue;
		}

		return PLIST_STATUS_OK;
	}
}

static bool
plist_tag_boundary(char character)
{
	return character == '>' || character == '/' || plist_is_space(character);
}

static plist_status_t
plist_consume_open_tag(plist_parser_t *parser, const char *name)
{
	if (parser->offset >= parser->length || parser->data[parser->offset] != '<') return PLIST_STATUS_MALFORMED;
	parser->offset++;

	uint64_t index = 0ULL;
	while (name[index] != '\0') {
		if (parser->offset >= parser->length || parser->data[parser->offset] != name[index]) return PLIST_STATUS_MALFORMED;
		parser->offset++;
		index++;
	}

	if (parser->offset >= parser->length || !plist_tag_boundary(parser->data[parser->offset])) return PLIST_STATUS_MALFORMED;

	bool quoted = false;
	char quote = '\0';
	while (parser->offset < parser->length) {
		char character = parser->data[parser->offset++];
		if (quoted) {
			if (character == quote) quoted = false;
			continue;
		}

		if (character == '\'' || character == '"') {
			quoted = true;
			quote = character;
			continue;
		}

		if (character == '>') return PLIST_STATUS_OK;
	}

	return PLIST_STATUS_MALFORMED;
}

static bool
plist_consume_exact(plist_parser_t *parser, const char *text)
{
	if (!plist_match_at(parser, parser->offset, text)) return false;
	uint64_t length = 0ULL;
	while (text[length] != '\0') length++;
	parser->offset += length;
	return true;
}

static plist_status_t
plist_decode_entity(const char *data, uint64_t length, uint64_t *offset, char *character)
{
	if (*offset >= length || data[*offset] != '&') return PLIST_STATUS_MALFORMED;

	struct plist_entity {
		const char *source;
		char value;
	};

	static const struct plist_entity entities[] = {
		{ "&amp;", '&' },
		{ "&lt;", '<' },
		{ "&gt;", '>' },
		{ "&quot;", '"' },
		{ "&apos;", '\'' }
	};

	for (uint32_t entity = 0U; entity < sizeof(entities) / sizeof(entities[0]); entity++) {
		uint64_t index = 0ULL;
		while (entities[entity].source[index] != '\0') {
			if (*offset + index >= length || data[*offset + index] != entities[entity].source[index]) break;
			index++;
		}

		if (entities[entity].source[index] != '\0') continue;
		*offset += index;
		*character = entities[entity].value;
		return PLIST_STATUS_OK;
	}

	return PLIST_STATUS_UNSUPPORTED;
}

static plist_status_t
plist_text_event(plist_parser_t *parser, plist_event_t *event, plist_event_type_t type, const char *closing)
{
	uint64_t written = 0ULL;

	while (parser->offset < parser->length && !plist_match_at(parser, parser->offset, closing)) {
		char character = parser->data[parser->offset];
		if (character == '&') {
			plist_status_t status = plist_decode_entity(parser->data, parser->length, &parser->offset, &character);
			if (status != PLIST_STATUS_OK) return status;
		} else {
			parser->offset++;
		}

		if (written + 1ULL >= PLIST_EVENT_TEXT_MAX) return PLIST_STATUS_TEXT_TOO_LONG;
		event->text[written++] = character;
	}

	if (!plist_consume_exact(parser, closing)) return PLIST_STATUS_MALFORMED;
	event->text[written] = '\0';
	event->type = type;
	return PLIST_STATUS_OK;
}

static plist_status_t
plist_integer_event(plist_parser_t *parser, plist_event_t *event)
{
	bool negative = false;
	if (parser->offset < parser->length && parser->data[parser->offset] == '-') {
		negative = true;
		parser->offset++;
	}

	if (parser->offset >= parser->length || parser->data[parser->offset] < '0' || parser->data[parser->offset] > '9') {
		return PLIST_STATUS_MALFORMED;
	}

	uint64_t value = 0ULL;
	while (parser->offset < parser->length && parser->data[parser->offset] >= '0' && parser->data[parser->offset] <= '9') {
		uint64_t digit = (uint64_t)(parser->data[parser->offset] - '0');
		if (value > (UINT64_MAX - digit) / 10ULL) return PLIST_STATUS_UNSUPPORTED;
		value = value * 10ULL + digit;
		parser->offset++;
	}

	if (!plist_consume_exact(parser, "</integer>")) return PLIST_STATUS_MALFORMED;
	if (!negative && value > INT64_MAX) return PLIST_STATUS_UNSUPPORTED;
	if (negative && value > (uint64_t)INT64_MAX + 1ULL) return PLIST_STATUS_UNSUPPORTED;

	event->type = PLIST_EVENT_INTEGER;
	if (negative && value == (uint64_t)INT64_MAX + 1ULL) event->integer = INT64_MIN;
	else event->integer = negative ? -(int64_t)value : (int64_t)value;
	return PLIST_STATUS_OK;
}

plist_status_t
plist_parser_init(plist_parser_t *parser, const char *data, uint64_t length)
{
	if (parser == 0 || data == 0 || length == 0ULL) return PLIST_STATUS_INVALID_ARGUMENT;

	*parser = (plist_parser_t) {
		.data = data,
		.length = length,
		.offset = 0ULL,
		.document_open = false,
		.complete = false
	};

	if (length >= 3ULL && (uint8_t)data[0] == 0xEFU && (uint8_t)data[1] == 0xBBU && (uint8_t)data[2] == 0xBFU) {
		parser->offset = 3ULL;
	}

	plist_status_t status = plist_skip_markup(parser);
	if (status != PLIST_STATUS_OK) return status;
	if (!plist_match_at(parser, parser->offset, "<plist")) return PLIST_STATUS_MALFORMED;

	status = plist_consume_open_tag(parser, "plist");
	if (status != PLIST_STATUS_OK) return status;
	parser->document_open = true;
	return PLIST_STATUS_OK;
}

plist_status_t
plist_parser_next(plist_parser_t *parser, plist_event_t *event)
{
	if (parser == 0 || event == 0 || !parser->document_open) return PLIST_STATUS_INVALID_ARGUMENT;
	if (parser->complete) return PLIST_STATUS_END;

	*event = (plist_event_t) { 0 };
	plist_status_t status = plist_skip_markup(parser);
	if (status != PLIST_STATUS_OK) return status;

	if (plist_consume_exact(parser, "</plist>")) {
		parser->complete = true;
		return PLIST_STATUS_END;
	}

	if (plist_consume_exact(parser, "<dict>")) {
		event->type = PLIST_EVENT_DICT_BEGIN;
		return PLIST_STATUS_OK;
	}

	if (plist_consume_exact(parser, "</dict>")) {
		event->type = PLIST_EVENT_DICT_END;
		return PLIST_STATUS_OK;
	}

	if (plist_consume_exact(parser, "<array>")) {
		event->type = PLIST_EVENT_ARRAY_BEGIN;
		return PLIST_STATUS_OK;
	}

	if (plist_consume_exact(parser, "</array>")) {
		event->type = PLIST_EVENT_ARRAY_END;
		return PLIST_STATUS_OK;
	}

	if (plist_consume_exact(parser, "<true/>")) {
		event->type = PLIST_EVENT_BOOLEAN;
		event->boolean = true;
		return PLIST_STATUS_OK;
	}

	if (plist_consume_exact(parser, "<false/>")) {
		event->type = PLIST_EVENT_BOOLEAN;
		event->boolean = false;
		return PLIST_STATUS_OK;
	}

	if (plist_consume_exact(parser, "<true />")) {
		event->type = PLIST_EVENT_BOOLEAN;
		event->boolean = true;
		return PLIST_STATUS_OK;
	}

	if (plist_consume_exact(parser, "<false />")) {
		event->type = PLIST_EVENT_BOOLEAN;
		event->boolean = false;
		return PLIST_STATUS_OK;
	}

	if (plist_consume_exact(parser, "<key>")) return plist_text_event(parser, event, PLIST_EVENT_KEY, "</key>");
	if (plist_consume_exact(parser, "<string>")) return plist_text_event(parser, event, PLIST_EVENT_STRING, "</string>");
	if (plist_consume_exact(parser, "<integer>")) return plist_integer_event(parser, event);

	return PLIST_STATUS_UNSUPPORTED;
}

const char *
plist_status_name(plist_status_t status)
{
	switch (status) {
	case PLIST_STATUS_OK: return "ok";
	case PLIST_STATUS_END: return "end";
	case PLIST_STATUS_INVALID_ARGUMENT: return "invalid argument";
	case PLIST_STATUS_MALFORMED: return "malformed plist";
	case PLIST_STATUS_UNSUPPORTED: return "unsupported plist syntax";
	case PLIST_STATUS_TEXT_TOO_LONG: return "plist text too long";
	default: return "unknown plist error";
	}
}
