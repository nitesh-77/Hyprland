#include "ScreenshareManager.hpp"
#include "../../pointer/PointerManager.hpp"
#include "../input/InputManager.hpp"
#include "../permissions/DynamicPermissionManager.hpp"
#include "../../protocols/ColorManagement.hpp"
#include "../../protocols/XDGShell.hpp"
#include "../../Compositor.hpp"
#include "../../render/Renderer.hpp"
#include "../../render/OpenGL.hpp"
#include "../../output/Monitor.hpp"
#include "../../state/MonitorState.hpp"
#include "../../desktop/view/Window.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../render/pass/ClearPassElement.hpp"
#include "../../render/pass/RectPassElement.hpp"
#include "helpers/cm/ColorManagement.hpp"
#include "../../managers/fullscreen/FullscreenController.hpp"
#include <hyprutils/math/Region.hpp>
#include <hyprgraphics/egl/Egl.hpp>

using namespace Hyprgraphics::Egl;
using namespace Screenshare;
using namespace Desktop::View;

CScreenshareFrame::CScreenshareFrame(WP<CScreenshareSession> session, bool overlayCursor, bool isFirst) :
    m_session(session), m_bufferSize(m_session->bufferSize()), m_overlayCursor(overlayCursor), m_isFirst(isFirst) {
    ;
}

CScreenshareFrame::~CScreenshareFrame() {
    if (m_failed || !m_shared)
        return;

    if (!m_copied && m_callback) {
        FScreenshareCallback cb;
        std::swap(cb, m_callback);
        cb(RESULT_NOT_COPIED);
    }
}

bool CScreenshareFrame::done() const {
    if (m_session.expired() || m_session->m_stopped)
        return true;

    if (m_session->m_type == SHARE_NONE || m_bufferSize == Vector2D(0, 0))
        return true;

    if (m_failed || m_copied)
        return true;

    if (m_session->m_type == SHARE_MONITOR && !m_session->monitor())
        return true;

    if (m_session->m_type == SHARE_REGION && !m_session->monitor())
        return true;

    if (m_session->m_type == SHARE_WINDOW && (!m_session->monitor() || !validMapped(m_session->m_window)))
        return true;

    if (!m_shared)
        return false;

    if (!m_buffer || !m_buffer->m_resource || !m_buffer->m_resource->good())
        return true;

    if (!m_callback)
        return true;

    return false;
}

