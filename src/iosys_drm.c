/*
 * This file is part of txproto.
 *
 * txproto is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * txproto is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with txproto; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "../config.h"

#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <libgen.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>

#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#ifdef HAVE_VAAPI_DRM
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#endif

#include "iosys_common.h"
#include "os_compat.h"
#include "utils.h"
#include "libtxproto/log.h"
#include "metrics.h"

#include "ctrl_template.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <stdatomic.h>
#include <fcntl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "iosys_drm_wrapper.h"
#include "iosys_drm_gl.h"

const IOSysAPI src_drm;

typedef struct {
    //this should be the first member
    SPClass* class;

    /* Sinks list */
    SPBufferList* entries;
    SPBufferList* events;

    pthread_t updateThread;
    pthread_mutex_t update_mutex;
    atomic_int quit;

    struct tx_metrics *metrics;
} DRMGrabCtx;

typedef struct {
    DRMGrabCtx* ctx;
    char* node_path;
    int32_t plane;
} DRMGrabPriv;

static void destroy_entry(void *opaque, uint8_t *data)
{
    IOSysEntry *entry = (IOSysEntry *)data;
    DRMGrabPriv* priv = entry->api_priv;
    if (priv) {
        free(priv->node_path);
        free(priv);
    }
    sp_class_free(entry);
    av_free(entry);
}

static int drmctx_update_or_create_entry(
    DRMGrabCtx *ctx,
    DrmCardPath* card_path, DrmCard* card, uint32_t plane_id,
    int x, int y,
    int width, int height,
    SPRotation rotation, bool is_default
) {
    IOSysEntry *entry = NULL;
    bool change = false;
    bool new_entry = false;

    uint32_t id = (card_path->number & 0xFF) | plane_id << 8;

    AVBufferRef* entry_ref = sp_bufferlist_ref(
        ctx->entries,
        sp_bufferlist_iosysentry_by_id,
        &id);

    if (!entry_ref) {
        entry = (IOSysEntry*)av_mallocz(sizeof(*entry));
        DRMGrabPriv* priv = (DRMGrabPriv*)av_mallocz(sizeof(*priv));
        priv->ctx = ctx;
        priv->node_path = strdup(card_path->path);
        priv->plane = plane_id;

        char* name = NULL;
        char* connector_name = drm_card_get_connector_name(card, plane_id);
        if (connector_name) {
            name = connector_name;
        } else {
            if (asprintf(&name, "card%i:%i", card_path->number, plane_id) < 0) {
                name = NULL;
            }
        }

        sp_class_alloc(entry, NULL, SP_TYPE_VIDEO_BIDIR, ctx);
        sp_class_set_name(entry, name);

        sp_log(ctx, SP_LOG_INFO, "DRM: Detected screen '%s' at (%d,%d) %dx%d on %s plane %u%s\n",
               name, x, y, width, height, card_path->path, plane_id,
               is_default ? " (default)" : "");

        free(name);

        entry->identifier = id;
        entry->api_id = id;
        entry->type = SP_IO_TYPE_VIDEO_DISPLAY;
        entry->frames = sp_frame_fifo_create(entry, 0, 0);
        entry->api_priv = priv;

        new_entry = true;
    } else {
        entry = (IOSysEntry *)entry_ref->data;
    }

    change = (entry->x != x) ||
        (entry->y != y) ||
        (entry->width != width) ||
        (entry->height != height) ||
        (entry->rotation != rotation);

    /* Framerate will be known only at capture start */
    entry->framerate = av_make_q(0, 0);
    entry->scale = 1;
    entry->x = x;
    entry->y = y;
    entry->width = width;
    entry->height = height;
    entry->rotation = rotation;
    entry->is_default = is_default;

    if (new_entry) {
        AVBufferRef *buf = av_buffer_create((uint8_t *)entry, sizeof(*entry), destroy_entry, NULL, 0);

        sp_bufferlist_append_noref(ctx->entries, buf);
    } else {
        if (change) {
            sp_eventlist_dispatch(entry, ctx->events,
                                    (SPEventType)(SP_EVENT_ON_CHANGE |
                                    SP_EVENT_TYPE_SOURCE |
                                    SP_EVENT_TYPE_SINK),
                                    entry);
        }
        av_buffer_unref(&entry_ref);
    }
    return 0;
}


static void drmctx_update_entries(DRMGrabCtx *ctx) {
    int ret;

    DrmCardPath* card_path_list = list_drm_card_paths();
    if (!card_path_list)
        return;

    pthread_mutex_lock(&ctx->update_mutex);

    bool is_default = true;
    for (int card_idx = 0; card_path_list[card_idx].path != NULL; ++card_idx) {
        const char* card_path = card_path_list[card_idx].path;
        DrmCard* card = drm_card_create();
        ret = drm_card_init(card, card_path);
        if (ret < 0) {
            goto loop_card;
        }
        for(int plane_idx = 0; plane_idx < card->planes->count_planes; ++plane_idx) {
            uint32_t plane_id = card->planes->planes[plane_idx];
            FbWrapper* fb = NULL;
            PlaneWrapper* plane = drm_card_get_plane(card, plane_id);
            if (!plane)
                continue;

            if (plane->m_id == 0) {
                sp_log(ctx, SP_LOG_DEBUG, "plane %i is invalid, skip\n", plane_id);
                goto loop_plane;
            }

            if (plane_wrapper_is_cursor(plane)) {
                goto loop_plane;
            }

            fb = plane_wrapper_get_frame_buffer(plane);
            if (!fb) {
                goto loop_plane;
            }

            if (fb->handles[0] == 0) {
                sp_log(ctx, SP_LOG_DEBUG, "plane %i fb has no handle, skip\n", plane_id);
                goto loop_plane;
            }

            if (fb_wrapper_map_dma_buffer(fb) != 0) {
                sp_log(ctx, SP_LOG_DEBUG, "plane fail to map frame buffer from plane %i\n", plane_id);
                goto loop_plane;
            }

            //plane is usable
            int entry_x = plane->m_plane->x;
            int entry_y = plane->m_plane->y;
            int entry_w = fb->width;
            int entry_h = fb->height;

            drmModeCrtc* crtc = plane_wrapper_get_crtc(plane);
            if (crtc) {
                if (crtc->mode_valid) {
                    entry_w = crtc->mode.hdisplay;
                    entry_h = crtc->mode.vdisplay;
                    entry_x = crtc->x;
                    entry_y = crtc->y;
                }
                drmModeFreeCrtc(crtc);
            }

            drmctx_update_or_create_entry(
                ctx,
                &card_path_list[card_idx], card, plane_id,
                entry_x, entry_y, entry_w, entry_h,
                ROTATION_IDENTITY, is_default
                );

            is_default = false;

        loop_plane:
            if (fb)
                fb_wrapper_destroy(fb);
            if (plane)
                plane_wrapper_destroy(plane);
        }

    loop_card:
        drm_card_destroy(card);
    }
    free_drm_card_paths(card_path_list);
    pthread_mutex_unlock(&ctx->update_mutex);
}

