#ifndef NXU_COREFOUNDATION_PLIST_H
#define NXU_COREFOUNDATION_PLIST_H

#include <stdbool.h>
#include <stdint.h>

#define PLIST_EVENT_TEXT_MAX 512U

typedef enum {
	PLIST_STATUS_OK = 0,
	PLIST_STATUS_END,
	PLIST_STATUS_INVALID_ARGUMENT,
	PLIST_STATUS_MALFORMED,
	PLIST_STATUS_UNSUPPORTED,
	PLIST_STATUS_TEXT_TOO_LONG
} plist_status_t;

typedef enum {
	PLIST_EVENT_NONE = 0,
	PLIST_EVENT_DICT_BEGIN,
	PLIST_EVENT_DICT_END,
	PLIST_EVENT_ARRAY_BEGIN,
	PLIST_EVENT_ARRAY_END,
	PLIST_EVENT_KEY,
	PLIST_EVENT_STRING,
	PLIST_EVENT_INTEGER,
	PLIST_EVENT_BOOLEAN
} plist_event_type_t;

typedef struct {
	plist_event_type_t type;
	char text[PLIST_EVENT_TEXT_MAX];
	int64_t integer;
	bool boolean;
} plist_event_t;

typedef struct {
	const char *data;
	uint64_t length;
	uint64_t offset;
	bool document_open;
	bool complete;
} plist_parser_t;

plist_status_t plist_parser_init(plist_parser_t *parser, const char *data, uint64_t length);
plist_status_t plist_parser_next(plist_parser_t *parser, plist_event_t *event);
const char *plist_status_name(plist_status_t status);

#endif
