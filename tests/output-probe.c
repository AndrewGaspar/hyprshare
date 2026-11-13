// output-probe: a minimal monitor/region capture client, like wf-recorder (wlr-screencopy) or a
// portal on ext-image-copy-capture. Captures the first wl_output into shm, one frame per interval:
//
//   frame <n> <width>x<height> ready      (copied)
//   frame <n> failed                      (the compositor answered `failed`)
//   frame <n> timeout                     (no answer within 2 s)
//   session stopped                       (--ext: the capture session was stopped by the compositor)
//
// Options:
//   --ext                  ext-image-copy-capture-v1 + ext-output-image-capture-source-v1 instead
//                          of wlr-screencopy
//   --region X Y W H       wlr-screencopy capture_output_region (a SHARE_REGION session)
//   --count <n>            frames (0 = forever, default)
//   --interval-ms <ms>     default 100
//   --exit-on-failed       exit 4 at the first failed frame / stopped session (wf-recorder, grim);
//                          default is to keep re-requesting (a portal's retry), re-creating the ext
//                          session after `stopped`

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

#include <wayland-client.h>

#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"
#include "wlr-screencopy-unstable-v1-client.h"

static struct wl_output*                                  g_output        = NULL;
static struct wl_shm*                                     g_shm           = NULL;
static struct zwlr_screencopy_manager_v1*                 g_screencopy    = NULL;
static struct ext_output_image_capture_source_manager_v1* g_sourceManager = NULL;
static struct ext_image_copy_capture_manager_v1*          g_copyManager   = NULL;

// Per-frame / per-session state.
static uint32_t g_width = 0, g_height = 0, g_stride = 0, g_format = 0;
static bool     g_haveFormat = false;
static bool     g_bufferDone = false, g_ready = false, g_failed = false;
static bool     g_sessionDone = false, g_sessionStopped = false;

static void     onRegistryGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (strcmp(interface, wl_output_interface.name) == 0 && !g_output)
        g_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        g_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0)
        g_screencopy = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, version < 3 ? version : 3);
    else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
        g_sourceManager = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
        g_copyManager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
}

static void onRegistryGlobalRemove(void* data, struct wl_registry* registry, uint32_t name) {
    ;
}

static const struct wl_registry_listener REGISTRY_LISTENER = {
    .global        = onRegistryGlobal,
    .global_remove = onRegistryGlobalRemove,
};

// ---- wlr-screencopy -------------------------------------------------------------------------------

static void onWlrBuffer(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    g_format     = format;
    g_width      = width;
    g_height     = height;
    g_stride     = stride;
    g_haveFormat = true;
}

static void onWlrFlags(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t flags) {
    ;
}

static void onWlrReady(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t a, uint32_t b, uint32_t c) {
    g_ready = true;
}

static void onWlrFailed(void* data, struct zwlr_screencopy_frame_v1* f) {
    g_failed = true;
}

static void onWlrDamage(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    ;
}

static void onWlrDmabuf(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t format, uint32_t width, uint32_t height) {
    ;
}

static void onWlrBufferDone(void* data, struct zwlr_screencopy_frame_v1* f) {
    g_bufferDone = true;
}

static const struct zwlr_screencopy_frame_v1_listener WLR_FRAME_LISTENER = {
    .buffer       = onWlrBuffer,
    .flags        = onWlrFlags,
    .ready        = onWlrReady,
    .failed       = onWlrFailed,
    .damage       = onWlrDamage,
    .linux_dmabuf = onWlrDmabuf,
    .buffer_done  = onWlrBufferDone,
};

// ---- ext-image-copy-capture -------------------------------------------------------------------------

static void onExtBufferSize(void* data, struct ext_image_copy_capture_session_v1* s, uint32_t width, uint32_t height) {
    g_width  = width;
    g_height = height;
}

static void onExtShmFormat(void* data, struct ext_image_copy_capture_session_v1* s, uint32_t format) {
    // Prefer the 32bpp formats we know the stride of; the compositor lists its alpha format first.
    if (!g_haveFormat &&
        (format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888 ||
         format == WL_SHM_FORMAT_XBGR2101010 || format == WL_SHM_FORMAT_ABGR2101010)) {
        g_format     = format;
        g_haveFormat = true;
    }
}

static void onExtDmabufDevice(void* data, struct ext_image_copy_capture_session_v1* s, struct wl_array* device) {
    ;
}