static void* drmctx_update_sources_thread(void *s)
{
    DRMGrabCtx *ctx = s;
    while (!ctx->quit) {
        drmctx_update_entries(ctx);
        av_usleep(1000000);
    }
    return 0;
}

static int drmctx_ctrl(DRMGrabCtx *ctx, SPEventType ctrl, void *arg) {
    int err = 0;
    if (ctrl & SP_EVENT_CTRL_NEW_EVENT) {
        AVBufferRef *event = (AVBufferRef *)arg;
        char *fstr = sp_event_flags_to_str_buf(event);
        sp_log(ctx, SP_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
        av_free(fstr);

        if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
            /* Bring up the new event to speed with current affairs */
            SPBufferList *tmp_event = sp_bufferlist_new();
            err = sp_eventlist_add(ctx, tmp_event, event, 1);
            if (err < 0) {
                sp_bufferlist_free(&tmp_event);
                return err;
            }

            drmctx_update_entries(ctx);

            AVBufferRef *obj = NULL;
            while ((obj = sp_bufferlist_iter_ref(ctx->entries))) {
                sp_eventlist_dispatch(obj->data, tmp_event,
                                    (SPEventType) (SP_EVENT_ON_CHANGE | SP_EVENT_TYPE_SOURCE), obj->data);
                av_buffer_unref(&obj);
            }

            sp_bufferlist_free(&tmp_event);
        }

        /* Add it to the list now to receive events dynamically */
        err = sp_eventlist_add(ctx, ctx->events, event, 1);
        if (err < 0)
            return err;
    }
    return 0;
}

static AVBufferRef* drmctx_refEntry(DRMGrabCtx* ctx, uint32_t identifier) {
    return sp_bufferlist_ref(ctx->entries, sp_bufferlist_iosysentry_by_id, (void*)&identifier);
}

static void drmctx_release(DRMGrabCtx* ctx) {
    if (ctx->class) {
        atomic_store(&ctx->quit, 1);
        pthread_join(ctx->updateThread, NULL);
        pthread_mutex_destroy(&ctx->update_mutex);

        if (ctx->events)
            sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, ctx);
        sp_class_free(ctx);
    }
    sp_bufferlist_free(&ctx->entries);
    av_free(ctx);
}

static int drmctx_init(DRMGrabCtx *ctx) {
    int err = sp_class_alloc(ctx, src_drm.name, SP_TYPE_CONTEXT, NULL);
    if (err < 0)
        return err;

    ctx->entries = sp_bufferlist_new();
    if (!ctx->entries) {
        sp_log(ctx, SP_LOG_ERROR, "sp_bufferlist_new() failed at line %d\n", __LINE__);
        return AVERROR(ENOMEM);
    }
    ctx->events = sp_bufferlist_new();
    if (!ctx->events) {
        sp_log(ctx, SP_LOG_ERROR, "sp_bufferlist_new() failed at line %d\n", __LINE__);
        return AVERROR(ENOMEM);
    }

    pthread_mutex_init(&ctx->update_mutex, NULL);
    pthread_create(&ctx->updateThread, NULL, &drmctx_update_sources_thread, ctx);

    return 0;
}

// plugin entry points

static int drmgrab_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    DRMGrabCtx *ctx = (DRMGrabCtx *)ctx_ref->data;
    return drmctx_ctrl(ctx, ctrl, arg);
}

static void drmgrab_uninit(void *opaque, uint8_t *data)
{
    DRMGrabCtx *ctx = (DRMGrabCtx *)data;
    drmctx_release(ctx);
}

static int drmgrab_init(AVBufferRef **s)
{
    int err = 0;
    DRMGrabCtx *ctx = av_mallocz(sizeof(*ctx));

    if (!ctx) {
        sp_log(NULL, SP_LOG_ERROR, "av_mallocz() failed at line %d\n", __LINE__);
        return AVERROR(ENOMEM);
    }
    ctx->metrics = NULL;

    err = drmctx_init(ctx);
    if (err != 0) {
        sp_log(ctx, SP_LOG_ERROR, "drmctx_init() failed: %d\n", err);
        drmctx_release(ctx);
        return err;
    }

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            drmgrab_uninit, NULL, 0);
    if (!ctx_ref) {
        sp_log(ctx, SP_LOG_ERROR, "av_buffer_create() failed at line %d\n", __LINE__);
        drmctx_release(ctx);
        return AVERROR(ENOMEM);
    }

    *s = ctx_ref;
    return 0;
}

