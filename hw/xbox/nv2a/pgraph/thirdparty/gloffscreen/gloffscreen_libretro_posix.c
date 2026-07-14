/*
 *  Offscreen OpenGL abstraction layer -- libretro/EGL+GLX based
 *
 *  POSIX counterpart of gloffscreen_libretro.c: RetroArch owns the
 *  primary GL context; worker threads (PFIFO/PGRAPH) get their own
 *  contexts created in the same share group so texture ids remain
 *  valid across the frontend/emulator boundary.
 *
 *  The frontend context is captured in libretro_gl_prepare(), which the
 *  core calls from context_reset while RetroArch's context is current.
 *  EGL is preferred (Wayland, KMS, X11/EGL frontends); GLX is used when
 *  the current context turns out to be a GLX one (X11 frontends).
 *
 *  Copyright (c) 2026
 *  SPDX-License-Identifier: MIT
 */

#ifdef LIBRETRO

#ifndef _WIN32

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

#include "gloffscreen.h"

#include <epoxy/egl.h>

#if defined(__has_include)
#if __has_include(<epoxy/glx.h>) && __has_include(<X11/Xlib.h>)
#define GLO_HAVE_GLX 1
#endif
#endif

#ifdef GLO_HAVE_GLX
#include <epoxy/glx.h>
#endif

typedef enum GloBackend {
    GLO_BACKEND_NONE = 0,
    GLO_BACKEND_EGL,
    GLO_BACKEND_GLX,
} GloBackend;

struct _GloContext {
    GloBackend backend;

    /* EGL */
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface; /* EGL_NO_SURFACE when surfaceless */

#ifdef GLO_HAVE_GLX
    /* GLX */
    Display *glx_display;
    GLXContext glx_context;
    GLXPbuffer glx_pbuffer;
#endif
};

/* Captured frontend state */
static GloBackend g_backend = GLO_BACKEND_NONE;
static EGLDisplay g_frontend_egl_display = EGL_NO_DISPLAY;
static EGLContext g_frontend_egl_context = EGL_NO_CONTEXT;
#ifdef GLO_HAVE_GLX
static Display *g_frontend_glx_display = NULL;
static GLXContext g_frontend_glx_context = NULL;
#endif

static volatile bool g_libretro_gl_ready = false;
static bool g_standalone_gl_mode = false;
static GloContext *g_standalone_root_ctx = NULL;

/* Manual-reset event the PFIFO thread waits on (mirrors the Win32
 * CreateEvent/SetEvent pair in gloffscreen_libretro.c). */
static pthread_mutex_t g_event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_event_cond = PTHREAD_COND_INITIALIZER;
static bool g_event_set = false;

static void glo_event_wait(int timeout_secs)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_secs;

    pthread_mutex_lock(&g_event_lock);
    while (!g_event_set) {
        if (pthread_cond_timedwait(&g_event_cond, &g_event_lock, &deadline)) {
            break;
        }
    }
    pthread_mutex_unlock(&g_event_lock);
}

void libretro_gl_init_wait_event(void)
{
    /* Statically initialized; nothing to do. */
}

void libretro_gl_prepare(void)
{
    /* Identify the API that owns the current context. A frontend using
     * EGL leaves a current EGL context here; otherwise probe GLX. */
    EGLContext ectx = eglGetCurrentContext();
    if (ectx != EGL_NO_CONTEXT) {
        g_backend = GLO_BACKEND_EGL;
        g_frontend_egl_display = eglGetCurrentDisplay();
        g_frontend_egl_context = ectx;
    } else {
#ifdef GLO_HAVE_GLX
        GLXContext gctx = glXGetCurrentContext();
        if (gctx) {
            g_backend = GLO_BACKEND_GLX;
            g_frontend_glx_display = glXGetCurrentDisplay();
            g_frontend_glx_context = gctx;
        }
#endif
    }

    if (g_backend == GLO_BACKEND_NONE) {
        fprintf(stderr,
                "[glo] libretro_gl_prepare: no current EGL/GLX context\n");
    }

    g_libretro_gl_ready = true;
}

void libretro_gl_wake_pfifo(void)
{
    pthread_mutex_lock(&g_event_lock);
    g_event_set = true;
    pthread_cond_broadcast(&g_event_cond);
    pthread_mutex_unlock(&g_event_lock);
}

void libretro_gl_wait_for_contexts(void)
{
    glo_event_wait(30);
}

void libretro_gl_signal_ready(void)
{
    libretro_gl_prepare();
    libretro_gl_wake_pfifo();
}

