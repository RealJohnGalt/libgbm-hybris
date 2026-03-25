#define _GNU_SOURCE

#include <fcntl.h> 
#include <stddef.h>
#include <xf86drm.h>
#include <drm/drm_fourcc.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <malloc.h>

#include <linux/memfd.h>

#include <gbm.h>
#include "gbm_backend_abi.h"

#include <hybris/gralloc/gralloc.h>

#include <hardware/gralloc.h>

#include <assert.h>

struct gbm_hybris_bo {
    struct gbm_bo base;
    buffer_handle_t handle;
    int fd;
};

struct gbm_hybris_surface {
    struct gbm_surface base;
    struct gbm_hybris_bo *front_bo;
    bool front_locked;
    struct gbm_hybris_bo *bo[16];
    unsigned int bo_count;
};

static const struct gbm_core *core;

struct gbm_surface *hybris_gbm_surface_create(struct gbm_device *gbm,
					      uint32_t width, uint32_t height,
					      uint32_t format, uint32_t flags,
					      const uint64_t *modifiers,
					      const unsigned count);

struct gbm_hybris_bo *gbm_hybris_bo(struct gbm_bo *bo)
{
   return (struct gbm_hybris_bo *) bo;
}

static int get_hal_pixel_format(uint32_t gbm_format)
{
    int format;

    switch (gbm_format) {
    case GBM_FORMAT_ABGR8888:
        format = HAL_PIXEL_FORMAT_RGBA_8888;
        break;
    case GBM_FORMAT_XBGR8888:
        format = HAL_PIXEL_FORMAT_RGBX_8888;
        break;
    case GBM_FORMAT_RGB888:
        format = HAL_PIXEL_FORMAT_RGB_888;
        break;
    case GBM_FORMAT_RGB565:
        format = HAL_PIXEL_FORMAT_RGB_565;
        break;
    case GBM_FORMAT_ARGB8888:
        format = HAL_PIXEL_FORMAT_BGRA_8888;
        break;
    case GBM_FORMAT_GR88:
        /* GR88 corresponds to YV12 which is planar */
        format = HAL_PIXEL_FORMAT_YV12;
        break;
    case GBM_FORMAT_ABGR16161616F:
        format = HAL_PIXEL_FORMAT_RGBA_FP16;
        break;
    case GBM_FORMAT_ABGR2101010:
        format = HAL_PIXEL_FORMAT_RGBA_1010102;
        break;
    default:
        format = HAL_PIXEL_FORMAT_RGBA_8888; // Invalid or unsupported format assume RGBA8888
        break;
    }

    return format;
}

// Dummy func to identify hybris gdb_device/bo/surface
static struct gbm_device *gbm_device_hybris(int x)
{
    return NULL;
}

struct gbm_bo* hybris_gbm_bo_create(struct gbm_device* device, uint32_t width, uint32_t height, uint32_t format, uint32_t flags, const uint64_t *modifiers, const unsigned int count)
{
    if (!device || device->v0.fd < 0 || !core) {
        errno = EINVAL;
        fprintf(stderr, "[libgbm-hybris] Invalid GBM device/backend state.\n");
        return NULL;
    }

    struct gbm_hybris_bo *bo = calloc(1, sizeof(*bo));
    if (!bo) {
        errno = ENOMEM;
        fprintf(stderr, "[libgbm-hybris] Failed to allocate memory for GBM buffer object.\n");
        return NULL;
    }

    bo->base.gbm = device;
    bo->base.v0.width = width;
    bo->base.v0.height = height;
    bo->base.v0.format = core->v0.format_canonicalize(format);

    int usage = GRALLOC_USAGE_HW_RENDER |
                 GRALLOC_USAGE_HW_TEXTURE |
                 GRALLOC_USAGE_HW_COMPOSER;

    int gralloc_stride = 0;

    int hal_format = get_hal_pixel_format(bo->base.v0.format);

    if (hybris_gralloc_allocate(width, height, hal_format,
                                usage, &bo->handle, &gralloc_stride) != 0 || !bo->handle) {
        fprintf(stderr, "[libgbm-hybris] Gralloc allocation failed.\n");
        free(bo);
        return NULL;
    }

    bo->base.v0.stride = gralloc_stride * 4;

    bo->fd = memfd_create("gbm-hybris-handle", MFD_CLOEXEC);
    if (bo->fd < 0) {
        hybris_gralloc_release(bo->handle, 1);
        free(bo);
        fprintf(stderr, "[libgbm-hybris] memfd_create failed.\n");
        return NULL;
    }

    int version  = bo->handle->version;
    int numFds   = bo->handle->numFds;
    int numInts  = bo->handle->numInts;

    size_t buf_size = sizeof(int) * (3 + numFds + numInts);

    int *buf = malloc(buf_size);
    if (!buf) {
        close(bo->fd);
        hybris_gralloc_release(bo->handle, 1);
        free(bo);
        fprintf(stderr, "[libgbm-hybris] Failed to allocate metadata.\n");
        return NULL;
    }

    buf[0] = version;
    buf[1] = numFds;
    buf[2] = numInts;

