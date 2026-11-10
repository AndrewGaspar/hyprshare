// click-logger: a minimal xdg-shell window for the glyph tests.
//
//   click-logger <app-id> [rrggbb]
//
// Fills itself with one solid colour (default 00ff00), so a capture readback can tell window pixels
// from glyph pixels, and prints every wl_pointer button it receives:
//
//   button <code> <pressed|released> <surface-x> <surface-y>
//
// A glyph click that the plugin consumed must never show up here.

#define _GNU_SOURCE

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client.h"

static struct wl_compositor* g_compositor = NULL;
static struct wl_shm*        g_shm        = NULL;
static struct wl_seat*       g_seat       = NULL;
static struct xdg_wm_base*   g_wmBase     = NULL;
static struct wl_surface*    g_surface    = NULL;
static struct wl_buffer*     g_buffer     = NULL;
static int32_t               g_width = 0, g_height = 0;
static uint32_t              g_color   = 0xFF00FF00;
static bool                  g_running = true;
static double                g_px = 0, g_py = 0;

static void                  onPing(void* data, struct xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener WM_BASE_LISTENER = {.ping = onPing};

static struct wl_buffer*                 makeBuffer(int32_t w, int32_t h) {
    const int32_t stride = w * 4;
    const size_t  size   = (size_t)stride * h;
    const int     fd     = memfd_create("click-logger", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0)
        return NULL;
    uint32_t* px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (size_t i = 0; i < size / 4; ++i)
        px[i] = g_color;
    munmap(px, size);
    struct wl_shm_pool* pool   = wl_shm_create_pool(g_shm, fd, (int32_t)size);
    struct wl_buffer*   buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void onSurfaceConfigure(void* data, struct xdg_surface* s, uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
    const int32_t w = g_width > 0 ? g_width : 400, h = g_height > 0 ? g_height : 300;
    if (g_buffer)
        wl_buffer_destroy(g_buffer);
    g_buffer = makeBuffer(w, h);
    wl_surface_attach(g_surface, g_buffer, 0, 0);
    wl_surface_damage_buffer(g_surface, 0, 0, w, h);
    wl_surface_commit(g_surface);
}
static const struct xdg_surface_listener SURFACE_LISTENER = {.configure = onSurfaceConfigure};

static void                              onToplevelConfigure(void* data, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states) {
    if (w > 0 && h > 0) {
        g_width  = w;
        g_height = h;
    }
}
static void onToplevelClose(void* data, struct xdg_toplevel* t) {
    g_running = false;
}
static void onToplevelBounds(void* data, struct xdg_toplevel* t, int32_t w, int32_t h) {
    ;
}
static void onToplevelCaps(void* data, struct xdg_toplevel* t, struct wl_array* caps) {
    ;
}
static const struct xdg_toplevel_listener TOPLEVEL_LISTENER = {
    .configure        = onToplevelConfigure,
    .close            = onToplevelClose,
    .configure_bounds = onToplevelBounds,
    .wm_capabilities  = onToplevelCaps,
};

static void onEnter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
    g_px = wl_fixed_to_double(x);
    g_py = wl_fixed_to_double(y);
}
static void onLeave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s) {
    ;
}
static void onMotion(void* d, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    g_px = wl_fixed_to_double(x);
    g_py = wl_fixed_to_double(y);
}
static void onButton(void* d, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    printf("button %u %s %.0f %.0f\n", button, state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released", g_px, g_py);
    fflush(stdout);
}
static void onAxis(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value) {
    ;
}
static void onFrame(void* d, struct wl_pointer* p) {
    ;
}
static void onAxisSource(void* d, struct wl_pointer* p, uint32_t src) {
    ;
}
static void onAxisStop(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis) {
    ;
}
static void onAxisDiscrete(void* d, struct wl_pointer* p, uint32_t axis, int32_t discrete) {
    ;
}
static void onAxis120(void* d, struct wl_pointer* p, uint32_t axis, int32_t value) {
    ;
}
static void onAxisDir(void* d, struct wl_pointer* p, uint32_t axis, uint32_t dir) {
    ;
}
static const struct wl_pointer_listener POINTER_LISTENER = {
    .enter                   = onEnter,
    .leave                   = onLeave,
    .motion                  = onMotion,
    .button                  = onButton,
    .axis                    = onAxis,
    .frame                   = onFrame,
    .axis_source             = onAxisSource,
    .axis_stop               = onAxisStop,
    .axis_discrete           = onAxisDiscrete,
    .axis_value120           = onAxis120,
    .axis_relative_direction = onAxisDir,
};

static void onSeatCaps(void* data, struct wl_seat* seat, uint32_t caps) {
    static struct wl_pointer* pointer = NULL;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &POINTER_LISTENER, NULL);
    }
}
static void onSeatName(void* data, struct wl_seat* seat, const char* name) {
    ;
}
static const struct wl_seat_listener SEAT_LISTENER = {.capabilities = onSeatCaps, .name = onSeatName};

static void                          onGlobal(void* data, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        g_compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (strcmp(iface, wl_seat_interface.name) == 0) {
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, version < 9 ? version : 9);
        wl_seat_add_listener(g_seat, &SEAT_LISTENER, NULL);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        g_wmBase = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(g_wmBase, &WM_BASE_LISTENER, NULL);
    }
}
static void onGlobalRemove(void* data, struct wl_registry* r, uint32_t name) {
    ;
}
static const struct wl_registry_listener REGISTRY_LISTENER = {.global = onGlobal, .global_remove = onGlobalRemove};

int                                      main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <app-id> [rrggbb]\n", argv[0]);
        return 2;
    }
    if (argc >= 3)
        g_color = 0xFF000000 | (uint32_t)strtoul(argv[2], NULL, 16);

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &REGISTRY_LISTENER, NULL);
    wl_display_roundtrip(display);
    if (!g_compositor || !g_shm || !g_wmBase) {
        fprintf(stderr, "missing globals\n");
        return 1;
    }

    g_surface                        = wl_compositor_create_surface(g_compositor);
    struct xdg_surface*  xdgSurface  = xdg_wm_base_get_xdg_surface(g_wmBase, g_surface);
    struct xdg_toplevel* xdgToplevel = xdg_surface_get_toplevel(xdgSurface);
    xdg_surface_add_listener(xdgSurface, &SURFACE_LISTENER, NULL);
    xdg_toplevel_add_listener(xdgToplevel, &TOPLEVEL_LISTENER, NULL);
    xdg_toplevel_set_app_id(xdgToplevel, argv[1]);
    xdg_toplevel_set_title(xdgToplevel, argv[1]);
    wl_surface_commit(g_surface);

    while (g_running && wl_display_dispatch(display) >= 0)
        ;
    return 0;
}
