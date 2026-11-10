#pragma once

// CShareRedirector: the whole policy of the hyprshare spike.
//
// xdg-desktop-portal-hyprland re-requests the toplevel-export frame for EVERY captured frame, and the
// compositor answers each request from the managed session for (client, window). So whatever window we
// hand to CToplevelExportClient::captureToplevel is what the next frame shows; serving window B for a
// share that was picked as window A just looks like A resized (and changed content) to the portal.
//
// This class tracks those requests ("captures"), keeps the redirect table, and maintains the
// `screenshared` tag on whichever window is actually being served. It lives on the main thread only:
// toplevel-export requests are dispatched from the Wayland event loop, and so are dispatchers, hyprctl
// commands, Lua calls and event-loop timers. None of this is reachable from the XR frame thread.

#include <src/SharedDefs.hpp>
#include <src/desktop/DesktopTypes.hpp>
#include <src/helpers/memory/Memory.hpp>
#include <src/helpers/time/Time.hpp>

#include <wayland-server-core.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class CEventLoopTimer;

namespace HyprShare {
    // The tag maintained on the window(s) currently served to an active window capture.
    inline constexpr const char* SHARED_TAG = "screenshared";

    class CShareRedirector {
      public:
        CShareRedirector();
        ~CShareRedirector();

        CShareRedirector(const CShareRedirector&)            = delete;
        CShareRedirector& operator=(const CShareRedirector&) = delete;

        struct SServeDecision {
            PHLWINDOW window;        // what the compositor should actually capture (may be null)
            uint64_t  captureID = 0; // 0 = not tracked (nothing to watch)
        };

        // Called from the captureToplevel hook BEFORE the original runs.
        SServeDecision onCaptureRequest(wl_client* client, PHLWINDOW requested);
        // Called from the hook AFTER the original created (or failed to create) frame object `frameID`.
        void            watchFrame(wl_client* client, uint32_t frameID, uint64_t captureID);

        SDispatchResult shareWindow(const std::string& selector);
        SDispatchResult clear();
        std::string     statusJSON() const;

      private:
        // One (client, picked window) pair. Electron apps open 2-3 portal sessions per share, but XDPH
        // multiplexes them all over its single toplevel-export manager, so they collapse into one
        // capture here — and they all follow a redirect together, which is exactly what we want.
        struct SCapture {
            uint64_t        id     = 0;
            wl_client*      client = nullptr; // identity ONLY — never dereferenced after the request returns
            pid_t           pid    = -1;
            PHLWINDOWREF    origin;         // the window the portal asked for
            uintptr_t       originAddr = 0; // identity of `origin`, still usable after it is destroyed
            PHLWINDOWREF    served;         // the window we last served for it
            Time::steady_tp lastSeen;
            uint32_t        outstandingFrames = 0; // frame objects alive (e.g. waiting for damage)
        };

        struct SRedirect {
            PHLWINDOWREF origin;
            uintptr_t    originAddr = 0;

            bool         matches(const SCapture& capture) const;
        };

        // A destroy listener on one frame wl_resource. Owned here, unlinked on destruction.
        struct SFrameWatch {
            wl_listener       listener  = {};
            uint64_t          captureID = 0;
            CShareRedirector* owner     = nullptr;
        };

        std::vector<SCapture>        m_captures;
        std::vector<SRedirect>       m_redirects;
        PHLWINDOWREF                 m_target;
        std::vector<PHLWINDOWREF>    m_tagged;
        std::vector<UP<SFrameWatch>> m_frameWatches;
        uint64_t                     m_nextCaptureID = 1;

        SP<CEventLoopTimer>          m_timer;

        SCapture*                    captureByID(uint64_t id);
        SCapture*                    findCapture(wl_client* client, const PHLWINDOW& requested);
        bool                         isActive(const SCapture& capture, Time::steady_tp now) const;
        bool                         isRedirected(const SCapture& capture) const;
        PHLWINDOW                    resolve(const SCapture& capture) const;
        PHLWINDOW                    validTarget() const;

        void                         onFrameDestroyed(SFrameWatch* watch);
        static void                  onFrameDestroyedThunk(wl_listener* listener, void* data);

        void                         armTimer();
        void                         onTick();
        void                         prune(Time::steady_tp now);
        void                         updateTags();
    };

    UP<CShareRedirector>& redirector();
}
