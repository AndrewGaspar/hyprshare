// capture-probe: a minimal stand-in for xdg-desktop-portal-hyprland's window capture loop.
//
// Like XDPH, it issues a fresh hyprland_toplevel_export_manager_v1 capture request for every frame,
// and prints the buffer size the compositor advertises for it:
//
//   frame <n> <width>x<height>     (or "frame <n> failed" / "frame <n> none")
//
// By default it never copies: it destroys each frame right after buffer_done (or, with --hold, keeps
// the first frame alive forever, like a frame stuck waiting for damage). With --copy it allocates a
// wl_shm buffer of the advertised size, asks for a copy (ignore_damage=1, so it completes without
// new damage) and appends the outcome: "frame <n> <w>x<h> ready" / "... copyfailed" / "... copytimeout".
//
// Selecting the window:
//   --handle <n>      capture_toplevel with a uint handle (low 32 bits of the hyprctl address)
//   --app-id <id>     capture_toplevel_with_wlr_toplevel_handle with the zwlr_foreign_toplevel
//                     handle whose app_id matches — the exact request XDPH uses. The handle is kept
//                     after the toplevel closes, as XDPH does.
//
// --xdph waits for buffer_done/failed forever instead of giving up after 1s, like the portal; on
// `failed` it destroys the spent frame and re-requests (patched XDPH's failed-frame handling).
//
// Other options: --count <n> (0 = forever, default), --interval-ms <ms> (default 100), --copy.

#define _GNU_SOURCE

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <fcntl.h>
#include <gbm.h>
#include <wayland-client.h>

#include "hyprland-toplevel-export-v1-client.h"
#include "linux-dmabuf-v1-client.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client.h"

struct SToplevel {
    struct zwlr_foreign_toplevel_handle_v1* handle;
    char*                                   appID;
    bool                                    closed;
};

static struct hyprland_toplevel_export_manager_v1* g_exportManager   = NULL;
static uint32_t                                    g_exportVersion   = 0;
static struct zwlr_foreign_toplevel_manager_v1*    g_toplevelManager = NULL;
static struct wl_shm*                              g_shm             = NULL;
static struct zwp_linux_dmabuf_v1*                 g_dmabuf          = NULL;
static uint32_t                                    g_dmabufFormat = 0, g_dmabufW = 0, g_dmabufH = 0;
static struct gbm_device*                          g_gbm         = NULL;
static struct wl_buffer*                           g_paramBuffer = NULL;
static struct SToplevel                            g_toplevels[256];
static size_t                                      g_toplevelCount = 0;

static bool                                        g_frameDone = false;
static uint32_t                                    g_width = 0, g_height = 0, g_stride = 0, g_format = 0;
static bool                                        g_failed   = false;
static bool                                        g_copyDone = false, g_copyReady = false;

static void                                        onRegistryGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (strcmp(interface, hyprland_toplevel_export_manager_v1_interface.name) == 0) {
        g_exportVersion = version < 2 ? version : 2;
        g_exportManager = wl_registry_bind(registry, name, &hyprland_toplevel_export_manager_v1_interface, g_exportVersion);
    } else if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0)
        g_toplevelManager = wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, version < 3 ? version : 3);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        g_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0)
        g_dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, version < 3 ? version : 3);
}

static void onRegistryGlobalRemove(void* data, struct wl_registry* registry, uint32_t name) {
    ;
}

static const struct wl_registry_listener REGISTRY_LISTENER = {
    .global        = onRegistryGlobal,
    .global_remove = onRegistryGlobalRemove,
};

static void onToplevelTitle(void* data, struct zwlr_foreign_toplevel_handle_v1* h, const char* title) {
    ;
}

static void onToplevelAppID(void* data, struct zwlr_foreign_toplevel_handle_v1* h, const char* appID) {
    struct SToplevel* t = data;
    free(t->appID);
    t->appID = strdup(appID);
}

static void onToplevelOutputEnter(void* data, struct zwlr_foreign_toplevel_handle_v1* h, struct wl_output* o) {
    ;
}

static void onToplevelOutputLeave(void* data, struct zwlr_foreign_toplevel_handle_v1* h, struct wl_output* o) {
    ;
}

static void onToplevelState(void* data, struct zwlr_foreign_toplevel_handle_v1* h, struct wl_array* state) {
    ;
}

static void onToplevelDone(void* data, struct zwlr_foreign_toplevel_handle_v1* h) {
    ;
}

