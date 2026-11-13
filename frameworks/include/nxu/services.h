#ifndef NXU_SERVICES_H
#define NXU_SERVICES_H

/*
 * The system services behind System Settings and the shell tools:
 *
 *   preferencesd  com.nxu.preferencesd  writes /Library/Preferences/<domain>.plist
 *   timed         com.nxu.timed         the wall clock and the time zone
 *   storaged      com.nxu.storaged      volumes, their space and what uses it
 *   diskd         com.nxu.diskd         disks and partitions; mount, unmount
 *
 * Every request and reply is one nxu_service_message_t over NXPC, the reply
 * going to a port sent with the request (NXU_IPC_XFER_PORT). The services
 * authorise from the kernel's sender identity (nxu_ipc_peer_t), never from
 * the payload. The client calls below wrap all of it: each looks the service
 * up, sends, and waits for the answer, returning 0 or -NXU_SYS_E_*.
 */

#include <nxu/ipc_peer.h>
#include <stdbool.h>
#include <stdint.h>

#define NXU_PREFERENCES_SERVICE "com.nxu.preferencesd"
#define NXU_TIMED_SERVICE "com.nxu.timed"
#define NXU_STORAGED_SERVICE "com.nxu.storaged"
#define NXU_DISKD_SERVICE "com.nxu.diskd"

#define NXU_SERVICES_VERSION 1U
#define NXU_SERVICE_NAME_MAX 160U
#define NXU_SERVICE_KEY_MAX 64U
#define NXU_SERVICE_DATA_MAX 3072U

typedef struct {
	uint32_t version;	/* NXU_SERVICES_VERSION */
	uint32_t operation; /* the service's NXU_<SERVICE>_* */
	int32_t status;		/* reply: 0, or -NXU_SYS_E_* */
	uint32_t length;	/* bytes of data in use */
	uint64_t arguments[4];
	char name[NXU_SERVICE_NAME_MAX]; /* a domain, a path, a zone */
	char key[NXU_SERVICE_KEY_MAX];
	uint8_t data[NXU_SERVICE_DATA_MAX];
} nxu_service_message_t;

_Static_assert(sizeof(nxu_service_message_t) <= 4096U, "one NXPC message");

/* ----- preferencesd ----- */

/* Domains are reverse-DNS names (letters, digits, '.', '-', '_'), stored
 * as /Volumes/mainOS/Library/Preferences/<domain>.plist. Anyone may read;
 * only root may change them. SET and REMOVE work on flat dictionaries
 * (strings, integers, booleans); WRITE replaces the whole file. */
enum {
	NXU_PREFERENCES_READ = 1,	/* name: domain -> data: the file */
	NXU_PREFERENCES_WRITE,		/* name: domain, data: a whole property list */
	NXU_PREFERENCES_SET,		/* name: domain, key, data: value, arguments[0]: type */
	NXU_PREFERENCES_REMOVE,		/* name: domain, key ("" removes the domain) */
	NXU_PREFERENCES_WATCH,		/* name: domain ("" for all); the port sent with it gets notices */
};

/*
 * What a watcher's port receives when a domain it watches changes (and once,
 * the full reply, when it starts watching): instead of reading its files
 * again and again, a reader waits for one of these.
 */
typedef struct {
	uint32_t version;
	uint32_t operation; /* NXU_PREFERENCES_CHANGED */
	char domain[96];
} nxu_preferences_notice_t;

#define NXU_PREFERENCES_CHANGED 100U

enum {
	NXU_PREFERENCE_STRING = 1,
	NXU_PREFERENCE_INTEGER,
	NXU_PREFERENCE_BOOLEAN,
};

int64_t nxu_preferences_read(const char *domain, char *buffer, uint64_t capacity);
int64_t nxu_preferences_write(const char *domain, const char *plist, uint64_t length);
int64_t nxu_preferences_set_string(const char *domain, const char *key, const char *value);
int64_t nxu_preferences_set_integer(const char *domain, const char *key, int64_t value);
int64_t nxu_preferences_set_boolean(const char *domain, const char *key, bool value);
int64_t nxu_preferences_remove(const char *domain, const char *key);
/* Send notices about `domain` ("" for every domain) to `port`, which the
 * caller allocated and keeps receiving on. Watches end when the port does,
 * or when preferencesd restarts: callers re-read now and then regardless. */
int64_t nxu_preferences_watch(const char *domain, uint32_t port);

/* ----- timed ----- */

