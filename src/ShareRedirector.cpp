#include "ShareRedirector.hpp"

#include <src/Compositor.hpp>
#include <src/config/shared/actions/ConfigActions.hpp>
#include <src/desktop/state/FocusState.hpp>
#include <src/desktop/state/ViewQuery.hpp>
#include <src/desktop/state/ViewState.hpp>
#include <src/desktop/view/Window.hpp>
#include <src/helpers/Format.hpp>
#include <src/helpers/cm/ColorManagement.hpp>
#include <src/managers/eventLoop/EventLoopManager.hpp>
#include <src/managers/eventLoop/EventLoopTimer.hpp>
#include <src/managers/screenshare/ScreenshareManager.hpp>
#include <src/protocols/types/Buffer.hpp>
#include <src/render/Renderer.hpp>
#include <src/render/pass/ClearPassElement.hpp>
#include <src/state/MonitorState.hpp>

#include <hyprgraphics/egl/Egl.hpp>
#include <hyprland-toplevel-export-v1.hpp>

#include <algorithm>
#include <format>
#include <tuple>

using namespace HyprShare;
using namespace Hyprgraphics::Egl;

// A capture is "active" if it asked for a frame this recently, or still holds a frame object (a
// static window, or a placeholder waiting on a throttled copy, keeps one alive).
static constexpr auto ACTIVE_TIMEOUT = std::chrono::milliseconds(1000);
// Inactive captures are forgotten after this long; a share that resumes just starts a new record.
static constexpr auto FORGET_TIMEOUT = std::chrono::seconds(10);
// Tag/prune/release cadence while anything is tracked. Disarmed when idle: no wakeups otherwise.
static constexpr auto TICK_INTERVAL = std::chrono::milliseconds(250);
// Placeholder frame pacing: ~10 fps of black is plenty and keeps the copy path cheap.
static constexpr auto PLACEHOLDER_THROTTLE = std::chrono::milliseconds(100);

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

    // Placeholder frames: answer any still open (so the client doesn't hang), then DISOWN the
    // resources rather than destroy them. These are CLIENT-created objects; destroying one from the
    // server (which the wrapper destructor would do) leaves the client with a stale proxy, and its
    // next request on that id posts wl_display error 0 (invalid object) — the very crash we're
    // avoiding. Clearing the resource's user_data makes the wrapper destructor leave the wl_resource
    // alive; with user_data null its copy/destroy stubs become no-ops, so no freed lambda is ever
    // called, and the client destroys the object itself in the normal way.
    for (auto& ph : m_placeholders) {
        if (ph->copyTimer) {
            ph->copyTimer->cancel();
            g_pEventLoopManager->removeTimer(ph->copyTimer);
        }
        if (ph->copyAsked && !ph->answered && ph->resource && ph->resource->resource())
            ph->resource->sendFailed();
        if (ph->resource && ph->resource->resource())
            wl_resource_set_user_data(ph->resource->resource(), nullptr);
    }
    m_placeholders.clear();

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

    // The portal's handle resolves to no window (the picked window closed, but XDPH keeps the handle).
    // All we know is the client. Pick one of its captures whose origin is dead — preferring a
    // FOLLOW_TARGET one, whose live target we can keep serving. Captures of one client are otherwise
    // indistinguishable here, but they share a mode after any share/clear, so it doesn't matter which.
    const auto DEAD = [&](const auto& c) { return c.client == client && c.origin.expired(); };
    auto       it   = std::ranges::find_if(m_captures, [&](const auto& c) { return DEAD(c) && c.mode == SCapture::FOLLOW_TARGET && !c.nothing; });
    if (it == m_captures.end())
        it = std::ranges::find_if(m_captures, DEAD);
    return it == m_captures.end() ? nullptr : &*it;
}

bool CShareRedirector::isActive(const SCapture& capture, Time::steady_tp now) const {
    return capture.outstandingFrames > 0 || now - capture.lastSeen < ACTIVE_TIMEOUT;
}

