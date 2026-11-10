#include "ShareRedirector.hpp"

#include <src/Compositor.hpp>
#include <src/config/shared/actions/ConfigActions.hpp>
#include <src/desktop/state/FocusState.hpp>
#include <src/desktop/state/ViewQuery.hpp>
#include <src/desktop/state/ViewState.hpp>
#include <src/desktop/view/Window.hpp>
#include <src/managers/eventLoop/EventLoopManager.hpp>
#include <src/managers/eventLoop/EventLoopTimer.hpp>

#include <hyprland-toplevel-export-v1.hpp>

#include <algorithm>
#include <format>
#include <tuple>

using namespace HyprShare;

// A capture counts as "active" if it asked for a frame this recently, or still holds a frame object
// (a static window can keep one frame pending, waiting for damage, for a long time).
static constexpr auto ACTIVE_TIMEOUT = std::chrono::milliseconds(1000);
// Inactive captures are forgotten after this long; a share that resumes just starts a new record.
static constexpr auto FORGET_TIMEOUT = std::chrono::seconds(10);
// Tag/prune cadence while anything is being tracked. Disarmed when idle: no wakeups otherwise.
static constexpr auto TICK_INTERVAL = std::chrono::milliseconds(250);

static uintptr_t      addressOf(const PHLWINDOW& window) {
    return rc<uintptr_t>(window.get());
}

static std::string addressString(uintptr_t address) {
    return std::format("0x{:x}", address);
}

static std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (sc<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", sc<unsigned>(c));
                else
                    out += c;
        }
    }
    return out;
}

// `{"address": ..., "alive": ..., "class": ..., "title": ...}` for a possibly-dead window.
static std::string windowJSON(const PHLWINDOWREF& ref, uintptr_t addressIfDead) {
    const auto WINDOW = ref.lock();
    if (!WINDOW)
        return std::format(R"({{"address": "{}", "alive": false}})", addressString(addressIfDead));

    return std::format(R"({{"address": "{}", "alive": true, "mapped": {}, "class": "{}", "title": "{}"}})", addressString(addressOf(WINDOW)), WINDOW->m_isMapped,
                       jsonEscape(WINDOW->m_class), jsonEscape(WINDOW->m_title));
}

static void setSharedTag(const PHLWINDOW& window, bool shared) {
    // Same path as the `tagwindow` dispatcher: applies the tag, re-evaluates rules, refreshes decorations.
    if (!window || !window->m_isMapped)
        return;

    std::ignore = Config::Actions::tag(std::format("{}{}", shared ? "+" : "-", SHARED_TAG), window);
}

bool CShareRedirector::SRedirect::matches(const SCapture& capture) const {
    // Both weak refs expire together if the origin is gone, so this also matches a dead origin
    // (XDPH keeps capturing the closed foreign-toplevel handle). The address check stops a dead
    // redirect from matching an unrelated capture.
    return originAddr == capture.originAddr && origin.lock() == capture.origin.lock();
}

CShareRedirector::CShareRedirector() {
    m_timer = makeShared<CEventLoopTimer>(std::nullopt, [this](SP<CEventLoopTimer> self, void* data) { onTick(); }, nullptr);
    g_pEventLoopManager->addTimer(m_timer);
}

CShareRedirector::~CShareRedirector() {
    // Leave nothing behind on unload: no listeners pointing into unmapped plugin code, no timer, no tags.
    for (auto& watch : m_frameWatches) {
        wl_list_remove(&watch->listener.link);
    }
    m_frameWatches.clear();

    // Held frames: tell the consumer to re-request (it will reach the stock path from now on), then
    // destroy our wrappers. That destroys the resources server-side; libwayland zombifies the ids, so
    // the client's own later destroy requests are harmless. No lambda of ours survives the unload.
    for (auto& held : m_heldFrames) {
        if (!held->released && held->resource)
            held->resource->sendFailed();
    }
    m_heldFrames.clear();

    if (m_timer) {
        m_timer->cancel();
        g_pEventLoopManager->removeTimer(m_timer);
    }

    // At compositor shutdown the windows are about to go anyway; don't re-run their rules.
    if (g_pCompositor && g_pCompositor->m_isShuttingDown)
        return;

    for (const auto& ref : m_tagged) {
        setSharedTag(ref.lock(), false);
    }
}

