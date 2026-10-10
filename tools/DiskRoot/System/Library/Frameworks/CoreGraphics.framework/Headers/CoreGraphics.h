#ifndef CORE_GRAPHICS_H
#define CORE_GRAPHICS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * CoreGraphics.framework public NXU ABI.
 *
 * Applications do not include this header. NXU owns devices and memory;
 * CoreGraphics receives a versioned host table containing borrowed surfaces,
 * presentation callbacks and normalized input events.
 */

#define CORE_GRAPHICS_API_VERSION 1U

#define CORE_GRAPHICS_BUTTON_IDLE 0U
#define CORE_GRAPHICS_BUTTON_HOVERED 1U
#define CORE_GRAPHICS_BUTTON_PRESSED 2U
#define CORE_GRAPHICS_BUTTON_DISABLED 3U

#define CORE_GRAPHICS_ABI_VERSION_V1 1U
#define CORE_GRAPHICS_ABI_VERSION_V2 2U
#define CORE_GRAPHICS_ABI_VERSION_V3 3U
#define CORE_GRAPHICS_ABI_VERSION_V4 4U
#define CORE_GRAPHICS_ABI_VERSION_V5 5U
#define CORE_GRAPHICS_ABI_VERSION CORE_GRAPHICS_ABI_VERSION_V5

#define CORE_GRAPHICS_PIXEL_FORMAT_XRGB8888 1U

#define CORE_GRAPHICS_HOST_CAP_PRESENT (1ULL << 0U)
#define CORE_GRAPHICS_HOST_CAP_DAMAGE (1ULL << 1U)
#define CORE_GRAPHICS_HOST_CAP_INPUT (1ULL << 2U)
#define CORE_GRAPHICS_HOST_CAP_TIME (1ULL << 3U)
#define CORE_GRAPHICS_HOST_CAP_FS (1ULL << 4U)
/* The host sends CORE_GRAPHICS_EVENT_KEY_DOWN events (optional, any input host). */
#define CORE_GRAPHICS_HOST_CAP_KEYBOARD (1ULL << 5U)
/* The host fills in CoreGraphicsActivity through get_activity (optional, v5). */
#define CORE_GRAPHICS_HOST_CAP_ACTIVITY (1ULL << 6U)
#define CORE_GRAPHICS_HOST_CAPABILITIES_V1 (CORE_GRAPHICS_HOST_CAP_PRESENT | CORE_GRAPHICS_HOST_CAP_DAMAGE)
#define CORE_GRAPHICS_HOST_CAPABILITIES_V2 (CORE_GRAPHICS_HOST_CAPABILITIES_V1 | CORE_GRAPHICS_HOST_CAP_INPUT)
#define CORE_GRAPHICS_HOST_CAPABILITIES_V3 (CORE_GRAPHICS_HOST_CAPABILITIES_V2 | CORE_GRAPHICS_HOST_CAP_TIME)
#define CORE_GRAPHICS_HOST_CAPABILITIES_V4 CORE_GRAPHICS_HOST_CAPABILITIES_V3
/* V5's filesystem capability is optional even on a v5 host (About sevOS has
 * no use for it), so it is not folded into a "V5 requires FS" default set;
 * a host adds CORE_GRAPHICS_HOST_CAP_FS itself when it actually implements
 * list_directory. */

#define CORE_GRAPHICS_STATUS_OK 0U
#define CORE_GRAPHICS_STATUS_INVALID_ARGUMENT 1U
#define CORE_GRAPHICS_STATUS_BAD_VERSION 2U
#define CORE_GRAPHICS_STATUS_UNSUPPORTED 3U
#define CORE_GRAPHICS_STATUS_NO_SURFACE 4U
#define CORE_GRAPHICS_STATUS_PRESENT_FAILED 5U

#define CORE_GRAPHICS_EVENT_NONE 0U
#define CORE_GRAPHICS_EVENT_POINTER_MOVED 1U
#define CORE_GRAPHICS_EVENT_POINTER_DOWN 2U
#define CORE_GRAPHICS_EVENT_POINTER_UP 3U
/* The scroll wheel (or a trackpad's scroll) turned: `reserved` holds the signed
 * number of notches (an int32_t, positive = wheel up), `x`/`y` the pointer. */
#define CORE_GRAPHICS_EVENT_SCROLL 4U
/* A key went down or auto-repeats: `button` holds the evdev key code,
 * `reserved` the character it types (a Unicode scalar, 0 for none),
 * `x`/`y` the pointer. Only with CORE_GRAPHICS_HOST_CAP_KEYBOARD. */