eScreenshareError CScreenshareFrame::share(SP<IHLBuffer> buffer, const CRegion& clientDamage, FScreenshareCallback callback) {
    if UNLIKELY (done())
        return ERROR_STOPPED;

    if UNLIKELY (!m_session->monitor() || !State::monitorState()->contains(m_session->monitor())) {
        LOGM(Log::ERR, "Client requested sharing of a monitor that is gone");
        m_failed = true;
        return ERROR_STOPPED;
    }

    if UNLIKELY (m_session->m_type == SHARE_WINDOW && !validMapped(m_session->m_window)) {
        LOGM(Log::ERR, "Client requested sharing of window that is gone or not shareable!");
        m_failed = true;
        return ERROR_STOPPED;
    }

    if UNLIKELY (!buffer || !buffer->m_resource || !buffer->m_resource->good()) {
        LOGM(Log::ERR, "Client requested sharing to an invalid buffer");
        return ERROR_NO_BUFFER;
    }

    if UNLIKELY (buffer->size != m_bufferSize) {
        LOGM(Log::ERR, "Client requested sharing to an invalid buffer size");
        return ERROR_BUFFER_SIZE;
    }

    uint32_t bufFormat;
    if (buffer->dmabuf().success)
        bufFormat = buffer->dmabuf().format;
    else if (buffer->shm().success)
        bufFormat = buffer->shm().format;
    else {
        LOGM(Log::ERR, "Client requested sharing to an invalid buffer");
        return ERROR_NO_BUFFER;
    }

    if (std::ranges::count_if(m_session->allowedFormats(), [&](const DRMFormat& format) { return format == bufFormat; }) == 0) {
        LOGM(Log::ERR, "Invalid format {} in {:x}", bufFormat, (uintptr_t)this);
        return ERROR_BUFFER_FORMAT;
    }

    m_buffer   = buffer;
    m_callback = callback;
    m_shared   = true;

    if (m_session->m_type == SHARE_MONITOR || m_session->m_type == SHARE_REGION) {
        const auto PMONITOR = m_session->monitor();
        if (PMONITOR)
            PMONITOR->addDamage(PMONITOR->resources()->pendingMirrorFBDamage());
    }

    // schedule a frame so that when a screenshare starts it isn't black until the output is updated
    if (m_isFirst) {
        const auto PMONITOR = m_session->monitor();
        if (PMONITOR)
            PMONITOR->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_NEEDS_FRAME);
        g_pHyprRenderer->damageMonitor(PMONITOR);
    }

    // TODO: add a damage ring for output damage since last shared frame
    CRegion frameDamage = CRegion(0, 0, m_bufferSize.x, m_bufferSize.y);

    // copy everything on the first frame
    if (m_isFirst)
        m_damage = CRegion(0, 0, m_bufferSize.x, m_bufferSize.y);
    else
        m_damage = frameDamage.add(clientDamage);

    m_damage.intersect(0, 0, m_bufferSize.x, m_bufferSize.y);

    return ERROR_NONE;
}

void CScreenshareFrame::copy() {
    if (done() || m_copyInFlight)
        return;

    // tell client to send presented timestamp
    // TODO: is this right? this is right after we commit to aq, not when page flip happens..
    m_callback(RESULT_TIMESTAMP);

    // store a snapshot before the permission popup so we don't break screenshots
    const auto PERM = g_pDynamicPermissionManager->clientPermissionMode(m_session->m_client, PERMISSION_TYPE_SCREENCOPY);
    if (PERM == PERMISSION_RULE_ALLOW_MODE_PENDING) {
        if (!m_session->m_tempFB || !m_session->m_tempFB->isAllocated())
            storeTempFB();

        // don't copy a frame while allow is pending because screenshot tools will only take the first frame we give, which is empty
        return;
    }

    if (m_buffer->shm().success)
        m_failed = !copyShm();
    else if (m_buffer->dmabuf().success)
        m_failed = !copyDmabuf();

    if (!m_failed) {
        // screensharing has started again
        m_session->screenshareEvents(true);
        m_session->m_shareStopTimer->updateTimeout(std::chrono::milliseconds(500)); // check in half second
    } else
        m_callback(RESULT_NOT_COPIED);
}