PHLWINDOW CShareRedirector::validTarget() const {
    const auto TARGET = m_target.lock();
    return TARGET && TARGET->m_isMapped ? TARGET : nullptr;
}

PHLMONITOR CShareRedirector::placeholderMonitor(const SCapture& capture) const {
    if (const auto M = capture.monitor.lock(); M && State::monitorState()->contains(M))
        return M;
    // The recorded monitor is gone; any live monitor will do as the off-screen render target.
    const auto& MONS = State::monitorState()->monitors();
    return MONS.empty() ? nullptr : MONS.front();
}

void CShareRedirector::recordConstraints(SCapture& capture, const PHLWINDOW& window) {
    // Read the EXACT size/format the compositor will advertise for (client, window): its managed
    // session already exists (the portal is actively capturing it), and the original will reuse it.
    // The session is owned by a CUniquePointer, so we must NOT .lock() the weak ref (that asserts in
    // hyprutils) — reach through operator-> instead.
    const auto SESSION = Screenshare::mgr()->getManagedSession(capture.client, window);
    if (SESSION.expired())
        return;

    capture.size            = SESSION->bufferSize();
    const auto& FORMATS     = SESSION->allowedFormats();
    capture.format          = FORMATS.empty() ? 0 : FORMATS.front();
    capture.monitor         = SESSION->monitor();
    capture.haveConstraints = capture.size.x >= 1 && capture.size.y >= 1;
}

// Decide what to serve, mutating the capture's mode / nothing / served state. Null => placeholder.
PHLWINDOW CShareRedirector::serveWindow(SCapture& capture) {
    PHLWINDOW wanted;
    if (!capture.nothing) {
        if (capture.mode == SCapture::FOLLOW_TARGET)
            wanted = validTarget();
        else if (const auto O = capture.origin.lock(); O && O->m_isMapped)
            wanted = O;
    }

    if (wanted) {
        recordConstraints(capture, wanted);
        capture.served = wanted;
        return wanted;
    }

    // Nothing live to serve. Enter the sticky NOTHING state: from here only an explicit share/clear
    // (never a window reappearing, never a recycled address) leaves it.
    if (!capture.nothing) {
        capture.nothing = true;
        Log::logger->log(Log::DEBUG, "[hyprshare] capture {} entered NOTHING (served window gone); serving placeholder", capture.id);
    }
    capture.served = {};
    return nullptr;
}

size_t CShareRedirector::placeholderCount(uint64_t captureID) const {
    return std::ranges::count_if(m_placeholders, [captureID](const auto& p) { return p->captureID == captureID; });
}

