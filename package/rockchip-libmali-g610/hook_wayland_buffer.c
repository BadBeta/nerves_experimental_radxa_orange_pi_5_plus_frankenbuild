/*
 * GBM-export eglCreateWaylandBufferFromImageWL for Mali g24p0 blob
 *
 * The Mali g24p0 blob does NOT export eglCreateWaylandBufferFromImageWL,
 * but Cog/WPE requires it to create wl_buffers from EGLImages.
 *
 * WPE-FDO creates EGLImages with target EGL_WAYLAND_BUFFER_WL (0x31d5)
 * — an image created FROM a Wayland buffer on WPE's inner compositor.
 * We need to export this as a NEW wl_buffer on the outer compositor.
 *
 * Strategy: Try multiple GBM import paths in order:
 *   1. GBM_BO_IMPORT_EGL_IMAGE (direct, works if blob supports it)
 *   2. GBM_BO_IMPORT_WL_BUFFER (using stashed wl_buffer from eglCreateImageKHR)
 *   3. Both with different flags (RENDERING, SCANOUT, 0)
 *
 * Copyright (c) 2026, Vidar Hokstad
 * License: GPL-2.0
 */

#if defined(HAS_WAYLAND) && defined(HAS_EGL) && defined(HAS_GBM)

#define _GNU_SOURCE

/* Debug logging: define MALI_HOOK_DEBUG=1 at compile time to enable */
#ifndef MALI_HOOK_DEBUG
#define MALI_HOOK_DEBUG 0
#endif
#define HOOK_DBG(...) do { if (MALI_HOOK_DEBUG) fprintf(stderr, __VA_ARGS__); } while (0)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <wayland-client.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"

/* EGL target constants */
#ifndef EGL_WAYLAND_BUFFER_WL
#define EGL_WAYLAND_BUFFER_WL 0x31D5
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif

/* EGL dmabuf import attribute constants */
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT          0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT        0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT    0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT     0x3274
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif
#ifndef EGL_WIDTH
#define EGL_WIDTH                          0x3057
#endif
#ifndef EGL_HEIGHT
#define EGL_HEIGHT                         0x3056
#endif

/* GBM import type / flags */
#ifndef GBM_BO_IMPORT_WL_BUFFER
#define GBM_BO_IMPORT_WL_BUFFER  0x5501
#endif
#ifndef GBM_BO_IMPORT_EGL_IMAGE
#define GBM_BO_IMPORT_EGL_IMAGE  0x5502
#endif
#ifndef GBM_BO_USE_RENDERING
#define GBM_BO_USE_RENDERING     (1 << 2)
#endif
#ifndef GBM_BO_USE_SCANOUT
#define GBM_BO_USE_SCANOUT       (1 << 0)
#endif

/* ------------------------------------------------------------------ */
/* Image stash: map EGLImage → source wl_buffer + dmabuf attribs      */
/* ------------------------------------------------------------------ */

#define MAX_STASH 32

struct image_info {
    EGLImageKHR image;
    /* For EGL_WAYLAND_BUFFER_WL target: the source wl_buffer */
    struct wl_buffer *wl_buf;
    /* For EGL_LINUX_DMA_BUF_EXT target: dmabuf attributes */
    int         fd;       /* dup'd copy */
    uint32_t    format;
    uint32_t    width;
    uint32_t    height;
    uint32_t    stride;
    uint32_t    offset;
    uint64_t    modifier;
    EGLenum     target;
    int         in_use;
};

static struct image_info stash[MAX_STASH];

static void
stash_add_wl_buffer(EGLImageKHR image, struct wl_buffer *wl_buf)
{
    for (int i = 0; i < MAX_STASH; i++) {
        if (!stash[i].in_use) {
            memset(&stash[i], 0, sizeof(stash[i]));
            stash[i].image  = image;
            stash[i].wl_buf = wl_buf;
            stash[i].target = EGL_WAYLAND_BUFFER_WL;
            stash[i].fd     = -1;
            stash[i].in_use = 1;
            HOOK_DBG("[MALI-HOOK] stash[%d]: WL_BUFFER image=%p wl_buf=%p\n",
                    i, image, (void *)wl_buf);
            return;
        }
    }
    HOOK_DBG("[MALI-HOOK] stash FULL!\n");
}