static void onToplevelClosed(void* data, struct zwlr_foreign_toplevel_handle_v1* h) {
    struct SToplevel* t = data;
    t->closed           = true;
    fprintf(stderr, "toplevel %s closed (keeping the handle, like XDPH)\n", t->appID ? t->appID : "?");
}

static void onToplevelParent(void* data, struct zwlr_foreign_toplevel_handle_v1* h, struct zwlr_foreign_toplevel_handle_v1* parent) {
    ;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener TOPLEVEL_LISTENER = {
    .title        = onToplevelTitle,
    .app_id       = onToplevelAppID,
    .output_enter = onToplevelOutputEnter,
    .output_leave = onToplevelOutputLeave,
    .state        = onToplevelState,
    .done         = onToplevelDone,
    .closed       = onToplevelClosed,
    .parent       = onToplevelParent,
};

static void onToplevelManagerToplevel(void* data, struct zwlr_foreign_toplevel_manager_v1* m, struct zwlr_foreign_toplevel_handle_v1* handle) {
    if (g_toplevelCount >= sizeof(g_toplevels) / sizeof(g_toplevels[0]))
        return;

    struct SToplevel* t = &g_toplevels[g_toplevelCount++];
    t->handle           = handle;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &TOPLEVEL_LISTENER, t);
}

static void onToplevelManagerFinished(void* data, struct zwlr_foreign_toplevel_manager_v1* m) {
    ;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener TOPLEVEL_MANAGER_LISTENER = {
    .toplevel = onToplevelManagerToplevel,
    .finished = onToplevelManagerFinished,
};

static void onFrameBuffer(void* data, struct hyprland_toplevel_export_frame_v1* f, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    g_width  = width;
    g_height = height;
    g_stride = stride;
    g_format = format;
}

static void onFrameDamage(void* data, struct hyprland_toplevel_export_frame_v1* f, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    ;
}

static void onFrameFlags(void* data, struct hyprland_toplevel_export_frame_v1* f, uint32_t flags) {
    ;
}

static void onFrameReady(void* data, struct hyprland_toplevel_export_frame_v1* f, uint32_t a, uint32_t b, uint32_t c) {
    g_copyDone  = true;
    g_copyReady = true;
}

static void onFrameFailed(void* data, struct hyprland_toplevel_export_frame_v1* f) {
    if (g_frameDone) { // after buffer_done: the copy failed
        g_copyDone = true;
        return;
    }
    g_failed    = true;
    g_frameDone = true;
}

static void onFrameDmabuf(void* data, struct hyprland_toplevel_export_frame_v1* f, uint32_t format, uint32_t width, uint32_t height) {
    g_dmabufFormat = format;
    g_dmabufW      = width;
    g_dmabufH      = height;
}

static void onParamsCreated(void* data, struct zwp_linux_buffer_params_v1* params, struct wl_buffer* buffer) {
    g_paramBuffer = buffer;
}

static void onParamsFailed(void* data, struct zwp_linux_buffer_params_v1* params) {
    g_paramBuffer = NULL;
}

static const struct zwp_linux_buffer_params_v1_listener PARAMS_LISTENER = {
    .created = onParamsCreated,
    .failed  = onParamsFailed,
};

static void onFrameBufferDone(void* data, struct hyprland_toplevel_export_frame_v1* f) {
    g_frameDone = true;
}

static const struct hyprland_toplevel_export_frame_v1_listener FRAME_LISTENER = {
    .buffer       = onFrameBuffer,
    .damage       = onFrameDamage,
    .flags        = onFrameFlags,
    .ready        = onFrameReady,
    .failed       = onFrameFailed,
    .linux_dmabuf = onFrameDmabuf,
    .buffer_done  = onFrameBufferDone,
};

static void sleepMs(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// Pump events for up to `ms`, until *flag is set.
static void waitFor(struct wl_display* display, const bool* flag, int ms) {
    for (int waited = 0; !*flag && waited < ms; waited += 10) {
        struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);
        if (poll(&pfd, 1, 10) > 0)
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);
    }
}

// True if the first pixel (any 32bpp XRGB/ARGB variant) is opaque black.
static bool firstPixelBlack(const uint8_t* p) {
    return (p[0] & 0xFF) == 0 && (p[1] & 0xFF) == 0 && (p[2] & 0xFF) == 0;
}