CShareRedirector::SCapture* CShareRedirector::captureByID(uint64_t id) {
    const auto IT = std::ranges::find_if(m_captures, [id](const auto& c) { return c.id == id; });
    return IT == m_captures.end() ? nullptr : &*IT;
}

CShareRedirector::SCapture* CShareRedirector::findCapture(wl_client* client, const PHLWINDOW& requested) {
    if (requested) {
        const auto IT = std::ranges::find_if(m_captures, [&](const auto& c) { return c.client == client && c.origin.lock() == requested; });
        return IT == m_captures.end() ? nullptr : &*IT;
    }

    // The origin window is destroyed: the portal's handle now resolves to nothing. All we know is
    // which client asked, so pick one of its captures that lost its origin — preferring a redirected
    // one, whose target we can keep serving. (Several dead captures of one client are
    // indistinguishable; they all end up held or all follow the same target, so it doesn't matter.)
    const auto DEAD = [&](const auto& c) { return c.client == client && c.origin.expired(); };
    auto       it   = std::ranges::find_if(m_captures, [&](const auto& c) { return DEAD(c) && isRedirected(c); });
    if (it == m_captures.end())
        it = std::ranges::find_if(m_captures, DEAD);
    return it == m_captures.end() ? nullptr : &*it;
}

bool CShareRedirector::isActive(const SCapture& capture, Time::steady_tp now) const {
    return capture.outstandingFrames > 0 || now - capture.lastSeen < ACTIVE_TIMEOUT;
}

bool CShareRedirector::isRedirected(const SCapture& capture) const {
    return std::ranges::any_of(m_redirects, [&](const auto& r) { return r.matches(capture); });
}

PHLWINDOW CShareRedirector::validTarget() const {
    const auto TARGET = m_target.lock();
    return TARGET && TARGET->m_isMapped ? TARGET : nullptr;
}

PHLWINDOW CShareRedirector::resolve(const SCapture& capture) const {
    if (isRedirected(capture)) {
        if (const auto TARGET = validTarget(); TARGET)
            return TARGET;
    }

    // Not redirected, or the target is gone: serve what the portal picked (null if that died too;
    // the caller holds the frame rather than letting the compositor drop the request).
    return capture.origin.lock();
}

size_t CShareRedirector::heldFrameCount(uint64_t captureID) const {
    return std::ranges::count_if(m_heldFrames, [captureID](const auto& h) { return h->captureID == captureID && !h->released; });
}

CShareRedirector::SServeDecision CShareRedirector::onCaptureRequest(wl_client* client, PHLWINDOW requested) {
    const auto NOW     = Time::steadyNow();
    auto*      capture = findCapture(client, requested);

    if (!capture) {
        if (!requested)
            return {.window = nullptr}; // untracked dead handle: let the compositor log and drop it

        pid_t pid = -1;
        wl_client_get_credentials(client, &pid, nullptr, nullptr); // client is live: we are inside its request

        capture = &m_captures.emplace_back(SCapture{
            .id         = m_nextCaptureID++,
            .client     = client,
            .pid        = pid,
            .origin     = requested,
            .originAddr = addressOf(requested),
            .lastSeen   = NOW,
        });
    }

    capture->lastSeen = NOW;

    // Nothing live to serve (picked window gone or closing, no live target): the stock path would
    // log and drop the request without creating the frame, and the portal would then wait for that
    // frame forever — so the capture would go inactive and `sharewindow` could never move it.
    auto SERVED = resolve(*capture);
    if (SERVED && !SERVED->m_isMapped)
        SERVED = nullptr;

    if (SERVED != capture->served.lock()) {
        capture->served = SERVED;
        updateTags();
    }

    armTimer();

    return {.window = SERVED, .captureID = capture->id, .hold = !SERVED};
}

