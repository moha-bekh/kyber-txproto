#ifndef IOSYS_DRM_GL
#define IOSYS_DRM_GL

#include "iosys_drm_wrapper.h"
#include <GL/gl.h>
#include <EGL/egl.h>
#include <stdbool.h>

typedef struct DrmGL DrmGL;

DrmGL* drmgl_alloc(void);
int drmgl_init(DrmGL* gl, void* log, int card_fd);
void drmgl_release(DrmGL* gl);

int drmgl_bind_dmatexture(DrmGL* gl, const FbWrapper* buffer);

/* Set swizzle mode: true = swap R/B, false = pass through */
void drmgl_set_swizzle(DrmGL* gl, bool bgra_swizzle);

/* Set crop region for multi-monitor support.
 * Adjusts texture coordinates to sample only the specified sub-region.
 * Pass 0,0,0,0 to reset to full texture (no crop). */
void drmgl_set_crop(DrmGL* gl, unsigned int fb_w, unsigned int fb_h,
                    unsigned int crop_x, unsigned int crop_y,
                    unsigned int crop_w, unsigned int crop_h);

/* Initialize output: allocates render targets at the given dimensions.
 * If dmabuf_pool is true, uses GBM buffer objects for zero-copy DMA-BUF
 * export. Otherwise uses a plain GL texture. */
int drmgl_resize_output(DrmGL* gl, unsigned int width, unsigned int height,
                        bool dmabuf_pool);

/* Acquire the next free output buffer and bind it as GL render target.
 * Returns a buffer handle (>=0) on success, -1 if all buffers are busy. */
int drmgl_acquire_output(DrmGL* gl);

/* Release a previously acquired output buffer for reuse. */
void drmgl_release_output(DrmGL* gl, int handle);

/* Get a dup'd DMA-BUF fd for the given output buffer. Caller must close().
 * Only valid in dmabuf_pool mode. Returns -1 on error. */
int drmgl_get_output_fd(DrmGL* gl, int handle);

/* Get stride of the given output buffer. Only valid in dmabuf_pool mode. */
uint32_t drmgl_get_output_stride(DrmGL* gl, int handle);

GLuint drmgl_get_backtexture(const DrmGL* gl);
EGLDisplay drmgl_get_egl_display(const DrmGL* gl);
EGLContext drmgl_get_egl_context(const DrmGL* gl);

int drmgl_draw(DrmGL* gl);


#endif /* IOSYS_DRM_GL */