void CScreenshareFrame::renderMonitorBlackBox(PHLMONITOR PMONITOR) {
    // Restored, byte-for-byte, from the pre-#4 renderMonitor() body (commit 90beb7ca, before
    // the true-exclusion render body replaced it) - see docs/adr/0001-capture-exclusion-kill-switch.md.
    // Only reached when the HYPRLAND_DISABLE_CAPTURE_EXCLUSION kill-switch is set; must not be
    // "improved" or reconciled with the #4 code above it - it exists specifically to be the
    // old, known-good path, independent of anything #4/#5 introduced.
    auto TEXTURE = g_pHyprRenderer->m_renderData.pMonitor->resources()->getMirrorTexture();
    if (!TEXTURE) {
        LOGM(Log::ERR, "Invalid source texture");
        return;
    }

    if (!TEXTURE->m_imageDescription)
        Log::logger->log(Log::ERR, "CM: FIXME no source image description for screenshare");

    if (!g_pHyprRenderer->m_renderData.currentFB->imageDescription())
        Log::logger->log(Log::ERR, "CM: FIXME no target image description for screenshare");

    if (TEXTURE->m_imageDescription && g_pHyprRenderer->m_renderData.currentFB->imageDescription())
        Log::logger->log(Log::TRACE, "CM: screenshot renderMonitor {} -> {}", TEXTURE->m_imageDescription->value(),
                         g_pHyprRenderer->m_renderData.currentFB->imageDescription()->value());

    const bool IS_CM_AWARE                        = PROTO::colorManagement && PROTO::colorManagement->isClientCMAware(m_session->m_client);
    g_pHyprRenderer->m_renderData.transformDamage = false;
    g_pHyprRenderer->m_renderData.noSimplify      = true;

    // render monitor texture
    CBox       monbox = CBox{{}, PMONITOR->m_pixelSize}
                            .transform(Math::wlTransformToHyprutils(Math::invertTransform(PMONITOR->m_transform)), PMONITOR->m_pixelSize.x, PMONITOR->m_pixelSize.y)
                            .translate(-m_session->m_captureBox.pos()); // vvvv kinda ass-backwards but that's how I designed the renderer... sigh.

    const auto OLD                                    = g_pHyprRenderer->m_renderData.renderModif.enabled;
    g_pHyprRenderer->m_renderData.renderModif.enabled = false;
    g_pHyprRenderer->startRenderPass();
    g_pHyprRenderer->draw(
        CTexPassElement::SRenderData{
            .tex          = TEXTURE,
            .box          = monbox,
            .flipEndFrame = true,
            .cmBackToSRGB = !IS_CM_AWARE,
        },
        {0, 0, PMONITOR->m_pixelSize.x, PMONITOR->m_pixelSize.y});
    g_pHyprRenderer->m_renderData.renderModif.enabled = OLD;

    // render black boxes for noscreenshare
    auto hidePopups = [&](Vector2D popupBaseOffset) {
        return [&, popupBaseOffset](WP<Desktop::View::CPopup> popup, void*) {
            if (!popup->wlSurface() || !popup->wlSurface()->resource() || !popup->visible())
                return;

            const auto popRel = popup->coordsRelativeToParent();
            popup->wlSurface()->resource()->breadthfirst(
                [&](SP<CWLSurfaceResource> surf, const Vector2D& localOff, void*) {
                    const auto size = surf->m_current.size;
                    const auto surfBox =
                        CBox{popupBaseOffset + popRel + localOff, size}.translate(PMONITOR->m_position).scale(PMONITOR->m_scale).translate(-m_session->m_captureBox.pos());

                    if LIKELY (surfBox.w > 0 && surfBox.h > 0)
                        g_pHyprRenderer->draw(CRectPassElement::SRectData{.box = surfBox, .color = Colors::BLACK}, surfBox);
                },
                nullptr);
        };
    };

    for (auto const& l : Desktop::layerState()->layers()) {
        if (!l->m_ruleApplicator->noScreenShare().valueOrDefault())
            continue;

        if UNLIKELY (!l->visible())
            continue;

        const auto REALPOS  = l->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto REALSIZE = l->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

        const auto noScreenShareBox = CBox{REALPOS.x, REALPOS.y, std::max(REALSIZE.x, 5.0), std::max(REALSIZE.y, 5.0)}
                                          .translate(-PMONITOR->m_position)
                                          .scale(PMONITOR->m_scale)
                                          .translate(-m_session->m_captureBox.pos());

        g_pHyprRenderer->draw(CRectPassElement::SRectData{.box = noScreenShareBox, .color = Colors::BLACK}, noScreenShareBox);

        const auto     geom            = l->m_geometry;
        const Vector2D popupBaseOffset = REALPOS - Vector2D{geom.pos().x, geom.pos().y};
        if (l->m_popupHead)
            l->m_popupHead->breadthfirst(hidePopups(popupBaseOffset), nullptr);
    }

    for (auto const& w : Desktop::windowState()->windows()) {
        if (!w->m_ruleApplicator->noScreenShare().valueOrDefault())
            continue;

        if (!g_pHyprRenderer->shouldRenderWindow(w, PMONITOR))
            continue;

        if (w->isHidden())
            continue;

        const auto PWORKSPACE = w->m_workspace;

        if UNLIKELY (!PWORKSPACE && w->alphaValue(WINDOW_ALPHA_FADE) * w->alphaValue(WINDOW_ALPHA_FULLSCREEN) != 0.f)
            continue;

        const auto renderOffset     = PWORKSPACE && !w->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D{};
        const auto REALSIZE         = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto REALPOS          = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + renderOffset;
        const auto noScreenShareBox = CBox{REALPOS.x, REALPOS.y, std::max(REALSIZE.x, 5.0), std::max(REALSIZE.y, 5.0)}
                                          .translate(-PMONITOR->m_position)
                                          .scale(PMONITOR->m_scale)
                                          .translate(-m_session->m_captureBox.pos());

        // seems like rounding doesn't play well with how we manipulate the box position to render regions causing the window to leak through
        const auto dontRound     = m_session->m_captureBox.pos() != Vector2D() || Fullscreen::controller()->isFullscreen(w, Fullscreen::FSMODE_FULLSCREEN);
        const auto rounding      = dontRound ? 0 : w->rounding() * PMONITOR->m_scale;
        const auto roundingPower = dontRound ? 2.0f : w->roundingPower();

        g_pHyprRenderer->draw(
            CRectPassElement::SRectData{
                .box           = noScreenShareBox,
                .color         = Colors::BLACK,
                .round         = rounding,
                .roundingPower = roundingPower,
            },
            noScreenShareBox);

        if (w->m_isX11 || !w->m_popupHead)
            continue;

        const auto     geom            = w->m_xdgSurface->m_current.geometry;
        const Vector2D popupBaseOffset = REALPOS - Vector2D{geom.pos().x, geom.pos().y};

        w->m_popupHead->breadthfirst(hidePopups(popupBaseOffset), nullptr);
    }

    if (m_overlayCursor) {
        CRegion  fakeDamage = {0, 0, INT16_MAX, INT16_MAX};
        Vector2D cursorPos  = g_pInputManager->getMouseCoordsInternal() - PMONITOR->m_position - m_session->m_captureBox.pos() / PMONITOR->m_scale;
        Pointer::mgr()->renderSoftwareCursorsFor(PMONITOR, Time::steadyNow(), fakeDamage, cursorPos, true);
    }
}

