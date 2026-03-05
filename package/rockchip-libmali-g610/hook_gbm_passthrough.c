/*
 * GBM passthrough wrappers for hook-as-libgbm.so
 *
 * When libgbm.so symlinks to the hook (instead of the Mali blob), the hook
 * must export ALL GBM symbols. hook.c only wraps "newer" GBM functions.
 * This file provides thin forwarders for the basic GBM functions that the
 * blob exports but hook.c does not wrap.
 *
 * All functions dlsym the real implementation from the Mali blob (libmali.so.1)
 * and forward the call.
 */

#ifdef HAS_GBM

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <gbm.h>

/* Get the blob handle — same approach as hook.c's load_mali_symbols */
static void *
get_blob(void)
{
    static void *handle = NULL;
    if (!handle) {
        handle = dlopen(LIBMALI_SO, RTLD_LAZY | RTLD_NOLOAD);
        if (!handle)
            handle = dlopen(LIBMALI_SO, RTLD_LAZY);
        if (!handle)
            fprintf(stderr, "[MALI-HOOK] gbm_passthrough: dlopen(%s) failed: %s\n",
                    LIBMALI_SO, dlerror());
    }
    return handle;
}

#define PASSTHROUGH(ret, name, params, args) \
    ret name params { \
        static ret (*real) params = NULL; \
        if (!real) { \
            void *h = get_blob(); \
            if (h) real = dlsym(h, #name); \
        } \
        if (!real) { \
            fprintf(stderr, "[MALI-HOOK] %s: not found in blob\n", #name); \
            return (ret)0; \
        } \
        return real args; \
    }

#define PASSTHROUGH_VOID(name, params, args) \
    void name params { \
        static void (*real) params = NULL; \
        if (!real) { \
            void *h = get_blob(); \
            if (h) real = dlsym(h, #name); \
        } \
        if (!real) { \
            fprintf(stderr, "[MALI-HOOK] %s: not found in blob\n", #name); \
            return; \
        } \
        real args; \
    }

/* Basic device management */
PASSTHROUGH(struct gbm_device *, gbm_create_device, (int fd), (fd))
PASSTHROUGH_VOID(gbm_device_destroy, (struct gbm_device *gbm), (gbm))
PASSTHROUGH(int, gbm_device_get_fd, (struct gbm_device *gbm), (gbm))
PASSTHROUGH(int, gbm_device_is_format_supported,
            (struct gbm_device *gbm, uint32_t format, uint32_t flags),
            (gbm, format, flags))
PASSTHROUGH(const char *, gbm_device_get_backend_name,
            (struct gbm_device *gbm), (gbm))

/* Buffer object basics */
PASSTHROUGH_VOID(gbm_bo_destroy, (struct gbm_bo *bo), (bo))
PASSTHROUGH(uint32_t, gbm_bo_get_width, (struct gbm_bo *bo), (bo))
PASSTHROUGH(uint32_t, gbm_bo_get_height, (struct gbm_bo *bo), (bo))
PASSTHROUGH(uint32_t, gbm_bo_get_stride, (struct gbm_bo *bo), (bo))
PASSTHROUGH(uint32_t, gbm_bo_get_format, (struct gbm_bo *bo), (bo))
PASSTHROUGH(union gbm_bo_handle, gbm_bo_get_handle, (struct gbm_bo *bo), (bo))
PASSTHROUGH(int, gbm_bo_get_fd, (struct gbm_bo *bo), (bo))
PASSTHROUGH(struct gbm_device *, gbm_bo_get_device, (struct gbm_bo *bo), (bo))
PASSTHROUGH(int, gbm_bo_write, (struct gbm_bo *bo, const void *buf, size_t count),
            (bo, buf, count))

/* User data */
PASSTHROUGH_VOID(gbm_bo_set_user_data,
                 (struct gbm_bo *bo, void *data, void (*destroy_user_data)(struct gbm_bo *, void *)),
                 (bo, data, destroy_user_data))
PASSTHROUGH(void *, gbm_bo_get_user_data, (struct gbm_bo *bo), (bo))

/* Surface management */
PASSTHROUGH_VOID(gbm_surface_destroy, (struct gbm_surface *surface), (surface))
PASSTHROUGH(struct gbm_bo *, gbm_surface_lock_front_buffer,
            (struct gbm_surface *surface), (surface))
PASSTHROUGH_VOID(gbm_surface_release_buffer,
                 (struct gbm_surface *surface, struct gbm_bo *bo),
                 (surface, bo))
PASSTHROUGH(int, gbm_surface_has_free_buffers,
            (struct gbm_surface *surface), (surface))

#endif /* HAS_GBM */
