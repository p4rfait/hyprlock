#include "LockSurface.hpp"
#include "hyprlock.hpp"
#include "../helpers/Log.hpp"
#include "Egl.hpp"
#include "../renderer/Renderer.hpp"

static void handleConfigure(void* data, ext_session_lock_surface_v1* surf, uint32_t serial, uint32_t width, uint32_t height) {
    const auto PSURF = (CSessionLockSurface*)data;
    PSURF->configure({width, height}, serial);
}

static const ext_session_lock_surface_v1_listener lockListener = {
    .configure = handleConfigure,
};

static void handlePreferredScale(void* data, wp_fractional_scale_v1* wp_fractional_scale_v1, uint32_t scale) {
    const auto PSURF       = (CSessionLockSurface*)data;
    PSURF->fractionalScale = scale / 120.0;
    Debug::log(LOG, "got fractional {}", PSURF->fractionalScale);
}

static const wp_fractional_scale_v1_listener fsListener = {
    .preferred_scale = handlePreferredScale,
};

CSessionLockSurface::~CSessionLockSurface() {
    if (fractional) {
        wp_viewport_destroy(viewport);
        wp_fractional_scale_v1_destroy(fractional);
    }
    wl_egl_window_destroy(eglWindow);
    ext_session_lock_surface_v1_destroy(lockSurface);
    wl_surface_destroy(surface);
}

CSessionLockSurface::CSessionLockSurface(COutput* output) : output(output) {
    surface = wl_compositor_create_surface(g_pHyprlock->getCompositor());

    if (!surface) {
        Debug::log(CRIT, "Couldn't create wl_surface");
        exit(1);
    }

    fractional = wp_fractional_scale_manager_v1_get_fractional_scale(g_pHyprlock->getFractionalMgr(), surface);
    if (fractional) {
        wp_fractional_scale_v1_add_listener(fractional, &fsListener, this);
        viewport = wp_viewporter_get_viewport(g_pHyprlock->getViewporter(), surface);
        wl_display_roundtrip(g_pHyprlock->getDisplay());
    } else {
        Debug::log(LOG, "No fractional-scale support! Oops, won't be able to scale!");
    }

    configure(output->size, 0);
    g_pRenderer->renderLock(*this);

    lockSurface = ext_session_lock_v1_get_lock_surface(g_pHyprlock->getSessionLock(), surface, output->output);

    if (!surface) {
        Debug::log(CRIT, "Couldn't create ext_session_lock_surface_v1");
        exit(1);
    }

    ext_session_lock_surface_v1_add_listener(lockSurface, &lockListener, this);
    wl_display_roundtrip(g_pHyprlock->getDisplay());
    wl_display_flush(g_pHyprlock->getDisplay());
}

void CSessionLockSurface::configure(const Vector2D& size_, uint32_t serial_) {
    Debug::log(LOG, "configure with serial {}", serial);

    serial      = serial_;
    size        = (size_ * fractionalScale).floor();
    logicalSize = size_;
    if (serial != 0)
        ext_session_lock_surface_v1_ack_configure(lockSurface, serial);

    if (fractional)
        wp_viewport_set_destination(viewport, logicalSize.x, logicalSize.y);

    wl_surface_set_buffer_scale(surface, 1);
    wl_surface_damage_buffer(surface, 0, 0, 0xFFFF, 0xFFFF);

    if (!eglWindow)
        eglWindow = wl_egl_window_create(surface, size.x, size.y);
    else
        wl_egl_window_resize(eglWindow, size.x, size.y, 0, 0);

    if (!eglWindow) {
        Debug::log(CRIT, "Couldn't create eglWindow");
        exit(1);
    }

    if (serial == 0)
        eglSurface = g_pEGL->eglCreatePlatformWindowSurfaceEXT(g_pEGL->eglDisplay, g_pEGL->eglConfig, eglWindow, nullptr);

    if (!eglSurface) {
        Debug::log(CRIT, "Couldn't create eglSurface");
        exit(1);
    }

    readyForFrame = true;

    if (serial != 0)
        render();

    if (fractional)
        wp_viewport_set_destination(viewport, logicalSize.x, logicalSize.y);

    wl_surface_set_buffer_scale(surface, 1);
    wl_surface_damage_buffer(surface, 0, 0, 0xFFFF, 0xFFFF);

    wl_surface_commit(surface);
    wl_display_roundtrip(g_pHyprlock->getDisplay());
    wl_display_flush(g_pHyprlock->getDisplay());
}

static void handleDone(void* data, wl_callback* wl_callback, uint32_t callback_data) {
    const auto PSURF = (CSessionLockSurface*)data;
    PSURF->onCallback();
}

static const wl_callback_listener callbackListener = {
    .done = handleDone,
};

void CSessionLockSurface::render() {
    Debug::log(TRACE, "render lock");

    const auto FEEDBACK = g_pRenderer->renderLock(*this);
    frameCallback       = wl_surface_frame(surface);
    wl_callback_add_listener(frameCallback, &callbackListener, this);
    eglSwapBuffers(g_pEGL->eglDisplay, eglSurface);

    if (fractional)
        wp_viewport_set_destination(viewport, logicalSize.x, logicalSize.y);

    wl_surface_damage_buffer(surface, 0, 0, 0xFFFF, 0xFFFF);
    wl_surface_set_buffer_scale(surface, 1);
    wl_surface_commit(surface);

    needsFrame = FEEDBACK.needsFrame;
}

void CSessionLockSurface::onCallback() {
    readyForFrame = true;
    frameCallback = nullptr;

    if (needsFrame && !g_pHyprlock->m_bTerminate && g_pEGL)
        render();
}