void libretro_gl_set_standalone_mode(void)
{
    g_standalone_gl_mode = true;
    g_libretro_gl_ready = true;
}

void libretro_gl_wait_ready(void)
{
    if (g_libretro_gl_ready) {
        return;
    }
    glo_event_wait(30);
}

bool libretro_gl_is_ready(void)
{
    return g_libretro_gl_ready;
}

/* ------------------------------------------------------------------ */
/* EGL                                                                */
/* ------------------------------------------------------------------ */

static bool egl_display_has_extension(EGLDisplay dpy, const char *ext)
{
    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    return exts && strstr(exts, ext);
}

/* Prefer the exact config the frontend context was created with;
 * fall back to a generic pbuffer-capable RGBA8/D24S8 config. */
static EGLConfig egl_pick_config(EGLDisplay dpy, EGLContext share)
{
    EGLint config_id = 0;
    if (share != EGL_NO_CONTEXT &&
        eglQueryContext(dpy, share, EGL_CONFIG_ID, &config_id) &&
        config_id > 0) {
        const EGLint attribs[] = { EGL_CONFIG_ID, config_id, EGL_NONE };
        EGLConfig config;
        EGLint num = 0;
        if (eglChooseConfig(dpy, attribs, &config, 1, &num) && num == 1) {
            return config;
        }
    }

    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num = 0;
    if (eglChooseConfig(dpy, attribs, &config, 1, &num) && num == 1) {
        return config;
    }
    return NULL;
}

static bool egl_context_init(GloContext *context, EGLDisplay dpy,
                             EGLContext share)
{
    eglBindAPI(EGL_OPENGL_API);

    EGLConfig config = egl_pick_config(dpy, share);
    if (config == NULL) {
        fprintf(stderr, "[glo] no usable EGLConfig\n");
        return false;
    }

    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, config, share, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[glo] eglCreateContext failed: 0x%x\n",
                eglGetError());
        return false;
    }

    EGLSurface surface = EGL_NO_SURFACE;
    if (!egl_display_has_extension(dpy, "EGL_KHR_surfaceless_context")) {
        const EGLint pbuffer_attribs[] = {
            EGL_WIDTH, 1,
            EGL_HEIGHT, 1,
            EGL_NONE
        };
        surface = eglCreatePbufferSurface(dpy, config, pbuffer_attribs);
        if (surface == EGL_NO_SURFACE) {
            fprintf(stderr, "[glo] eglCreatePbufferSurface failed: 0x%x\n",
                    eglGetError());
            eglDestroyContext(dpy, ctx);
            return false;
        }
    }

    context->backend = GLO_BACKEND_EGL;
    context->egl_display = dpy;
    context->egl_context = ctx;
    context->egl_surface = surface;
    return true;
}

/* ------------------------------------------------------------------ */
/* GLX                                                                */
/* ------------------------------------------------------------------ */

#ifdef GLO_HAVE_GLX
/* Prefer the frontend context's FBConfig; fall back to a generic
 * pbuffer-capable RGBA8/D24S8 config. */
static GLXFBConfig glx_pick_fbconfig(Display *dpy, GLXContext share,
                                     bool *found)
{
    *found = false;

    int config_id = 0;
    if (share &&
        glXQueryContext(dpy, share, GLX_FBCONFIG_ID, &config_id) == Success &&
        config_id > 0) {
        const int id_attribs[] = { GLX_FBCONFIG_ID, config_id, None };
        int num = 0;
        GLXFBConfig *configs =
            glXChooseFBConfig(dpy, DefaultScreen(dpy), id_attribs, &num);
        if (configs && num > 0) {
            GLXFBConfig config = configs[0];
            XFree(configs);
            *found = true;
            return config;
        }
        if (configs) {
            XFree(configs);
        }
    }

    const int attribs[] = {
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        GLX_STENCIL_SIZE, 8,
        None
    };
    int num = 0;
    GLXFBConfig *configs =
        glXChooseFBConfig(dpy, DefaultScreen(dpy), attribs, &num);
    if (configs && num > 0) {
        GLXFBConfig config = configs[0];
        XFree(configs);
        *found = true;
        return config;
    }
    if (configs) {
        XFree(configs);
    }
    return (GLXFBConfig)0;
}