static void
stash_add_dmabuf(EGLImageKHR image, int fd, uint32_t format,
                 uint32_t w, uint32_t h, uint32_t stride,
                 uint32_t offset, uint64_t modifier)
{
    int dup_fd = dup(fd);
    if (dup_fd < 0) {
        HOOK_DBG("[MALI-HOOK] stash: dup(fd=%d) failed\n", fd);
        return;
    }

    for (int i = 0; i < MAX_STASH; i++) {
        if (!stash[i].in_use) {
            memset(&stash[i], 0, sizeof(stash[i]));
            stash[i].image    = image;
            stash[i].fd       = dup_fd;
            stash[i].format   = format;
            stash[i].width    = w;
            stash[i].height   = h;
            stash[i].stride   = stride;
            stash[i].offset   = offset;
            stash[i].modifier = modifier;
            stash[i].target   = EGL_LINUX_DMA_BUF_EXT;
            stash[i].in_use   = 1;
            HOOK_DBG("[MALI-HOOK] stash[%d]: DMABUF image=%p fd=%d %ux%u "
                    "fmt=0x%x stride=%u mod=0x%x:%08x\n",
                    i, image, dup_fd, w, h, format, stride,
                    (uint32_t)(modifier >> 32), (uint32_t)(modifier & 0xFFFFFFFF));
            return;
        }
    }
    HOOK_DBG("[MALI-HOOK] stash FULL!\n");
    close(dup_fd);
}

static struct image_info *
stash_find(EGLImageKHR image)
{
    for (int i = 0; i < MAX_STASH; i++) {
        if (stash[i].in_use && stash[i].image == image)
            return &stash[i];
    }
    return NULL;
}