bool CScreenshareFrame::monitorHasNoScreenShareSurface(PHLMONITOR pMonitor) {
    // Windows: only those that would actually be rendered on this monitor count — a flagged
    // window that wouldn't be drawn anyway makes no difference to the output, so it must not
    // force the expensive path. m_bCaptureExclusionPass is false here, so shouldRenderWindow()
    // evaluates normally (it does not yet exclude flagged windows).
    for (auto const& w : Desktop::windowState()->windows()) {
        if (!w->m_ruleApplicator->noScreenShare().valueOrDefault())
            continue;

        if (!g_pHyprRenderer->shouldRenderWindow(w, pMonitor))
            continue;

        if (w->isHidden())
            continue;

        return true;
    }

    // Layers: check exactly the per-monitor layer sets the capture-exclusion render iterates
    // (via renderAllClientsForWorkspace), so the check matches what would actually be skipped.
    for (auto const& lsl : pMonitor->m_layerSurfaceLayers) {
        for (auto const& lsref : lsl) {
            const auto ls = lsref.lock();
            if (!ls || !ls->m_ruleApplicator->noScreenShare().valueOrDefault())
                continue;

            if (!ls->visible())
                continue;

            return true;
        }
    }

    return false;
}

void CScreenshareFrame::renderMonitor() {
    if ((m_session->m_type != SHARE_MONITOR && m_session->m_type != SHARE_REGION) || done())
        return;

    const auto PMONITOR = m_session->monitor();

    // Startup-only kill-switch (see docs/adr/0001-capture-exclusion-kill-switch.md,
    // HYPRLAND_DISABLE_CAPTURE_EXCLUSION): when set, always take the exact pre-#4 path -
    // the mirror texture + black-box drawing loop this ticket's capture-exclusion render
    // replaced - unconditionally, regardless of m_bCaptureExclusionPass or any window/layer's
    // no_screen_share flag. This check sits upstream of, and is intentionally isolated from,
    // the #4 early-exit/true-exclusion logic below: the switch acts as a hard override at
    // this call site, not a change to that logic. m_bCaptureExclusionDisabled is read via
    // getenv() exactly once, at compositor startup (IHyprRenderer's constructor) - never
    // here, never per-frame - and is restart-to-recover only, not a live/mid-session toggle.
    if (g_pHyprRenderer->m_bCaptureExclusionDisabled) {
        renderMonitorBlackBox(PMONITOR);
        return;
    }

    // Early-exit optimization (see CONTEXT.md, spec-true-capture-exclusion.md): if nothing on
    // this monitor is flagged no_screen_share, the capture-exclusion render would produce
    // exactly the same pixels as the cheap mirror-texture blit, so take the cheap path and skip
    // the second full composite entirely. This keeps the feature's cost at zero when it isn't
    // in use. No black-box loop is needed here either — nothing is flagged.

    // Whether the true-exclusion branch below ran, vs. the cheap mirror-texture branch.
    // Needed by the cursor draw call further down: the mirror-texture path's cursor may
    // already be baked into the texture by the normal per-frame render (which draws the
    // cursor with screencopy=false), so renderSoftwareCursorsFor()'s own screencopy-vs-
    // software-cursor guard correctly no-ops there to avoid double-drawing it. The
    // true-exclusion path has no such pre-existing texture - renderWorkspace() re-composites
    // the scene from scratch and never includes the cursor - so that same guard would
    // silently drop the cursor entirely unless forced. See renderWindow()'s equivalent call
    // below (forceRender=true) for the existing precedent this follows.
    bool tookExclusionPath = false;

    if (!monitorHasNoScreenShareSurface(PMONITOR)) {
        auto TEXTURE = g_pHyprRenderer->m_renderData.pMonitor->resources()->getMirrorTexture();
        if (!TEXTURE) {
            LOGM(Log::ERR, "Invalid source texture");
            return;
        }

        if (!TEXTURE->m_imageDescription)
            Log::logger->log(Log::ERR, "CM: FIXME no source image description for screenshare");

        if (!g_pHyprRenderer->m_renderData.currentFB->imageDescription())
            Log::logger->log(Log::ERR, "CM: FIXME no target image description for screenshare");

        if (TEXTURE->m_imageDescription && g_pHyprRenderer->m_renderData.currentFB->imageDescription())
            Log::logger->log(Log::TRACE, "CM: screenshot renderMonitor {} -> {}", TEXTURE->m_imageDescription->value(),
                             g_pHyprRenderer->m_renderData.currentFB->imageDescription()->value());

        const bool IS_CM_AWARE                        = PROTO::colorManagement && PROTO::colorManagement->isClientCMAware(m_session->m_client);
        g_pHyprRenderer->m_renderData.transformDamage = false;
        g_pHyprRenderer->m_renderData.noSimplify      = true;

        // render monitor texture
        CBox       monbox = CBox{{}, PMONITOR->m_pixelSize}
                                .transform(Math::wlTransformToHyprutils(Math::invertTransform(PMONITOR->m_transform)), PMONITOR->m_pixelSize.x, PMONITOR->m_pixelSize.y)
                                .translate(-m_session->m_captureBox.pos()); // vvvv kinda ass-backwards but that's how I designed the renderer... sigh.

        const auto OLD                                    = g_pHyprRenderer->m_renderData.renderModif.enabled;
        g_pHyprRenderer->m_renderData.renderModif.enabled = false;
        g_pHyprRenderer->startRenderPass();
        g_pHyprRenderer->draw(
            CTexPassElement::SRenderData{
                .tex          = TEXTURE,
                .box          = monbox,
                .flipEndFrame = true,
                .cmBackToSRGB = !IS_CM_AWARE,
            },
            {0, 0, PMONITOR->m_pixelSize.x, PMONITOR->m_pixelSize.y});
        g_pHyprRenderer->m_renderData.renderModif.enabled = OLD;
    } else {
        tookExclusionPath = true;

        // True capture-exclusion render (see CONTEXT.md, spec-true-capture-exclusion.md).
        //
        // Instead of drawing the fully-composited mirror texture (which has every window's real
        // pixels baked in) and then painting opaque black boxes over each no_screen_share surface,
        // we re-composite the monitor's active workspace directly into the capture buffer with
        // m_bCaptureExclusionPass set. shouldRenderWindow()/renderLayer() (see #2) skip any
        // no_screen_share-flagged surface during that composite, so whatever is behind it shows
        // through naturally — the surface is simply absent, not covered.
        //
        // This avoids the black-box failure modes by construction: there is no rectangle to
        // desync from its window during zoom (hyprwm/Hyprland#13150), and nothing is painted over
        // transparency/gaps on special workspaces (hyprwm/Hyprland#11610) — nothing is drawn on
        // top of anything at all.
        //
        // renderWorkspace() here is nested inside renderMonitor(), not a fresh beginRender cycle,
        // so the transformDamage/noSimplify values it sets would leak into the cursor draw below
        // (which runs in the same function) if not restored. renderMonitor()'s surrounding code
        // doesn't restore these today because the next beginRender cycle resets them; that
        // tolerance does not apply to a nested call, so we save the originals first and restore
        // them explicitly right after the call.
        const auto OLD_TRANSFORM_DAMAGE = g_pHyprRenderer->m_renderData.transformDamage;
        const auto OLD_NO_SIMPLIFY      = g_pHyprRenderer->m_renderData.noSimplify;

        g_pHyprRenderer->m_renderData.transformDamage = false;
        g_pHyprRenderer->m_renderData.noSimplify      = true;

        // geometry translate is in pixel space; m_captureBox is already scaled/rounded to pixels
        // (see CScreenshareSession), matching the -m_captureBox.pos() offset the old path used.
        const CBox renderGeometry = CBox{-m_session->m_captureBox.pos().x, -m_session->m_captureBox.pos().y, PMONITOR->m_pixelSize.x, PMONITOR->m_pixelSize.y};

        g_pHyprRenderer->startRenderPass();
        g_pHyprRenderer->m_bCaptureExclusionPass = true;
        g_pHyprRenderer->renderWorkspace(PMONITOR, PMONITOR->m_activeWorkspace, Time::steadyNow(), renderGeometry);
        g_pHyprRenderer->m_bCaptureExclusionPass = false;

        g_pHyprRenderer->m_renderData.transformDamage = OLD_TRANSFORM_DAMAGE;
        g_pHyprRenderer->m_renderData.noSimplify      = OLD_NO_SIMPLIFY;
    }

    if (m_overlayCursor) {
        CRegion  fakeDamage = {0, 0, INT16_MAX, INT16_MAX};
        Vector2D cursorPos  = g_pInputManager->getMouseCoordsInternal() - PMONITOR->m_position - m_session->m_captureBox.pos() / PMONITOR->m_scale;
        Pointer::mgr()->renderSoftwareCursorsFor(PMONITOR, Time::steadyNow(), fakeDamage, cursorPos, true, tookExclusionPath);
    }
}

