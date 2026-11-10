// vpointer: inject pointer input through zwlr_virtual_pointer_v1 (the compositor treats it like a
// real mouse, so it goes through CInputManager::onMouseButton and the plugin's button listener).
//
//   vpointer move  <x> <y>              absolute move, layout coordinates
//   vpointer click <x> <y> [button]     move, press, release (default BTN_LEFT = 272)
//   vpointer drag  <x> <y> <x2> <y2>    press at (x,y), move to (x2,y2), release
//
// Absolute coordinates are given against the extent passed as VP_EXTENT=WxH (default 1920x1080),
// which must match the layout of the nested test compositor.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client.h"

static struct zwlr_virtual_pointer_manager_v1* g_mgr  = NULL;
static struct wl_seat*                         g_seat = NULL;

static void                                    onGlobal(void* data, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
        g_mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (strcmp(iface, wl_seat_interface.name) == 0)
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}
static void onGlobalRemove(void* data, struct wl_registry* r, uint32_t name) {
    ;
}
static const struct wl_registry_listener REGISTRY_LISTENER = {.global = onGlobal, .global_remove = onGlobalRemove};

static uint32_t                          nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void pause_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s move|click|drag x y [button | x2 y2]\n", argv[0]);
        return 2;
    }

    uint32_t extW = 1920, extH = 1080;
    if (getenv("VP_EXTENT"))
        sscanf(getenv("VP_EXTENT"), "%ux%u", &extW, &extH);

    const char*        mode = argv[1];
    const uint32_t     x = (uint32_t)atoi(argv[2]), y = (uint32_t)atoi(argv[3]);

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &REGISTRY_LISTENER, NULL);
    wl_display_roundtrip(display);
    if (!g_mgr) {
        fprintf(stderr, "no zwlr_virtual_pointer_manager_v1\n");
        return 1;
    }

    struct zwlr_virtual_pointer_v1* vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(g_mgr, g_seat);
    wl_display_roundtrip(display);
    pause_ms(50); // let the compositor register the new pointer device

    zwlr_virtual_pointer_v1_motion_absolute(vp, nowMs(), x, y, extW, extH);
    zwlr_virtual_pointer_v1_frame(vp);
    wl_display_roundtrip(display);
    pause_ms(50);

    if (strcmp(mode, "click") == 0 || strcmp(mode, "drag") == 0) {
        const uint32_t button = (strcmp(mode, "click") == 0 && argc >= 5) ? (uint32_t)atoi(argv[4]) : 272;
        zwlr_virtual_pointer_v1_button(vp, nowMs(), button, WL_POINTER_BUTTON_STATE_PRESSED);
        zwlr_virtual_pointer_v1_frame(vp);
        wl_display_roundtrip(display);
        pause_ms(40);

        if (strcmp(mode, "drag") == 0 && argc >= 6) {
            zwlr_virtual_pointer_v1_motion_absolute(vp, nowMs(), (uint32_t)atoi(argv[4]), (uint32_t)atoi(argv[5]), extW, extH);
            zwlr_virtual_pointer_v1_frame(vp);
            wl_display_roundtrip(display);
            pause_ms(40);
        }

        zwlr_virtual_pointer_v1_button(vp, nowMs(), button, WL_POINTER_BUTTON_STATE_RELEASED);
        zwlr_virtual_pointer_v1_frame(vp);
        wl_display_roundtrip(display);
        pause_ms(40);
    }

    zwlr_virtual_pointer_v1_destroy(vp);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