CShareRedirector::SServeDecision CShareRedirector::onCaptureRequest(wl_client* client, PHLWINDOW requested) {
    const auto NOW     = Time::steadyNow();
    auto*      capture = findCapture(client, requested);

    if (!capture) {
        if (!requested) {
            // Untracked client asking for a dead/unknown handle: we have no size to serve a
            // placeholder, and the original would drop the request without a resource (the crash).
            // Create the resource ourselves and send `failed` — the portal re-requests, never hangs.
            return {.action = ACT_FAIL};
        }

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

    const auto SERVED = serveWindow(*capture);
    updateTags();
    armTimer();

    if (SERVED)
        return {.action = ACT_SERVE, .window = SERVED, .captureID = capture->id};

    // NOTHING: placeholder if we know a size, else fail (never reached once a capture served once).
    return {.action = capture->haveConstraints ? ACT_PLACEHOLDER : ACT_FAIL, .captureID = capture->id};
}

void CShareRedirector::watchFrame(wl_client* client, uint32_t frameID, uint64_t captureID) {
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
    if (auto* const CAPTURE = captureByID(watch->captureID); CAPTURE && CAPTURE->outstandingFrames > 0)
        CAPTURE->outstandingFrames--;

    std::erase_if(m_frameWatches, [watch](const auto& w) { return w.get() == watch; });
    armTimer();
}

void CShareRedirector::placeholderFrame(wl_client* client, uint32_t version, uint32_t frameID, uint64_t captureID) {
    auto* const CAPTURE = captureByID(captureID);
    if (!CAPTURE || !CAPTURE->haveConstraints) {
        failFrame(client, version, frameID);
        return;
    }

    auto resource = makeShared<CHyprlandToplevelExportFrameV1>(client, version, frameID);
    if (!resource->resource()) {
        wl_client_post_no_memory(client);
        return;
    }

    auto& ph      = m_placeholders.emplace_back(makeUnique<SPlaceholderFrame>());
    ph->resource  = resource;
    ph->captureID = captureID;
    ph->size      = CAPTURE->size;
    ph->format    = CAPTURE->format;
    ph->monitor   = placeholderMonitor(*CAPTURE);
    ph->owner     = this;

    auto* const RAW = ph.get();
    resource->setDestroy([this](CHyprlandToplevelExportFrameV1* r) { dropPlaceholder(r); });
    resource->setOnDestroy([this](CHyprlandToplevelExportFrameV1* r) { dropPlaceholder(r); });
    resource->setCopy([this, RAW](CHyprlandToplevelExportFrameV1* r, wl_resource* buffer, int32_t ignoreDamage) {
        if (RAW->copyAsked || RAW->answered)
            return;
        RAW->copyAsked = true;

        const auto PBUFFERRES = CWLBufferResource::fromResource(buffer);
        if (!PBUFFERRES || !PBUFFERRES->m_buffer) {
            r->sendFailed();
            RAW->answered = true;
            return;
        }

        RAW->copyBuffer = PBUFFERRES->m_buffer.lock();
        RAW->timestamp  = Time::steadyNow();

        // Throttle: render+ready ~100ms later (≈10 fps), from the event loop — not synchronously in
        // the client's request.
        RAW->copyTimer = makeShared<CEventLoopTimer>(PLACEHOLDER_THROTTLE, [this, RAW](SP<CEventLoopTimer> self, void*) { doPlaceholderCopy(RAW); }, nullptr);
        g_pEventLoopManager->addTimer(RAW->copyTimer);
    });

    CAPTURE->outstandingFrames++;

    const auto PSHMINFO = getPixelFormatFromDRM(ph->format);
    if (!PSHMINFO) {
        resource->sendFailed();
        RAW->answered = true;
        return;
    }

    const auto STRIDE = minStride(PSHMINFO, ph->size.x);
    resource->sendBuffer(NFormatUtils::drmToShm(ph->format), ph->size.x, ph->size.y, STRIDE);
    if (ph->format != DRM_FORMAT_INVALID)
        resource->sendLinuxDmabuf(ph->format, ph->size.x, ph->size.y);
    resource->sendBufferDone();
}

void CShareRedirector::failFrame(wl_client* client, uint32_t version, uint32_t frameID) {
    // Create the resource for the new_id (so libwayland never leaves it NULL) and immediately fail it.
    auto resource = makeShared<CHyprlandToplevelExportFrameV1>(client, version, frameID);
    if (!resource->resource()) {
        wl_client_post_no_memory(client);
        return;
    }

    auto& ph      = m_placeholders.emplace_back(makeUnique<SPlaceholderFrame>());
    ph->resource  = resource;
    ph->captureID = 0;
    ph->answered  = true;
    ph->owner     = this;

    resource->setDestroy([this](CHyprlandToplevelExportFrameV1* r) { dropPlaceholder(r); });
    resource->setOnDestroy([this](CHyprlandToplevelExportFrameV1* r) { dropPlaceholder(r); });
    resource->setCopy([](CHyprlandToplevelExportFrameV1* r, wl_resource*, int32_t) { r->sendFailed(); });

    resource->sendFailed();
}

void CShareRedirector::doPlaceholderCopy(SPlaceholderFrame* frame) {
    if (frame->copyTimer) {
        frame->copyTimer->cancel();
        g_pEventLoopManager->removeTimer(frame->copyTimer);
        frame->copyTimer.reset();
    }

    if (frame->answered || !frame->resource)
        return;

    const auto BUFFER  = std::move(frame->copyBuffer);
    auto*      CAPTURE = captureByID(frame->captureID);
    const auto MONITOR = CAPTURE ? placeholderMonitor(*CAPTURE) : frame->monitor.lock(); // fresh: the recorded one may have gone
    frame->answered    = true;

    if (!BUFFER || !MONITOR || !renderPlaceholder(BUFFER, frame->size, MONITOR)) {
        frame->resource->sendFailed();
        return;
    }

    frame->resource->sendFlags(sc<hyprlandToplevelExportFrameV1Flags>(0));
    frame->resource->sendDamage(0, 0, frame->size.x, frame->size.y);
    const auto [sec, nsec] = Time::secNsec(frame->timestamp);
    frame->resource->sendReady((sizeof(sec) > 4) ? sec >> 32 : 0, sec & 0xFFFFFFFF, nsec);
}

// Render a solid black frame into the client's buffer. Mirrors CScreenshareFrame::copyShm /
// copyDmabuf, drawing only a black clear. beginRender/beginFullFakeRender make the EGL context
// current themselves (initRender), so this is safe to call from the throttle timer.
bool CShareRedirector::renderPlaceholder(const SP<IHLBuffer>& buffer, const Vector2D& size, PHLMONITOR monitor) {
    if (!g_pHyprRenderer)
        return false;

    CRegion damage = {0, 0, size.x, size.y};

    if (buffer->shm().success) {
        auto outFB = g_pHyprRenderer->createFB();
        if (!outFB->alloc(size.x, size.y, buffer->shm().format))
            return false;
        outFB->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);

        if (!g_pHyprRenderer->beginFullFakeRender(monitor, damage, outFB))
            return false;
        g_pHyprRenderer->draw(CClearPassElement::SClearData{Colors::BLACK}, damage);
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();

        bool ok = true;
        damage.forEachRect([&](const auto& rect) {
            if (ok && !outFB->readPixels(CHLBufferReference(buffer), rect.x1, rect.y1, rect.x2 - rect.x1, rect.y2 - rect.y1))
                ok = false;
        });
        g_pHyprRenderer->m_renderData.pMonitor.reset();
        return ok;
    }

    if (buffer->dmabuf().success) {
        if (!g_pHyprRenderer->beginRenderToBuffer(monitor, damage, buffer, true))
            return false;
        g_pHyprRenderer->m_renderData.currentFB->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{Colors::BLACK}, damage);
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender(); // a solid clear has nothing async to wait on
        return true;
    }

    return false;
}