void CScreenshareFrame::renderWindow() {
    if (m_session->m_type != SHARE_WINDOW || done())
        return;

    const auto PWINDOW  = m_session->m_window.lock();
    const auto PMONITOR = m_session->monitor();

    const auto NOW = Time::steadyNow();

    // TODO: implement a monitor independent render mode to buffer that does this in CHyprRenderer::begin() or something like that
    g_pHyprRenderer->m_renderData.fbSize = m_bufferSize;
    g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
    g_pHyprRenderer->m_renderData.transformDamage = false;
    g_pHyprRenderer->setViewport(0, 0, m_bufferSize.x, m_bufferSize.y);

    g_pHyprRenderer->m_bBlockSurfaceFeedback = g_pHyprRenderer->shouldRenderWindow(PWINDOW); // block the feedback to avoid spamming the surface if it's visible
    g_pHyprRenderer->renderWindow(PWINDOW, PMONITOR, NOW, false, Render::RENDER_PASS_ALL, true, true);
    g_pHyprRenderer->m_bBlockSurfaceFeedback = false;

    if (!m_overlayCursor)
        return;

    auto pointerSurfaceResource = g_pSeatManager->m_state.pointerFocus.lock();

    if (!pointerSurfaceResource)
        return;

    auto pointerSurface = Desktop::View::CWLSurface::fromResource(pointerSurfaceResource);
    if (!pointerSurface)
        return;

    auto box = pointerSurface->getSurfaceBoxGlobal();
    if (!box.has_value() || box->intersection(m_session->m_window->getFullWindowBoundingBox()).empty())
        return;

    if (Desktop::focusState()->window() != m_session->m_window)
        return;

    CRegion fakeDamage = {0, 0, INT16_MAX, INT16_MAX};
    Pointer::mgr()->renderSoftwareCursorsFor(PMONITOR->m_self.lock(), NOW, fakeDamage,
                                             g_pInputManager->getMouseCoordsInternal() - PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), true, true);
}