#define CORE_GRAPHICS_EVENT_KEY_DOWN 5U

#define CORE_GRAPHICS_POINTER_BUTTON_NONE 0U
#define CORE_GRAPHICS_POINTER_BUTTON_PRIMARY 1U
#define CORE_GRAPHICS_POINTER_BUTTON_SECONDARY 2U
#define CORE_GRAPHICS_POINTER_BUTTON_MIDDLE 3U

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
} CoreGraphicsABIHeader;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} CoreGraphicsDamageRect;

typedef struct {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride_pixels;
    uint32_t pixel_format;
} CoreGraphicsSurfaceDescriptor;

typedef struct {
    uint32_t event_type;
    uint32_t struct_size;
    int32_t x;
    int32_t y;
    uint32_t button;
    /* Zero for every pointer event; the signed scroll amount, in notches, for
     * CORE_GRAPHICS_EVENT_SCROLL (an int32_t stored as its uint32_t bit pattern,
     * so the struct's size and layout are unchanged). */
    uint32_t reserved;
} CoreGraphicsHostEvent;

typedef uint32_t (*CoreGraphicsGetSurfaceFn)(void *context, CoreGraphicsSurfaceDescriptor *surface);
typedef uint32_t (*CoreGraphicsPresentFn)(void *context, const CoreGraphicsDamageRect *damage);
typedef uint32_t (*CoreGraphicsPollEventFn)(void *context, CoreGraphicsHostEvent *event);
/* Seconds since 1970-01-01 00:00:00 UTC, written through *unix_seconds. */
typedef uint32_t (*CoreGraphicsGetTimeFn)(void *context, uint64_t *unix_seconds);

#define CORE_GRAPHICS_FS_NAME_MAX 63U

#define CORE_GRAPHICS_FS_KIND_REGULAR 0U
#define CORE_GRAPHICS_FS_KIND_DIRECTORY 1U
#define CORE_GRAPHICS_FS_KIND_OTHER 2U

#define CORE_GRAPHICS_FS_STATUS_OK 0U
#define CORE_GRAPHICS_FS_STATUS_NOT_FOUND 1U
#define CORE_GRAPHICS_FS_STATUS_NOT_A_DIRECTORY 2U
#define CORE_GRAPHICS_FS_STATUS_ERROR 3U

/*
 * One directory entry. `name` is NUL-terminated and truncated to
 * CORE_GRAPHICS_FS_NAME_MAX bytes if the real name is longer; `name_length` is
 * its length before any truncation/NUL. `kind` is one of
 * CORE_GRAPHICS_FS_KIND_*. `size_bytes` is 0 for anything but a regular file.
 *
 * This layout is mirrored bit-for-bit by `ui_core::fs::DirEntry` on the
 * Rust side (`#[repr(C)]`, same field order/types) so the host can write
 * directly into an app-provided buffer with no intermediate copy -- keep
 * the two in sync if this ever changes.
 */
typedef struct {
    uint32_t name_length;
    char name[CORE_GRAPHICS_FS_NAME_MAX + 1U];
    uint32_t kind;
    uint64_t size_bytes;
} CoreGraphicsDirEntry;

/*
 * List up to `capacity` entries of the directory at `path` (an absolute
 * path, `path_len` bytes, not necessarily NUL-terminated) into `entries`.
 * `*count_out` receives the number actually written (<= capacity).
 * `*truncated_out` is set true when the directory holds more entries than
 * `capacity` allowed -- the first `*count_out` are still valid, just not
 * the whole directory. Returns a CORE_GRAPHICS_FS_STATUS_* code.
 */
typedef uint32_t (*CoreGraphicsListDirectoryFn)(
    void *context,
    const char *path,
    uint32_t path_len,
    CoreGraphicsDirEntry *entries,
    uint32_t capacity,
    uint32_t *count_out,
    bool *truncated_out
);

/*
 * What Activity Monitor shows: the machine's CPUs and memory, and one
 * CoreGraphicsProcessInfo per live process. Mirrored bit-for-bit by
 * `ui_core::activity` (repr(C), same order). Every 64-bit field sits at a
 * multiple of 8 so the layout is the same on i386, where a C uint64_t in a
 * struct is only 4-aligned.
 *
 * CPU time is counted in scheduler ticks, not seconds: a process's share of
 * the machine is how far its cpu_ticks moved against one CPU's `ticks` over
 * the same interval (100% = one whole CPU), which needs no tick rate.
 */