static bool glx_context_init(GloContext *context, Display *dpy,
                             GLXContext share)
{
    bool found = false;
    GLXFBConfig config = glx_pick_fbconfig(dpy, share, &found);
    if (!found) {
        fprintf(stderr, "[glo] no usable GLXFBConfig\n");
        return false;
    }

    const int ctx_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
        GLX_CONTEXT_MINOR_VERSION_ARB, 0,
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        None
    };
    GLXContext ctx =
        glXCreateContextAttribsARB(dpy, config, share, True, ctx_attribs);
    if (!ctx) {
        fprintf(stderr, "[glo] glXCreateContextAttribsARB failed\n");
        return false;
    }

    const int pbuffer_attribs[] = {
        GLX_PBUFFER_WIDTH, 1,
        GLX_PBUFFER_HEIGHT, 1,
        None
    };
    GLXPbuffer pbuffer = glXCreatePbuffer(dpy, config, pbuffer_attribs);
    if (!pbuffer) {
        fprintf(stderr, "[glo] glXCreatePbuffer failed\n");
        glXDestroyContext(dpy, ctx);
        return false;
    }

    context->backend = GLO_BACKEND_GLX;
    context->glx_display = dpy;
    context->glx_context = ctx;
    context->glx_pbuffer = pbuffer;
    return true;
}
#endif /* GLO_HAVE_GLX */

/* ------------------------------------------------------------------ */
/* glo API                                                            */
/* ------------------------------------------------------------------ */

GloContext *glo_context_create(void)
{
    GloContext *context = (GloContext *)calloc(1, sizeof(GloContext));
    assert(context != NULL);

    libretro_gl_wait_ready();

    if (g_standalone_gl_mode) {
        /* Self-contained contexts, shared only with each other. */
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
            fprintf(stderr, "[glo] standalone: no EGL display\n");
            free(context);
            return NULL;
        }
        EGLContext share = g_standalone_root_ctx
                               ? g_standalone_root_ctx->egl_context
                               : EGL_NO_CONTEXT;
        if (!egl_context_init(context, dpy, share)) {
            free(context);
            return NULL;
        }
        if (!g_standalone_root_ctx) {
            g_standalone_root_ctx = context;
        }
        return context;
    }

    switch (g_backend) {
    case GLO_BACKEND_EGL:
        if (!egl_context_init(context, g_frontend_egl_display,
                              g_frontend_egl_context)) {
            free(context);
            return NULL;
        }
        return context;
#ifdef GLO_HAVE_GLX
    case GLO_BACKEND_GLX:
        if (!glx_context_init(context, g_frontend_glx_display,
                              g_frontend_glx_context)) {
            free(context);
            return NULL;
        }
        return context;
#endif
    default:
        /* No frontend context captured (mirrors the WGL implementation,
         * which returns an inert context in this case). */
        return context;
    }
}

void glo_set_current(GloContext *context)
{
    if (context == NULL || context->backend == GLO_BACKEND_NONE) {
        switch (g_backend) {
#ifdef GLO_HAVE_GLX
        case GLO_BACKEND_GLX:
            if (g_frontend_glx_display) {
                glXMakeContextCurrent(g_frontend_glx_display, None, None,
                                      NULL);
            }
            break;
#endif
        default:
            if (g_frontend_egl_display != EGL_NO_DISPLAY ||
                g_standalone_root_ctx) {
                EGLDisplay dpy = g_frontend_egl_display != EGL_NO_DISPLAY
                                     ? g_frontend_egl_display
                                     : g_standalone_root_ctx->egl_display;
                eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               EGL_NO_CONTEXT);
            }
            break;
        }
        return;
    }

    if (context->backend == GLO_BACKEND_EGL) {
        eglBindAPI(EGL_OPENGL_API);
        eglMakeCurrent(context->egl_display, context->egl_surface,
                       context->egl_surface, context->egl_context);
#ifdef GLO_HAVE_GLX
    } else if (context->backend == GLO_BACKEND_GLX) {
        glXMakeContextCurrent(context->glx_display, context->glx_pbuffer,
                              context->glx_pbuffer, context->glx_context);
#endif
    }
}

void glo_context_destroy(GloContext *context)
{
    if (!context) {
        return;
    }

    if (context == g_standalone_root_ctx) {
        g_standalone_root_ctx = NULL;
    }

    if (context->backend == GLO_BACKEND_EGL) {
        eglMakeCurrent(context->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (context->egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(context->egl_display, context->egl_surface);
        }
        eglDestroyContext(context->egl_display, context->egl_context);
#ifdef GLO_HAVE_GLX
    } else if (context->backend == GLO_BACKEND_GLX) {
        glXMakeContextCurrent(context->glx_display, None, None, NULL);
        glXDestroyPbuffer(context->glx_display, context->glx_pbuffer);
        glXDestroyContext(context->glx_display, context->glx_context);
#endif
    }

    free(context);
}

#endif /* !_WIN32 */
#endif /* LIBRETRO */