void CShareRedirector::finishPlaceholders(uint64_t captureID) {
    // A pending placeholder whose capture just became servable again is answered now (failed, which
    // patched XDPH requeues at once), so the next request reaches the new target and renegotiates.
    for (auto& ph : m_placeholders) {
        if (ph->captureID != captureID || ph->answered)
            continue;

        if (ph->copyTimer) {
            ph->copyTimer->cancel();
            g_pEventLoopManager->removeTimer(ph->copyTimer);
            ph->copyTimer.reset();
        }
        if (ph->resource)
            ph->resource->sendFailed();
        ph->answered = true;
    }
}

void CShareRedirector::dropPlaceholder(CHyprlandToplevelExportFrameV1* resource) {
    for (auto& ph : m_placeholders) {
        if (ph->resource.get() != resource)
            continue;
        if (ph->copyTimer) {
            ph->copyTimer->cancel();
            g_pEventLoopManager->removeTimer(ph->copyTimer);
            ph->copyTimer.reset();
        }
        if (auto* const CAPTURE = captureByID(ph->captureID); CAPTURE && CAPTURE->outstandingFrames > 0)
            CAPTURE->outstandingFrames--;
    }
    std::erase_if(m_placeholders, [resource](const auto& p) { return p->resource.get() == resource; });
    armTimer();
}

void CShareRedirector::armTimer() {
    if (m_timer && !m_timer->armed())
        m_timer->updateTimeout(TICK_INTERVAL);
}