static AVBufferRef *drmgrab_ref_entry(AVBufferRef *ctx_ref, uint32_t identifier)
{
    DRMGrabCtx *ctx = (DRMGrabCtx *)ctx_ref->data;
    return drmctx_refEntry(ctx, identifier);
}


////// capture

typedef struct
{
    // this should be the first member
    SPClass* class;

    pthread_t captureThread;
    atomic_int quit;
    int64_t epoch;

    DRMGrabCtx* ctx;
    AVBufferRef* hw_device_context;
    AVBufferRef* hw_frames_ctx;
    AVRational framerate;
    bool got_first_frame;
    int dropped_frames;
    int width;
    int height;

    /* CRTC region to crop from the framebuffer */
    int crtc_x;
    int crtc_y;
    int crtc_w;
    int crtc_h;

    struct {
        DrmCard* card;
        PlaneWrapper* plane;
        FbWrapper* fb;
        uint32_t plane_id;
    } drm;
    DrmGL* gl;

    AVDictionary *opts;  // Configuration options
} DRMGrabCapture;


static void drmcap_releaseTexturePool(DRMGrabCapture* cap)
{
    if (cap->hw_frames_ctx)
        av_buffer_unref(&cap->hw_frames_ctx);
}

static int drmcap_allocateTexturePool(DRMGrabCapture* cap, size_t width, size_t height)
{
    DRMGrabCtx* ctx = cap->ctx;
    int err = 0;

    if (width == 0 || height == 0) {
        sp_log(ctx, SP_LOG_ERROR, "invalid texture size %zux%zu\n", width, height);
        return AVERROR(EINVAL);
    }

    // If no hw_device_context (software fallback), skip hw_frames_ctx allocation
    if (!cap->hw_device_context) {
        sp_log(ctx, SP_LOG_DEBUG, "No hw_device_context, using software path\n");
        return 0;
    }

    /* Init av_hwframe_ctx */
    AVBufferRef* hw_frames_ctx = av_hwframe_ctx_alloc(cap->hw_device_context);
    if (!hw_frames_ctx) {
        sp_log(ctx, SP_LOG_ERROR, "av_hwframe_ctx_alloc() failed at line %d\n", __LINE__);
        return AVERROR(ENOMEM);
    }

    AVHWFramesContext *hw_frames_ctx_cfg = (AVHWFramesContext *)hw_frames_ctx->data;
    hw_frames_ctx_cfg->format = AV_PIX_FMT_VAAPI;
    hw_frames_ctx_cfg->sw_format = AV_PIX_FMT_BGRA;
    hw_frames_ctx_cfg->width = width;
    hw_frames_ctx_cfg->height = height;

    err = av_hwframe_ctx_init(hw_frames_ctx);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "av_hwframe_ctx_init() failed: %d\n", err);
        av_buffer_unref(&hw_frames_ctx);
        return err;
    }

    cap->hw_frames_ctx = hw_frames_ctx;

    return 0;
}

#ifdef HAVE_VAAPI_DRM
static int drm_find_render_node(const char* primary_node, char* render_node,
                                 size_t max_len, DRMGrabCtx* ctx)
{
    char card_name[256];
    char card_sysfs_path[PATH_MAX];
    char card_device_path[PATH_MAX];
    char render_device_path[PATH_MAX];
    char render_sysfs_path[PATH_MAX];
    DIR* drm_dir;
    struct dirent* entry;
    ssize_t len;

    if (!primary_node || !render_node || max_len < 16) {
        sp_log(ctx, SP_LOG_ERROR, "Invalid arguments to drm_find_render_node\n");
        return -EINVAL;
    }

    // Extract card name from path (e.g., "card1" from "/dev/dri/card1")
    char primary_node_copy[PATH_MAX];
    strncpy(primary_node_copy, primary_node, sizeof(primary_node_copy) - 1);
    primary_node_copy[sizeof(primary_node_copy) - 1] = '\0';
    strncpy(card_name, basename(primary_node_copy), sizeof(card_name) - 1);
    card_name[sizeof(card_name) - 1] = '\0';

    snprintf(card_sysfs_path, sizeof(card_sysfs_path),
             "/sys/class/drm/%s/device", card_name);

    len = readlink(card_sysfs_path, card_device_path, sizeof(card_device_path) - 1);
    if (len < 0) {
        sp_log(ctx, SP_LOG_WARN, "Cannot resolve %s: %s\n",
                        card_sysfs_path, strerror(errno));
        return -EIO;
    }
    card_device_path[len] = '\0';

    sp_log(ctx, SP_LOG_DEBUG, "Primary node %s resolves to PCI device: %s\n",
                    primary_node, card_device_path);

    drm_dir = opendir("/sys/class/drm");
    if (!drm_dir) {
        sp_log(ctx, SP_LOG_ERROR, "Cannot open /sys/class/drm: %s\n",
                        strerror(errno));
        return -EIO;
    }

    int result = -ENODEV;
    while ((entry = readdir(drm_dir)) != NULL) {
        if (strncmp(entry->d_name, "renderD", 7) != 0)
            continue;

        snprintf(render_sysfs_path, sizeof(render_sysfs_path),
                 "/sys/class/drm/%s/device", entry->d_name);

        len = readlink(render_sysfs_path, render_device_path,
                      sizeof(render_device_path) - 1);
        if (len < 0) {
            sp_log(ctx, SP_LOG_DEBUG, "Cannot resolve %s: %s\n",
                            render_sysfs_path, strerror(errno));
            continue;
        }
        render_device_path[len] = '\0';

        if (strcmp(card_device_path, render_device_path) == 0) {
            snprintf(render_node, max_len, "/dev/dri/%s", entry->d_name);
            sp_log(ctx, SP_LOG_INFO, "Found matching render node: %s\n", render_node);
            result = 0;
            break;
        }
    }

    closedir(drm_dir);

    if (result != 0) {
        sp_log(ctx, SP_LOG_WARN, "No render node found for primary node %s\n", primary_node);
    }

    return result;
}

