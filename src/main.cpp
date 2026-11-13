// hyprshare — spike: swap the window a running window-share shows, without the app noticing.
//
// Mechanism: hook CToplevelExportClient::captureToplevel (src/protocols/ToplevelExport.cpp) and hand
// the original a different window. See ShareRedirector.hpp for the policy, README.md for usage.
// The status-bar API (capturing/sessions in sharestatus, hyprshare>>changed, sharefocus, sharestop)
// lives in CaptureSessions / ChangeEvent; sharestop's grace window also hooks
// Screenshare::CScreenshareFrame::share, the copy request of every capture protocol.

#include "PrivateAccess.hpp"
#include "CaptureSessions.hpp"
#include "ChangeEvent.hpp"
#include "Glyph.hpp"
#include "ShareRedirector.hpp"

#include <src/managers/screenshare/ScreenshareManager.hpp>
#include <src/plugins/PluginAPI.hpp>
#include <src/protocols/ToplevelExport.hpp>

extern "C" {
#include <lua.h>
}

#include <format>
#include <stdexcept>
#include <string>

using namespace HyprShare;

static HANDLE PHANDLE = nullptr;

// The exact symbol we patch. If the compositor's signature ever differs we refuse to hook rather
// than call through a mismatched trampoline.
static constexpr const char* CAPTURE_TOPLEVEL_SIGNATURE = "CToplevelExportClient::captureToplevel(unsigned int, int, Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>)";

using FCaptureToplevel = void (*)(CToplevelExportClient*, uint32_t, int32_t, PHLWINDOW);

static CFunctionHook* g_pCaptureToplevelHook = nullptr;

// Every protocol's copy request (wlr-screencopy, ext-image-copy-capture, toplevel export) goes through
// this, which is where sharestop's grace window refuses frames from the clients it just stopped.
static constexpr const char* FRAME_SHARE_SIGNATURE =
    "Screenshare::CScreenshareFrame::share(Hyprutils::Memory::CSharedPointer<IHLBuffer>, Hyprutils::Math::CRegion const&, std::function<void (Screenshare::eScreenshareResult)>)";

using FFrameShare = Screenshare::eScreenshareError (*)(Screenshare::CScreenshareFrame*, SP<IHLBuffer>, const CRegion&, Screenshare::FScreenshareCallback);

static CFunctionHook* g_pFrameShareHook = nullptr;

// CToplevelExportClient::m_savedClient is private; see PrivateAccess.hpp.
struct SSavedClientTag {
    using type = wl_client* CToplevelExportClient::*;
    friend type             privateMember(SSavedClientTag);
};
template struct SPrivateMember<SSavedClientTag, &CToplevelExportClient::m_savedClient>;

// ... and so is its manager resource, whose version a frame we create ourselves must carry.
struct SManagerResourceTag {
    using type = SP<CHyprlandToplevelExportManagerV1> CToplevelExportClient::*;
    friend type                                       privateMember(SManagerResourceTag);
};
template struct SPrivateMember<SManagerResourceTag, &CToplevelExportClient::m_resource>;

static void hkCaptureToplevel(CToplevelExportClient* self, uint32_t frame, int32_t overlayCursor, PHLWINDOW handle) {
    const auto       ORIGINAL = rc<FCaptureToplevel>(g_pCaptureToplevelHook->m_original);

    wl_client* const CLIENT = self->*privateMember(SSavedClientTag{});
    if (!redirector() || !CLIENT) {
        ORIGINAL(self, frame, overlayCursor, handle);
        return;
    }

    const auto DECISION = redirector()->onCaptureRequest(CLIENT, handle);
    notifyChanged(); // cheap: one fingerprint check per event-loop iteration

    // For ACT_PLACEHOLDER / ACT_FAIL we must create the frame object for the client's new_id
    // OURSELVES: the original returns early for a null window without creating it, which leaves the
    // id a NULL object and kills the portal on the next destroy. We need the manager's version to
    // create a matching resource.
    if (DECISION.action != CShareRedirector::ACT_SERVE) {
        const auto& MANAGER = self->*privateMember(SManagerResourceTag{});
        if (MANAGER && MANAGER->resource()) {
            if (DECISION.action == CShareRedirector::ACT_PLACEHOLDER)
                redirector()->placeholderFrame(CLIENT, MANAGER->version(), frame, DECISION.captureID);
            else
                redirector()->failFrame(CLIENT, MANAGER->version(), frame, DECISION.action == CShareRedirector::ACT_FAIL_LATER);
            return;
        }
        // No manager resource (shouldn't happen): fall through to the original rather than drop.
    }

    ORIGINAL(self, frame, overlayCursor, DECISION.window);

    if (DECISION.action == CShareRedirector::ACT_SERVE && DECISION.captureID != 0)
        redirector()->watchFrame(CLIENT, frame, DECISION.captureID);
}

