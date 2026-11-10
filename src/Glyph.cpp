#include "Glyph.hpp"
#include "ShareRedirector.hpp"

#include <src/Compositor.hpp>
#include <src/config/values/types/ColorValue.hpp>
#include <src/config/values/types/FloatValue.hpp>
#include <src/config/values/types/IntValue.hpp>
#include <src/config/values/types/StringValue.hpp>
#include <src/desktop/Workspace.hpp>
#include <src/desktop/state/ViewHitTester.hpp>
#include <src/desktop/state/ViewState.hpp>
#include <src/desktop/state/WindowState.hpp>
#include <src/desktop/view/Window.hpp>
#include <src/managers/SessionLockManager.hpp>
#include <src/managers/fullscreen/FullscreenController.hpp>
#include <src/managers/input/InputManager.hpp>
#include <src/output/Monitor.hpp>
#include <src/plugins/PluginAPI.hpp>
#include <src/render/Renderer.hpp>
#include <src/render/pass/BorderPassElement.hpp>
#include <src/render/pass/RectPassElement.hpp>

#include <algorithm>
#include <cmath>
#include <format>

using namespace HyprShare;

static constexpr uint32_t BTN_LEFT_CODE = 0x110; // linux/input-event-codes.h BTN_LEFT
// A press and release further apart than this is a drag, not a click: ignored.
static constexpr double CLICK_SLOP = 6.0;

// ---- config ------------------------------------------------------------------------------------
//
// Names must be string literals: the config manager keeps the pointers. The .so is linked
// -z nodelete, so they (and every vtable/lambda we hand the compositor) stay mapped after unload.

namespace {
    struct SGlyphValues {
        SP<Config::Values::CStringValue> corner, visibility;
        SP<Config::Values::CIntValue>    offsetX, offsetY, hideFullscreen;
        SP<Config::Values::CFloatValue>  size, outline, opacity;
        SP<Config::Values::CColorValue>  liveFill, liveOutline, idleFill, idleOutline, pausedFill, pausedOutline;
    };
}

static SGlyphValues& values() {
    static SGlyphValues v;
    return v;
}

template <typename T, typename... Args>
static SP<T> addValue(HANDLE handle, Args&&... args) {
    auto v = makeShared<T>(std::forward<Args>(args)...);
    if (!HyprlandAPI::addConfigValueV2(handle, v))
        Log::logger->log(Log::ERR, "[hyprshare] failed to register config value {}", v->name());
    return v;
}

void HyprShare::registerGlyphConfig(HANDLE handle) {
    using namespace Config::Values;
    auto& v = values();
    // clang-format off
    v.corner         = addValue<CStringValue>(handle, "plugin:hyprshare:corner", "glyph corner: top_right, top_left, bottom_right, bottom_left", std::string{"top_right"});
    v.visibility     = addValue<CStringValue>(handle, "plugin:hyprshare:visibility", "glyphs on: all (every window) or shared_only (only LIVE/PAUSED)", std::string{"all"});
    v.offsetX        = addValue<CIntValue>(handle, "plugin:hyprshare:offset_x", "glyph inset from the window's side edge, logical px", 10);
    v.offsetY        = addValue<CIntValue>(handle, "plugin:hyprshare:offset_y", "glyph inset from the window's top/bottom edge, logical px", 10);
    v.hideFullscreen = addValue<CIntValue>(handle, "plugin:hyprshare:hide_fullscreen", "hide glyphs on fullscreen windows (0/1)", 1);
    v.size           = addValue<CFloatValue>(handle, "plugin:hyprshare:size", "glyph diameter, logical px", 14.F);
    v.outline        = addValue<CFloatValue>(handle, "plugin:hyprshare:outline", "glyph outline width, logical px", 2.F);
    v.opacity        = addValue<CFloatValue>(handle, "plugin:hyprshare:opacity", "glyph opacity multiplier (0..1)", 1.F);
    v.liveFill       = addValue<CColorValue>(handle, "plugin:hyprshare:live_fill", "LIVE glyph fill", 0xFFFF3030);
    v.liveOutline    = addValue<CColorValue>(handle, "plugin:hyprshare:live_outline", "LIVE glyph outline", 0xE6FFFFFF);
    v.idleFill       = addValue<CColorValue>(handle, "plugin:hyprshare:idle_fill", "IDLE glyph fill", 0x40000000);
    v.idleOutline    = addValue<CColorValue>(handle, "plugin:hyprshare:idle_outline", "IDLE glyph outline", 0xD9FFFFFF);
    v.pausedFill     = addValue<CColorValue>(handle, "plugin:hyprshare:paused_fill", "PAUSED glyph fill", 0xFFFFB000);
    v.pausedOutline  = addValue<CColorValue>(handle, "plugin:hyprshare:paused_outline", "PAUSED glyph outline", 0xE6FFFFFF);
    // clang-format on
}