static bool vaapi_check_dmabuf_import_support(DRMGrabCapture* cap)
{
    DRMGrabCtx* ctx = cap->ctx;

    if (!cap->hw_device_context) {
        return false;
    }

    AVHWDeviceContext* device_ctx = (AVHWDeviceContext*)cap->hw_device_context->data;
    if (!device_ctx || device_ctx->type != AV_HWDEVICE_TYPE_VAAPI) {
        sp_log(ctx, SP_LOG_INFO, "Not a VAAPI device context\n");
        return false;
    }

    AVVAAPIDeviceContext* vaapi_ctx = (AVVAAPIDeviceContext*)device_ctx->hwctx;
    if (!vaapi_ctx) {
        sp_log(ctx, SP_LOG_INFO, "VAAPI device context is NULL\n");
        return false;
    }

    VADisplay va_display = vaapi_ctx->display;

    // Create a temporary config to query surface attributes
    VAConfigID config_id;
    VAStatus va_status = vaCreateConfig(va_display, VAProfileH264Main, VAEntrypointEncSlice,
                                        NULL, 0, &config_id);
    if (va_status == VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT) {
        va_status = vaCreateConfig(va_display, VAProfileH264Main, VAEntrypointEncSliceLP,
                                    NULL, 0, &config_id);
        if (va_status != VA_STATUS_SUCCESS) {
            sp_log(ctx, SP_LOG_ERROR, "vaCreateConfig failed: 0x%x\n", va_status);
            return false;
        }
    }

    // Query surface attributes
    unsigned int num_attribs = 0;
    va_status = vaQuerySurfaceAttributes(va_display, config_id, NULL, &num_attribs);
    if (va_status != VA_STATUS_SUCCESS || num_attribs == 0) {
        sp_log(ctx, SP_LOG_ERROR, "vaQuerySurfaceAttributes count failed: 0x%x\n", va_status);
        vaDestroyConfig(va_display, config_id);
        return false;
    }

    VASurfaceAttrib* attribs = malloc(num_attribs * sizeof(VASurfaceAttrib));
    if (!attribs) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to allocate memory for VAAPI surface attributes\n");
        vaDestroyConfig(va_display, config_id);
        return false;
    }

    va_status = vaQuerySurfaceAttributes(va_display, config_id, attribs, &num_attribs);
    if (va_status != VA_STATUS_SUCCESS) {
        sp_log(ctx, SP_LOG_ERROR, "vaQuerySurfaceAttributes failed: 0x%x\n", va_status);
        free(attribs);
        vaDestroyConfig(va_display, config_id);
        return false;
    }

    bool dmabuf_supported = false;
    for (unsigned int i = 0; i < num_attribs; i++) {
        if (attribs[i].type == VASurfaceAttribMemoryType) {
            if (attribs[i].value.type == VAGenericValueTypeInteger) {
                unsigned int mem_types = attribs[i].value.value.i;
                if (mem_types & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
                    sp_log(ctx, SP_LOG_INFO, "VAAPI supports DMA-BUF import (DRM_PRIME_2)\n");
                    dmabuf_supported = true;
                    break;
                }
                if (mem_types & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) {
                    sp_log(ctx, SP_LOG_INFO, "VAAPI supports DMA-BUF import (DRM_PRIME)\n");
                    dmabuf_supported = true;
                    break;
                }
            }
        }
    }

    free(attribs);
    vaDestroyConfig(va_display, config_id);

    if (!dmabuf_supported) {
        sp_log(ctx, SP_LOG_WARN, "VAAPI does not support DMA-BUF import\n");
    }

    return dmabuf_supported;
}

static int vaapi_init_vaapi(DRMGrabCapture* cap, const char* drm_device)
{
    DRMGrabCtx* ctx = cap->ctx;
    int err = AVERROR(ENODEV);
    char render_node_path[PATH_MAX];

    // Check if the provided device is a primary node (card*), and try to find its render node
    char drm_device_copy[PATH_MAX];
    strncpy(drm_device_copy, drm_device, sizeof(drm_device_copy) - 1);
    drm_device_copy[sizeof(drm_device_copy) - 1] = '\0';
    const char* base_name = basename(drm_device_copy);
    if (strncmp(base_name, "card", 4) == 0) {
        sp_log(ctx, SP_LOG_INFO, "Provided device %s is a primary node, searching for matching render node...\n",
               drm_device);

        err = drm_find_render_node(drm_device, render_node_path, sizeof(render_node_path), ctx);
        if (err == 0) {
            sp_log(ctx, SP_LOG_INFO, "Trying VAAPI on matched render node: %s\n", render_node_path);

            err = av_hwdevice_ctx_create(&cap->hw_device_context, AV_HWDEVICE_TYPE_VAAPI,
                                          render_node_path, NULL, 0);
            if (err >= 0) {
                sp_log(ctx, SP_LOG_INFO, "VAAPI initialized successfully on %s\n", render_node_path);
                goto success;
            }

            sp_log(ctx, SP_LOG_WARN, "Failed to create VAAPI device context on matched render node %s: %s\n",
                   render_node_path, av_err2str(err));
        } else {
            sp_log(ctx, SP_LOG_WARN, "Could not find matching render node for %s\n", drm_device);
        }
    }

    // Fallback: try to create VAAPI device context with the primary node directly
    if (!cap->hw_device_context) {
        sp_log(ctx, SP_LOG_INFO, "Attempting VAAPI on primary node: %s\n", drm_device);
        err = av_hwdevice_ctx_create(&cap->hw_device_context, AV_HWDEVICE_TYPE_VAAPI,
                                      drm_device, NULL, 0);
        if (err >= 0) {
            sp_log(ctx, SP_LOG_INFO, "VAAPI initialized successfully on %s\n", drm_device);
        } else {
            sp_log(ctx, SP_LOG_ERROR, "Failed to create VAAPI device context on %s: %s\n",
                   drm_device, av_err2str(err));
            return err;
        }
    }

success:
    if (!cap->hw_device_context) {
        sp_log(ctx, SP_LOG_ERROR, "VAAPI device context not initialized\n");
        return AVERROR(ENODEV);
    }

    // Check DMA-BUF import support (required for DRM framebuffer import)
    if (!vaapi_check_dmabuf_import_support(cap)) {
        sp_log(ctx, SP_LOG_WARN, "VAAPI: DMA-BUF import not supported\n");
        return AVERROR(EINVAL);
    }

    sp_log(ctx, SP_LOG_INFO, "VAAPI: DRM capture initialized, RGB frames output\n");
    sp_log(ctx, SP_LOG_INFO, "VAAPI: Color conversion handled by filter chain\n");

    return 0;
}



