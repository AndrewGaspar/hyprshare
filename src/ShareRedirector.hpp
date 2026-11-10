#pragma once

// CShareRedirector: the policy of the hyprshare spike.
//
// xdg-desktop-portal-hyprland re-requests the toplevel-export frame for EVERY captured frame, and the
// compositor answers each request from the managed session for (client, window). So whatever window we
// hand to CToplevelExportClient::captureToplevel is what the next frame shows; serving window B for a
// share that was picked as window A just looks like A resized (and changed content) to the portal.
//
// Two hard invariants, both learned from live crashes:
//   1. EVERY capture request from a client must get a real frame resource for its new_id and must be
//      answered (buffer+copy→ready, or failed). 0.56's captureToplevel returns early for a null window
//      WITHOUT creating the resource; libwayland then reserves that id as a NULL object, and the next
//      destroy request on it posts wl_display error 0 (invalid object) and kills the portal. So we
//      never let a request reach that early return with null.
//   2. Closing the shared window must NOT silently reveal another window. When the served window dies
//      the capture enters NOTHING: it is served a black PLACEHOLDER (at the last advertised size, so no
//      renegotiation) until the user explicitly shares something again. NOTHING is sticky and keyed on
//      the capture, never on a window address (addresses get recycled).
//
// Everything runs on the compositor main thread (Wayland requests, dispatchers, hyprctl, Lua,
// event-loop timers). Nothing is reachable from the XR frame thread.

#include <src/SharedDefs.hpp>
#include <src/desktop/DesktopTypes.hpp>
#include <src/helpers/memory/Memory.hpp>
#include <src/helpers/time/Time.hpp>
#include <src/render/Framebuffer.hpp>

#include <hyprutils/math/Vector2D.hpp>

#include <wayland-server-core.h>

#include "Glyph.hpp"

#include <cstdint>
#include <string>
#include <vector>

class CEventLoopTimer;
class CHyprlandToplevelExportFrameV1;

namespace HyprShare {
    // The tag maintained on the window currently served to an active window capture.
    inline constexpr const char* SHARED_TAG = "screenshared";

    class CShareRedirector {
      public:
        CShareRedirector();
        ~CShareRedirector();

        CShareRedirector(const CShareRedirector&)            = delete;
        CShareRedirector& operator=(const CShareRedirector&) = delete;

        enum eAction : uint8_t {
            ACT_SERVE,       // let the original run with `window`
            ACT_PLACEHOLDER, // create the frame ourselves and serve a black placeholder
            ACT_FAIL,        // create the frame ourselves and send `failed` (untracked / unknown null)
        };

        struct SServeDecision {
            eAction   action = ACT_SERVE;
            PHLWINDOW window;        // valid iff action == ACT_SERVE
            uint64_t  captureID = 0; // 0 = untracked
        };

        // Called from the captureToplevel hook BEFORE the original runs.
        SServeDecision onCaptureRequest(wl_client* client, PHLWINDOW requested);
        // Called from the hook AFTER the original created frame object `frameID` for a real window.
        void watchFrame(wl_client* client, uint32_t frameID, uint64_t captureID);
        // Called from the hook INSTEAD of the original, for ACT_PLACEHOLDER / ACT_FAIL. Creates the
        // frame resource ourselves (so the id is never a NULL object) and either serves a placeholder
        // or sends `failed`.
        void            placeholderFrame(wl_client* client, uint32_t version, uint32_t frameID, uint64_t captureID);
        void            failFrame(wl_client* client, uint32_t version, uint32_t frameID);

        SDispatchResult shareWindow(const std::string& selector);
        SDispatchResult shareWindowTarget(const PHLWINDOW& target); // also resumes a pause
        SDispatchResult clear();
        // Every active capture goes to the sticky NOTHING placeholder state (same as a closed window);
        // the window it was serving is remembered as the paused window.
        SDispatchResult pause();
        std::string     statusJSON() const;

        // For the glyphs: is any share active at all, and what should `window`'s glyph show?
        bool        shareModeActive() const;
        eGlyphState glyphStateFor(const PHLWINDOW& window) const;
        void        onWindowOpened(const PHLWINDOW& window);

      private:
        // One (client, picked window) pair. Electron apps open 2-3 portal sessions per share, but XDPH
        // multiplexes them all over its single toplevel-export manager, so they collapse into one
        // capture here — and they all follow a redirect, or go to placeholder, together.
        struct SCapture {
            uint64_t     id     = 0;
            wl_client*   client = nullptr; // identity ONLY — never dereferenced after the request returns
            pid_t        pid    = -1;
            PHLWINDOWREF origin;         // the window the portal asked for
            uintptr_t    originAddr = 0; // for status display only, never for matching
            PHLWINDOWREF served;         // the real window we last served (null in NOTHING)