void CShareRedirector::onTick() {
    prune(Time::steadyNow());
    updateTags();

    if (!m_captures.empty() || !m_tagged.empty() || !m_placeholders.empty())
        armTimer();
}

void CShareRedirector::prune(Time::steady_tp now) {
    std::erase_if(m_captures, [&](const auto& c) { return c.outstandingFrames == 0 && now - c.lastSeen > FORGET_TIMEOUT && placeholderCount(c.id) == 0; });

    // The target closed. We do NOT fall back to the picked windows (privacy): FOLLOW_TARGET captures
    // enter NOTHING on their next request. Just forget the dangling target ref.
    if (!m_target.expired() && !validTarget())
        m_target.reset();
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

void CShareRedirector::onShareChanged() {
    for (auto& c : m_captures) {
        c.served = serveWindow(c);
        finishPlaceholders(c.id); // a capture now servable: fail its pending placeholder so XDPH requeues
    }
    updateTags();
    armTimer();
}

SDispatchResult CShareRedirector::shareWindow(const std::string& selector) {
    const auto TARGET = selector.empty() ? Desktop::focusState()->window() : Desktop::viewState()->query().selector(selector).runWindow();
    if (!TARGET || !TARGET->m_isMapped)
        return {.success = false, .error = selector.empty() ? "sharewindow: no focused window" : std::format("sharewindow: no window matches \"{}\"", selector)};

    const auto NOW = Time::steadyNow();

    // Every active capture follows the new target — including ones currently in NOTHING (that is the
    // whole point: after closing the shared window, this is how you resume). Shares that go idle stay
    // untouched until their next request, then follow too (mode is sticky).
    size_t affected = 0;
    for (auto& c : m_captures) {
        if (!isActive(c, NOW))
            continue;

        c.mode    = SCapture::FOLLOW_TARGET;
        c.nothing = false;
        affected++;
    }

    if (affected == 0)
        return {.success = false, .error = "sharewindow: no active window share to redirect"};

    m_target = TARGET;
    onShareChanged();

    Log::logger->log(Log::DEBUG, "[hyprshare] serving {} ({}) to {} share(s)", addressString(addressOf(TARGET)), TARGET->m_class, affected);
    return {};
}

SDispatchResult CShareRedirector::clear() {
    // Back to each portal's own pick (the user's mental model of "clear"). A capture whose pick is
    // gone simply re-enters NOTHING on its next request.
    for (auto& c : m_captures) {
        c.mode    = SCapture::FOLLOW_ORIGIN;
        c.nothing = false;
    }
    m_target.reset();
    onShareChanged();

    Log::logger->log(Log::DEBUG, "[hyprshare] redirects cleared");
    return {};
}

std::string CShareRedirector::statusJSON() const {
    const auto  NOW = Time::steadyNow();

    std::string captures;
    for (const auto& c : m_captures) {
        if (!captures.empty())
            captures += ",\n";

        const char* STATE = c.nothing ? "nothing" : (c.mode == SCapture::FOLLOW_TARGET ? "target" : "origin");
        const auto  AGO   = std::chrono::duration_cast<std::chrono::milliseconds>(NOW - c.lastSeen).count();
        captures +=
            std::format(R"(    {{"pid": {}, "active": {}, "state": "{}", "lastSeenMsAgo": {}, "outstandingFrames": {}, "placeholderFrames": {}, "origin": {}, "served": {}}})",
                        c.pid, isActive(c, NOW), STATE, AGO, c.outstandingFrames, placeholderCount(c.id), windowJSON(c.origin, c.originAddr), windowJSON(c.served, 0));
    }

    const auto TARGET = m_target.lock();
    return std::format("{{\n  \"target\": {},\n  \"captures\": [\n{}\n  ]\n}}\n", TARGET ? windowJSON(m_target, 0) : "null", captures);
}

UP<CShareRedirector>& HyprShare::redirector() {
    static UP<CShareRedirector> instance;
    return instance;
}