/* Cleanup callback for GL output buffer + VAAPI surface */
typedef struct {
    DrmGL *gl;
    int gl_buffer_handle;
    VADisplay va_display;
    VASurfaceID va_surface;
} GLVaapiFrameCleanup;

static void gl_vaapi_frame_cleanup(void *opaque, uint8_t *data)
{
    GLVaapiFrameCleanup *cleanup = (GLVaapiFrameCleanup *)opaque;
    if (cleanup) {
        if (cleanup->va_display && cleanup->va_surface != VA_INVALID_SURFACE)
            vaDestroySurfaces(cleanup->va_display, &cleanup->va_surface, 1);
        if (cleanup->gl)
            drmgl_release_output(cleanup->gl, cleanup->gl_buffer_handle);
        av_free(cleanup);
    }
}

/* Import a GL output DMA-BUF as a VAAPI surface and wrap in AVFrame */
static int vaapi_import_gl_output(DRMGrabCapture* cap, int gl_handle, AVFrame* frame)
{
    DRMGrabCtx* ctx = cap->ctx;

    AVHWDeviceContext* device_ctx = (AVHWDeviceContext*)cap->hw_device_context->data;
    AVVAAPIDeviceContext* vaapi_ctx = (AVVAAPIDeviceContext*)device_ctx->hwctx;
    VADisplay va_display = vaapi_ctx->display;

    int dma_fd = drmgl_get_output_fd(cap->gl, gl_handle);
    uint32_t stride = drmgl_get_output_stride(cap->gl, gl_handle);
    if (dma_fd < 0) {
        sp_log(ctx, SP_LOG_ERROR, "drmgl_get_output_fd failed\n");
        return AVERROR_EXTERNAL;
    }

    int eff_w = cap->width;
    int eff_h = cap->height;

    /* Build DRM PRIME descriptor for the GL output buffer */
    VADRMPRIMESurfaceDescriptor prime_desc = {0};
    prime_desc.fourcc = VA_FOURCC_BGRA;
    prime_desc.width = eff_w;
    prime_desc.height = eff_h;
    prime_desc.num_objects = 1;
    prime_desc.num_layers = 1;
    prime_desc.objects[0].fd = dma_fd;
    prime_desc.objects[0].size = stride * eff_h;
    prime_desc.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    prime_desc.layers[0].drm_format = DRM_FORMAT_ARGB8888;
    prime_desc.layers[0].num_planes = 1;
    prime_desc.layers[0].object_index[0] = 0;
    prime_desc.layers[0].offset[0] = 0;
    prime_desc.layers[0].pitch[0] = stride;

    VASurfaceAttrib attribs[2];
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &prime_desc;

    VASurfaceID va_surface = VA_INVALID_SURFACE;
    VAStatus va_status = vaCreateSurfaces(va_display, VA_RT_FORMAT_RGB32,
                                          eff_w, eff_h, &va_surface, 1,
                                          attribs, 2);
    if (va_status != VA_STATUS_SUCCESS) {
        sp_log(ctx, SP_LOG_ERROR, "vaCreateSurfaces from GL output failed: 0x%x\n", va_status);
        close(dma_fd);
        return AVERROR_EXTERNAL;
    }

    /* The VAAPI driver dups the fd internally, we can close ours */
    close(dma_fd);

    /* Set up cleanup: releases GL buffer + destroys VAAPI surface when frame is freed */
    GLVaapiFrameCleanup *cleanup = av_mallocz(sizeof(*cleanup));
    if (!cleanup) {
        vaDestroySurfaces(va_display, &va_surface, 1);
        return AVERROR(ENOMEM);
    }
    cleanup->gl = cap->gl;
    cleanup->gl_buffer_handle = gl_handle;
    cleanup->va_display = va_display;
    cleanup->va_surface = va_surface;

    frame->buf[0] = av_buffer_create(NULL, 0, gl_vaapi_frame_cleanup, cleanup, 0);
    if (!frame->buf[0]) {
        av_free(cleanup);
        vaDestroySurfaces(va_display, &va_surface, 1);
        return AVERROR(ENOMEM);
    }

    frame->format = AV_PIX_FMT_VAAPI;
    frame->width = eff_w;
    frame->height = eff_h;
    frame->data[3] = (uint8_t*)(uintptr_t)va_surface;
    frame->hw_frames_ctx = av_buffer_ref(cap->hw_frames_ctx);
    if (!frame->hw_frames_ctx) {
        av_frame_unref(frame);
        return AVERROR(ENOMEM);
    }

    return 0;
}
#endif /* HAVE_VAAPI_DRM */