void CShareRedirector::holdFrame(wl_client* client, uint32_t version, uint32_t frameID, uint64_t captureID) {
    auto resource = makeShared<CHyprlandToplevelExportFrameV1>(client, version, frameID);
    if (!resource->resource()) {
        wl_client_post_no_memory(client);
        return;
    }

    resource->setDestroy([this](CHyprlandToplevelExportFrameV1* r) { dropHeldFrame(r); });
    resource->setOnDestroy([this](CHyprlandToplevelExportFrameV1* r) { dropHeldFrame(r); });
    // The portal only copies after buffer_done, which a held frame never sends; answer anyway.
    resource->setCopy([this](CHyprlandToplevelExportFrameV1* r, wl_resource* buffer, int32_t ignoreDamage) {
        r->sendFailed();
        for (auto& held : m_heldFrames) {
            if (held->resource.get() == r)
                held->released = true;
        }
    });

    m_heldFrames.emplace_back(makeUnique<SHeldFrame>(SHeldFrame{.resource = std::move(resource), .captureID = captureID}));

    // Counted like any other frame object, so the capture stays active while held.
    watchFrame(client, frameID, captureID);

    Log::logger->log(Log::DEBUG, "[hyprshare] holding a frame of capture {}: nothing live to serve", captureID);
}

void CShareRedirector::dropHeldFrame(CHyprlandToplevelExportFrameV1* resource) {
    // Destroying the wrapper destroys the resource (destroy request), or just forgets it (resource
    // already being destroyed: the wrapper nulled its pointer). Same pattern as the compositor's.
    std::erase_if(m_heldFrames, [resource](const auto& h) { return h->resource.get() == resource; });
}

void CShareRedirector::releaseHeldFrames() {
    // A held capture that can be served again gets ONE `failed` per held frame: the portal destroys
    // the spent frame and re-requests at once, and that request is served normally.
    for (auto& held : m_heldFrames) {
        if (held->released || !held->resource)
            continue;

        const auto* CAPTURE = captureByID(held->captureID);
        if (!CAPTURE)
            continue;

        const auto SERVED = resolve(*CAPTURE);
        if (!SERVED || !SERVED->m_isMapped)
            continue;

        held->resource->sendFailed();
        held->released = true;
        Log::logger->log(Log::DEBUG, "[hyprshare] released held frame of capture {} (now serving {})", held->captureID, addressString(addressOf(SERVED)));
    }
}

void CShareRedirector::watchFrame(wl_client* client, uint32_t frameID, uint64_t captureID) {
    // The original created the frame resource for this new_id (unless it bailed out early, in which
    // case there is nothing to watch). We are still inside the client's request, so it is live.
    auto* const RESOURCE = wl_client_get_object(client, frameID);
    auto* const CAPTURE  = captureByID(captureID);
    if (!RESOURCE || !CAPTURE)
        return;

    auto& watch            = m_frameWatches.emplace_back(makeUnique<SFrameWatch>());
    watch->captureID       = captureID;
    watch->owner           = this;
    watch->listener.notify = &CShareRedirector::onFrameDestroyedThunk;
    wl_resource_add_destroy_listener(RESOURCE, &watch->listener);

    CAPTURE->outstandingFrames++;
}

void CShareRedirector::onFrameDestroyedThunk(wl_listener* listener, void* data) {
    SFrameWatch* watch = wl_container_of(listener, watch, listener);
    watch->owner->onFrameDestroyed(watch);
}

void CShareRedirector::onFrameDestroyed(SFrameWatch* watch) {
    // libwayland's final emit has already unlinked the listener, so freeing it here is fine.
    if (auto* const CAPTURE = captureByID(watch->captureID); CAPTURE && CAPTURE->outstandingFrames > 0)
        CAPTURE->outstandingFrames--;

    std::erase_if(m_frameWatches, [watch](const auto& w) { return w.get() == watch; });

    armTimer();
}

void CShareRedirector::armTimer() {
    if (m_timer && !m_timer->armed())
        m_timer->updateTimeout(TICK_INTERVAL);
}

void CShareRedirector::onTick() {
    prune(Time::steadyNow());
    updateTags();
    releaseHeldFrames();

    if (!m_captures.empty() || !m_tagged.empty() || !m_redirects.empty())
        armTimer();
}

void CShareRedirector::prune(Time::steady_tp now) {
    std::erase_if(m_captures, [&](const auto& c) { return c.outstandingFrames == 0 && now - c.lastSeen > FORGET_TIMEOUT; });

    // The target closed: drop the redirect table so the shares fall back to what the portal picked.
    if (!m_redirects.empty() && !validTarget()) {
        Log::logger->log(Log::DEBUG, "[hyprshare] redirect target is gone, falling back to the picked windows");
        m_redirects.clear();
        m_target.reset();
    }

    // A redirect whose origin died stays only while some capture can still use it (dead-handle path).
    std::erase_if(m_redirects, [&](const auto& r) {
        if (!r.origin.expired())
            return false;
        return std::ranges::none_of(m_captures, [&](const auto& c) { return r.matches(c); });
    });
}