void CScreenshareFrame::render() {
    const auto PERM = g_pDynamicPermissionManager->clientPermissionMode(m_session->m_client, PERMISSION_TYPE_SCREENCOPY);

    CRegion    frameRegion = {0, 0, g_pHyprRenderer->m_renderData.pMonitor->m_pixelSize.x, g_pHyprRenderer->m_renderData.pMonitor->m_pixelSize.y};

    g_pHyprRenderer->draw(CClearPassElement::SClearData{{0, 0, 0, 0}}, frameRegion);

    if (PERM == PERMISSION_RULE_ALLOW_MODE_PENDING)
        return;

    bool windowShareDenied = m_session->m_type == SHARE_WINDOW && m_session->m_window->m_ruleApplicator && m_session->m_window->m_ruleApplicator->noScreenShare().valueOrDefault();
    g_pHyprRenderer->startRenderPass();
    if (PERM == PERMISSION_RULE_ALLOW_MODE_DENY || windowShareDenied) {
        CBox texbox = CBox{m_bufferSize / 2.F, g_pHyprRenderer->m_screencopyDeniedTexture->m_size}.translate(-g_pHyprRenderer->m_screencopyDeniedTexture->m_size / 2.F);
        g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = g_pHyprRenderer->m_screencopyDeniedTexture, .box = texbox}, texbox);
        return;
    }

    if (m_session->m_tempFB && m_session->m_tempFB->isAllocated()) {
        CBox texbox = {{}, m_bufferSize};
        g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = m_session->m_tempFB->getTexture(), .box = texbox}, texbox);
        m_session->m_tempFB->release();
        return;
    }

    switch (m_session->m_type) {
        case SHARE_REGION: // TODO: could this be better? this is how screencopy works
        case SHARE_MONITOR: renderMonitor(); break;
        case SHARE_WINDOW: renderWindow(); break;
        case SHARE_NONE:
        default: return;
    }
}