static int drmcap_init_drm(DRMGrabCapture* cap, const char* cardname, uint32_t plane_id) {
    int ret;

    DRMGrabCtx* ctx = cap->ctx;

    //Open DRM card and search for our plan

    DrmCard* card = drm_card_create();
    ret = drm_card_init(card, cardname);
    if (ret != 0) {
        drm_card_destroy(card);
        return ret;
    }

    sp_log(ctx, SP_LOG_INFO, "found %i drm planes\n", card->planes->count_planes);
    //check that plane_id is a valid plane
    bool found  = false;
    for (int i = 0; i < card->planes->count_planes; ++i) {
        if (card->planes->planes[i] == plane_id) {
            found = true;
            break;
        }
    }

    if (!found) {
        sp_log(ctx, SP_LOG_ERROR, "plane %i not found\n", plane_id);
        drm_card_destroy(card);
        return -1;
    }

    cap->drm.plane_id = plane_id;
    cap->drm.card = card;

    return 0;
}


static int drmcap_start_capture(DRMGrabCapture* cap, DRMGrabPriv* priv) {
    int ret;
    DRMGrabCtx* ctx = cap->ctx;

    ret = drmcap_init_drm(cap, priv->node_path, priv->plane);
    if (ret != 0) {
        sp_log(ctx, SP_LOG_ERROR, "drmcap_init_drm() failed %d\n", ret);
        return -1;
    }

    /* GL is used for DMA-BUF import + crop */
    cap->gl = drmgl_alloc();
    ret = drmgl_init(cap->gl, cap, cap->drm.card->fd);
    if (ret != 0) {
        sp_log(ctx, SP_LOG_ERROR, "GL initialization failed %d\n", ret);
        return AVERROR(ENOTSUP);
    }
    drmgl_set_swizzle(cap->gl, false);

#ifdef HAVE_VAAPI_DRM
    ret = vaapi_init_vaapi(cap, priv->node_path);
    if (ret != 0)
        return ret;
#else
    sp_log(ctx, SP_LOG_ERROR, "VAAPI support not compiled in\n");
    return AVERROR(ENOSYS);
#endif

    PlaneWrapper* plane = drm_card_get_plane(cap->drm.card, priv->plane);
    if (plane) {
        drmModeCrtc* crtc = plane_wrapper_get_crtc(plane);
        if (crtc && crtc->mode_valid) {
            cap->framerate = av_make_q(crtc->mode.vrefresh, 1);
            cap->crtc_x = crtc->x;
            cap->crtc_y = crtc->y;
            cap->crtc_w = crtc->mode.hdisplay;
            cap->crtc_h = crtc->mode.vdisplay;
            sp_log(ctx, SP_LOG_INFO, "Display refresh rate: %d Hz, CRTC region: (%d,%d) %dx%d\n",
                   crtc->mode.vrefresh, cap->crtc_x, cap->crtc_y, cap->crtc_w, cap->crtc_h);
        } else {
            cap->framerate = av_make_q(60, 1);
            sp_log(ctx, SP_LOG_WARN, "Could not get refresh rate, defaulting to 60 Hz\n");
        }
        if (crtc)
            drmModeFreeCrtc(crtc);
        plane_wrapper_destroy(plane);
    } else {
        cap->framerate = av_make_q(60, 1);
        sp_log(ctx, SP_LOG_WARN, "Could not get plane, defaulting to 60 Hz\n");
    }

    cap->got_first_frame = false;
    return 0;
}

static int drmcap_stop_capture(DRMGrabCapture* cap) {
    DRMGrabCtx* ctx = cap->ctx;

    drmcap_releaseTexturePool(cap);

    if (cap->hw_device_context) {
        av_buffer_unref(&cap->hw_device_context);
        cap->hw_device_context = NULL;
    }

    if (cap->drm.fb) {
        fb_wrapper_destroy(cap->drm.fb);
        cap->drm.fb = NULL;
    }

    if (cap->drm.plane) {
        plane_wrapper_destroy(cap->drm.plane);
        cap->drm.plane = NULL;
    }

    if (cap->drm.card) {
        drm_card_destroy(cap->drm.card);
        cap->drm.card = NULL;
    }

    if (cap->gl) {
        drmgl_release(cap->gl);
        cap->gl = NULL;
    }

    if (cap->opts) {
        av_dict_free(&cap->opts);
        cap->opts = NULL;
    }

    sp_log(ctx, SP_LOG_DEBUG, "DRM capture resources released\n");

    return 0;
}


/* Get the effective output dimensions: CRTC region if set, otherwise full FB */
static void drmcap_get_effective_size(const DRMGrabCapture *cap, const FbWrapper *fb,
                                      int *out_w, int *out_h)
{
    if (cap->crtc_w > 0 && cap->crtc_h > 0) {
        *out_w = cap->crtc_w;
        *out_h = cap->crtc_h;
    } else {
        *out_w = fb->width;
        *out_h = fb->height;
    }
}

/* Configure GL output size and crop for the current framebuffer */
static int drmcap_setup_gl_output(DRMGrabCapture *cap, const FbWrapper *fb,
                                  int eff_w, int eff_h)
{
    int ret = drmgl_resize_output(cap->gl, eff_w, eff_h, true);
    if (ret < 0) {
        sp_log(cap->ctx, SP_LOG_ERROR, "drmgl_resize_output() failed: %d\n", ret);
        return ret;
    }
    /* Set crop if CRTC region differs from full FB */
    if (cap->crtc_w > 0 && cap->crtc_h > 0 &&
        ((int)fb->width != cap->crtc_w || (int)fb->height != cap->crtc_h)) {
        drmgl_set_crop(cap->gl, fb->width, fb->height,
                       cap->crtc_x, cap->crtc_y, cap->crtc_w, cap->crtc_h);
    } else {
        drmgl_set_crop(cap->gl, 0, 0, 0, 0, 0, 0);
    }
    return 0;
}