#define CORE_GRAPHICS_ACTIVITY_NAME_MAX 31U
#define CORE_GRAPHICS_ACTIVITY_CPU_MAX 8U

/* CoreGraphicsProcessInfo.state */
#define CORE_GRAPHICS_PROCESS_STATE_RUNNING 0U   /* one of its threads is on a CPU */
#define CORE_GRAPHICS_PROCESS_STATE_RUNNABLE 1U  /* ready, waiting for a CPU */
#define CORE_GRAPHICS_PROCESS_STATE_SLEEPING 2U  /* every thread is waiting */
#define CORE_GRAPHICS_PROCESS_STATE_STOPPED 3U
#define CORE_GRAPHICS_PROCESS_STATE_OTHER 4U     /* being created or exiting */

/* CoreGraphicsProcessInfo.flags */
#define CORE_GRAPHICS_PROCESS_FLAG_KERNEL (1U << 0U)
#define CORE_GRAPHICS_PROCESS_FLAG_SYSTEM (1U << 1U)

typedef struct {
    uint64_t uniqueid;
    /* Scheduler ticks its live threads have run, all CPUs together. */
    uint64_t cpu_ticks;
    uint32_t pid;
    uint32_t ppid;
    uint32_t state;
    uint32_t flags;
    uint32_t threads;
    /* The highest-priority (lowest-numbered) MLFQ level of its threads. */
    uint32_t mlfq_level;
    /* The CPU its most recently scheduled thread ran on. */
    uint32_t last_cpu;
    uint32_t name_length;
    char name[CORE_GRAPHICS_ACTIVITY_NAME_MAX + 1U];
} CoreGraphicsProcessInfo;

typedef struct {
    /* Scheduler ticks this CPU took, and how many found it running something. */
    uint64_t ticks;
    uint64_t busy_ticks;
    uint64_t context_switches;
    uint32_t online;
    uint32_t reserved;
} CoreGraphicsCpuInfo;

typedef struct {
    uint32_t struct_size;
    uint32_t cpu_count;
    uint64_t uptime_us;
    uint64_t page_size;
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t heap_pages;
    uint32_t process_count;
    uint32_t thread_count;
    CoreGraphicsCpuInfo cpus[CORE_GRAPHICS_ACTIVITY_CPU_MAX];
} CoreGraphicsActivity;

/*
 * Fill `activity` and up to `capacity` entries of `processes`; `*count_out`
 * receives how many were written (activity->process_count is the total,
 * which can be larger). Returns a CORE_GRAPHICS_STATUS_* code.
 */
typedef uint32_t (*CoreGraphicsGetActivityFn)(
    void *context,
    CoreGraphicsActivity *activity,
    CoreGraphicsProcessInfo *processes,
    uint32_t capacity,
    uint32_t *count_out
);

typedef struct {
    CoreGraphicsABIHeader header;
    uint64_t capabilities;
    void *context;
    CoreGraphicsGetSurfaceFn get_surface;
    CoreGraphicsPresentFn present;
} CoreGraphicsHostV1;

typedef struct {
    CoreGraphicsABIHeader header;
    uint64_t capabilities;
    void *context;
    CoreGraphicsGetSurfaceFn get_surface;
    CoreGraphicsPresentFn present;
    CoreGraphicsPollEventFn poll_event;
} CoreGraphicsHostV2;

typedef struct {
    CoreGraphicsABIHeader header;
    uint64_t capabilities;
    void *context;
    CoreGraphicsGetSurfaceFn get_surface;
    CoreGraphicsPresentFn present;
    CoreGraphicsPollEventFn poll_event;
    CoreGraphicsGetTimeFn get_time;
} CoreGraphicsHostV3;

typedef struct {
    CoreGraphicsABIHeader header;
    uint64_t capabilities;
    void *context;
    CoreGraphicsGetSurfaceFn get_surface;
    CoreGraphicsPresentFn present;
    CoreGraphicsPollEventFn poll_event;
    CoreGraphicsGetTimeFn get_time;
    /*
     * Physical pixels per design point, as thousandths (e.g. 2000 for a
     * genuine 2x-Retina host, 1000 for a 1x/non-Retina one). 0 means
     * "unknown"; CoreGraphics keeps its historical 2x default in that case.
     * Lets CoreGraphics size window chrome, fonts and control metrics for the
     * host's actual density instead of assuming every canvas is 2x.
     */
    uint32_t content_scale_permille;
} CoreGraphicsHostV4;

