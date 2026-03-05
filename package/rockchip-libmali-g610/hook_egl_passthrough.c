/*
 * EGL passthrough wrappers for hook-as-libEGL.so
 *
 * When libEGL.so symlinks to the hook (instead of the Mali blob), the hook
 * must export ALL EGL symbols. hook.c only wraps a handful of EGL functions
 * (eglGetProcAddress, eglGetDisplay, eglGetPlatformDisplay, eglDestroySurface,
 * eglChooseConfig, eglCreatePlatform*Surface, eglCreateImageKHR,
 * eglDestroyImageKHR).
 *
 * This file provides thin forwarders for the remaining standard EGL functions
 * that the blob exports but hook.c does not wrap.
 *
 * All functions dlsym the real implementation from the Mali blob (libmali.so.1)
 * and forward the call.
 */

#ifdef HAS_EGL

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

/* EGL types — avoid pulling in full EGL headers to prevent conflicts with
 * hook.c's own EGL definitions. Use the same typedefs as EGL/egl.h. */
#include <EGL/egl.h>
#include <EGL/eglext.h>

/* Get the blob handle — same approach as hook.c and hook_gbm_passthrough.c */
static void *
egl_get_blob(void)
{
    static void *handle = NULL;
    if (!handle) {
        handle = dlopen(LIBMALI_SO, RTLD_LAZY | RTLD_NOLOAD);
        if (!handle)
            handle = dlopen(LIBMALI_SO, RTLD_LAZY);
        if (!handle)
            fprintf(stderr, "[MALI-HOOK] egl_passthrough: dlopen(%s) failed: %s\n",
                    LIBMALI_SO, dlerror());
    }
    return handle;
}