static eGlyphCorner parseCorner(const std::string& s) {
    if (s == "top_left")
        return CORNER_TOP_LEFT;
    if (s == "bottom_right")
        return CORNER_BOTTOM_RIGHT;
    if (s == "bottom_left")
        return CORNER_BOTTOM_LEFT;
    return CORNER_TOP_RIGHT;
}

SGlyphConfig HyprShare::readGlyphConfig() {
    SGlyphConfig c;
    const auto&  v = values();
    if (!v.corner)
        return c;

    c.corner         = parseCorner(v.corner->value());
    c.sharedOnly     = v.visibility->value() == "shared_only";
    c.offset         = {sc<double>(v.offsetX->value()), sc<double>(v.offsetY->value())};
    c.hideFullscreen = v.hideFullscreen->value() != 0;
    c.size           = std::clamp(v.size->value(), 4.F, 256.F);
    c.outline        = std::clamp(v.outline->value(), 0.F, c.size / 2.F);
    c.opacity        = std::clamp(v.opacity->value(), 0.F, 1.F);
    c.liveFill       = CHyprColor(sc<uint64_t>(v.liveFill->value()));
    c.liveOutline    = CHyprColor(sc<uint64_t>(v.liveOutline->value()));
    c.idleFill       = CHyprColor(sc<uint64_t>(v.idleFill->value()));
    c.idleOutline    = CHyprColor(sc<uint64_t>(v.idleOutline->value()));
    c.pausedFill     = CHyprColor(sc<uint64_t>(v.pausedFill->value()));
    c.pausedOutline  = CHyprColor(sc<uint64_t>(v.pausedOutline->value()));
    return c;
}

const char* HyprShare::glyphStateName(eGlyphState state) {
    switch (state) {
        case GLYPH_IDLE: return "idle";
        case GLYPH_LIVE: return "live";
        case GLYPH_PAUSED: return "paused";
        default: return "none";
    }
}

const char* HyprShare::glyphCornerName(eGlyphCorner corner) {
    switch (corner) {
        case CORNER_TOP_LEFT: return "top_left";
        case CORNER_BOTTOM_RIGHT: return "bottom_right";
        case CORNER_BOTTOM_LEFT: return "bottom_left";
        default: return "top_right";
    }
}

// ---- decoration --------------------------------------------------------------------------------

CGlyphDecoration::CGlyphDecoration(PHLWINDOW window) : IHyprWindowDecoration(window), m_window(window) {
    ;
}

CGlyphDecoration::~CGlyphDecoration() {
    // The window may destroy us long after unload (a hidden window only processes removals when it
    // is shown again); the manager is gone then and there is nothing to unregister from.
    if (glyphs())
        glyphs()->onDecorationDestroyed(this);
}

SDecorationPositioningInfo CGlyphDecoration::getPositioningInfo() {
    // Drawn inside the window: reserve nothing, change no geometry. Four edges = a valid anchor
    // (zero edges makes the positioner log an error on every lookup).
    SDecorationPositioningInfo info;
    info.policy   = DECORATION_POSITION_ABSOLUTE;
    info.edges    = DECORATION_EDGE_TOP | DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT;
    info.priority = 1;
    info.reserved = false;
    return info;
}