typedef struct {
    CoreGraphicsABIHeader header;
    uint64_t capabilities;
    void *context;
    CoreGraphicsGetSurfaceFn get_surface;
    CoreGraphicsPresentFn present;
    CoreGraphicsPollEventFn poll_event;
    CoreGraphicsGetTimeFn get_time;
    uint32_t content_scale_permille;
    /*
     * Read-only directory listing. NULL, with CORE_GRAPHICS_HOST_CAP_FS unset,
     * on a host that has no filesystem to offer (or nothing worth exposing
     * yet) -- an app that needs it treats that as "unavailable", not an
     * error, and falls back to whatever static content it shipped with.
     */
    CoreGraphicsListDirectoryFn list_directory;
    /*
     * Processes, CPUs and memory for Activity Monitor. NULL, with
     * CORE_GRAPHICS_HOST_CAP_ACTIVITY unset, on a host with nothing to report.
     */
    CoreGraphicsGetActivityFn get_activity;
} CoreGraphicsHostV5;

uint32_t CoreGraphicsAPIVersion(void);
uint32_t CoreGraphicsABIVersion(void);
uint32_t CoreGraphicsHasInter(void);
uint32_t CoreGraphicsHostV1Size(void);
uint32_t CoreGraphicsHostV2Size(void);
uint32_t CoreGraphicsHostV3Size(void);
uint32_t CoreGraphicsHostV4Size(void);
uint32_t CoreGraphicsHostV5Size(void);
uint32_t CoreGraphicsValidateHost(const CoreGraphicsHostV1 *host);
uint32_t CoreGraphicsValidateHostV2(const CoreGraphicsHostV2 *host);
uint32_t CoreGraphicsValidateHostV3(const CoreGraphicsHostV3 *host);
uint32_t CoreGraphicsValidateHostV4(const CoreGraphicsHostV4 *host);
uint32_t CoreGraphicsValidateHostV5(const CoreGraphicsHostV5 *host);
uint32_t CoreGraphicsHostClear(const CoreGraphicsHostV1 *host, uint32_t xrgb8888);
/* Bring-up helpers. Normal applications should use the Rust app API. */
uint32_t CoreGraphicsClear(
    uint32_t *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride_pixels,
    uint32_t xrgb8888
);

uint32_t CoreGraphicsDrawButton(
    uint32_t *pixels,
    uint32_t surface_width,
    uint32_t surface_height,
    uint32_t stride_pixels,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t state
);

uint32_t CoreGraphicsDrawDemo(
    uint32_t *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride_pixels
);

typedef uint32_t CGWindowID;
CGWindowID CG_Create_Window(int32_t x, int32_t y, uint32_t width, uint32_t height, bool opaque, uint32_t corner_radius);
CGWindowID CG_Create_Background_Window(int32_t x, int32_t y, uint32_t width, uint32_t height);
bool CG_Destroy_Window(CGWindowID window_id);
bool CG_Move_Window(CGWindowID window_id, int32_t x, int32_t y);
bool CG_Set_Size_Limits(CGWindowID window_id, uint32_t min_width, uint32_t min_height, uint32_t max_width, uint32_t max_height);
bool CG_Resize_Window(CGWindowID window_id, int32_t x, int32_t y, uint32_t width, uint32_t height);
bool CG_Focus_Window(CGWindowID window_id);
bool CG_Set_Window_Shadow(CGWindowID window_id, uint32_t style);
bool CG_Set_Window_Opaque_Interior(CGWindowID window_id, bool opaque_interior);
bool CG_Set_Window_Key(CGWindowID window_id, bool key);
void CG_Set_Shadow_Scale(uint32_t permille);
bool CG_Render_Window(CGWindowID window_id, const uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride);
bool CG_Set_Cursor(uint32_t kind, const uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride, int32_t hotspot_x, int32_t hotspot_y);
bool CG_Set_Cursor_Kind(uint32_t kind);
bool CG_Pointer_Move(int32_t x, int32_t y);
bool CG_Present(void);
bool CoreGraphicsDisplaySize(uint32_t *width, uint32_t *height);
bool CoreGraphicsConnectionAlive(void);

#endif
