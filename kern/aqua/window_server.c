/**
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */

/**
 * File:        kern/aqua/window_server.c
 *
 * NXU host bridge for WindowServer.framework.
 *
 * NXU owns hardware input and display presentation. This bridge translates
 * NXU's normalized relative mouse state into absolute WindowServer pointer
 * events and presents frames whenever the scene changes.
 */

#include <kern/aqua/window_server.h>

#include <drivers/input/mouse.h>
#include <drivers/video/display.h>
#include <drivers/virtio/virtio_input.h>

#include <WindowServer/WSPrivate.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Clamp an accumulated relative pointer coordinate to the visible display.
 */
static int32_t
windowserver_clamp_pointer(int64_t value, uint32_t extent)
{
    if (extent == 0U || value < 0) return 0;
    if ((uint64_t)value >= (uint64_t)extent) return (int32_t)(extent - 1U);
    return (int32_t)value;
}

/*
 * Translate one NXU mouse button mask into the WindowServer C ABI value.
 */
static uint32_t
windowserver_button_from_mask(uint32_t mask)
{
    if (mask == MOUSE_BUTTON_LEFT) return 0U;
    if (mask == MOUSE_BUTTON_RIGHT) return 1U;
    if (mask == MOUSE_BUTTON_MIDDLE) return 2U;
    return UINT32_MAX;
}

bool
windowserver_bootstrap(void)
{
#if !defined(NXU_WINDOWSERVER)
    return false;
#else
    display_device_t *display = display_primary();

    if (display == 0 ||
        display->framebuffer == 0 ||
        display->width == 0U ||
        display->height == 0U ||
        display->stride < display->width ||
        display->bytes_per_pixel != 4U) {
        return false;
    }

    WSDisplay ws_display = {
        .framebuffer = (uint32_t *)display->framebuffer,
        .width = display->width,
        .height = display->height,
        .stride = display->stride,
    };

    if (!windowserver_nxu_init(ws_display)) {
        return false;
    }

    /*
     * Keep the logical cursor independent from the mouse driver's raw relative
     * accumulator. This avoids the classic bug where starting at (0, 0) means
     * negative movement is permanently clamped and the cursor appears stuck.
     */
    int32_t pointer_x = (int32_t)(display->width / 2U);
    int32_t pointer_y = (int32_t)(display->height / 2U);
    int64_t raw_x = mouse_x();
    int64_t raw_y = mouse_y();
    uint64_t packet_count = mouse_packet_count();
    uint32_t buttons = mouse_buttons();

    if (!windowserver_nxu_pointer_move(pointer_x, pointer_y)) {
        return false;
    }

    if (!windowserver_nxu_present()) {
        return false;
    }

    if (!display_present_full(display)) {
        return false;
    }

    /*
     * This function intentionally does not return while the WindowServer boot
     * test owns the graphical session. The previous implementation returned
     * after one frame, after which kern_init() slept forever and no input was
     * ever forwarded into Rust.
     */
    for (;;) {
        bool changed = false;

        /* Drain VirtIO input and let the normalized mouse driver commit it. */
        virtio_input_service();

        uint64_t packets = mouse_packet_count();
        if (packets != packet_count) {
            int64_t next_raw_x = mouse_x();
            int64_t next_raw_y = mouse_y();

            int64_t next_x = (int64_t)pointer_x + (next_raw_x - raw_x);
            int64_t next_y = (int64_t)pointer_y + (next_raw_y - raw_y);

            int32_t clamped_x = windowserver_clamp_pointer(next_x, display->width);
            int32_t clamped_y = windowserver_clamp_pointer(next_y, display->height);

            raw_x = next_raw_x;
            raw_y = next_raw_y;
            packet_count = packets;

            if (clamped_x != pointer_x || clamped_y != pointer_y) {
                pointer_x = clamped_x;
                pointer_y = clamped_y;

                if (!windowserver_nxu_pointer_move(pointer_x, pointer_y)) {
                    return false;
                }

                changed = true;
            }
        }

        uint32_t next_buttons = mouse_buttons();
        uint32_t changed_buttons = buttons ^ next_buttons;

        const uint32_t button_masks[] = {
            MOUSE_BUTTON_LEFT,
            MOUSE_BUTTON_RIGHT,
            MOUSE_BUTTON_MIDDLE,
        };

        for (uint32_t i = 0U; i < 3U; ++i) {
            uint32_t mask = button_masks[i];
            if ((changed_buttons & mask) == 0U) continue;

            uint32_t button = windowserver_button_from_mask(mask);
            bool pressed = (next_buttons & mask) != 0U;

            if (button == UINT32_MAX ||
                !windowserver_nxu_pointer_button(pointer_x, pointer_y, button, pressed)) {
                return false;
            }

            changed = true;
        }

        buttons = next_buttons;

        if (changed) {
            if (!windowserver_nxu_present()) {
                return false;
            }

            if (!display_present_full(display)) {
                return false;
            }
        }

        /*
         * Temporary polling loop.
         *
         * Do not sleep here yet: early VirtIO input polling does not
         * currently guarantee an ARM event that wakes WFE.
         */
        __asm__ volatile("yield");
    }
#endif
}
