#include "CaptureSessions.hpp"
#include "ChangeEvent.hpp"
#include "PrivateAccess.hpp"
#include "ShareRedirector.hpp"

#include <src/Compositor.hpp>
#include <src/config/values/types/IntValue.hpp>
#include <src/desktop/view/Window.hpp>
#include <src/event/EventBus.hpp>
#include <src/managers/eventLoop/EventLoopManager.hpp>
#include <src/managers/eventLoop/EventLoopTimer.hpp>
#include <src/managers/screenshare/ScreenshareManager.hpp>
#include <src/output/Monitor.hpp>
#include <src/plugins/PluginAPI.hpp>

#include <algorithm>
#include <format>

using namespace HyprShare;
using namespace Screenshare;

// ---- private compositor state (see PrivateAccess.hpp) -------------------------------------------

using SessionList = std::vector<WP<CScreenshareSession>>;
using FrameList   = std::vector<WP<CScreenshareFrame>>;

HYPRSHARE_PRIVATE_MEMBER(SMgrSessionsTag, CScreenshareManager, SessionList, m_sessions);
HYPRSHARE_PRIVATE_MEMBER(SMgrPendingFramesTag, CScreenshareManager, FrameList, m_pendingFrames);
HYPRSHARE_PRIVATE_MEMBER(SSessionTypeTag, CScreenshareSession, eScreenshareType, m_type);
HYPRSHARE_PRIVATE_MEMBER(SSessionSharingTag, CScreenshareSession, bool, m_sharing);
HYPRSHARE_PRIVATE_MEMBER(SSessionStoppedTag, CScreenshareSession, bool, m_stopped);
HYPRSHARE_PRIVATE_MEMBER(SSessionClientTag, CScreenshareSession, wl_client*, m_client);
HYPRSHARE_PRIVATE_MEMBER(SSessionWindowTag, CScreenshareSession, PHLWINDOWREF, m_window);
HYPRSHARE_PRIVATE_MEMBER(SFrameSessionTag, CScreenshareFrame, WP<CScreenshareSession>, m_session);
HYPRSHARE_PRIVATE_MEMBER(SFrameSharedTag, CScreenshareFrame, bool, m_shared);
HYPRSHARE_PRIVATE_MEMBER(SFrameCopiedTag, CScreenshareFrame, bool, m_copied);
HYPRSHARE_PRIVATE_MEMBER(SFrameFailedTag, CScreenshareFrame, bool, m_failed);
HYPRSHARE_PRIVATE_MEMBER(SFrameInFlightTag, CScreenshareFrame, bool, m_copyInFlight);
HYPRSHARE_PRIVATE_MEMBER(SFrameCallbackTag, CScreenshareFrame, FScreenshareCallback, m_callback);

// The sessions are owned by CUniquePointers: never .lock() these weak refs (hyprutils asserts), only
// check expired() and reach through operator-> / get().
static const SessionList* sessionList() {
    const auto& MGR = Screenshare::mgr();
    return MGR ? &(MGR.get()->*privateMember(SMgrSessionsTag{})) : nullptr;
}

static bool live(const WP<CScreenshareSession>& s) {
    return !s.expired() && !(s.get()->*privateMember(SSessionStoppedTag{}));
}

static const char* typeName(eScreenshareType type) {
    switch (type) {
        case SHARE_MONITOR: return "monitor";
        case SHARE_WINDOW: return "window";
        case SHARE_REGION: return "region";
        default: return "none";
    }
}

bool HyprShare::clientConnected(wl_client* client) {
    if (!client || !g_pCompositor || !g_pCompositor->m_wlDisplay)
        return false;

    wl_client* c = nullptr;
    wl_client_for_each(c, wl_display_get_client_list(g_pCompositor->m_wlDisplay)) {
        if (c == client)
            return true;
    }
    return false;
}

// ---- config ---------------------------------------------------------------------------------------

static SP<Config::Values::CIntValue>& graceValue() {
    static SP<Config::Values::CIntValue> v;
    return v;
}

void HyprShare::registerStopConfig(void* handle) {
    // The name must be a string literal: the config manager keeps the pointer (-z nodelete keeps it mapped).
    graceValue() = makeShared<Config::Values::CIntValue>("plugin:hyprshare:stop_grace_ms", "after sharestop, refuse new frames from the stopped clients for this long (ms)", 2000);
    if (!HyprlandAPI::addConfigValueV2(handle, graceValue()))
        Log::logger->log(Log::ERR, "[hyprshare] failed to register plugin:hyprshare:stop_grace_ms");
}