void CGlyphDecoration::onPositioningReply(const SDecorationPositioningReply& reply) {
    ;
}

PHLWINDOW CGlyphDecoration::window() const {
    return m_window.lock();
}

CBox CGlyphDecoration::boxGlobal() const {
    const auto WINDOW = m_window.lock();
    if (!WINDOW || !glyphs())
        return {};

    const auto& C  = glyphs()->config();
    CBox        wb = WINDOW->getWindowMainSurfaceBox();
    if (WINDOW->m_workspace && !WINDOW->m_pinned)
        wb.translate(WINDOW->m_workspace->m_renderOffset->value());
    wb.translate(WINDOW->m_floatingOffset);

    const double S     = C.size;
    const bool   RIGHT = C.corner == CORNER_TOP_RIGHT || C.corner == CORNER_BOTTOM_RIGHT;
    const bool   BOT   = C.corner == CORNER_BOTTOM_RIGHT || C.corner == CORNER_BOTTOM_LEFT;
    const double X     = RIGHT ? wb.x + wb.w - C.offset.x - S : wb.x + C.offset.x;
    const double Y     = BOT ? wb.y + wb.h - C.offset.y - S : wb.y + C.offset.y;
    return {X, Y, S, S};
}

eGlyphState CGlyphDecoration::state() const {
    const auto WINDOW = m_window.lock();
    if (!WINDOW || !glyphs() || !redirector())
        return GLYPH_NONE;

    // Fast path first: draw() runs every frame for every window, and with no share there is nothing.
    if (!redirector()->shareModeActive())
        return GLYPH_NONE;

    if (!WINDOW->m_isMapped || WINDOW->isHidden() || WINDOW->isX11OverrideRedirect())
        return GLYPH_NONE;

    const auto& C = glyphs()->config();
    if (C.hideFullscreen && Fullscreen::controller()->isFullscreen(WINDOW))
        return GLYPH_NONE;

    // Too small to hold the glyph inside it: skip rather than draw over the edge.
    const CBox WB = WINDOW->getWindowMainSurfaceBox();
    if (WB.w < C.size + C.offset.x * 2 || WB.h < C.size + C.offset.y * 2)
        return GLYPH_NONE;

    const auto STATE = redirector()->glyphStateFor(WINDOW);
    if (STATE == GLYPH_IDLE && C.sharedOnly)
        return GLYPH_NONE;
    return STATE;
}