bool CScreenshareFrame::copyDmabuf() {
    if (done())
        return false;

    if (!g_pHyprRenderer->beginRender(m_session->monitor(), m_damage, Render::RENDER_MODE_TO_BUFFER, m_buffer, nullptr, true)) {
        LOGM(Log::ERR, "Can't copy: failed to begin rendering to dma frame");
        return false;
    }
    g_pHyprRenderer->m_renderData.currentFB->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);

    render();

    g_pHyprRenderer->m_renderData.blockScreenShader = true;

    m_copyInFlight = true;

    g_pHyprRenderer->endRender([self = m_self]() {
        if (!self || self.expired())
            return;

        self->m_copyInFlight = false;

        if (self->m_copied)
            return;

        LOGM(Log::TRACE, "Copied frame via dma");
        self->m_callback(RESULT_COPIED);
        self->m_copied = true;
    });

    return true;
}

bool CScreenshareFrame::copyShm() {
    if (done())
        return false;

    auto       shm = m_buffer->shm();

    const auto PFORMAT = getPixelFormatFromDRM(shm.format);
    if (!PFORMAT) {
        LOGM(Log::ERR, "Can't copy: failed to find a pixel format");
        return false;
    }

    const auto PMONITOR = m_session->monitor();

    auto       outFB = g_pHyprRenderer->createFB();
    outFB->alloc(m_bufferSize.x, m_bufferSize.y, shm.format);
    outFB->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);

    if (!g_pHyprRenderer->beginFullFakeRender(PMONITOR, m_damage, outFB)) {
        LOGM(Log::ERR, "Can't copy: failed to begin rendering");
        return false;
    }

    render();

    g_pHyprRenderer->m_renderData.blockScreenShader = true;

    g_pHyprRenderer->endRender();

    bool readSucceeded = true;
    m_damage.forEachRect([&](const auto& rect) {
        if (!readSucceeded)
            return;

        int width  = rect.x2 - rect.x1;
        int height = rect.y2 - rect.y1;
        if (!outFB->readPixels(m_buffer, rect.x1, rect.y1, width, height))
            readSucceeded = false;
    });

    g_pHyprRenderer->m_renderData.pMonitor.reset();

    if (!readSucceeded) {
        LOGM(Log::ERR, "Can't copy: failed to read pixels to shm");
        return false;
    }

    if (!m_copied) {
        LOGM(Log::TRACE, "Copied frame via shm");
        m_callback(RESULT_COPIED);
        m_copied = true;
    }

    return true;
}