#define EGL_PASSTHROUGH(ret, name, params, args) \
    ret name params { \
        static ret (*real) params = NULL; \
        if (!real) { \
            void *h = egl_get_blob(); \
            if (h) real = dlsym(h, #name); \
        } \
        if (!real) { \
            fprintf(stderr, "[MALI-HOOK] %s: not found in blob\n", #name); \
            return (ret)0; \
        } \
        return real args; \
    }

#define EGL_PASSTHROUGH_VOID(name, params, args) \
    void name params { \
        static void (*real) params = NULL; \
        if (!real) { \
            void *h = egl_get_blob(); \
            if (h) real = dlsym(h, #name); \
        } \
        if (!real) { \
            fprintf(stderr, "[MALI-HOOK] %s: not found in blob\n", #name); \
            return; \
        } \
        real args; \
    }

/* ========================================================================
 * EGL 1.0 Core
 * ======================================================================== */

EGL_PASSTHROUGH(EGLBoolean, eglBindAPI, (EGLenum api), (api))

EGL_PASSTHROUGH(EGLBoolean, eglBindTexImage,
                (EGLDisplay dpy, EGLSurface surface, EGLint buffer),
                (dpy, surface, buffer))

EGL_PASSTHROUGH(EGLBoolean, eglCopyBuffers,
                (EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target),
                (dpy, surface, target))

EGL_PASSTHROUGH(EGLContext, eglCreateContext,
                (EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                 const EGLint *attrib_list),
                (dpy, config, share_context, attrib_list))

EGL_PASSTHROUGH(EGLSurface, eglCreatePbufferFromClientBuffer,
                (EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer,
                 EGLConfig config, const EGLint *attrib_list),
                (dpy, buftype, buffer, config, attrib_list))

EGL_PASSTHROUGH(EGLSurface, eglCreatePbufferSurface,
                (EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list),
                (dpy, config, attrib_list))

EGL_PASSTHROUGH(EGLSurface, eglCreatePixmapSurface,
                (EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap,
                 const EGLint *attrib_list),
                (dpy, config, pixmap, attrib_list))

EGL_PASSTHROUGH(EGLSurface, eglCreateWindowSurface,
                (EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                 const EGLint *attrib_list),
                (dpy, config, win, attrib_list))

EGL_PASSTHROUGH(EGLBoolean, eglDestroyContext,
                (EGLDisplay dpy, EGLContext ctx),
                (dpy, ctx))

EGL_PASSTHROUGH(EGLBoolean, eglGetConfigAttrib,
                (EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value),
                (dpy, config, attribute, value))

EGL_PASSTHROUGH(EGLBoolean, eglGetConfigs,
                (EGLDisplay dpy, EGLConfig *configs, EGLint config_size,
                 EGLint *num_config),
                (dpy, configs, config_size, num_config))

EGL_PASSTHROUGH(EGLContext, eglGetCurrentContext, (void), ())

EGL_PASSTHROUGH(EGLDisplay, eglGetCurrentDisplay, (void), ())

EGL_PASSTHROUGH(EGLSurface, eglGetCurrentSurface, (EGLint readdraw), (readdraw))

EGL_PASSTHROUGH(EGLint, eglGetError, (void), ())

EGL_PASSTHROUGH(EGLBoolean, eglInitialize,
                (EGLDisplay dpy, EGLint *major, EGLint *minor),
                (dpy, major, minor))

EGL_PASSTHROUGH(EGLBoolean, eglMakeCurrent,
                (EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx),
                (dpy, draw, read, ctx))

EGL_PASSTHROUGH(EGLenum, eglQueryAPI, (void), ())

EGL_PASSTHROUGH(EGLBoolean, eglQueryContext,
                (EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value),
                (dpy, ctx, attribute, value))

EGL_PASSTHROUGH(const char *, eglQueryString,
                (EGLDisplay dpy, EGLint name),
                (dpy, name))

EGL_PASSTHROUGH(EGLBoolean, eglQuerySurface,
                (EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value),
                (dpy, surface, attribute, value))

EGL_PASSTHROUGH(EGLBoolean, eglReleaseTexImage,
                (EGLDisplay dpy, EGLSurface surface, EGLint buffer),
                (dpy, surface, buffer))

EGL_PASSTHROUGH(EGLBoolean, eglReleaseThread, (void), ())

EGL_PASSTHROUGH(EGLBoolean, eglSurfaceAttrib,
                (EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value),
                (dpy, surface, attribute, value))

EGL_PASSTHROUGH(EGLBoolean, eglSwapBuffers,
                (EGLDisplay dpy, EGLSurface surface),
                (dpy, surface))

EGL_PASSTHROUGH(EGLBoolean, eglSwapInterval,
                (EGLDisplay dpy, EGLint interval),
                (dpy, interval))

EGL_PASSTHROUGH(EGLBoolean, eglTerminate, (EGLDisplay dpy), (dpy))

EGL_PASSTHROUGH(EGLBoolean, eglWaitClient, (void), ())

EGL_PASSTHROUGH(EGLBoolean, eglWaitGL, (void), ())

EGL_PASSTHROUGH(EGLBoolean, eglWaitNative, (EGLint engine), (engine))

/* ========================================================================
 * EGL 1.5 / KHR Extensions
 * ======================================================================== */

EGL_PASSTHROUGH(EGLImage, eglCreateImage,
                (EGLDisplay dpy, EGLContext ctx, EGLenum target,
                 EGLClientBuffer buffer, const EGLAttrib *attrib_list),
                (dpy, ctx, target, buffer, attrib_list))

EGL_PASSTHROUGH(EGLBoolean, eglDestroyImage,
                (EGLDisplay dpy, EGLImage image),
                (dpy, image))

EGL_PASSTHROUGH(EGLSync, eglCreateSync,
                (EGLDisplay dpy, EGLenum type, const EGLAttrib *attrib_list),
                (dpy, type, attrib_list))

EGL_PASSTHROUGH(EGLBoolean, eglDestroySync,
                (EGLDisplay dpy, EGLSync sync),
                (dpy, sync))

EGL_PASSTHROUGH(EGLint, eglClientWaitSync,
                (EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout),
                (dpy, sync, flags, timeout))

EGL_PASSTHROUGH(EGLBoolean, eglGetSyncAttrib,
                (EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib *value),
                (dpy, sync, attribute, value))

EGL_PASSTHROUGH(EGLBoolean, eglWaitSync,
                (EGLDisplay dpy, EGLSync sync, EGLint flags),
                (dpy, sync, flags))

/* ========================================================================
 * KHR Extensions (legacy integer-based signatures)
 * ======================================================================== */

EGL_PASSTHROUGH(EGLSyncKHR, eglCreateSyncKHR,
                (EGLDisplay dpy, EGLenum type, const EGLint *attrib_list),
                (dpy, type, attrib_list))

EGL_PASSTHROUGH(EGLBoolean, eglDestroySyncKHR,
                (EGLDisplay dpy, EGLSyncKHR sync),
                (dpy, sync))

EGL_PASSTHROUGH(EGLint, eglClientWaitSyncKHR,
                (EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout),
                (dpy, sync, flags, timeout))

EGL_PASSTHROUGH(EGLBoolean, eglGetSyncAttribKHR,
                (EGLDisplay dpy, EGLSyncKHR sync, EGLint attribute, EGLint *value),
                (dpy, sync, attribute, value))

EGL_PASSTHROUGH(EGLint, eglWaitSyncKHR,
                (EGLDisplay dpy, EGLSyncKHR sync, EGLint flags),
                (dpy, sync, flags))

/* ========================================================================
 * EXT Extensions
 * ======================================================================== */

EGL_PASSTHROUGH(EGLBoolean, eglQueryDmaBufFormatsEXT,
                (EGLDisplay dpy, EGLint max_formats, EGLint *formats,
                 EGLint *num_formats),
                (dpy, max_formats, formats, num_formats))

EGL_PASSTHROUGH(EGLBoolean, eglQueryDmaBufModifiersEXT,
                (EGLDisplay dpy, EGLint format, EGLint max_modifiers,
                 EGLuint64KHR *modifiers, EGLBoolean *external_only,
                 EGLint *num_modifiers),
                (dpy, format, max_modifiers, modifiers, external_only, num_modifiers))

EGL_PASSTHROUGH(EGLBoolean, eglSetDamageRegionKHR,
                (EGLDisplay dpy, EGLSurface surface, EGLint *rects, EGLint n_rects),
                (dpy, surface, rects, n_rects))

#endif /* HAS_EGL */