void CGlyphDecoration::draw(PHLMONITOR monitor, float const& a) {
    const auto STATE = state();
    if (STATE == GLYPH_NONE || !monitor)
        return;

    const auto  WINDOW = m_window.lock();
    const auto& C      = glyphs()->config();

    CBox        box = boxGlobal();
    box.translate(-monitor->m_position + g_pHyprRenderer->depthRenderOffset(WINDOW)).scale(monitor->m_scale).round();
    if (box.w < 2 || box.h < 2)
        return;

    const float ALPHA = a * C.opacity;
    CHyprColor  fill, outline;
    switch (STATE) {
        case GLYPH_LIVE:
            fill    = C.liveFill;
            outline = C.liveOutline;
            break;
        case GLYPH_PAUSED:
            fill    = C.pausedFill;
            outline = C.pausedOutline;
            break;
        default:
            fill    = C.idleFill;
            outline = C.idleOutline;
            break;
    }
    fill.a *= ALPHA;

    const double BORDER = std::round(C.outline * monitor->m_scale);
    CBox         inner  = box.copy().expand(-BORDER);
    if (inner.w < 1 || inner.h < 1)
        inner = box;

    // fill disc
    g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
        .box           = inner,
        .color         = fill,
        .round         = sc<int>(inner.w / 2.0),
        .roundingPower = 2.F,
    }));

    // outline ring
    if (BORDER >= 1) {
        CBorderPassElement::SBorderData ring;
        ring.box           = inner;
        ring.grad1         = Config::CGradientValueData(outline);
        ring.a             = ALPHA;
        ring.round         = sc<int>(inner.w / 2.0);
        ring.outerRound    = sc<int>(box.w / 2.0);
        ring.borderSize    = sc<int>(BORDER);
        ring.roundingPower = 2.F;
        ring.window        = WINDOW;
        g_pHyprRenderer->addPassElement(makeUnique<CBorderPassElement>(ring));
    }

    // PAUSED: two bars, in the outline colour
    if (STATE == GLYPH_PAUSED) {
        CHyprColor   bar  = outline;
        const double BW   = std::max(1.0, std::round(inner.w * 0.18));
        const double BH   = std::max(2.0, std::round(inner.h * 0.5));
        const double GAP  = std::max(1.0, std::round(inner.w * 0.14));
        const double TOP  = inner.y + (inner.h - BH) / 2.0;
        const double LEFT = inner.x + (inner.w - (BW * 2 + GAP)) / 2.0;
        bar.a *= ALPHA;
        for (int i = 0; i < 2; ++i) {
            g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
                .box   = CBox{LEFT + i * (BW + GAP), TOP, BW, BH}.round(),
                .color = bar,
            }));
        }
    }
}

eDecorationType CGlyphDecoration::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CGlyphDecoration::updateWindow(PHLWINDOW) {
    ;
}

void CGlyphDecoration::damageEntire() {
    const auto BOX = boxGlobal();
    if (BOX.w <= 0 || BOX.h <= 0)
        return;
    g_pHyprRenderer->damageBox(BOX.copy().expand(2));
}

eDecorationLayer CGlyphDecoration::getDecorationLayer() {
    return DECORATION_LAYER_OVER; // above the window, below its popups/menus
}

uint64_t CGlyphDecoration::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

std::string CGlyphDecoration::getDisplayName() {
    return "hyprshare-glyph";
}

// ---- manager -----------------------------------------------------------------------------------

CGlyphManager::CGlyphManager(HANDLE handle) : m_handle(handle), m_config(readGlyphConfig()) {
    m_openListener = Event::bus()->m_events.window.openLate.listen([this](PHLWINDOW w) {
        attach(w);
        if (redirector())
            redirector()->onWindowOpened(w);
    });

    m_reloadListener = Event::bus()->m_events.config.reloaded.listen([this] {
        m_config = readGlyphConfig();
        damageAll();
    });

    m_buttonListener = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onButton(e, info); });
}

CGlyphManager::~CGlyphManager() {
    m_openListener.reset();
    m_reloadListener.reset();
    m_buttonListener.reset();

    // Damage where glyphs were, then detach. Removal is queued per window and processed at once
    // for mapped, visible windows; a hidden window processes it when shown again — fine, because
    // the .so is nodelete and a detached glyph draws nothing (state() needs glyphs()).
    damageAll();
    const auto DECOS = m_decos;
    m_decos.clear();
    for (auto* const deco : DECOS) {
        HyprlandAPI::removeWindowDecoration(m_handle, deco);
    }
}

const SGlyphConfig& CGlyphManager::config() const {
    return m_config;
}

void CGlyphManager::damageAll() {
    for (auto* const deco : m_decos) {
        deco->damageEntire();
    }
}

CGlyphDecoration* CGlyphManager::decoFor(const PHLWINDOW& window) const {
    const auto IT = std::ranges::find_if(m_decos, [&](const auto* d) { return d->window() == window; });
    return IT == m_decos.end() ? nullptr : *IT;
}

void CGlyphManager::attach(PHLWINDOW window) {
    if (!window || !window->m_isMapped || decoFor(window))
        return;

    auto  deco = makeUnique<CGlyphDecoration>(window);
    auto* RAW  = deco.get();
    if (HyprlandAPI::addWindowDecoration(m_handle, window, std::move(deco)))
        m_decos.emplace_back(RAW);
}