static Screenshare::eScreenshareError hkFrameShare(Screenshare::CScreenshareFrame* self, SP<IHLBuffer> buffer, const CRegion& damage, Screenshare::FScreenshareCallback callback) {
    // Refused: every caller answers ERROR_STOPPED with `failed` on the frame the CLIENT created, which
    // it then destroys itself. Nothing is copied, no session starts sharing.
    if (captureSessions() && captureSessions()->onFrameShare(self))
        return Screenshare::ERROR_STOPPED;

    return rc<FFrameShare>(g_pFrameShareHook->m_original)(self, std::move(buffer), damage, std::move(callback));
}

static SDispatchResult dispatchShareWindow(std::string args) {
    if (!redirector())
        return {.success = false, .error = "hyprshare: not initialized"};
    return redirector()->shareWindow(args);
}

static SDispatchResult dispatchSharePause(std::string args) {
    if (!redirector())
        return {.success = false, .error = "hyprshare: not initialized"};
    return redirector()->pause();
}

static SDispatchResult dispatchShareClear(std::string args) {
    if (!redirector())
        return {.success = false, .error = "hyprshare: not initialized"};
    return redirector()->clear();
}

static SDispatchResult dispatchShareFocus(std::string args) {
    if (!redirector())
        return {.success = false, .error = "hyprshare: not initialized"};
    return redirector()->focusShared();
}

static size_t stopAll() {
    return captureSessions() ? captureSessions()->stopAll() : 0;
}

static SDispatchResult dispatchShareStop(std::string args) {
    if (!captureSessions())
        return {.success = false, .error = "hyprshare: not initialized"};
    stopAll();
    return {};
}

// hyprctl commands: work under both config front ends (under Lua, `hyprctl dispatch X` is evaluated
// as `hl.dispatch(X)`, so plugin dispatchers registered by name are unreachable from it).
static std::string resultString(const SDispatchResult& res) {
    return res.success ? "ok" : res.error;
}

static std::string commandArgs(const std::string& request) {
    // request is "<command>[ args]"; hyprctl flags have already been stripped
    const auto SPACE = request.find(' ');
    if (SPACE == std::string::npos)
        return "";

    const auto BEGIN = request.find_first_not_of(' ', SPACE);
    if (BEGIN == std::string::npos)
        return "";

    const auto END = request.find_last_not_of(" \n");
    return request.substr(BEGIN, END - BEGIN + 1);
}

static std::string ctlShareWindow(eHyprCtlOutputFormat, std::string request) {
    return resultString(dispatchShareWindow(commandArgs(request)));
}

static std::string ctlSharePause(eHyprCtlOutputFormat, std::string request) {
    return resultString(dispatchSharePause(""));
}

static std::string ctlShareClear(eHyprCtlOutputFormat, std::string request) {
    return resultString(dispatchShareClear(""));
}

static std::string ctlShareFocus(eHyprCtlOutputFormat, std::string request) {
    return resultString(dispatchShareFocus(""));
}

static std::string ctlShareStop(eHyprCtlOutputFormat, std::string request) {
    if (!captureSessions())
        return "hyprshare: not initialized";
    return std::format("ok {}", stopAll());
}

static std::string statusJSON() {
    if (captureSessions())
        captureSessions()->scan(); // attribute sessions created since the last event-loop check
    return redirector() ? redirector()->statusJSON() : "{}";
}

static std::string ctlShareStatus(eHyprCtlOutputFormat, std::string request) {
    return statusJSON();
}

// Lua: hl.plugin.hyprshare.share([selector]) / .clear() / .pause() / .focus() / .stop() / .status().
// They act immediately and return `true` or `false, err` (stop: `true, n`). They never raise (luaL_error longjmps through C++ frames).
static int luaReturn(lua_State* L, const SDispatchResult& res) {
    lua_pushboolean(L, res.success);
    if (res.success)
        return 1;

    lua_pushstring(L, res.error.c_str());
    return 2;
}

static int luaShare(lua_State* L) {
    std::string selector;
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        if (lua_type(L, 1) != LUA_TSTRING)
            return luaReturn(L, {.success = false, .error = "hl.plugin.hyprshare.share: selector must be a string"});
        selector = lua_tostring(L, 1);
    }

    return luaReturn(L, dispatchShareWindow(selector));
}

static int luaPause(lua_State* L) {
    return luaReturn(L, dispatchSharePause(""));
}

static int luaClear(lua_State* L) {
    return luaReturn(L, dispatchShareClear(""));
}

static int luaFocus(lua_State* L) {
    return luaReturn(L, dispatchShareFocus(""));
}

// true, <sessions stopped>
static int luaStop(lua_State* L) {
    if (!captureSessions())
        return luaReturn(L, {.success = false, .error = "hyprshare: not initialized"});
    lua_pushboolean(L, true);
    lua_pushinteger(L, sc<lua_Integer>(stopAll()));
    return 2;
}