// Copy the frame into a fresh shm buffer. Returns ready[-black|-nonblack], copyfailed or copytimeout.
static const char* copyFrameShm(struct wl_display* display, struct hyprland_toplevel_export_frame_v1* frame) {
    const size_t size = (size_t)g_stride * g_height;
    const int    fd   = memfd_create("capture-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0)
        return "copyfailed";

    uint8_t* map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map != MAP_FAILED)
        memset(map, 0x7f, size); // fill grey first, so "black" readback means the copy really wrote

    struct wl_shm_pool* pool   = wl_shm_create_pool(g_shm, fd, (int32_t)size);
    struct wl_buffer*   buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)g_width, (int32_t)g_height, (int32_t)g_stride, g_format);

    g_copyDone = g_copyReady = false;
    hyprland_toplevel_export_frame_v1_copy(frame, buffer, 1);
    wl_display_flush(display);
    waitFor(display, &g_copyDone, 2000);

    const char* result = !g_copyDone ? "copytimeout" : g_copyReady ? (map != MAP_FAILED && firstPixelBlack(map) ? "ready-black" : "ready-nonblack") : "copyfailed";

    wl_buffer_destroy(buffer);
    wl_shm_pool_destroy(pool);
    if (map != MAP_FAILED)
        munmap(map, size);
    close(fd);
    return result;
}

// Copy the frame into a gbm dmabuf (the path real XDPH/PipeWire uses). Returns ready[-black|-nonblack],
// copyfailed or copytimeout. Reads the first pixel back by mapping the linear bo.
static const char* copyFrameDmabuf(struct wl_display* display, struct hyprland_toplevel_export_frame_v1* frame) {
    if (!g_gbm || !g_dmabuf || g_dmabufW == 0)
        return "copyfailed";

    // LINEAR so we can map it for readback, and RENDERING so the compositor can import+render to it.
    struct gbm_bo* bo = gbm_bo_create(g_gbm, g_dmabufW, g_dmabufH, g_dmabufFormat, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo)
        return "copyfailed";

    const int                          fd       = gbm_bo_get_fd(bo);
    const uint32_t                     stride   = gbm_bo_get_stride(bo);
    const uint64_t                     modifier = gbm_bo_get_modifier(bo);

    struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(g_dmabuf);
    zwp_linux_buffer_params_v1_add_listener(params, &PARAMS_LISTENER, NULL);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride, (uint32_t)(modifier >> 32), (uint32_t)(modifier & 0xFFFFFFFF));

    g_paramBuffer            = NULL;
    struct wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(params, (int32_t)g_dmabufW, (int32_t)g_dmabufH, g_dmabufFormat, 0);

    g_copyDone = g_copyReady = false;
    hyprland_toplevel_export_frame_v1_copy(frame, buffer, 1);
    wl_display_flush(display);
    waitFor(display, &g_copyDone, 2000);

    const char* result = "copyfailed";
    if (!g_copyDone)
        result = "copytimeout";
    else if (g_copyReady) {
        void*    mapData   = NULL;
        uint32_t mapStride = 0;
        void*    handle    = NULL;
        uint8_t* px        = gbm_bo_map(bo, 0, 0, 1, 1, GBM_BO_TRANSFER_READ, &mapStride, &handle);
        result             = (px && px != MAP_FAILED && firstPixelBlack(px)) ? "ready-black" : "ready-nonblack";
        if (px && px != MAP_FAILED)
            gbm_bo_unmap(bo, handle);
        (void)mapData;
    }

    wl_buffer_destroy(buffer);
    zwp_linux_buffer_params_v1_destroy(params);
    close(fd);
    gbm_bo_destroy(bo);
    return result;
}

static int usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s (--handle <n> | --app-id <id>) [--count <n>] [--interval-ms <ms>]\n"
            "          [--hold | --copy | --copy-dmabuf] [--xdph] [--drm-node <path>]\n",
            argv0);
    return 2;
}

