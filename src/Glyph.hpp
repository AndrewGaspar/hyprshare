#pragma once

// The share glyph: a small clickable dot drawn in a corner of every eligible window while a window
// share is active.
//
//   LIVE   — the window currently served to the share (filled red by default)
//   IDLE   — any other window: click to share it            (hollow ring)
//   PAUSED — the window whose share the user paused: click to resume (filled amber, pause bars)
//
// Rendering: an ordinary window decoration (IHyprWindowDecoration) that adds rect/border pass
// elements in the normal render pass, like the border and group bar. Window capture renders with
// decorate=false (CScreenshareFrame::renderWindow), so the glyph never reaches the shared stream.
//
// Input: NOT via DECORATION_ALLOWS_MOUSE_INPUT. CWindow::checkInputOnDecos only offers a click to a
// decoration whose positioner box contains the cursor, and the positioner gives ABSOLUTE decorations
// an empty box (STICKY ones get a box, but outside the window and with reserved layout space). So
// the glyph listens on the cancellable Event::bus() input.mouse.button signal instead, hit-tests its
// own box, and cancels the event — before keybinds, focus changes or the client ever see it.
//
// All of this runs on the compositor main thread (render pass, input, timers, config reload).

#include <src/desktop/DesktopTypes.hpp>
#include <src/devices/IPointer.hpp>
#include <src/event/EventBus.hpp>
#include <src/helpers/Color.hpp>
#include <src/helpers/memory/Memory.hpp>
#include <src/render/decorations/IHyprWindowDecoration.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace HyprShare {
    enum eGlyphState : uint8_t {
        GLYPH_NONE = 0, // no share active, or window not eligible: nothing drawn, no input
        GLYPH_IDLE,
        GLYPH_LIVE,
        GLYPH_PAUSED,
    };

    enum eGlyphCorner : uint8_t {
        CORNER_TOP_RIGHT = 0,
        CORNER_TOP_LEFT,
        CORNER_BOTTOM_RIGHT,
        CORNER_BOTTOM_LEFT,
    };

    // Parsed once per config (re)load; the render pass and input path read only this.
    struct SGlyphConfig {
        eGlyphCorner corner         = CORNER_TOP_RIGHT;
        Vector2D     offset         = {10, 10}; // logical px, inset from the window edge
        float        size           = 14.F;     // logical px diameter
        float        outline        = 2.F;      // logical px
        float        opacity        = 1.F;
        bool         sharedOnly     = false; // only LIVE/PAUSED glyphs
        bool         hideFullscreen = true;
        CHyprColor   liveFill, liveOutline, idleFill, idleOutline, pausedFill, pausedOutline;
    };

    const char* glyphStateName(eGlyphState state);
    const char* glyphCornerName(eGlyphCorner corner);

    // Registers plugin:hyprshare:* config values. Must run inside pluginInit.
    void         registerGlyphConfig(void* handle);
    SGlyphConfig readGlyphConfig();

    class CGlyphDecoration : public IHyprWindowDecoration {
      public:
        CGlyphDecoration(PHLWINDOW window);
        virtual ~CGlyphDecoration();

        virtual SDecorationPositioningInfo getPositioningInfo();
        virtual void                       onPositioningReply(const SDecorationPositioningReply& reply);
        virtual void                       draw(PHLMONITOR, float const& a);
        virtual eDecorationType            getDecorationType();
        virtual void                       updateWindow(PHLWINDOW);
        virtual void                       damageEntire();
        virtual eDecorationLayer           getDecorationLayer();
        virtual uint64_t                   getDecorationFlags();
        virtual std::string                getDisplayName();

        // State as it would be drawn right now; GLYPH_NONE if nothing is drawn.
        eGlyphState state() const;
        // The glyph's box in global logical coordinates (includes workspace render offset).
        CBox      boxGlobal() const;
        PHLWINDOW window() const;

      private:
        PHLWINDOWREF m_window;
    };

    class CGlyphManager {
      public:
        CGlyphManager(void* handle);
        ~CGlyphManager();

        CGlyphManager(const CGlyphManager&)                 = delete;
        CGlyphManager&      operator=(const CGlyphManager&) = delete;

        const SGlyphConfig& config() const;
        void                damageAll();
        void                attachAll();
        void                onDecorationDestroyed(CGlyphDecoration* deco);
        std::string         statusJSON() const;

      private:
        struct SPress {
            uint32_t     button = 0;
            PHLWINDOWREF window;
            Vector2D     pos;
            bool         onGlyph = false;
        };

        void*                          m_handle = nullptr; // plugin HANDLE
        SGlyphConfig                   m_config;
        std::vector<CGlyphDecoration*> m_decos; // owned by their windows; unregistered by their dtor
        std::vector<SPress>            m_presses;

        CHyprSignalListener            m_openListener, m_reloadListener, m_buttonListener;

        void                           attach(PHLWINDOW window);
        CGlyphDecoration*              decoFor(const PHLWINDOW& window) const;
        void                           onButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
        void                           onClick(const PHLWINDOW& window, eGlyphState state);
    };

    // Null when the plugin is not initialised (or already unloading).
    UP<CGlyphManager>& glyphs();
}