void CGlyphManager::attachAll() {
    for (const auto& w : Desktop::windowState()->windows()) {
        attach(w);
    }
}

void CGlyphManager::onDecorationDestroyed(CGlyphDecoration* deco) {
    std::erase(m_decos, deco);
}

void CGlyphManager::onButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
    const auto COORDS = g_pInputManager->getMouseCoordsInternal();

    if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
        // Release of a button whose press we consumed: consume it too (the client never saw the
        // press), and act if it is a left click that didn't turn into a drag.
        const auto IT = std::ranges::find_if(m_presses, [&](const auto& p) { return p.button == e.button; });
        if (IT == m_presses.end())
            return;

        const auto PRESS = *IT;
        m_presses.erase(IT);
        info.cancelled = true;

        if (e.button != BTN_LEFT_CODE || PRESS.pos.distance(COORDS) > CLICK_SLOP)
            return;

        const auto WINDOW = PRESS.window.lock();
        auto*      DECO   = WINDOW ? decoFor(WINDOW) : nullptr;
        if (!DECO)
            return;
        const auto STATE = DECO->state();
        if (STATE != GLYPH_NONE && DECO->boxGlobal().containsPoint(COORDS))
            onClick(WINDOW, STATE);
        return;
    }

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;

    if (!redirector() || !redirector()->shareModeActive())
        return;

    // Leave modified clicks (SUPER+drag binds, etc.) and locked / layer-focused input alone.
    if (g_pInputManager->getModsFromAllKBs() != 0 || g_pInputManager->m_lastFocusOnLS || g_pSessionLockManager->isSessionLocked())
        return;

    const auto WINDOW = Desktop::viewState()->hitTest().windowAt(COORDS, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
    if (!WINDOW || WINDOW->hasPopupAt(COORDS))
        return;

    auto* const DECO = decoFor(WINDOW);
    if (!DECO || DECO->state() == GLYPH_NONE || !DECO->boxGlobal().containsPoint(COORDS))
        return;

    m_presses.emplace_back(SPress{.button = e.button, .window = WINDOW, .pos = COORDS, .onGlyph = true});
    info.cancelled = true;
}

void CGlyphManager::onClick(const PHLWINDOW& window, eGlyphState state) {
    if (!redirector())
        return;

    SDispatchResult res;
    switch (state) {
        case GLYPH_IDLE: res = redirector()->shareWindowTarget(window); break;
        case GLYPH_LIVE: res = redirector()->pause(); break;
        case GLYPH_PAUSED: res = redirector()->shareWindowTarget(window); break;
        default: return;
    }

    Log::logger->log(Log::DEBUG, "[hyprshare] glyph click on 0x{:x} ({}): {}", rc<uintptr_t>(window.get()), glyphStateName(state), res.success ? "ok" : res.error);
}

std::string CGlyphManager::statusJSON() const {
    std::string out;
    for (auto* const deco : m_decos) {
        const auto STATE = deco->state();
        if (STATE == GLYPH_NONE)
            continue;
        const auto BOX = deco->boxGlobal();
        if (!out.empty())
            out += ",\n";
        out += std::format(R"(    {{"address": "0x{:x}", "state": "{}", "box": [{}, {}, {}, {}]}})", rc<uintptr_t>(deco->window().get()), glyphStateName(STATE), BOX.x, BOX.y,
                           BOX.w, BOX.h);
    }
    return std::format("{{\n  \"corner\": \"{}\",\n  \"visibility\": \"{}\",\n  \"glyphs\": [\n{}\n  ]\n}}", glyphCornerName(m_config.corner),
                       m_config.sharedOnly ? "shared_only" : "all", out);
}

UP<CGlyphManager>& HyprShare::glyphs() {
    static UP<CGlyphManager> instance;
    return instance;
}