/* Zones are named like the tz database ("Europe/Lisbon"); timed knows a
 * fixed table of them with their offsets and daylight-saving rules (US, EU,
 * none). The choice is kept in com.nxu.timed (TimeZone), with the current
 * offset (OffsetSeconds) for readers that only need that, like the menu bar. */
enum {
	NXU_TIMED_NOW = 1,	/* -> data: nxu_time_info_t */
	NXU_TIMED_ZONE,		/* arguments[0]: index -> data: nxu_time_zone_t */
	NXU_TIMED_SET_ZONE, /* name: zone (root only) */
};

#define NXU_TIME_ZONE_NAME_MAX 48U

typedef struct {
	int64_t utc_seconds;   /* since 1970-01-01 UTC */
	int64_t local_seconds; /* utc_seconds + offset_seconds */
	int32_t offset_seconds;
	uint32_t daylight; /* daylight-saving time is in effect */
	char zone[NXU_TIME_ZONE_NAME_MAX];
	char abbreviation[8];
} nxu_time_info_t;

typedef struct {
	int32_t standard_offset_seconds;
	uint32_t has_daylight;
	char name[NXU_TIME_ZONE_NAME_MAX];
	char city[32];
} nxu_time_zone_t;

int64_t nxu_timed_now(nxu_time_info_t *info);
int64_t nxu_timed_zone(uint32_t index, nxu_time_zone_t *zone);
int64_t nxu_timed_set_zone(const char *zone);

/* ----- storaged ----- */

enum {
	NXU_STORAGED_VOLUME = 1, /* arguments[0]: index -> data: nxu_volume_info_t */
	NXU_STORAGED_USAGE,		 /* name: a mount point -> data: nxu_storage_usage_t */
};

#define NXU_STORAGE_CATEGORY_MAX 6U

typedef struct {
	char path[64];
	char filesystem[16];
	char device[32];
	uint32_t device_index;
	uint32_t read_only;
	uint64_t total_bytes;
	uint64_t free_bytes;
} nxu_volume_info_t;

typedef struct {
	uint64_t total_bytes;
	uint64_t free_bytes;
	uint32_t category_count;
	uint32_t files_counted;
	struct {
		char name[24];
		uint64_t bytes;
	} categories[NXU_STORAGE_CATEGORY_MAX];
} nxu_storage_usage_t;

int64_t nxu_storaged_volume(uint32_t index, nxu_volume_info_t *info);
int64_t nxu_storaged_usage(const char *mount_point, nxu_storage_usage_t *usage);

/* ----- diskd ----- */

enum {
	NXU_DISKD_DISK = 1,	 /* arguments[0]: index -> data: nxu_disk_info_t */
	NXU_DISKD_PARTITION, /* arguments[0]: disk, [1]: partition -> data: nxu_partition_info_t */
	NXU_DISKD_MOUNT,	 /* arguments[0]: device, key: file system, name: path (root only) */
	NXU_DISKD_UNMOUNT,	 /* name: path (root only; never / or the system volume) */
};

typedef struct {
	char name[32];
	uint64_t sector_count;
	uint32_t sector_size;
	uint32_t read_only;
	uint32_t scheme; /* NXU_RECOVERY_BLOCK_LAYOUT_* */
	uint32_t partition_count;
} nxu_disk_info_t;

typedef struct {
	char name[40];
	uint64_t start_sector;
	uint64_t sector_count;
	uint32_t index;
	uint32_t type;
} nxu_partition_info_t;

int64_t nxu_diskd_disk(uint32_t index, nxu_disk_info_t *info);
int64_t nxu_diskd_partition(uint32_t disk, uint32_t partition, nxu_partition_info_t *info);
int64_t nxu_diskd_mount(uint32_t device, const char *filesystem, const char *path);
int64_t nxu_diskd_unmount(const char *path);

/* ----- for the services themselves ----- */

/* Handle `message` from `peer` in place: what it holds when this returns is
 * the reply. */
typedef void (*nxu_service_handler_t)(nxu_service_message_t *message, const nxu_ipc_peer_t *peer);

/* Register `label` with bootd and answer requests for good, calling `idle`
 * (if any) at least every `idle_us` microseconds. Returns only on failure. */
int nxu_service_run(const char *label, nxu_service_handler_t handler, void (*idle)(void), uint32_t idle_us);

/* Within a handler: the port the request's answer goes to, and keep it
 * (rather than release it after answering) to send to again later. */
uint32_t nxu_service_reply_port(void);
void nxu_service_keep_reply_port(void);

/* Copy a NUL-terminated string into a fixed field, always terminating it. */
void nxu_service_copy(char *destination, uint64_t capacity, const char *source);

#endif
