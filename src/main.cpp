// hyprshare — spike: swap the window a running window-share shows, without the app noticing.
//
// Mechanism: hook CToplevelExportClient::captureToplevel (src/protocols/ToplevelExport.cpp) and hand
// the original a different window. See ShareRedirector.hpp for the policy, README.md for usage.

#include "PrivateAccess.hpp"
#include "ShareRedirector.hpp"

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

// CToplevelExportClient::m_savedClient is private; see PrivateAccess.hpp.
struct SSavedClientTag {
    using type = wl_client* CToplevelExportClient::*;
    friend type             privateMember(SSavedClientTag);
};
template struct SPrivateMember<SSavedClientTag, &CToplevelExportClient::m_savedClient>;

static void hkCaptureToplevel(CToplevelExportClient* self, uint32_t frame, int32_t overlayCursor, PHLWINDOW handle) {
    const auto       ORIGINAL = rc<FCaptureToplevel>(g_pCaptureToplevelHook->m_original);

    wl_client* const CLIENT = self->*privateMember(SSavedClientTag{});
    if (!redirector() || !CLIENT) {
        ORIGINAL(self, frame, overlayCursor, handle);
        return;
    }

    const auto DECISION = redirector()->onCaptureRequest(CLIENT, handle);

    ORIGINAL(self, frame, overlayCursor, DECISION.window);

    if (DECISION.captureID != 0)
        redirector()->watchFrame(CLIENT, frame, DECISION.captureID);
}

static SDispatchResult dispatchShareWindow(std::string args) {
    if (!redirector())
        return {.success = false, .error = "hyprshare: not initialized"};
    return redirector()->shareWindow(args);
}

static SDispatchResult dispatchShareClear(std::string args) {
    if (!redirector())
        return {.success = false, .error = "hyprshare: not initialized"};
    return redirector()->clear();
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

static std::string ctlShareClear(eHyprCtlOutputFormat, std::string request) {
    return resultString(dispatchShareClear(""));
}

static std::string ctlShareStatus(eHyprCtlOutputFormat, std::string request) {
    return redirector() ? redirector()->statusJSON() : "{}";
}

// Lua: hl.plugin.hyprshare.share([selector]) / .clear() / .status(). They act immediately and
// return `true` or `false, err`. They never raise (luaL_error longjmps through C++ frames).
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

static int luaClear(lua_State* L) {
    return luaReturn(L, dispatchShareClear(""));
}

static int luaStatus(lua_State* L) {
    const auto STATUS = redirector() ? redirector()->statusJSON() : std::string{"{}"};
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

    redirector() = makeUnique<CShareRedirector>();

    g_pCaptureToplevelHook = HyprlandAPI::createFunctionHook(PHANDLE, target, rc<void*>(&hkCaptureToplevel));
    if (!g_pCaptureToplevelHook || !g_pCaptureToplevelHook->hook()) {
        redirector().reset();
        failInit("failed to hook captureToplevel");
    }

    HyprlandAPI::addDispatcherV2(PHANDLE, "sharewindow", dispatchShareWindow);
    HyprlandAPI::addDispatcherV2(PHANDLE, "shareclear", dispatchShareClear);

    HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "sharewindow", .exact = false, .fn = ctlShareWindow});
    HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "shareclear", .exact = false, .fn = ctlShareClear});
    HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "sharestatus", .exact = false, .fn = ctlShareStatus});

    // Only exists under the Lua front end; returns false (harmlessly) under hyprlang.
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprshare", "share", luaShare);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprshare", "clear", luaClear);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprshare", "status", luaStatus);

    return {"hyprshare", "Redirect running window shares to another window (spike)", "Andrew Gaspar", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Unhook first so no request can reach a half-torn-down redirector; the plugin system would
    // remove the hook after this returns anyway.
    if (g_pCaptureToplevelHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pCaptureToplevelHook);
    g_pCaptureToplevelHook = nullptr;

    redirector().reset();
}