            enum eMode : uint8_t {
                FOLLOW_ORIGIN, // serve the picked window
                FOLLOW_TARGET, // serve the global redirect target
            } mode = FOLLOW_ORIGIN;

            bool            nothing      = false; // sticky: served window died; serve placeholder until an explicit share
            bool            pausedByUser = false; // the NOTHING came from pause(), not a closed window

            Time::steady_tp lastSeen;
            uint32_t        outstandingFrames = 0; // frame objects alive (real or placeholder)

            // Constraints of the last REAL frame, reused for the placeholder so nothing renegotiates.
            bool          haveConstraints = false;
            Vector2D      size;
            uint32_t      format = 0; // DRM format (0 == DRM_FORMAT_INVALID)
            PHLMONITORREF monitor;
        };

        // A destroy listener on one real frame wl_resource (counts outstandingFrames).
        struct SFrameWatch {
            wl_listener       listener  = {};
            uint64_t          captureID = 0;
            CShareRedirector* owner     = nullptr;
        };

        // A destroy listener on a capturing wl_client: its captures die with it. Without this a new
        // client that happens to get the same wl_client address (e.g. a restarted portal) would
        // inherit the old client's redirect/pause state.
        struct SClientWatch {
            wl_listener       listener = {};
            wl_client*        client   = nullptr; // identity only
            CShareRedirector* owner    = nullptr;
        };

        // A frame object we created ourselves to serve a placeholder.
        struct SPlaceholderFrame {
            SP<CHyprlandToplevelExportFrameV1> resource;
            uint64_t                           captureID = 0;
            Vector2D                           size;
            uint32_t                           format = 0;
            PHLMONITORREF                      monitor;
            bool                               answered  = false; // ready/failed already sent for this frame
            bool                               copyAsked = false;
            SP<IHLBuffer>                      copyBuffer; // set while a throttled copy is pending
            Time::steady_tp                    timestamp;
            SP<CEventLoopTimer>                copyTimer;
            CShareRedirector*                  owner = nullptr;
        };

        std::vector<SCapture>              m_captures;
        PHLWINDOWREF                       m_target;
        PHLWINDOWREF                       m_paused; // the window whose share the user paused
        std::vector<PHLWINDOWREF>          m_tagged;
        std::vector<PHLWINDOWREF>          m_modeTagged; // windows carrying `screenshare-mode`
        bool                               m_modeActive = false;
        std::string                        m_glyphFingerprint;
        std::vector<UP<SFrameWatch>>       m_frameWatches;
        std::vector<UP<SClientWatch>>      m_clientWatches;
        std::vector<UP<SPlaceholderFrame>> m_placeholders;
        uint64_t                           m_nextCaptureID = 1;

        SP<CEventLoopTimer>                m_timer;

        SCapture*                          captureByID(uint64_t id);
        SCapture*                          findCapture(wl_client* client, const PHLWINDOW& requested);
        bool                               isActive(const SCapture& capture, Time::steady_tp now) const;
        PHLWINDOW                          validTarget() const;
        PHLMONITOR                         placeholderMonitor(const SCapture& capture) const;
        // Resolve what a capture should be served, updating its mode/nothing state. Null => placeholder.
        PHLWINDOW   serveWindow(SCapture& capture);
        void        recordConstraints(SCapture& capture, const PHLWINDOW& window);
        size_t      placeholderCount(uint64_t captureID) const;

        void        onFrameDestroyed(SFrameWatch* watch);
        static void onFrameDestroyedThunk(wl_listener* listener, void* data);
        void        watchClient(wl_client* client);
        void        onClientDestroyed(SClientWatch* watch);
        static void onClientDestroyedThunk(wl_listener* listener, void* data);

        void        doPlaceholderCopy(SPlaceholderFrame* frame);
        bool        renderPlaceholder(const SP<IHLBuffer>& buffer, const Vector2D& size, PHLMONITOR monitor);
        void        finishPlaceholders(uint64_t captureID); // answer+drop this capture's pending frames
        void        dropPlaceholder(CHyprlandToplevelExportFrameV1* resource);

        void        armTimer();
        void        onTick();
        void        prune(Time::steady_tp now);
        void        updateTags();
        void        syncModeTags(bool force); // `screenshare-mode` on every window while a share is active
        void        refreshGlyphs();          // damage glyphs if their states changed
        void        onShareChanged();         // after share/clear: refresh served, release placeholders, tags
    };

    UP<CShareRedirector>& redirector();
}