    for (int i = 0; i < numInts; i++)
        buf[3 + i] = bo->handle->data[numFds + i];

    if (ftruncate(bo->fd, buf_size) < 0 || pwrite(bo->fd, buf, buf_size, 0) != (ssize_t)buf_size) {
        fprintf(stderr, "[libgbm-hybris] Failed to write memfd.\n");
        free(buf);
        close(bo->fd);
        hybris_gralloc_release(bo->handle, 1);
        free(bo);
        return NULL;
    }

    free(buf);

    return &bo->base;
}

static void hybris_gbm_bo_destroy(struct gbm_bo *_bo)
{
    if (!_bo)
        return;

    struct gbm_hybris_bo *bo = gbm_hybris_bo(_bo);

    if(bo->fd >= 0)
        close(bo->fd);

    if(bo->handle)
        hybris_gralloc_release(bo->handle, 1);

    free(bo);
}

static void hybris_gbm_device_destroy(struct gbm_device *device)
{
    free(device);
}

struct gbm_bo *hybris_gbm_bo_create_with_modifiers(struct gbm_device *gbm,
                             uint32_t width, uint32_t height,
                             uint32_t format,
                             const uint64_t *modifiers,
                             const unsigned int count)
{
   /* Force linear: ignore modifier list and allocate a normal BO */
   return hybris_gbm_bo_create(gbm, width, height, format, 0, NULL, 0);
}

struct gbm_bo * hybris_gbm_bo_create_with_modifiers2(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, const uint64_t *modifiers, const unsigned int count, uint32_t flags){
    /* Force linear: ignore modifier list and allocate a normal BO */
    return hybris_gbm_bo_create(gbm, width, height, format, flags, NULL, 0);
}

struct gbm_bo *hybris_gbm_bo_import(struct gbm_device *gbm, uint32_t type, void *buffer, uint32_t usage){
// How do that even work with fake dma buf's?
   fprintf(stderr, "[libgbm-hybris] gbm_bo_import called\n");
   return NULL;
}

// Suprisingly not part of libgbm
uint32_t hybris_gbm_bo_get_stride(struct gbm_bo* bo, int plane) {
    // x4 the stride, as it's checked by drm and drm expexcts stride to be at very least width*bpp
    return bo ? (uint32_t)(bo->v0.stride) : 0;
}

uint32_t hybris_gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane)
{
    if (!bo) {
        errno = EINVAL;
        return 0;
    }
    if (plane != 0) {
        errno = EINVAL;
        return 0;
    }
    return hybris_gbm_bo_get_stride(bo, plane);
}

uint64_t hybris_gbm_bo_get_modifier(struct gbm_bo* bo) {
    return DRM_FORMAT_MOD_LINEAR;
}

void* hybris_gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t flags, uint32_t *stride, void **map_data) {
//TBD: Implement based on grlloc lock
    fprintf(stderr, "[libgbm-hybris] gbm_bo_map called with x: %u, y: %u, width: %u, height: %u, flags: %u\n", x, y, width, height, flags);
    return NULL;
}

void hybris_gbm_surface_destroy(struct gbm_surface *surf) {
    struct gbm_hybris_surface *hsurf = (struct gbm_hybris_surface *)surf;
    int i;

    if (!hsurf)
        return;

    // We own nothing
    free(hsurf);
}

int hybris_gbm_surface_has_free_buffers(struct gbm_surface *surface)
{
    struct gbm_hybris_surface *hsurf = (struct gbm_hybris_surface *)surface;

    if(hsurf->front_locked)
        return 1;

    return 0;
}

struct gbm_bo* hybris_gbm_surface_lock_front_buffer(struct gbm_surface* surface) {
    struct gbm_hybris_surface *hsurf = (struct gbm_hybris_surface *)surface;

    if (!hsurf || !hsurf->front_bo) {
        errno = EAGAIN;
        return NULL;
    }

    if (hsurf->front_locked) {
        errno = EAGAIN;
        return NULL;
    }

    hsurf->front_locked = true;
    return &hsurf->front_bo->base;
}

void hybris_gbm_surface_release_buffer(struct gbm_surface* surface, struct gbm_bo* bo) {
    struct gbm_hybris_surface *hsurf = (struct gbm_hybris_surface *)surface;

    if (!hsurf || !bo)
        return;

    if (hsurf->front_bo == (struct gbm_hybris_bo *)bo)
        hsurf->front_locked = false;
}

static union gbm_bo_handle hybris_gbm_bo_get_handle_for_plane(struct gbm_bo *_bo, int plane)
{
    union gbm_bo_handle handle = {0};
    return handle;
}

int hybris_gbm_bo_get_plane_count(struct gbm_bo *bo)
{
    struct gbm_hybris_bo *hbo = gbm_hybris_bo(bo);
    if(!hbo->handle || !hbo->fd)
        return -1;

    return hbo->handle->numFds + 1;
}

int hybris_gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane)
{
    struct gbm_hybris_bo *hbo = gbm_hybris_bo(bo);

    if (!hbo->handle || !hbo->fd || plane < 0)
        return -1;

    if (plane < hbo->handle->numFds)
        return dup(hbo->handle->data[plane]);

    if (plane == hbo->handle->numFds)
        return dup(hbo->fd);

    return -1;
}