static void onExtDmabufFormat(void* data, struct ext_image_copy_capture_session_v1* s, uint32_t format, struct wl_array* modifiers) {
    ;
}

static void onExtDone(void* data, struct ext_image_copy_capture_session_v1* s) {
    g_sessionDone = true;
}

static void onExtStopped(void* data, struct ext_image_copy_capture_session_v1* s) {
    g_sessionStopped = true;
}

static const struct ext_image_copy_capture_session_v1_listener EXT_SESSION_LISTENER = {
    .buffer_size   = onExtBufferSize,
    .shm_format    = onExtShmFormat,
    .dmabuf_device = onExtDmabufDevice,
    .dmabuf_format = onExtDmabufFormat,
    .done          = onExtDone,
    .stopped       = onExtStopped,
};

static void onExtTransform(void* data, struct ext_image_copy_capture_frame_v1* f, uint32_t transform) {
    ;
}

static void onExtDamage(void* data, struct ext_image_copy_capture_frame_v1* f, int32_t x, int32_t y, int32_t w, int32_t h) {
    ;
}

static void onExtPresentation(void* data, struct ext_image_copy_capture_frame_v1* f, uint32_t a, uint32_t b, uint32_t c) {
    ;
}

static void onExtReady(void* data, struct ext_image_copy_capture_frame_v1* f) {
    g_ready = true;
}

static void onExtFailed(void* data, struct ext_image_copy_capture_frame_v1* f, uint32_t reason) {
    g_failed = true;
}

static const struct ext_image_copy_capture_frame_v1_listener EXT_FRAME_LISTENER = {
    .transform         = onExtTransform,
    .damage            = onExtDamage,
    .presentation_time = onExtPresentation,
    .ready             = onExtReady,
    .failed            = onExtFailed,
};

// ---- helpers ------------------------------------------------------------------------------------------

static void sleepMs(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// Pump events for up to `ms`, until any of the flags is set.
static void waitFor(struct wl_display* display, const bool* a, const bool* b, const bool* c, int ms) {
    for (int waited = 0; !*a && !(b && *b) && !(c && *c) && waited < ms; waited += 10) {
        struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);
        if (poll(&pfd, 1, 10) > 0)
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);
        if (wl_display_get_error(display) != 0)
            return;
    }
}

struct SShm {
    struct wl_buffer* buffer;
    void*             map;
    size_t            size;
};