static int check_resolution_change(DRMGrabCapture *cap, const FbWrapper* fb)
{
    assert(cap->drm.fb);
    DRMGrabCtx* ctx = cap->ctx;
    int ret;

    int eff_w, eff_h;
    drmcap_get_effective_size(cap, fb, &eff_w, &eff_h);

    if (eff_w != cap->width || eff_h != cap->height) {
        drmcap_releaseTexturePool(cap);

        sp_log(ctx, SP_LOG_INFO, "video size changed to %ix%i\n", eff_w, eff_h);

        ret = drmcap_allocateTexturePool(cap, eff_w, eff_h);
        if (ret < 0) {
            sp_log(ctx, SP_LOG_ERROR, "drmcap_allocateTexturePool() failed: %d\n", ret);
            return ret;
        }

        /* Resize GL output and configure crop */
        if (cap->gl) {
            ret = drmcap_setup_gl_output(cap, fb, eff_w, eff_h);
            if (ret < 0)
                return ret;
        }

        cap->width = eff_w;
        cap->height = eff_h;
    }

    return 0;
}

static int drmcap_capture_frame(DRMGrabCapture* cap, IOSysEntry* entry, AVFrame** out_frame) {
    int ret = 0;
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        sp_log(entry, SP_LOG_ERROR, "av_frame_alloc() failed at line %d\n", __LINE__);
        return AVERROR(ENOMEM);
    }

    /* GL path: import DRM FB and draw with crop */
    ret = drmgl_bind_dmatexture(cap->gl, cap->drm.fb);
    if (ret < 0) {
        sp_log(entry, SP_LOG_ERROR, "drmgl_bind_dmatexture() failed: %d\n", ret);
        av_frame_free(&frame);
        return ret;
    }

    int gl_handle = drmgl_acquire_output(cap->gl);
    if (gl_handle < 0) {
        sp_log(entry, SP_LOG_WARN, "drmgl_acquire_output() failed, dropping frame\n");
        av_frame_free(&frame);
        return AVERROR(EAGAIN);
    }

    drmgl_draw(cap->gl);

#ifdef HAVE_VAAPI_DRM
    ret = vaapi_import_gl_output(cap, gl_handle, frame);
    if (ret < 0) {
        drmgl_release_output(cap->gl, gl_handle);
        av_frame_free(&frame);
        sp_log(entry, SP_LOG_ERROR, "vaapi_import_gl_output() failed: %d\n", ret);
        return ret;
    }
    /* gl_handle is now owned by the frame cleanup callback */
#else
    drmgl_release_output(cap->gl, gl_handle);
    av_frame_free(&frame);
    sp_log(entry, SP_LOG_ERROR, "VAAPI support not compiled in\n");
    return AVERROR(ENOSYS);
#endif

    *out_frame = frame;
    return 0;
}