static int luaStatus(lua_State* L) {
    const auto STATUS = statusJSON();
    lua_pushstring(L, STATUS.c_str());
    return 1;
}

static void failInit(const std::string& why) {
    HyprlandAPI::addNotification(PHANDLE, "[hyprshare] " + why, CHyprColor{1.0, 0.2, 0.2, 1.0}, 10000);
    throw std::runtime_error("[hyprshare] " + why);
}

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // The compositor does not enforce this; it is on us. A mismatch means our view of every struct
    // layout may be wrong, so bail before touching anything.
    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH)
        failInit(std::format("version mismatch (compositor {}, plugin {}); rebuild against the running commit", HASH, CLIENT_HASH));

    void* target = nullptr;
    for (const auto& match : HyprlandAPI::findFunctionsByName(PHANDLE, "captureToplevel")) {
        if (match.demangled != CAPTURE_TOPLEVEL_SIGNATURE)
            continue;
        target = match.address;
        break;
    }

    if (!target)
        failInit(std::format("could not find {}", CAPTURE_TOPLEVEL_SIGNATURE));

    void* shareTarget = nullptr;
    for (const auto& match : HyprlandAPI::findFunctionsByName(PHANDLE, "share")) {
        if (match.demangled != FRAME_SHARE_SIGNATURE)
            continue;
        shareTarget = match.address;
        break;
    }

    if (!shareTarget)
        failInit(std::format("could not find {}", FRAME_SHARE_SIGNATURE));

    // Config values may only be registered during pluginInit.
    registerGlyphConfig(PHANDLE);
    registerStopConfig(PHANDLE);

    redirector()      = makeUnique<CShareRedirector>();
    captureSessions() = makeUnique<CCaptureSessions>();

    g_pCaptureToplevelHook = HyprlandAPI::createFunctionHook(PHANDLE, target, rc<void*>(&hkCaptureToplevel));
    if (!g_pCaptureToplevelHook || !g_pCaptureToplevelHook->hook()) {
        captureSessions().reset();
        redirector().reset();
        failInit("failed to hook captureToplevel");
    }

    g_pFrameShareHook = HyprlandAPI::createFunctionHook(PHANDLE, shareTarget, rc<void*>(&hkFrameShare));
    if (!g_pFrameShareHook || !g_pFrameShareHook->hook()) {
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pCaptureToplevelHook);
        g_pCaptureToplevelHook = nullptr;
        captureSessions().reset();
        redirector().reset();
        failInit("failed to hook CScreenshareFrame::share");
    }

    HyprlandAPI::addDispatcherV2(PHANDLE, "sharewindow", dispatchShareWindow);
    HyprlandAPI::addDispatcherV2(PHANDLE, "shareclear", dispatchShareClear);
    HyprlandAPI::addDispatcherV2(PHANDLE, "sharepause", dispatchSharePause);
    HyprlandAPI::addDispatcherV2(PHANDLE, "sharefocus", dispatchShareFocus);
    HyprlandAPI::addDispatcherV2(PHANDLE, "sharestop", dispatchShareStop);

    HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "sharewindow", .exact = false, .fn = ctlShareWindow});
    HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "shareclear", .exact = false, .fn = ctlShareClear});
    HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "sharepause", .exact = false, .fn = ctlSharePause});
    HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "sharestatus", .exact = false, .fn = ctlShareStatus});
    HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "sharefocus", .exact = false, .fn = ctlShareFocus});
    HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "sharestop", .exact = false, .fn = ctlShareStop});

    // Only exists under the Lua front end; returns false (harmlessly) under hyprlang.
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprshare", "share", luaShare);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprshare", "clear", luaClear);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprshare", "pause", luaPause);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprshare", "status", luaStatus);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprshare", "focus", luaFocus);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprshare", "stop", luaStop);

    // Glyph decorations on every window (drawn only while a share is active), new ones on open.
    glyphs() = makeUnique<CGlyphManager>(PHANDLE);
    glyphs()->attachAll();

    // Loading changes nothing a bar shows; only later changes post `hyprshare>>changed`.
    primeChanged();

    return {"hyprshare", "Redirect running window shares to another window (spike)", "Andrew Gaspar", "0.5"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Unhook first so no request can reach a half-torn-down redirector; the plugin system would
    // remove the hook after this returns anyway.
    if (g_pCaptureToplevelHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pCaptureToplevelHook);
    g_pCaptureToplevelHook = nullptr;
    if (g_pFrameShareHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pFrameShareHook);
    g_pFrameShareHook = nullptr;

    // Glyphs first: detaching them damages and queues removal, and their state reads the redirector.
    glyphs().reset();
    redirector().reset();
    captureSessions().reset();
}