static void
stash_remove(EGLImageKHR image)
{
    for (int i = 0; i < MAX_STASH; i++) {
        if (stash[i].in_use && stash[i].image == image) {
            if (stash[i].fd >= 0)
                close(stash[i].fd);
            stash[i].in_use = 0;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Parse EGLint* attribute list for dmabuf import attrs               */
/* ------------------------------------------------------------------ */

static int
parse_dmabuf_attribs(const EGLint *attribs,
                     int *out_fd, uint32_t *out_format,
                     uint32_t *out_w, uint32_t *out_h,
                     uint32_t *out_stride, uint32_t *out_offset,
                     uint64_t *out_modifier)
{
    *out_fd = -1;
    *out_format = 0;
    *out_w = 0;
    *out_h = 0;
    *out_stride = 0;
    *out_offset = 0;
    *out_modifier = 0;

    if (!attribs)
        return -1;

    for (int i = 0; attribs[i] != EGL_NONE; i += 2) {
        switch (attribs[i]) {
        case EGL_WIDTH:
            *out_w = (uint32_t)attribs[i + 1]; break;
        case EGL_HEIGHT:
            *out_h = (uint32_t)attribs[i + 1]; break;
        case EGL_LINUX_DRM_FOURCC_EXT:
            *out_format = (uint32_t)attribs[i + 1]; break;
        case EGL_DMA_BUF_PLANE0_FD_EXT:
            *out_fd = (int)attribs[i + 1]; break;
        case EGL_DMA_BUF_PLANE0_OFFSET_EXT:
            *out_offset = (uint32_t)attribs[i + 1]; break;
        case EGL_DMA_BUF_PLANE0_PITCH_EXT:
            *out_stride = (uint32_t)attribs[i + 1]; break;
        case EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT:
            *out_modifier = (*out_modifier & 0xFFFFFFFF00000000ULL) |
                            (uint64_t)(uint32_t)attribs[i + 1];
            break;
        case EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT:
            *out_modifier = (*out_modifier & 0xFFFFFFFFULL) |
                            ((uint64_t)(uint32_t)attribs[i + 1] << 32);
            break;
        }
    }

    return (*out_fd >= 0 && *out_w > 0 && *out_h > 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Mali blob function resolution                                      */
/* ------------------------------------------------------------------ */

static void *
get_blob_handle(void)
{
    static void *handle = NULL;
    if (!handle) {
        handle = dlopen(LIBMALI_SO, RTLD_LAZY | RTLD_NOLOAD);
        if (!handle)
            handle = dlopen(LIBMALI_SO, RTLD_LAZY);
        if (!handle)
            HOOK_DBG("[MALI-HOOK] wayland_buffer: dlopen(%s) failed: %s\n",
                    LIBMALI_SO, dlerror());
    }
    return handle;
}

static void *
get_mali_proc(const char *name)
{
    static __eglMustCastToProperFunctionPointerType
        (*mali_getproc)(const char *) = NULL;
    static int init = 0;

    if (!init) {
        void *h = get_blob_handle();
        if (h) {
            mali_getproc =
                (__eglMustCastToProperFunctionPointerType (*)(const char *))
                dlsym(h, "eglGetProcAddress");
            HOOK_DBG("[MALI-HOOK] get_mali_proc: blob=%p getproc=%p\n",
                    h, (void *)mali_getproc);
        }
        init = 1;
    }

    if (mali_getproc)
        return (void *)mali_getproc(name);

    void *h = get_blob_handle();
    if (h)
        return dlsym(h, name);

    return NULL;
}

/* Resolve GBM function from the blob directly */
static void *
get_gbm_proc(const char *name)
{
    void *h = get_blob_handle();
    return h ? dlsym(h, name) : NULL;
}

/* ------------------------------------------------------------------ */
/* GBM device (lazy init from /dev/dri/renderD128)                    */
/* ------------------------------------------------------------------ */

static struct gbm_device *hook_gbm_dev = NULL;

static struct gbm_device *
ensure_gbm_device(void)
{
    if (hook_gbm_dev)
        return hook_gbm_dev;

    typedef struct gbm_device *(*CreateDevFn)(int fd);
    CreateDevFn create_dev = (CreateDevFn)get_gbm_proc("gbm_create_device");
    if (!create_dev) {
        HOOK_DBG("[MALI-HOOK] gbm_create_device not found in blob\n");
        return NULL;
    }

    const char *paths[] = {"/dev/dri/renderD128", "/dev/dri/card0", NULL};
    for (int i = 0; paths[i]; i++) {
        int fd = open(paths[i], O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            hook_gbm_dev = create_dev(fd);
            if (hook_gbm_dev) {
                HOOK_DBG("[MALI-HOOK] GBM device from %s\n", paths[i]);
                return hook_gbm_dev;
            }
            close(fd);
        }
    }

    HOOK_DBG("[MALI-HOOK] failed to create GBM device\n");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Shared state — hook_wl_display is set by hook.c                    */
/* ------------------------------------------------------------------ */

struct wl_display *hook_wl_display = NULL;

/* ------------------------------------------------------------------ */
/* zwp_linux_dmabuf_v1 binding (lazy, from compositor registry)       */
/* ------------------------------------------------------------------ */

static struct zwp_linux_dmabuf_v1 *hook_dmabuf = NULL;

static void
registry_global(void *data, struct wl_registry *registry,
                uint32_t name, const char *interface, uint32_t version)
{
    if (!strcmp(interface, "zwp_linux_dmabuf_v1")) {
        uint32_t bind_ver = version < 3 ? version : 3;
        hook_dmabuf = wl_registry_bind(registry, name,
                                        &zwp_linux_dmabuf_v1_interface,
                                        bind_ver);
    }
}

static void
registry_global_remove(void *data, struct wl_registry *registry,
                       uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int
ensure_dmabuf(void)
{
    if (hook_dmabuf)
        return 0;

    if (!hook_wl_display) {
        HOOK_DBG("[MALI-HOOK] wayland_buffer: no wl_display captured\n");
        return -1;
    }

    struct wl_registry *reg = wl_display_get_registry(hook_wl_display);
    if (!reg)
        return -1;

    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(hook_wl_display);
    wl_registry_destroy(reg);

    if (!hook_dmabuf) {
        HOOK_DBG("[MALI-HOOK] wayland_buffer: compositor has no "
                "zwp_linux_dmabuf_v1\n");
        return -1;
    }

    HOOK_DBG("[MALI-HOOK] wayland_buffer: bound zwp_linux_dmabuf_v1\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* eglCreateImageKHR — intercept to stash source info                 */
/* ------------------------------------------------------------------ */

EGLImageKHR
hook_eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                       EGLClientBuffer buffer, const EGLint *attribs)
{
    typedef EGLImageKHR (*Fn)(EGLDisplay, EGLContext, EGLenum,
                               EGLClientBuffer, const EGLint *);
    static Fn realFn = NULL;
    if (!realFn)
        realFn = (Fn)get_mali_proc("eglCreateImageKHR");

    EGLImageKHR image = realFn ?
        realFn(dpy, ctx, target, buffer, attribs) : EGL_NO_IMAGE_KHR;

    HOOK_DBG("[MALI-HOOK] eglCreateImageKHR: target=0x%x image=%p buffer=%p\n",
            (unsigned)target, image, (void *)buffer);

    if (image != EGL_NO_IMAGE_KHR) {
        if ((EGLint)target == EGL_WAYLAND_BUFFER_WL) {
            /* buffer IS the wl_buffer from WPE-FDO's inner compositor */
            stash_add_wl_buffer(image, (struct wl_buffer *)buffer);
        } else if ((EGLint)target == EGL_LINUX_DMA_BUF_EXT) {
            int fd;
            uint32_t format, w, h, stride, offset;
            uint64_t modifier;
            if (parse_dmabuf_attribs(attribs, &fd, &format, &w, &h,
                                      &stride, &offset, &modifier) == 0) {
                stash_add_dmabuf(image, fd, format, w, h, stride, offset, modifier);
            }
        }
    }

    return image;
}

/* ------------------------------------------------------------------ */
/* eglDestroyImageKHR — clean up stash entry                          */
/* ------------------------------------------------------------------ */

EGLBoolean
hook_eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR image)
{
    typedef EGLBoolean (*Fn)(EGLDisplay, EGLImageKHR);
    static Fn realFn = NULL;
    if (!realFn)
        realFn = (Fn)get_mali_proc("eglDestroyImageKHR");

    stash_remove(image);
    return realFn ? realFn(dpy, image) : EGL_FALSE;
}

/* ------------------------------------------------------------------ */
/* GBM function pointers                                              */
/* ------------------------------------------------------------------ */

typedef struct gbm_bo *(*gbm_import_fn)(struct gbm_device *, uint32_t, void *, uint32_t);
typedef int            (*gbm_get_fd_fn)(struct gbm_bo *);
typedef uint32_t       (*gbm_get_u32_fn)(struct gbm_bo *);
typedef uint64_t       (*gbm_get_u64_fn)(struct gbm_bo *);
typedef uint32_t       (*gbm_get_plane_u32_fn)(struct gbm_bo *, int);
typedef void           (*gbm_destroy_fn)(struct gbm_bo *);

static struct {
    gbm_import_fn          import;
    gbm_get_fd_fn          get_fd;
    gbm_get_u32_fn         get_width;
    gbm_get_u32_fn         get_height;
    gbm_get_u32_fn         get_format;
    gbm_get_u32_fn         get_stride;
    gbm_get_plane_u32_fn   get_stride_plane;
    gbm_get_plane_u32_fn   get_offset;
    gbm_get_u64_fn         get_modifier;
    gbm_destroy_fn         destroy;
    int                    resolved;
} gbm_fn;

static int
resolve_gbm_fns(void)
{
    if (gbm_fn.resolved)
        return (gbm_fn.import && gbm_fn.get_fd && gbm_fn.destroy) ? 0 : -1;

    gbm_fn.import          = (gbm_import_fn)get_gbm_proc("gbm_bo_import");
    gbm_fn.get_fd          = (gbm_get_fd_fn)get_gbm_proc("gbm_bo_get_fd");
    gbm_fn.get_width       = (gbm_get_u32_fn)get_gbm_proc("gbm_bo_get_width");
    gbm_fn.get_height      = (gbm_get_u32_fn)get_gbm_proc("gbm_bo_get_height");
    gbm_fn.get_format      = (gbm_get_u32_fn)get_gbm_proc("gbm_bo_get_format");
    gbm_fn.get_stride      = (gbm_get_u32_fn)get_gbm_proc("gbm_bo_get_stride");
    gbm_fn.get_stride_plane = (gbm_get_plane_u32_fn)get_gbm_proc("gbm_bo_get_stride_for_plane");
    gbm_fn.get_offset      = (gbm_get_plane_u32_fn)get_gbm_proc("gbm_bo_get_offset");
    gbm_fn.get_modifier    = (gbm_get_u64_fn)get_gbm_proc("gbm_bo_get_modifier");
    gbm_fn.destroy         = (gbm_destroy_fn)get_gbm_proc("gbm_bo_destroy");
    gbm_fn.resolved        = 1;

    HOOK_DBG("[MALI-HOOK] GBM procs: import=%p fd=%p fmt=%p destroy=%p\n",
            (void *)gbm_fn.import, (void *)gbm_fn.get_fd,
            (void *)gbm_fn.get_format, (void *)gbm_fn.destroy);

    if (!gbm_fn.import || !gbm_fn.get_fd || !gbm_fn.destroy) {
        HOOK_DBG("[MALI-HOOK] wayland_buffer: critical GBM functions missing\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Try to import as GBM BO using various methods                      */
/* ------------------------------------------------------------------ */

static struct gbm_bo *
try_gbm_import(struct gbm_device *gbm, EGLImageKHR image,
               struct image_info *info)
{
    struct gbm_bo *bo = NULL;

    /* Try 1: If we have a stashed wl_buffer, import via GBM_BO_IMPORT_WL_BUFFER.
     * This is the working path for Mali g24p0 blob.
     * GBM_BO_IMPORT_EGL_IMAGE does NOT work for EGL_WAYLAND_BUFFER_WL images
     * and attempting it may corrupt blob state, so we skip it entirely. */
    if (info && info->wl_buf) {
        HOOK_DBG("[MALI-HOOK] GBM import: trying WL_BUFFER wl_buf=%p\n",
                (void *)info->wl_buf);
        bo = gbm_fn.import(gbm, GBM_BO_IMPORT_WL_BUFFER, info->wl_buf, 0);
        if (bo) {
            HOOK_DBG("[MALI-HOOK] GBM import: WL_BUFFER OK\n");
            return bo;
        }
        HOOK_DBG("[MALI-HOOK] GBM import: WL_BUFFER failed\n");
    }

    /* Try 2: GBM_BO_IMPORT_EGL_IMAGE as last resort (non-WL_BUFFER images) */
    if (!info || !info->wl_buf) {
        HOOK_DBG("[MALI-HOOK] GBM import: trying EGL_IMAGE\n");
        bo = gbm_fn.import(gbm, GBM_BO_IMPORT_EGL_IMAGE, image, 0);
        if (bo) {
            HOOK_DBG("[MALI-HOOK] GBM import: EGL_IMAGE OK\n");
            return bo;
        }
        HOOK_DBG("[MALI-HOOK] GBM import: EGL_IMAGE failed\n");
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Create wl_buffer from GBM BO via zwp_linux_dmabuf_v1               */
/* ------------------------------------------------------------------ */

static struct wl_buffer *
create_wl_buffer_from_bo(struct gbm_bo *bo)
{
    int fd = gbm_fn.get_fd(bo);
    if (fd < 0) {
        HOOK_DBG("[MALI-HOOK] wayland_buffer: gbm_bo_get_fd failed\n");
        return NULL;
    }

    uint32_t width  = gbm_fn.get_width  ? gbm_fn.get_width(bo)  : 0;
    uint32_t height = gbm_fn.get_height ? gbm_fn.get_height(bo) : 0;
    uint32_t format = gbm_fn.get_format ? gbm_fn.get_format(bo) : 0;

    uint32_t stride = gbm_fn.get_stride_plane ?
        gbm_fn.get_stride_plane(bo, 0) :
        (gbm_fn.get_stride ? gbm_fn.get_stride(bo) : 0);

    uint32_t offset = gbm_fn.get_offset ? gbm_fn.get_offset(bo, 0) : 0;

    uint64_t modifier = gbm_fn.get_modifier ?
        gbm_fn.get_modifier(bo) : 0;

    /* DRM_FORMAT_MOD_INVALID means "unknown modifier" — treat as LINEAR */
    if (modifier == 0xFFFFFFFFFFFFFFFFULL)
        modifier = 0;  /* DRM_FORMAT_MOD_LINEAR */

    uint32_t mod_hi = (uint32_t)(modifier >> 32);
    uint32_t mod_lo = (uint32_t)(modifier & 0xFFFFFFFF);

    HOOK_DBG("[MALI-HOOK] wayland_buffer: GBM BO %ux%u fmt=0x%x "
            "fd=%d stride=%u offset=%u mod=0x%x:%08x\n",
            width, height, format, fd, stride, offset, mod_hi, mod_lo);

    if (ensure_dmabuf() < 0) {
        close(fd);
        return NULL;
    }

    struct zwp_linux_buffer_params_v1 *params =
        zwp_linux_dmabuf_v1_create_params(hook_dmabuf);
    if (!params) {
        HOOK_DBG("[MALI-HOOK] wayland_buffer: create_params failed\n");
        close(fd);
        return NULL;
    }

    zwp_linux_buffer_params_v1_add(params, fd, 0, offset, stride,
                                    mod_hi, mod_lo);

    struct wl_buffer *buffer =
        zwp_linux_buffer_params_v1_create_immed(params,
                                                  (int32_t)width,
                                                  (int32_t)height,
                                                  format, 0);

    zwp_linux_buffer_params_v1_destroy(params);
    close(fd);

    if (buffer) {
        HOOK_DBG("[MALI-HOOK] wayland_buffer: created wl_buffer "
                "%ux%u fmt=0x%x OK\n", width, height, format);
    } else {
        HOOK_DBG("[MALI-HOOK] wayland_buffer: create_immed FAILED "
                "%ux%u fmt=0x%x\n", width, height, format);
    }

    return buffer;
}

/* ------------------------------------------------------------------ */
/* Create wl_buffer from stashed dmabuf attributes (direct path)      */
/* ------------------------------------------------------------------ */

static struct wl_buffer *
create_wl_buffer_from_stashed_dmabuf(struct image_info *info)
{
    if (ensure_dmabuf() < 0)
        return NULL;

    /* dup the fd — the compositor takes ownership via Wayland socket */
    int buf_fd = dup(info->fd);
    if (buf_fd < 0) {
        HOOK_DBG("[MALI-HOOK] wayland_buffer: dup(fd=%d) failed\n", info->fd);
        return NULL;
    }

    uint32_t mod_hi = (uint32_t)(info->modifier >> 32);
    uint32_t mod_lo = (uint32_t)(info->modifier & 0xFFFFFFFF);

    HOOK_DBG("[MALI-HOOK] wayland_buffer: stashed DMABUF %ux%u fmt=0x%x "
            "fd=%d stride=%u mod=0x%x:%08x\n",
            info->width, info->height, info->format,
            buf_fd, info->stride, mod_hi, mod_lo);

    struct zwp_linux_buffer_params_v1 *params =
        zwp_linux_dmabuf_v1_create_params(hook_dmabuf);
    if (!params) {
        close(buf_fd);
        return NULL;
    }

    zwp_linux_buffer_params_v1_add(params, buf_fd, 0,
                                    info->offset, info->stride,
                                    mod_hi, mod_lo);

    struct wl_buffer *buffer =
        zwp_linux_buffer_params_v1_create_immed(params,
                                                  (int32_t)info->width,
                                                  (int32_t)info->height,
                                                  info->format, 0);

    zwp_linux_buffer_params_v1_destroy(params);
    close(buf_fd);

    if (buffer) {
        HOOK_DBG("[MALI-HOOK] wayland_buffer: created wl_buffer from "
                "stashed dmabuf %ux%u OK\n", info->width, info->height);
    }

    return buffer;
}

/* ------------------------------------------------------------------ */
/* eglCreateWaylandBufferFromImageWL — multi-strategy                 */
/* ------------------------------------------------------------------ */

struct wl_buffer *
hook_eglCreateWaylandBufferFromImageWL(EGLDisplay dpy, EGLImageKHR image)
{
    struct image_info *info = stash_find(image);

    HOOK_DBG("[MALI-HOOK] wayland_buffer: image=%p stash=%s target=0x%x\n",
            image, info ? "found" : "miss",
            info ? (unsigned)info->target : 0);

    /* Path A: If we stashed dmabuf attributes, use them directly (fastest) */
    if (info && info->target == EGL_LINUX_DMA_BUF_EXT && info->fd >= 0) {
        return create_wl_buffer_from_stashed_dmabuf(info);
    }

    /* Path B: Try GBM import (works for various EGLImage types) */
    if (resolve_gbm_fns() == 0) {
        struct gbm_device *gbm = ensure_gbm_device();
        if (gbm) {
            struct gbm_bo *bo = try_gbm_import(gbm, image, info);
            if (bo) {
                struct wl_buffer *buffer = create_wl_buffer_from_bo(bo);
                gbm_fn.destroy(bo);
                return buffer;
            }
        }
    }

    HOOK_DBG("[MALI-HOOK] wayland_buffer: ALL paths failed for image=%p\n",
            image);
    return NULL;
}

#endif /* HAS_WAYLAND && HAS_EGL && HAS_GBM */
