#include "ChangeEvent.hpp"
#include "CaptureSessions.hpp"
#include "ShareRedirector.hpp"

#include <src/managers/EventManager.hpp>
#include <src/managers/eventLoop/EventLoopManager.hpp>

#include <string>

using namespace HyprShare;

// Statics survive unload/reload (the .so is -z nodelete); primeChanged() resets them at init.
static bool        g_scheduled = false;
static std::string g_lastFingerprint;

static std::string currentFingerprint() {
    if (!redirector() || !captureSessions())
        return "";
    captureSessions()->scan();
    return captureSessions()->fingerprint() + "#" + redirector()->fingerprint();
}

void HyprShare::notifyChanged() {
    if (g_scheduled || !g_pEventLoopManager)
        return;
    g_scheduled = true;

    // Runs on the idle source after the current dispatch. If the plugin unloads first, the lambda
    // (kept mapped by -z nodelete) finds no redirector and does nothing.
    g_pEventLoopManager->doLater([] {
        g_scheduled = false;
        if (!redirector() || !captureSessions() || !g_pEventManager)
            return;

        auto fp = currentFingerprint();
        if (fp == g_lastFingerprint)
            return;
        g_lastFingerprint = std::move(fp);
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "hyprshare", .data = "changed"});
    });
}

void HyprShare::primeChanged() {
    g_lastFingerprint = currentFingerprint();
}