std::chrono::milliseconds HyprShare::stopGrace() {
    const auto& V = graceValue();
    return std::chrono::milliseconds(V ? std::clamp<int64_t>(V->value(), 0, 60000) : 2000);
}

// ---- CCaptureSessions -------------------------------------------------------------------------------

CCaptureSessions::CCaptureSessions() {
    // Sharing flips (the exact moments the compositor posts `screencast` events).
    m_shareStateListener = Event::bus()->m_events.screenshare.state.listen([] { notifyChanged(); });
    // The served window closing (its session stops too, but the redirect state matters as well), or
    // the shared window's title changing.
    m_windowCloseListener = Event::bus()->m_events.window.close.listen([] { notifyChanged(); });
    m_windowTitleListener = Event::bus()->m_events.window.title.listen([] { notifyChanged(); });

    m_graceTimer = makeShared<CEventLoopTimer>(
        std::nullopt,
        [this](SP<CEventLoopTimer>, void*) {
            pruneRefusals(Time::steadyNow());
            notifyChanged();
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_graceTimer);

    scan();
}

CCaptureSessions::~CCaptureSessions() {
    for (auto& r : m_refusals) {
        wl_list_remove(&r->listener.link);
    }
    m_refusals.clear();
    m_watches.clear(); // drops the `stopped` listeners
    for (auto& w : m_clientWatches) {
        wl_list_remove(&w->listener.link);
    }
    m_clientWatches.clear();

    if (m_graceTimer) {
        m_graceTimer->cancel();
        g_pEventLoopManager->removeTimer(m_graceTimer);
    }
}

bool CCaptureSessions::watching(const CScreenshareSession* session) const {
    return std::ranges::any_of(m_watches, [session](const auto& w) { return w.session == session; });
}

void CCaptureSessions::scan() {
    std::erase_if(m_watches, [](const auto& w) { return w.ref.expired(); });

    const auto* SESSIONS = sessionList();
    if (!SESSIONS)
        return;

    for (const auto& s : *SESSIONS) {
        if (!live(s) || watching(s.get()))
            continue;

        // A session that stops without ever sharing posts no `screencast` event; this catches it.
        wl_client* const CLIENT    = s.get()->*privateMember(SSessionClientTag{});
        const bool       CONNECTED = clientConnected(CLIENT);
        m_watches.emplace_back(SSessionWatch{
            .session  = s.get(),
            .ref      = s,
            .stopped  = s->m_events.stopped.listen([] { notifyChanged(); }),
            .orphaned = !CONNECTED,
        });
        if (CONNECTED)
            watchClient(CLIENT);
    }
}

void CCaptureSessions::watchClient(wl_client* client) {
    if (std::ranges::any_of(m_clientWatches, [client](const auto& w) { return w->client == client; }))
        return;

    auto& w            = m_clientWatches.emplace_back(makeUnique<SClientWatch>());
    w->client          = client;
    w->owner           = this;
    w->listener.notify = &CCaptureSessions::onClientDestroyedThunk;
    wl_client_add_destroy_listener(client, &w->listener);
}

void CCaptureSessions::onClientDestroyedThunk(wl_listener* listener, void* data) {
    SClientWatch* watch = wl_container_of(listener, watch, listener);
    watch->owner->onClientDestroyed(watch);
}

void CCaptureSessions::onClientDestroyed(SClientWatch* watch) {
    // libwayland's final emit already unlinked the listener.
    for (auto& w : m_watches) {
        if (!w.ref.expired() && w.ref.get()->*privateMember(SSessionClientTag{}) == watch->client)
            w.orphaned = true;
    }
    std::erase_if(m_clientWatches, [watch](const auto& w) { return w.get() == watch; });
    notifyChanged();
}

wl_client* CCaptureSessions::sessionClient(const CScreenshareSession* session) const {
    const auto IT = std::ranges::find_if(m_watches, [session](const auto& w) { return w.session == session; });
    if (IT != m_watches.end() && IT->orphaned)
        return nullptr;

    wl_client* const CLIENT = session->*privateMember(SSessionClientTag{});
    return clientConnected(CLIENT) ? CLIENT : nullptr;
}

bool CCaptureSessions::capturing() const {
    const auto* SESSIONS = sessionList();
    return SESSIONS && std::ranges::any_of(*SESSIONS, [](const auto& s) { return live(s) && s.get()->*privateMember(SSessionSharingTag{}); });
}

std::string CCaptureSessions::sessionsJSON() const {
    const auto* SESSIONS = sessionList();
    if (!SESSIONS)
        return "[]";

    std::string out;
    for (const auto& s : *SESSIONS) {
        if (!live(s))
            continue;

        auto* const SESSION = s.get();
        const auto  TYPE    = SESSION->*privateMember(SSessionTypeTag{});

        // The compositor keeps a managed session after its client disconnects (it never stops it):
        // its client pointer is dangling then (or reused by a new client), so only ask the client
        // that really owns it.
        wl_client* const CLIENT    = sessionClient(SESSION);
        const bool       CONNECTED = CLIENT != nullptr;
        pid_t            pid       = -1;
        if (CONNECTED)
            wl_client_get_credentials(CLIENT, &pid, nullptr, nullptr);

        const auto  MONITOR = SESSION->monitor();
        std::string window;
        if (TYPE == SHARE_WINDOW) {
            const auto& W = SESSION->*privateMember(SSessionWindowTag{});
            if (!W.expired())
                window = std::format("0x{:x}", rc<uintptr_t>(W.get()));
        }

        if (!out.empty())
            out += ",\n";
        out += std::format(R"(    {{"type": "{}", "sharing": {}, "pid": {}, "clientAlive": {}, "monitor": "{}", "window": "{}"}})", typeName(TYPE),
                           SESSION->*privateMember(SSessionSharingTag{}), pid, CONNECTED, MONITOR ? MONITOR->m_name : "", window);
    }

    return out.empty() ? "[]" : "[\n" + out + "\n  ]";
}

std::string CCaptureSessions::stopGraceJSON() const {
    const auto  NOW    = Time::steadyNow();
    int64_t     msLeft = 0;
    std::string pids;
    for (const auto& r : m_refusals) {
        if (r->until <= NOW)
            continue;
        msLeft = std::max<int64_t>(msLeft, std::chrono::duration_cast<std::chrono::milliseconds>(r->until - NOW).count());
        pids += std::format("{}{}", pids.empty() ? "" : ", ", r->pid);
    }
    return std::format(R"({{"active": {}, "msLeft": {}, "pids": [{}]}})", msLeft > 0, msLeft, pids);
}

std::string CCaptureSessions::fingerprint() const {
    const auto* SESSIONS = sessionList();
    if (!SESSIONS)
        return "";

    std::string fp;
    for (const auto& s : *SESSIONS) {
        if (!live(s))
            continue;
        fp += std::format("{:x}:{}:{};", rc<uintptr_t>(s.get()), sc<int>(s.get()->*privateMember(SSessionTypeTag{})), s.get()->*privateMember(SSessionSharingTag{}));
    }
    return fp;
}

size_t CCaptureSessions::stopAll() {
    scan();
    const auto* SESSIONS = sessionList();
    if (!SESSIONS) {
        if (redirector())
            redirector()->forgetAll();
        return 0;
    }

    // 1. Answer every frame that was handed a buffer and is waiting for an output commit: once its
    //    session stops, the compositor drops it from the copy queue WITHOUT answering, and the client
    //    (wf-recorder, ...) would wait on it forever. Its own protocol callback sends `failed`; marking
    //    it failed first keeps the frame's destructor from answering twice. Copies already in flight
    //    (a dmabuf blit waiting on its fence) complete normally.
    const auto& MGR     = Screenshare::mgr();
    const auto  PENDING = MGR.get()->*privateMember(SMgrPendingFramesTag{}); // copy: callbacks may touch the list
    for (const auto& f : PENDING) {
        if (f.expired())
            continue;
        auto* const FRAME = f.get();
        if (!(FRAME->*privateMember(SFrameSharedTag{})) || FRAME->*privateMember(SFrameCopiedTag{}) || FRAME->*privateMember(SFrameFailedTag{}) ||
            FRAME->*privateMember(SFrameInFlightTag{}))
            continue;

        FRAME->*privateMember(SFrameFailedTag{}) = true;
        FScreenshareCallback cb;
        std::swap(cb, FRAME->*privateMember(SFrameCallbackTag{}));
        if (cb)
            cb(RESULT_NOT_COPIED);
    }

    // 2. Stop every live session (all types). A managed session (screencopy, toplevel export) is
    //    destroyed by its own stop; an ext-image-copy-capture session tells its client `stopped`.
    std::vector<wl_client*> clients;
    size_t                  stopped  = 0;
    const auto              SNAPSHOT = *SESSIONS; // stop() may destroy sessions
    for (const auto& s : SNAPSHOT) {
        if (!live(s))
            continue;

        wl_client* const CLIENT = sessionClient(s.get());
        if (CLIENT && !std::ranges::contains(clients, CLIENT))
            clients.emplace_back(CLIENT);

        s->stop();
        stopped++;
    }

    // 3. The redirect forgets everything (targets, pause, NOTHING, tags) so nothing is re-served.
    if (redirector()) {
        for (wl_client* const CLIENT : redirector()->forgetAll()) {
            if (clientConnected(CLIENT) && !std::ranges::contains(clients, CLIENT))
                clients.emplace_back(CLIENT);
        }
    }

    // 4. Grace window: these clients' retries are refused for a while.
    const auto GRACE = stopGrace();
    if (GRACE.count() > 0) {
        const auto UNTIL = Time::steadyNow() + GRACE;
        for (wl_client* const CLIENT : clients) {
            refuse(CLIENT, UNTIL);
        }
        m_graceTimer->updateTimeout(GRACE + std::chrono::milliseconds(1));
    }

    Log::logger->log(Log::DEBUG, "[hyprshare] sharestop: stopped {} session(s), refusing {} client(s) for {} ms", stopped, clients.size(), GRACE.count());
    notifyChanged();
    return stopped;
}

void CCaptureSessions::refuse(wl_client* client, Time::steady_tp until) {
    if (const auto IT = std::ranges::find_if(m_refusals, [client](const auto& r) { return r->client == client; }); IT != m_refusals.end()) {
        (*IT)->until = until;
        return;
    }

    auto& r            = m_refusals.emplace_back(makeUnique<SRefusal>());
    r->client          = client;
    r->until           = until;
    r->owner           = this;
    r->listener.notify = &CCaptureSessions::onRefusedClientDestroyedThunk;
    wl_client_get_credentials(client, &r->pid, nullptr, nullptr); // connected: checked by the caller
    wl_client_add_destroy_listener(client, &r->listener);
}

bool CCaptureSessions::refused(wl_client* client) const {
    const auto NOW = Time::steadyNow();
    return std::ranges::any_of(m_refusals, [&](const auto& r) { return r->client == client && r->until > NOW; });
}

void CCaptureSessions::pruneRefusals(Time::steady_tp now) {
    std::erase_if(m_refusals, [now](auto& r) {
        if (r->until > now)
            return false;
        wl_list_remove(&r->listener.link);
        return true;
    });
}

void CCaptureSessions::onRefusedClientDestroyedThunk(wl_listener* listener, void* data) {
    SRefusal* refusal = wl_container_of(listener, refusal, listener);
    refusal->owner->onRefusedClientDestroyed(refusal);
}

void CCaptureSessions::onRefusedClientDestroyed(SRefusal* refusal) {
    // libwayland's final emit already unlinked the listener.
    std::erase_if(m_refusals, [refusal](const auto& r) { return r.get() == refusal; });
}

bool CCaptureSessions::onFrameShare(CScreenshareFrame* frame) {
    const auto& SESSION = frame->*privateMember(SFrameSessionTag{});
    if (SESSION.expired())
        return false; // the original answers ERROR_STOPPED itself

    wl_client* const CLIENT = SESSION.get()->*privateMember(SSessionClientTag{});

    // A session we have not seen yet just asked for its first frame: a new capture.
    const auto IT = std::ranges::find_if(m_watches, [&](const auto& w) { return w.session == SESSION.get(); });
    if (IT == m_watches.end())
        notifyChanged();
    else if (IT->orphaned && clientConnected(CLIENT)) {
        // The compositor handed a dead client's leftover session to a new client that reuses its
        // address (managed sessions are keyed on the wl_client pointer). It is that client's now.
        IT->orphaned = false;
        watchClient(CLIENT);
        notifyChanged();
    }

    if (!refused(CLIENT))
        return false;

    // Refuse: the protocol answers `failed`. Stop the session too (not from inside the protocol's own
    // request handler), so an ext-image-copy-capture client is told `stopped` and a managed
    // screencopy session is torn down rather than lingering.
    g_pEventLoopManager->doLater([weak = SESSION] {
        if (!weak.expired())
            weak->stop();
    });
    return true;
}

UP<CCaptureSessions>& HyprShare::captureSessions() {
    static UP<CCaptureSessions> instance;
    return instance;
}