static void drmcap_capture_thread(DRMGrabCapture* cap, IOSysEntry* entry)
{
    DRMGrabPriv* priv = entry->api_priv;
    DRMGrabCtx* ctx = priv->ctx;
    AVFrame* frame = NULL;
    int ret = 0;

    sp_set_thread_name_self(sp_class_get_name(entry));
    sp_eventlist_dispatch(entry, entry->events, (SPEventType)(SP_EVENT_ON_CONFIG | SP_EVENT_ON_INIT), NULL);

    ret = drmcap_start_capture(cap, priv);
    if (ret != 0) {
        sp_log(entry, SP_LOG_ERROR, "fail to start capture %d\n", ret);
        goto fail;
    }

    while (!cap->quit) {
        drmVBlank vbl = {0};
        vbl.request.type = DRM_VBLANK_RELATIVE;
        vbl.request.sequence = 1;

        int res = drmWaitVBlank(cap->drm.card->fd, &vbl);
        if (res != 0) {
            sp_log(cap->ctx, SP_LOG_WARN, "drmWaitVBlank failed: %d\n", res);
            continue;
        }

        if (cap->drm.plane) {
            plane_wrapper_destroy(cap->drm.plane);
            cap->drm.plane = NULL;
        }

        cap->drm.plane = drm_card_get_plane(cap->drm.card, cap->drm.plane_id);
        if (!cap->drm.plane) {
            sp_log(entry, SP_LOG_WARN, "drm_card_get_plane() failed\n");
            continue;
        }

        uint32_t new_fb_id = cap->drm.plane->m_plane->fb_id;
        if (!cap->drm.fb || cap->drm.fb->fb_id != new_fb_id) {
            if (cap->drm.fb) {
                fb_wrapper_destroy(cap->drm.fb);
                cap->drm.fb = NULL;
            }

            cap->drm.fb = plane_wrapper_get_frame_buffer(cap->drm.plane);
            if (!cap->drm.fb) {
                sp_log(entry, SP_LOG_WARN, "plane_wrapper_get_frame_buffer() failed\n");
                continue;
            }

            ret = fb_wrapper_map_dma_buffer(cap->drm.fb);
            if (ret < 0) {
                sp_log(entry, SP_LOG_WARN, "fb_wrapper_map_dma_buffer() failed: %d\n", ret);
                continue;
            }
        }

        if (cap->drm.fb->width == 0 || cap->drm.fb->height == 0) {
            sp_log(entry, SP_LOG_DEBUG, "plane has invalid dimensions %ux%u, skipping\n",
                   cap->drm.fb->width, cap->drm.fb->height);
            continue;
        }

        // Initialize texture pool on first frame
        if (!cap->got_first_frame) {
            int eff_w, eff_h;
            drmcap_get_effective_size(cap, cap->drm.fb, &eff_w, &eff_h);

            sp_log(entry, SP_LOG_INFO, "Initializing texture pool: %dx%d\n", eff_w, eff_h);
            ret = drmcap_allocateTexturePool(cap, eff_w, eff_h);
            if (ret < 0) {
                sp_log(entry, SP_LOG_ERROR, "drmcap_allocateTexturePool() failed: %d\n", ret);
                continue;
            }

            if (cap->gl) {
                ret = drmcap_setup_gl_output(cap, cap->drm.fb, eff_w, eff_h);
                if (ret < 0)
                    continue;
            }

            cap->width = eff_w;
            cap->height = eff_h;
            cap->got_first_frame = true;
            sp_log(entry, SP_LOG_INFO, "Texture pool initialized successfully, hw_frames_ctx=%p\n", cap->hw_frames_ctx);
        }

        ret = check_resolution_change(cap, cap->drm.fb);
        if (ret < 0) {
            sp_log(entry, SP_LOG_WARN, "check_resolution_change() failed: %d\n", ret);
            continue;
        }

        ret = drmcap_capture_frame(cap, entry, &frame);
        if (ret < 0) {
            sp_log(entry, SP_LOG_WARN, "drmcap_capture_frame() failed: %d at %s:%d\n", ret, __FILE__, __LINE__);
            continue;
        }

        int64_t ts = av_gettime_relative();

        frame->pts = ts - cap->epoch;
        frame->time_base = AV_TIME_BASE_Q;
        frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));
        if (!frame->opaque_ref) {
            av_frame_free(&frame);
            continue;
        }

        FormatExtraData *fe = (FormatExtraData *)frame->opaque_ref->data;
        fe->time_base = AV_TIME_BASE_Q;
        fe->avg_frame_rate = cap->framerate;
        fe->rotation = ROTATION_IDENTITY;

        if (ctx->metrics) {
            const tx_metrics_entry metrics[] = {
                { .key = "pts", .value = frame->pts },
                { .key = "acquired", .value = ts },
            };
            tx_metrics_push(ctx->metrics, metrics, SP_ARRAY_ELEMS(metrics));
        }

        ret = sp_frame_fifo_push(entry->frames, frame);
        av_frame_free(&frame);

        if (ret == AVERROR(ENOBUFS)) {
            cap->dropped_frames++;
            SPGenericData entries[] = {
                { "dropped_frames", NULL, &cap->dropped_frames, SP_DATA_TYPE_INT},
                { 0 },
            };
            sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_STATS, entries);
        } else if (ret) {
            sp_log(entry, SP_LOG_ERROR, "Unable to push frame to FIFO: %s!\n", av_err2str(ret));
            break;
        }
    }

fail:
    av_frame_free(&frame);
    drmcap_stop_capture(cap);
    drmcap_releaseTexturePool(cap);

    if (ret < 0)
        sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_ERROR, NULL);

    sp_log(entry, SP_LOG_INFO, "Acquisition stopped\n");
}

static void *drmcap_capture_thread_cb(void *s)
{
    IOSysEntry* entry = s;
    DRMGrabCapture* cap = entry->io_priv;
    drmcap_capture_thread(cap, entry);
    return NULL;
}

static int drmgrab_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx, void *ctx,
                             void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = (SPCtrlTemplateCbCtx*)callback_ctx;

    IOSysEntry *entry = (IOSysEntry*)ctx;
    DRMGrabCapture *cap = (DRMGrabCapture*)entry->io_priv;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        cap->epoch = atomic_load(event->epoch);
        pthread_create(&cap->captureThread, NULL, &drmcap_capture_thread_cb, entry);
        sp_log(entry, SP_LOG_VERBOSE, "Started capture thread\n");
        return 0;
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        atomic_store(&cap->quit, 1);
        pthread_join(cap->captureThread, NULL);
        sp_log(entry, SP_LOG_VERBOSE, "Stopped capture thread\n");
        return 0;
    } else {
        return AVERROR(ENOTSUP);
    }
}

static int drmgrab_ioctx_ctrl(AVBufferRef *entry, SPEventType ctrl, void *arg)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    return sp_ctrl_template(iosys_entry, iosys_entry->events, (SPEventType)0x0,
                            drmgrab_ioctx_ctrl_cb, ctrl, arg);
}

static int drmgrab_init_io(AVBufferRef *ctx_ref, AVBufferRef *entry,
                       AVDictionary *opts)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    DRMGrabPriv *priv = iosys_entry->api_priv;
    DRMGrabCapture *cap_priv = (DRMGrabCapture*)av_mallocz(sizeof(*cap_priv));

    if (!cap_priv)
        return AVERROR(ENOMEM);

    cap_priv->ctx = priv->ctx;

    if (opts) {
        av_dict_copy(&cap_priv->opts, opts, 0);
    }

    iosys_entry->ctrl = drmgrab_ioctx_ctrl;
    iosys_entry->events = sp_bufferlist_new();

    assert(cap_priv->drm.fb == NULL);

    iosys_entry->io_priv = cap_priv;

    return 0;
}

static int drmgrab_set_metrics(AVBufferRef *buffer, struct tx_metrics *metrics)
{
    DRMGrabCtx *ctx = (DRMGrabCtx *)buffer->data;

    assert(!metrics || metrics->cb);
    ctx->metrics = metrics;

    return 0;
}

const IOSysAPI src_drm = {
    .name        = "drm",
    .init_sys    = drmgrab_init,
    .set_metrics = drmgrab_set_metrics,
    .init_io     = drmgrab_init_io,
    .ref_entry   = drmgrab_ref_entry,
    .ctrl        = drmgrab_ctrl,
};
