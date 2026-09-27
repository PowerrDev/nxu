# Skylight

Skylight is NXU's graphics API: the layer between whoever draws (WindowServer,
UIService, and through them the apps) and the graphics driver
(`skylight/skylight.h`, `skylight/skylight.c`).

It only understands graphics concepts:

| Concept | What it is |
| --- | --- |
| device | a graphics device, registered by its driver with a backend interface |
| display | a scanout of a device, with a surface whose pixels it shows |
| surface | pixels (`SL_FORMAT_XRGB8888` or `SL_FORMAT_ARGB8888`), created and destroyed, mapped for CPU rendering |
| rectangle | `sl_rect_t`; damage is the union of what changed |
| operations | `sl_fill_rect`, `sl_copy` (surface to surface, optionally blending ARGB over), `sl_upload` (a caller's pixels into a surface) |
| present | `sl_present`: a rectangle (or the accumulated damage) of a display's scanout surface onto the device |

It knows nothing of windows, buttons, fonts, the Dock or input. WindowServer
decides "Voyager is at (120, 80), Activity Monitor overlaps it, this region
needs recomposition"; Skylight performs the graphics operations and presents
the result; the backend decides how that reaches its device.

## Backends

A driver registers a device (`sl_device_register`) with an `sl_backend_ops_t`
and its scanouts (`sl_display_register`): a display's scanout surface wraps
memory the driver already owns (its scanout backing), and `present` tells the
device a rectangle of it changed. VirtIOGPUFamily (`drivers/virtio/virtio_gpu.c`)
is the first backend: `present` is its TRANSFER_TO_HOST_2D + RESOURCE_FLUSH,
waiting for completion by interrupt when it has one.

## Clients

- WindowServer (`kern/aqua/window_server.c`) composes into the primary
  display's scanout surface and presents through `sl_present`.
- UIService's host (`platform/*/services/ui_service.c`, the login screen)
  uploads its backbuffer's damage (`sl_upload`) and presents it. RAMFB, the
  arm64 emergency scanout, is not a Skylight backend and is still written
  directly.
- `drivers/video/display.c` remains the early path (the boot splash, the
  consoles, recovery) that runs before anything else exists.

## Synchronization

A scanout surface is busy while it is being presented: its pixels are the
device's copy source. `sl_surface_map` waits for that to end, so a buffer is
never modified mid-present. Presentation is synchronous today.

## Not yet

Command buffers, asynchronous submission with fences, textures, GPU
acceleration and other backends. Moving a window already reuses its surface
(WindowServer keeps each window's pixels); the next step is apps rendering
straight into Skylight surfaces shared with their process, instead of the UI
session bridge copying each frame in. No shaders, no 3D.
