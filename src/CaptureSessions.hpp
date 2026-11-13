#pragma once

// CCaptureSessions: the compositor-wide view of screen capture, for a status bar.
//
// Every capture the compositor serves — wlr-screencopy (wf-recorder, grim, XDPH monitor/region
// shares), ext-image-copy-capture and hyprland-toplevel-export (XDPH window shares, including the
// ones hyprshare redirects) — is a Screenshare::CScreenshareSession. Its private `m_sharing` flag is
// exactly what the compositor's own `screencast` IPC events report (set on a copied frame, cleared
// ~500 ms after the last one, or on stop), so `capturing()` agrees with those events.
//
// `stopAll()` is the "end all capturing now" button: it answers every pending frame `failed`, stops
// every live session, makes the redirector forget its state, and for `plugin:hyprshare:stop_grace_ms`
// refuses new frames from the clients it just stopped, so a client's immediate retry cannot restart
// the capture. Refusal happens in two hooks: the captureToplevel hook (toplevel export, before any
// session exists) and a hook on CScreenshareFrame::share (every protocol's copy request), which
// answers ERROR_STOPPED (each protocol sends `failed`) and stops the session it belongs to.
//
// Everything here runs on the compositor main thread.

#include <src/helpers/memory/Memory.hpp>
#include <src/helpers/signal/Signal.hpp>
#include <src/helpers/time/Time.hpp>

#include <wayland-server-core.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class CEventLoopTimer;

namespace Screenshare {
    class CScreenshareFrame;
    class CScreenshareSession;
}

namespace HyprShare {
    // Registers plugin:hyprshare:stop_grace_ms. Must run inside pluginInit.
    void                      registerStopConfig(void* handle);
    std::chrono::milliseconds stopGrace();

    class CCaptureSessions {
      public:
        CCaptureSessions();
        ~CCaptureSessions();

        CCaptureSessions(const CCaptureSessions&)            = delete;
        CCaptureSessions& operator=(const CCaptureSessions&) = delete;

        // Any live compositor capture session currently sharing (== the `screencast` events' view).
        bool        capturing() const;
        std::string sessionsJSON() const;  // JSON array, one object per live (non-stopped) session
        std::string stopGraceJSON() const; // JSON object describing the refusal window
        std::string fingerprint() const;   // what a bar cares about; changes => `hyprshare>>changed`

        // Stop everything; returns the number of sessions stopped.
        size_t stopAll();

        // Inside the post-stop grace window for this client?
        bool refused(wl_client* client) const;

        // From the CScreenshareFrame::share hook. True => refuse this frame (answer ERROR_STOPPED).
        bool onFrameShare(Screenshare::CScreenshareFrame* frame);

        // Start watching sessions we have not seen yet (their `stopped` signal posts `changed`).
        void scan();

      private:
        struct SSessionWatch {
            const Screenshare::CScreenshareSession* session = nullptr; // identity only
            WP<Screenshare::CScreenshareSession>    ref;
            CHyprSignalListener                     stopped;
            // The session's client disconnected. The compositor keeps such a (screencopy / toplevel
            // export) session around with a dangling client pointer, which a NEW client may then
            // reuse: once orphaned, the session is never attributed to whoever owns that address now.
            bool orphaned = false;
        };

        // One destroy listener per client that owns a watched session (orphans its sessions).
        struct SClientWatch {
            wl_listener       listener = {};
            wl_client*        client   = nullptr; // identity only
            CCaptureSessions* owner    = nullptr;
        };

        // A client refused until `until`; dropped early if the client disconnects, so a new client that
        // reuses the wl_client address is not refused.
        struct SRefusal {
            wl_listener       listener = {};
            wl_client*        client   = nullptr; // identity only
            pid_t             pid      = -1;
            Time::steady_tp   until;
            CCaptureSessions* owner = nullptr;
        };

        std::vector<SSessionWatch>    m_watches;
        std::vector<UP<SClientWatch>> m_clientWatches;
        std::vector<UP<SRefusal>>     m_refusals;
        CHyprSignalListener           m_shareStateListener, m_windowCloseListener, m_windowTitleListener;
        SP<CEventLoopTimer>           m_graceTimer;

        bool                          watching(const Screenshare::CScreenshareSession* session) const;
        // The session's client if it is still the one that created the session, else null.
        wl_client*  sessionClient(const Screenshare::CScreenshareSession* session) const;
        void        watchClient(wl_client* client);
        void        onClientDestroyed(SClientWatch* watch);
        static void onClientDestroyedThunk(wl_listener* listener, void* data);
        void        refuse(wl_client* client, Time::steady_tp until);
        void        pruneRefusals(Time::steady_tp now);
        void        onRefusedClientDestroyed(SRefusal* refusal);
        static void onRefusedClientDestroyedThunk(wl_listener* listener, void* data);
    };

    // Null when the plugin is not initialised (or already unloading).
    UP<CCaptureSessions>& captureSessions();

    // True if `client` is a currently connected wl_client of the compositor's display.
    bool clientConnected(wl_client* client);
}