static bool makeShm(struct SShm* out, uint32_t width, uint32_t height, uint32_t stride, uint32_t format) {
    out->size    = (size_t)stride * height;
    const int fd = memfd_create("output-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)out->size) != 0)
        return false;
    out->map                 = mmap(NULL, out->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_shm_pool* pool = wl_shm_create_pool(g_shm, fd, (int32_t)out->size);
    out->buffer              = wl_shm_pool_create_buffer(pool, 0, (int32_t)width, (int32_t)height, (int32_t)stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    return true;
}

static void freeShm(struct SShm* shm) {
    if (shm->buffer)
        wl_buffer_destroy(shm->buffer);
    if (shm->map && shm->map != MAP_FAILED)
        munmap(shm->map, shm->size);
    memset(shm, 0, sizeof(*shm));
}

static int usage(const char* argv0) {
    fprintf(stderr, "usage: %s [--ext] [--region X Y W H] [--count <n>] [--interval-ms <ms>] [--exit-on-failed]\n", argv0);
    return 2;
}

static struct ext_image_copy_capture_session_v1* newExtSession(struct wl_display* display) {
    g_haveFormat = g_sessionDone = g_sessionStopped = false;
    g_width = g_height = 0;

    struct ext_image_capture_source_v1*       source  = ext_output_image_capture_source_manager_v1_create_source(g_sourceManager, g_output);
    struct ext_image_copy_capture_session_v1* session = ext_image_copy_capture_manager_v1_create_session(g_copyManager, source, 0);
    ext_image_capture_source_v1_destroy(source);
    ext_image_copy_capture_session_v1_add_listener(session, &EXT_SESSION_LISTENER, NULL);
    wl_display_flush(display);
    waitFor(display, &g_sessionDone, &g_sessionStopped, NULL, 2000);
    return session;
}

int main(int argc, char** argv) {
    bool    ext = false, exitOnFailed = false, region = false;
    int32_t rx = 0, ry = 0, rw = 0, rh = 0;
    long    count = 0, intervalMs = 100;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ext") == 0)
            ext = true;
        else if (strcmp(argv[i], "--exit-on-failed") == 0)
            exitOnFailed = true;
        else if (strcmp(argv[i], "--region") == 0 && i + 4 < argc) {
            region = true;
            rx     = (int32_t)strtol(argv[++i], NULL, 0);
            ry     = (int32_t)strtol(argv[++i], NULL, 0);
            rw     = (int32_t)strtol(argv[++i], NULL, 0);
            rh     = (int32_t)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
            count = strtol(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc)
            intervalMs = strtol(argv[++i], NULL, 0);
        else
            return usage(argv[0]);
    }

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect to the Wayland display\n");
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &REGISTRY_LISTENER, NULL);
    wl_display_roundtrip(display);

    if (!g_output || !g_shm || (ext ? (!g_sourceManager || !g_copyManager) : !g_screencopy)) {
        fprintf(stderr, "compositor lacks wl_output / wl_shm / the %s capture globals\n", ext ? "ext-image-copy-capture" : "wlr-screencopy");
        return 1;
    }

    struct ext_image_copy_capture_session_v1* session = ext ? newExtSession(display) : NULL;

    for (long n = 1; count == 0 || n <= count; ++n) {
        g_ready = g_failed = g_bufferDone = false;
        struct SShm shm                   = {0};

        if (!ext) {
            g_haveFormat                           = false;
            struct zwlr_screencopy_frame_v1* frame = region ? zwlr_screencopy_manager_v1_capture_output_region(g_screencopy, 0, g_output, rx, ry, rw, rh) :
                                                              zwlr_screencopy_manager_v1_capture_output(g_screencopy, 0, g_output);
            zwlr_screencopy_frame_v1_add_listener(frame, &WLR_FRAME_LISTENER, NULL);
            wl_display_flush(display);
            waitFor(display, &g_bufferDone, &g_failed, NULL, 2000);

            if (g_bufferDone && !g_failed && g_haveFormat && makeShm(&shm, g_width, g_height, g_stride, g_format)) {
                zwlr_screencopy_frame_v1_copy(frame, shm.buffer);
                wl_display_flush(display);
                waitFor(display, &g_ready, &g_failed, NULL, 2000);
            }
            zwlr_screencopy_frame_v1_destroy(frame);
        } else {
            if (g_sessionStopped) {
                printf("session stopped\n");
                fflush(stdout);
                ext_image_copy_capture_session_v1_destroy(session);
                if (exitOnFailed)
                    return 4;
                sleepMs(intervalMs);
                session = newExtSession(display);
                continue;
            }

            if (g_sessionDone && g_haveFormat && makeShm(&shm, g_width, g_height, g_width * 4, g_format)) {
                struct ext_image_copy_capture_frame_v1* frame = ext_image_copy_capture_session_v1_create_frame(session);
                ext_image_copy_capture_frame_v1_add_listener(frame, &EXT_FRAME_LISTENER, NULL);
                ext_image_copy_capture_frame_v1_attach_buffer(frame, shm.buffer);
                ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, (int32_t)g_width, (int32_t)g_height);
                ext_image_copy_capture_frame_v1_capture(frame);
                wl_display_flush(display);
                waitFor(display, &g_ready, &g_failed, &g_sessionStopped, 2000);
                ext_image_copy_capture_frame_v1_destroy(frame);
            } else
                waitFor(display, &g_sessionStopped, NULL, NULL, 200);
        }

        if (g_ready)
            printf("frame %ld %ux%u ready\n", n, g_width, g_height);
        else if (g_failed)
            printf("frame %ld failed\n", n);
        else if (!(ext && g_sessionStopped))
            printf("frame %ld timeout\n", n);
        fflush(stdout);
        freeShm(&shm);

        if (wl_display_get_error(display) != 0) {
            printf("display-error %d\n", wl_display_get_error(display));
            fflush(stdout);
            return 3;
        }
        if (g_failed && exitOnFailed)
            return 4;

        sleepMs(intervalMs);
    }

    wl_display_roundtrip(display);
    const int err = wl_display_get_error(display);
    printf("%s\n", err == 0 ? "display-ok" : "display-error");
    fflush(stdout);
    wl_display_disconnect(display);
    return err == 0 ? 0 : 3;
}