void CScreenshareFrame::storeTempFB() {
    if (!m_session->m_tempFB)
        m_session->m_tempFB = g_pHyprRenderer->createFB();
    m_session->m_tempFB->alloc(m_bufferSize.x, m_bufferSize.y);
    m_session->m_tempFB->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);

    CRegion fakeDamage = {0, 0, INT16_MAX, INT16_MAX};

    if (!g_pHyprRenderer->beginFullFakeRender(m_session->monitor(), fakeDamage, m_session->m_tempFB)) {
        LOGM(Log::ERR, "Can't copy: failed to begin rendering to temp fb");
        return;
    }

    switch (m_session->m_type) {
        case SHARE_REGION: // TODO: could this be better? this is how screencopy works
        case SHARE_MONITOR: renderMonitor(); break;
        case SHARE_WINDOW: renderWindow(); break;
        case SHARE_NONE:
        default: return;
    }

    g_pHyprRenderer->endRender();
}

Vector2D CScreenshareFrame::bufferSize() const {
    return m_bufferSize;
}

wl_output_transform CScreenshareFrame::transform() const {
    switch (m_session->m_type) {
        case SHARE_REGION:
        case SHARE_MONITOR: return m_session->monitor()->m_transform;
        default:
        case SHARE_WINDOW: return WL_OUTPUT_TRANSFORM_NORMAL;
    }
}

const CRegion& CScreenshareFrame::damage() const {
    return m_damage;
}