uint32_t hybris_bo_get_offset(struct gbm_bo *bo, int plane)
{
//   printf("[libgbm-hybris] gbm_bo_get_offset called\n");
   return 0;
}

struct gbm_surface *hybris_gbm_surface_create_with_modifiers(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, const uint64_t *modifiers, const unsigned int count){
   fprintf(stderr, "[libgbm-hybris] gbm_surface_create_with_modifiers\n");
   if ((count && !modifiers) || (modifiers && !count)) {
      errno = EINVAL;
      return NULL;
   }

   return hybris_gbm_surface_create(gbm, width, height, format, 0, modifiers, count);
}

struct gbm_surface *hybris_gbm_surface_create(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, uint32_t flags, const uint64_t *modifiers, const unsigned count) {
    struct gbm_hybris_surface *surf;
    uint32_t canon_format = format;

    fprintf(stderr, "[libgbm-hybris] gbm_surface_create called with width: %u, height: %u, format: %u, flags: %u\n", width, height, format, flags);

    surf = calloc(1, sizeof *surf);
    if (surf == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    if (core && core->v0.format_canonicalize) {
        canon_format = core->v0.format_canonicalize(format);
    }

    surf->base.gbm = gbm;
    surf->base.v0.width = width;
    surf->base.v0.height = height;
    surf->base.v0.format = canon_format;
    surf->base.v0.flags = flags;
    surf->base.v0.modifiers = NULL;
    surf->base.v0.count = 0;

    if (count) {
	// Force linear
        surf->base.v0.modifiers = calloc(1, sizeof(uint64_t));
        if (!surf->base.v0.modifiers) {
            errno = ENOMEM;
            free(surf);
            return NULL;
        }
        surf->base.v0.modifiers[0] = DRM_FORMAT_MOD_LINEAR;
        surf->base.v0.count = 1;
    }

    return &surf->base;
}

void hybris_gbm_bo_unmap(struct gbm_bo* bo, void* map_data) {
//TBD: Implement using gralloc unlock
//    printf("[libgbm-hybris] gbm_bo_unmap called\n");
    if (map_data) {
        free(map_data);
    }
}

int hybris_gbm_bo_write(struct gbm_bo *bo, const void *buf, size_t count){
    return 0;
}

char *hybris_gbm_format_get_name(uint32_t gbm_format, struct gbm_format_name_desc *desc)
{
//TBD
   //gbm_format = gbm_format_canonicalize(gbm_format);
//   printf("[libgbm-hybris] gbm_format_get_name called\n");
   desc->name[0] = 0;
   desc->name[1] = 0;
   desc->name[2] = 0;
   desc->name[3] = 0;
   desc->name[4] = 0;

   return desc->name;
}

static struct gbm_device *hybris_device_create(int fd, uint32_t gbm_backend_version){
  //  printf("[libgbm-hybris] hybris_device_create called\n");
    struct gbm_device *device;

    if (gbm_backend_version != GBM_BACKEND_ABI_VERSION) {
        fprintf(stderr, "Wrong gbm version, built for: %d current: %d\n", GBM_BACKEND_ABI_VERSION, gbm_backend_version);
        return NULL;
    }

    device = calloc(1, sizeof *device);
    if (!device)
       return NULL;

   device->dummy = gbm_device_hybris;
   device->v0.fd = fd;
   device->v0.backend_version = gbm_backend_version;
   device->v0.bo_create = hybris_gbm_bo_create;
   device->v0.bo_destroy = hybris_gbm_bo_destroy;
   device->v0.destroy = hybris_gbm_device_destroy;
   device->v0.bo_get_handle = hybris_gbm_bo_get_handle_for_plane;
   device->v0.bo_get_stride = hybris_gbm_bo_get_stride;
   device->v0.bo_get_modifier = hybris_gbm_bo_get_modifier;
   device->v0.bo_get_planes = hybris_gbm_bo_get_plane_count;
   device->v0.bo_get_plane_fd = hybris_gbm_bo_get_fd_for_plane;
   device->v0.surface_create = hybris_gbm_surface_create;
   device->v0.surface_destroy = hybris_gbm_surface_destroy;
   device->v0.surface_lock_front_buffer = hybris_gbm_surface_lock_front_buffer;
   device->v0.surface_release_buffer = hybris_gbm_surface_release_buffer;
   device->v0.surface_has_free_buffers = hybris_gbm_surface_has_free_buffers;
   device->v0.bo_get_offset = hybris_bo_get_offset;
   device->v0.bo_write = hybris_gbm_bo_write;
   return device;
}

struct gbm_backend gbm_hybris_backend = {
   .v0.backend_version = GBM_BACKEND_ABI_VERSION,
   .v0.backend_name = "hybris",
   .v0.create_device = hybris_device_create,
};

struct gbm_backend * gbmint_get_backend(const struct gbm_core *gbm_core);

struct gbm_backend *
gbmint_get_backend(const struct gbm_core *gbm_core) {
   core = gbm_core;
   return &gbm_hybris_backend;
};