void CShareRedirector::updateTags() {
    const auto             NOW = Time::steadyNow();

    std::vector<PHLWINDOW> desired;
    for (const auto& c : m_captures) {
        if (!isActive(c, NOW))
            continue;

        const auto SERVED = c.served.lock();
        if (!SERVED || !SERVED->m_isMapped || std::ranges::contains(desired, SERVED))
            continue;

        desired.emplace_back(SERVED);
    }

    std::erase_if(m_tagged, [&](const auto& ref) {
        const auto WINDOW = ref.lock();
        if (!WINDOW)
            return true;
        if (std::ranges::contains(desired, WINDOW))
            return false;

        setSharedTag(WINDOW, false);
        return true;
    });

    for (const auto& w : desired) {
        if (std::ranges::any_of(m_tagged, [&](const auto& ref) { return ref.lock() == w; }))
            continue;

        setSharedTag(w, true);
        m_tagged.emplace_back(w);
    }
}

SDispatchResult CShareRedirector::shareWindow(const std::string& selector) {
    const auto TARGET = selector.empty() ? Desktop::focusState()->window() : Desktop::viewState()->query().selector(selector).runWindow();
    if (!TARGET || !TARGET->m_isMapped)
        return {.success = false, .error = selector.empty() ? "sharewindow: no focused window" : std::format("sharewindow: no window matches \"{}\"", selector)};

    const auto NOW = Time::steadyNow();

    // Every share currently running gets redirected — including held ones whose picked window is
    // gone — and existing redirects (even of paused shares) are retargeted. Shares started later are
    // left alone until the next invocation.
    for (const auto& c : m_captures) {
        if (!isActive(c, NOW) || isRedirected(c))
            continue;

        m_redirects.emplace_back(SRedirect{.origin = c.origin, .originAddr = c.originAddr});
    }

    if (m_redirects.empty())
        return {.success = false, .error = "sharewindow: no active window share to redirect"};

    m_target = TARGET;

    // Move the tag now rather than on the next frame request.
    for (auto& c : m_captures) {
        c.served = resolve(c);
    }
    updateTags();
    releaseHeldFrames();
    armTimer();

    Log::logger->log(Log::DEBUG, "[hyprshare] serving {} ({}) to {} redirected share(s)", addressString(addressOf(TARGET)), TARGET->m_class, m_redirects.size());

    return {};
}

SDispatchResult CShareRedirector::clear() {
    m_redirects.clear();
    m_target.reset();

    for (auto& c : m_captures) {
        c.served = resolve(c);
    }
    updateTags();
    releaseHeldFrames();

    Log::logger->log(Log::DEBUG, "[hyprshare] redirects cleared");

    return {};
}

std::string CShareRedirector::statusJSON() const {
    const auto  NOW = Time::steadyNow();

    std::string captures;
    for (const auto& c : m_captures) {
        if (!captures.empty())
            captures += ",\n";

        const auto AGO = std::chrono::duration_cast<std::chrono::milliseconds>(NOW - c.lastSeen).count();
        captures +=
            std::format(R"(    {{"pid": {}, "active": {}, "redirected": {}, "lastSeenMsAgo": {}, "outstandingFrames": {}, "heldFrames": {}, "origin": {}, "served": {}}})", c.pid,
                        isActive(c, NOW), isRedirected(c), AGO, c.outstandingFrames, heldFrameCount(c.id), windowJSON(c.origin, c.originAddr), windowJSON(c.served, 0));
    }

    std::string redirects;
    for (const auto& r : m_redirects) {
        if (!redirects.empty())
            redirects += ",\n";
        redirects += std::format("    {}", windowJSON(r.origin, r.originAddr));
    }

    const auto TARGET = m_target.lock();

    return std::format("{{\n  \"target\": {},\n  \"redirectedOrigins\": [\n{}\n  ],\n  \"captures\": [\n{}\n  ]\n}}\n", TARGET ? windowJSON(m_target, 0) : "null", redirects,
                       captures);
}

UP<CShareRedirector>& HyprShare::redirector() {
    static UP<CShareRedirector> instance;
    return instance;
}