int main(int argc, char** argv) {
    long        handle     = -1;
    const char* appID      = NULL;
    long        count      = 0;
    long        intervalMs = 100;
    bool        hold       = false;
    bool        copy       = false;
    bool        copyDmabuf = false;
    bool        xdph       = false;
    const char* drmNode    = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--handle") == 0 && i + 1 < argc)
            handle = strtol(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--app-id") == 0 && i + 1 < argc)
            appID = argv[++i];
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
            count = strtol(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc)
            intervalMs = strtol(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--hold") == 0)
            hold = true;
        else if (strcmp(argv[i], "--copy") == 0)
            copy = true;
        else if (strcmp(argv[i], "--copy-dmabuf") == 0)
            copyDmabuf = true;
        else if (strcmp(argv[i], "--xdph") == 0)
            xdph = true;
        else if (strcmp(argv[i], "--drm-node") == 0 && i + 1 < argc)
            drmNode = argv[++i];
        else
            return usage(argv[0]);
    }

    if ((handle < 0) == (appID == NULL))
        return usage(argv[0]);

    int gbmFd = -1;
    if (copyDmabuf) {
        const char* node = drmNode ? drmNode : "/dev/dri/renderD128";
        gbmFd            = open(node, O_RDWR | O_CLOEXEC);
        if (gbmFd < 0 || !(g_gbm = gbm_create_device(gbmFd))) {
            fprintf(stderr, "cannot open gbm device %s\n", node);
            return 1;
        }
    }

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect to the Wayland display\n");
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &REGISTRY_LISTENER, NULL);
    wl_display_roundtrip(display);

    if (!g_exportManager) {
        fprintf(stderr, "compositor has no hyprland_toplevel_export_manager_v1\n");
        return 1;
    }

    struct zwlr_foreign_toplevel_handle_v1* wlrHandle = NULL;
    if (appID) {
        if (!g_toplevelManager || g_exportVersion < 2) {
            fprintf(stderr, "need zwlr_foreign_toplevel_manager_v1 and toplevel export v2 for --app-id\n");
            return 1;
        }

        zwlr_foreign_toplevel_manager_v1_add_listener(g_toplevelManager, &TOPLEVEL_MANAGER_LISTENER, NULL);
        wl_display_roundtrip(display);
        wl_display_roundtrip(display);

        for (size_t i = 0; i < g_toplevelCount; ++i) {
            if (g_toplevels[i].appID && strcmp(g_toplevels[i].appID, appID) == 0) {
                wlrHandle = g_toplevels[i].handle;
                break;
            }
        }

        if (!wlrHandle) {
            fprintf(stderr, "no toplevel with app_id %s\n", appID);
            return 1;
        }
    }

    for (long n = 1; count == 0 || n <= count; ++n) {
        g_frameDone = false;
        g_failed    = false;
        g_width = g_height = 0;

        struct hyprland_toplevel_export_frame_v1* frame = wlrHandle ? hyprland_toplevel_export_manager_v1_capture_toplevel_with_wlr_toplevel_handle(g_exportManager, 0, wlrHandle) :
                                                                      hyprland_toplevel_export_manager_v1_capture_toplevel(g_exportManager, 0, (uint32_t)handle);
        hyprland_toplevel_export_frame_v1_add_listener(frame, &FRAME_LISTENER, NULL);
        wl_display_flush(display);

        // Wait up to 1s for buffer_done/failed. A compositor that rejects the request outright (dead
        // window) sends nothing at all, which is what XDPH would see too.
        waitFor(display, &g_frameDone, xdph ? 1000 * 1000 * 1000 : 1000);

        if (!g_frameDone)
            printf("frame %ld none\n", n);
        else if (g_failed)
            printf("frame %ld failed\n", n);
        else if (copyDmabuf)
            printf("frame %ld %ux%u %s\n", n, g_width, g_height, copyFrameDmabuf(display, frame));
        else if (copy && g_shm)
            printf("frame %ld %ux%u %s\n", n, g_width, g_height, copyFrameShm(display, frame));
        else
            printf("frame %ld %ux%u\n", n, g_width, g_height);
        fflush(stdout);

        if (hold) {
            // Keep this frame object alive (as if waiting for damage) and just stay connected.
            for (;;) {
                if (wl_display_dispatch(display) < 0)
                    return 0;
            }
        }

        // Destroying the frame: if the server answered (buffer_done or failed) the resource exists and
        // a destroy REQUEST is correct — this is exactly the sequence (destroy after a null-window
        // request) that crashed the portal before the plugin always created the resource. Only when
        // nothing came back ("none", no resource) must we destroy the proxy locally.
        if (g_frameDone)
            hyprland_toplevel_export_frame_v1_destroy(frame);
        else
            wl_proxy_destroy((struct wl_proxy*)frame);
        wl_display_flush(display);

        // Catch a protocol kill (e.g. invalid object) as soon as it happens.
        if (wl_display_get_error(display) != 0) {
            printf("display-error %d\n", wl_display_get_error(display));
            fflush(stdout);
            return 3;
        }
        sleepMs(intervalMs);
    }

    // Final roundtrip so any late protocol error from our last destroys surfaces before we report OK.
    wl_display_roundtrip(display);
    const int err = wl_display_get_error(display);
    printf("%s\n", err == 0 ? "display-ok" : "display-error");
    fflush(stdout);

    wl_display_disconnect(display);
    if (g_gbm)
        gbm_device_destroy(g_gbm);
    if (gbmFd >= 0)
        close(gbmFd);
    return err == 0 ? 0 : 3;
}
