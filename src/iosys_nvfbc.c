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
#include <assert.h>

#include "NvFBC.h"

#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#include <ffnvcodec/dynlink_loader.h>
#include <libavutil/hwcontext_cuda.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include "iosys_common.h"
#include "libtxproto/log.h"
#include "metrics.h"

#include "ctrl_template.h"
#include "os_compat.h"
#include "utils.h"

#define LIB_NVFBC_NAME "libnvidia-fbc.so.1"


static NVFBC_API_FUNCTION_LIST nvfbc;

typedef struct NVFBCCtx {
    SPClass *class;

    CudaFunctions* cuda;
    void* libNVFBC;

    NVFBC_SESSION_HANDLE fbc_handle;

    atomic_int quit;
    pthread_t source_update;

    struct tx_metrics *metrics;

    /* Sinks list */
    SPBufferList *entries;
    SPBufferList *events;
} NVFBCCtx;

const IOSysAPI src_nvfbc;


typedef struct NVFBCCapture {
    atomic_int quit;
    pthread_t pull_thread;
    int err;

    /* Hardware frames */
    AVBufferRef *hw_device_context;
    AVBufferRef *hw_frames_ctx;
    int width;
    int height;
    AVRational framerate;

    NVFBC_SESSION_HANDLE fbc_handle;

    int64_t epoch;

    int got_first_frame;
    int dropped_frames;
} NVFBCCapture;

static void log_nvfbc_error(void* ctx, NVFBC_SESSION_HANDLE handle, const char* msg)
{
    sp_log(ctx, SP_LOG_ERROR, "%s: %s\n", msg, nvfbc.nvFBCGetLastErrorStr(handle));
}

static NVFBC_BOOL cuda_init(NVFBCCtx* ctx, CUcontext *cuCtx)
{
    CUresult cuRes;
    CUdevice cuDev;
    assert(ctx);
    
    if (!ctx->cuda || !ctx->cuda->cuInit) {
        sp_log(ctx, SP_LOG_ERROR, "Cuda functions not properly loaded");
        return NVFBC_FALSE;
    }


    cuRes = ctx->cuda->cuInit(0);
    if (cuRes != CUDA_SUCCESS) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to initialize CUDA (result: %d)\n", cuRes);
        return NVFBC_FALSE;
    }

    cuRes = ctx->cuda->cuDeviceGet(&cuDev, 0);
    if (cuRes != CUDA_SUCCESS) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to get CUDA device (result: %d)\n", cuRes);
        return NVFBC_FALSE;
    }

    assert(cuCtx);
    cuRes = ctx->cuda->cuCtxCreate(cuCtx, 0, cuDev);
    if (cuRes != CUDA_SUCCESS) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to create CUDA context (result: %d)\n", cuRes);
        return NVFBC_FALSE;
    }

    return NVFBC_TRUE;
}

static void cuda_release(AVHWDeviceContext* device_ctx)
{
    AVCUDADeviceContext* hwctx = device_ctx->hwctx;
    NVFBCCtx* ctx = device_ctx->user_opaque;
    ctx->cuda->cuCtxDestroy(hwctx->cuda_ctx);
    cuda_free_functions(&ctx->cuda);
    ctx->cuda = NULL;
}

static int nvfbc_load_library(NVFBCCtx* ctx)
{
    ctx->libNVFBC = dlopen(LIB_NVFBC_NAME, RTLD_NOW);
    if (ctx->libNVFBC == NULL) {
        sp_log(ctx, SP_LOG_ERROR,"Unable to open '%s'\n", LIB_NVFBC_NAME);
        return AVERROR(ENOSYS);
    }

    PNVFBCCREATEINSTANCE NvFBCCreateInstance_ptr = (PNVFBCCREATEINSTANCE) dlsym(ctx->libNVFBC, "NvFBCCreateInstance");
    if (NvFBCCreateInstance_ptr == NULL) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to resolve symbol 'NvFBCCreateInstance'\n");
        return AVERROR(ENOSYS);
    }

    memset(&nvfbc, 0, sizeof(nvfbc));
    nvfbc.dwVersion = NVFBC_VERSION;

    NVFBCSTATUS fbcStatus = NvFBCCreateInstance_ptr(&nvfbc);
    if (fbcStatus != NVFBC_SUCCESS) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to create NvFBC instance (status: %d)\n",
                fbcStatus);
        return AVERROR(ENOSYS);
    }

    return 0;
}

static void destroy_entry(void *opaque, uint8_t *data)
{
    IOSysEntry *entry = (IOSysEntry *)data;

    sp_class_free(entry);
    av_free(entry);
}


static int update_or_create_entry(
    NVFBCCtx *ctx,
    uint32_t id, const char* name,
    int x, int y,
    int width, int height,
    SPRotation rotation, bool is_default
) {
    IOSysEntry *entry = NULL;
    int change = 0;
    int new_entry = 0;

    AVBufferRef *entry_ref = sp_bufferlist_ref(ctx->entries,
                                                sp_bufferlist_iosysentry_by_id,
                                                &id);

    if (!entry_ref) {
        entry = av_mallocz(sizeof(*entry));

        sp_class_alloc(entry, NULL, SP_TYPE_VIDEO_BIDIR, ctx);
        sp_class_set_name(entry, name);

        entry->identifier = id;
        entry->api_id = id;
        entry->type = SP_IO_TYPE_VIDEO_DISPLAY;
        entry->frames = sp_frame_fifo_create(entry, 0, 0);
        entry->api_priv = ctx;

        new_entry = 1;
    } else {
        entry = (IOSysEntry *)entry_ref->data;
    }

    change |= (entry->x != x) ||
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
        AVBufferRef *buf = av_buffer_create((uint8_t *)entry, sizeof(*entry),
                                            destroy_entry, NULL, 0);

        sp_bufferlist_append_noref(ctx->entries, buf);
    } else {
        if (change) {
            sp_eventlist_dispatch(entry, ctx->events,
                                    SP_EVENT_ON_CHANGE |
                                    SP_EVENT_TYPE_SOURCE |
                                    SP_EVENT_TYPE_SINK,
                                    entry);
        }
        av_buffer_unref(&entry_ref);
    }
    return 0;
}

static int update_entries(NVFBCCtx *ctx)
{
    NVFBC_GET_STATUS_PARAMS statusParams;
    memset(&statusParams, 0, sizeof(statusParams));
    statusParams.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;

    NVFBCSTATUS fbcStatus = nvfbc.nvFBCGetStatus(ctx->fbc_handle, &statusParams);
    if (fbcStatus != NVFBC_SUCCESS) {
        log_nvfbc_error(ctx, ctx->fbc_handle, "nvFBCGetStatus");
        return AVERROR_EXTERNAL;
    }

    if (!statusParams.bIsCapturePossible) {
        sp_log(ctx, SP_LOG_WARN, "NvFBC capture is not possible\n");
        return AVERROR_EXTERNAL;
    }

    if (!statusParams.bXRandRAvailable) {
        sp_log(ctx, SP_LOG_DEBUG, "XRandR not available, skipping output enumeration\n");
        return 0;
    }

    for (uint32_t i = 0; i < statusParams.dwOutputNum; ++i) {
        NVFBC_RANDR_OUTPUT_INFO* screenInfo = &statusParams.outputs[i];

        update_or_create_entry(
            ctx,
            screenInfo->dwId, screenInfo->name,
            screenInfo->trackedBox.x, screenInfo->trackedBox.y,
            screenInfo->trackedBox.w, screenInfo->trackedBox.h,
            ROTATION_IDENTITY, i == 0
        );
    }

    return 0;
}

static void *source_update_thread(void *s)
{
    NVFBCCtx *ctx = s;

    while (!atomic_load(&ctx->quit)) {
        update_entries(ctx);
        av_usleep(1000000);
    }

    return 0;
}

static int nvfbc_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    int err = 0;
    NVFBCCtx *ctx = (NVFBCCtx *)ctx_ref->data;

    if (ctrl & SP_EVENT_CTRL_NEW_EVENT) {
        AVBufferRef *event = (AVBufferRef *)arg;
        char *fstr = sp_event_flags_to_str_buf(event);
        sp_log(ctx, SP_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
        av_free(fstr);

        if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
            /* Bring up the new event to speed with current affairs */
            SPBufferList *tmp_event = sp_bufferlist_new();
            sp_eventlist_add(ctx, tmp_event, event, 1);

            update_entries(ctx);

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

static void nvfbc_uninit(void *opaque, uint8_t *data)
{
    NVFBCCtx *ctx = (NVFBCCtx *)data;

    atomic_store(&ctx->quit, 1);
    pthread_join(ctx->source_update, NULL);

    NVFBC_DESTROY_HANDLE_PARAMS destroyHandleParams;
    memset(&destroyHandleParams, 0, sizeof(destroyHandleParams));
    destroyHandleParams.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
    NVFBCSTATUS fbcStatus = nvfbc.nvFBCDestroyHandle(ctx->fbc_handle, &destroyHandleParams);
    if (fbcStatus != NVFBC_SUCCESS) {
        log_nvfbc_error(ctx, ctx->fbc_handle, "nvFBCDestroyHandle");
    }

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, ctx);
    sp_bufferlist_free(&ctx->events);
    sp_bufferlist_free(&ctx->entries);

    sp_class_free(ctx);
    av_free(ctx);
}

static int nvfbc_init(AVBufferRef **s)
{
    int err = 0;
    NVFBCSTATUS fbcStatus;

    NVFBCCtx *ctx = av_mallocz(sizeof(*ctx));
    ctx->metrics = NULL;

    if (cuda_load_functions(&ctx->cuda, ctx) != 0) {
        sp_log(ctx, SP_LOG_WARN, "CUDA not available, NvFBC disabled\n");
        av_free(ctx);
        return AVERROR(ENOSYS);
    }

    err = nvfbc_load_library(ctx);
    if (err < 0) {
        av_free(ctx);
        return AVERROR(ENOSYS);
    }

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            nvfbc_uninit, NULL, 0);
    if (!ctx_ref) {
        av_free(ctx);
        return AVERROR(ENOSYS);
    }

    ctx->entries = sp_bufferlist_new();
    if (!ctx->entries) {
        err = AVERROR(ENOSYS);
        goto fail;
    }

    ctx->events = sp_bufferlist_new();
    if (!ctx->events) {
        err = AVERROR(ENOSYS);
        goto fail;
    }

    err = sp_class_alloc(ctx, src_nvfbc.name, SP_TYPE_CONTEXT, NULL);
    if (err < 0)
        goto fail;

    NVFBC_CREATE_HANDLE_PARAMS createHandleParams;
    memset(&createHandleParams, 0, sizeof(createHandleParams));
    createHandleParams.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;

    fbcStatus = nvfbc.nvFBCCreateHandle(&ctx->fbc_handle, &createHandleParams);
    if (fbcStatus != NVFBC_SUCCESS) {
        log_nvfbc_error(ctx, ctx->fbc_handle, "nvFBCCreateHandle");
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    NVFBC_GET_STATUS_PARAMS statusParams;
    memset(&statusParams, 0, sizeof(statusParams));
    statusParams.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;
    fbcStatus = nvfbc.nvFBCGetStatus(ctx->fbc_handle, &statusParams);
    if (fbcStatus != NVFBC_SUCCESS) {
        log_nvfbc_error(ctx, ctx->fbc_handle, "nvFBCGetStatus");
        err = AVERROR(ENOMEM);
        goto fail;
    }

    if (!statusParams.bIsCapturePossible) {
        sp_log(ctx, SP_LOG_ERROR, "capture is not possible\n");
        err = AVERROR(EPERM);
        goto fail;
    }

    ctx->quit = 0;
    pthread_create(&ctx->source_update, NULL, source_update_thread, ctx);

    *s = ctx_ref;

    return 0;

fail:
    av_buffer_unref(&ctx_ref);

    return err;
}

static AVBufferRef *nvfbc_ref_entry(AVBufferRef *ctx_ref, uint32_t identifier)
{
    NVFBCCtx *ctx = (NVFBCCtx *)ctx_ref->data;
    return sp_bufferlist_pop(ctx->entries, sp_bufferlist_iosysentry_by_id, &identifier);
}


static void release_texture_pool(NVFBCCapture *priv)
{
    if (priv->hw_frames_ctx) {
        av_buffer_unref(&priv->hw_frames_ctx);
    }

    priv->width = -1;
    priv->height = -1;
}

static int allocate_texture_pool(NVFBCCtx *ctx, NVFBCCapture *priv, size_t width, size_t height)
{
    int err;

    err = av_hwdevice_ctx_init(priv->hw_device_context);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "av_hwdevice_ctx_init() failed\n");
        goto fail;
    }

    priv->hw_frames_ctx = av_hwframe_ctx_alloc(priv->hw_device_context);
    if (!priv->hw_frames_ctx) {
        sp_log(ctx, SP_LOG_ERROR, "av_hwframe_ctx_alloc() failed\n");
        err = AVERROR(ENOMEM);
        goto fail;
    }

    AVHWFramesContext *hw_frames_ctx_cfg = (AVHWFramesContext *)priv->hw_frames_ctx->data;
    hw_frames_ctx_cfg->format = AV_PIX_FMT_CUDA;
    hw_frames_ctx_cfg->sw_format = AV_PIX_FMT_BGRA;
    hw_frames_ctx_cfg->width = width;
    hw_frames_ctx_cfg->height = height;
    hw_frames_ctx_cfg->initial_pool_size = 6;

    err = av_hwframe_ctx_init(priv->hw_frames_ctx);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "av_hwframe_ctx_init() failed: %d\n", err);
        goto fail;
    }

    return 0;

fail:
    release_texture_pool(priv);

    return err;
}

static void stop_capture(NVFBCCapture* priv, NVFBCCtx* ctx)
{
    NVFBCSTATUS fbcStatus;

    NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroyCaptureParams;
    memset(&destroyCaptureParams, 0, sizeof(destroyCaptureParams));
    destroyCaptureParams.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;

    fbcStatus = nvfbc.nvFBCDestroyCaptureSession(priv->fbc_handle, &destroyCaptureParams);
    if (fbcStatus != NVFBC_SUCCESS) {
        log_nvfbc_error(ctx, priv->fbc_handle, "stop_capture nvFBCDestroyCaptureSession");
    }

    NVFBC_DESTROY_HANDLE_PARAMS destroyHandleParams;
    memset(&destroyHandleParams, 0, sizeof(destroyHandleParams));
    destroyHandleParams.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;

    fbcStatus = nvfbc.nvFBCDestroyHandle(priv->fbc_handle, &destroyHandleParams);
    if (fbcStatus != NVFBC_SUCCESS) {
        log_nvfbc_error(ctx, priv->fbc_handle, "stop_capture nvFBCDestroyHandle");
    }
    
    int res = dlclose(ctx->libNVFBC);
    if (res != 0) {
        sp_log(ctx, SP_LOG_WARN, "Could not close NvFBC library: %s\n", dlerror());
    }
}

static uint32_t get_display_refresh_rate(NVFBCCtx *ctx, uint32_t output_id)
{
    uint32_t refresh_rate = 60;

    xcb_connection_t *con = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(con)) {
        sp_log(ctx, SP_LOG_WARN, "Could not open XCB connection, defaulting to 60 Hz\n");
        return refresh_rate;
    }

    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(con)).data;
    if (!screen) {
        sp_log(ctx, SP_LOG_WARN, "Could not get XCB screen, defaulting to 60 Hz\n");
        xcb_disconnect(con);
        return refresh_rate;
    }

    xcb_randr_get_screen_resources_cookie_t resources_c;
    xcb_randr_get_screen_resources_reply_t *resources_r = NULL;

    resources_c = xcb_randr_get_screen_resources(con, screen->root);
    resources_r = xcb_randr_get_screen_resources_reply(con, resources_c, NULL);
    if (!resources_r) {
        sp_log(ctx, SP_LOG_WARN, "XRandR screen resources not available, defaulting to 60 Hz\n");
        xcb_disconnect(con);
        return refresh_rate;
    }

    xcb_randr_mode_t active_mode = 0;
    xcb_randr_get_output_info_cookie_t output_info_c;
    xcb_randr_get_output_info_reply_t *output_info_r;

    output_info_c = xcb_randr_get_output_info(con, output_id, XCB_CURRENT_TIME);
    output_info_r = xcb_randr_get_output_info_reply(con, output_info_c, NULL);
    if (output_info_r && output_info_r->crtc != XCB_NONE) {
        xcb_randr_get_crtc_info_cookie_t crtc_info_c;
        xcb_randr_get_crtc_info_reply_t *crtc_info_r;

        crtc_info_c = xcb_randr_get_crtc_info(con, output_info_r->crtc, XCB_CURRENT_TIME);
        crtc_info_r = xcb_randr_get_crtc_info_reply(con, crtc_info_c, NULL);
        if (crtc_info_r && crtc_info_r->mode) {
            active_mode = crtc_info_r->mode;
        }
        free(crtc_info_r);
    }
    free(output_info_r);

    if (active_mode) {
        xcb_randr_mode_info_t *modes = xcb_randr_get_screen_resources_modes(resources_r);
        int num_modes = xcb_randr_get_screen_resources_modes_length(resources_r);

        for (int i = 0; i < num_modes; i++) {
            if (modes[i].id == active_mode) {
                if (modes[i].htotal && modes[i].vtotal) {
                    refresh_rate = modes[i].dot_clock / (modes[i].htotal * modes[i].vtotal);
                }
                break;
            }
        }
    } else {
        sp_log(ctx, SP_LOG_WARN, "Could not find active mode for output %u, defaulting to 60 Hz\n", output_id);
    }

    free(resources_r);
    xcb_disconnect(con);

    return refresh_rate;
}

static int start_capture(NVFBCCapture *priv, NVFBCCtx *ctx, IOSysEntry *entry)
{
    NVFBCSTATUS fbcStatus;
    int err = 0;
    CUcontext cuCtx = NULL;
    if (cuda_init(ctx, &cuCtx) < 0) {
        return AVERROR_EXTERNAL;
    }

    priv->hw_device_context = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if (!priv->hw_device_context) {
        sp_log(ctx, SP_LOG_ERROR, "av_hwdevice_ctx_alloc() failed\n");
        ctx->cuda->cuCtxDestroy(cuCtx);
        return AVERROR(ENOMEM);
    }

    AVHWDeviceContext* device_ctx = ((AVHWDeviceContext*) priv->hw_device_context->data);
    if (!device_ctx) {
        sp_log(ctx, SP_LOG_ERROR, "Got NULL AVCUDADeviceContext\n");
        err = AVERROR_EXTERNAL;
        goto fail;
    }
    device_ctx->free = cuda_release;
    device_ctx->user_opaque = ctx;

    AVCUDADeviceContext* device_hwctx = device_ctx->hwctx;
    device_hwctx->cuda_ctx = cuCtx;

    err = av_hwdevice_ctx_init(priv->hw_device_context);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "av_hwdevice_ctx_init() failed\n");
        goto fail;
    }

    NVFBC_CREATE_HANDLE_PARAMS createHandleParams;
    memset(&createHandleParams, 0, sizeof(createHandleParams));
    createHandleParams.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;

    fbcStatus = nvfbc.nvFBCCreateHandle(&priv->fbc_handle, &createHandleParams);
    if (fbcStatus != NVFBC_SUCCESS) {
        log_nvfbc_error(ctx, ctx->fbc_handle, "nvFBCCreateHandle");
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    NVFBC_CREATE_CAPTURE_SESSION_PARAMS createCaptureParams;
    memset(&createCaptureParams, 0, sizeof(createCaptureParams));
    createCaptureParams.dwVersion     = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
    createCaptureParams.eCaptureType  = NVFBC_CAPTURE_SHARED_CUDA;
    createCaptureParams.bWithCursor   = NVFBC_FALSE;
    createCaptureParams.frameSize.w   = 0;
    createCaptureParams.frameSize.h   = 0;
    createCaptureParams.eTrackingType = NVFBC_TRACKING_OUTPUT;
    createCaptureParams.dwOutputId    = entry->api_id;

    fbcStatus = nvfbc.nvFBCCreateCaptureSession(priv->fbc_handle, &createCaptureParams);
    if (fbcStatus != NVFBC_SUCCESS) {
        log_nvfbc_error(ctx, priv->fbc_handle, "nvFBCCreateCaptureSession");
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    NVFBC_TOCUDA_SETUP_PARAMS setupParams;
    memset(&setupParams, 0, sizeof(setupParams));
    setupParams.dwVersion     = NVFBC_TOCUDA_SETUP_PARAMS_VER;
    setupParams.eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA;

    fbcStatus = nvfbc.nvFBCToCudaSetUp(priv->fbc_handle, &setupParams);
    if (fbcStatus != NVFBC_SUCCESS) {
        log_nvfbc_error(ctx, priv->fbc_handle, "nvFBCToCudaSetUp");
        goto fail;
    }

    /* Get framerate from display via XRandR */
    uint32_t refreshRate = get_display_refresh_rate(ctx, entry->api_id);
    priv->framerate = av_make_q(refreshRate, 1);
    sp_log(ctx, SP_LOG_INFO, "Display refresh rate: %d Hz\n", refreshRate);
    sp_log(ctx, SP_LOG_INFO, "NvFBC capture session started\n");

    priv->got_first_frame = 0;

    return 0;

fail:
    stop_capture(priv, ctx);

    return AVERROR_EXTERNAL;
}

static int check_resolution_change(NVFBCCapture *priv,
                                   NVFBCCtx *ctx,
                                   NVFBC_FRAME_GRAB_INFO* grabInfo)
{
    if (grabInfo->dwWidth != priv->width || grabInfo->dwHeight != priv->height) {
        release_texture_pool(priv);

        priv->width = grabInfo->dwWidth;
        priv->height = grabInfo->dwHeight;

        int err = allocate_texture_pool(ctx,
            priv,
            priv->width,
            priv->height);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "allocate_texture_pool() failed: %d\n", err);
            return err;
        }
    }

    return 0;
}

static void *nvfbc_capture_thread(void *s)
{
    IOSysEntry* entry = s;
    NVFBCCapture* priv = entry->io_priv;
    NVFBCCtx* ctx = entry->api_priv;

    AVFrame* frame = NULL;

    sp_set_thread_name_self(sp_class_get_name(entry));
    sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_CONFIG | SP_EVENT_ON_INIT, NULL);

    int err = start_capture(priv, ctx, entry);
    if (err < 0) {
        sp_log(entry, SP_LOG_ERROR, "start_capture() failed: %d\n", err);
        return NULL;
    }

    NVFBCSTATUS fbcStatus;

    while (!atomic_load(&priv->quit)) {
        CUdeviceptr cuDevicePtr;

        NVFBC_FRAME_GRAB_INFO frameInfo;
        memset(&frameInfo, 0, sizeof(frameInfo));

        NVFBC_TOCUDA_GRAB_FRAME_PARAMS grabParams;
        memset(&grabParams, 0, sizeof(grabParams));
        grabParams.dwVersion = NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER;
        grabParams.dwFlags = NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT_IF_NEW_FRAME_READY;
        grabParams.pCUDADeviceBuffer = &cuDevicePtr;
        grabParams.pFrameGrabInfo = &frameInfo;

        fbcStatus = nvfbc.nvFBCToCudaGrabFrame(priv->fbc_handle, &grabParams);
        if (fbcStatus != NVFBC_SUCCESS) {
            log_nvfbc_error(ctx, priv->fbc_handle, "nvFBCToCudaGrabFrame");
            goto fail;
        }

        err  = check_resolution_change( priv, ctx, &frameInfo);
        if (err < 0) {
            sp_log(entry, SP_LOG_WARN, "check_resolution_change() failed: %d\n", err);
            av_frame_free(&frame);
            continue;
        }

        frame = av_frame_alloc();

        err = av_hwframe_get_buffer(priv->hw_frames_ctx, frame, 0);
        if (err < 0) {
            sp_log(entry, SP_LOG_WARN, "av_hwframe_get_buffer() failed: %d\n", err);

            av_frame_free(&frame);
            continue;
        }

        // This ::CUdeviceptr will be mapped to a segment in video memory containing
        // the frame.  It is not possible to process a CUDA device while capturing
        // a new frame.  If the application wants to do so, it must copy the CUDA
        // device using ::cuMemcpyDtoD or ::cuMemcpyDtoH beforehand.

        CUdeviceptr frameDevicePtr = (CUdeviceptr)frame->data[0];
        ctx->cuda->cuMemcpyDtoD(frameDevicePtr, cuDevicePtr,  frameInfo.dwByteSize);

        int64_t ts = av_gettime_relative();

        frame->pts         = ts - priv->epoch;
        frame->time_base   = AV_TIME_BASE_Q;
        frame->opaque_ref  = av_buffer_allocz(sizeof(FormatExtraData));

        FormatExtraData *fe = (FormatExtraData *)frame->opaque_ref->data;
        fe->time_base       = AV_TIME_BASE_Q;
        fe->avg_frame_rate  = priv->framerate;
        fe->rotation        = ROTATION_IDENTITY;

        if (ctx->metrics) {
            const tx_metrics_entry metrics[] = {
                { .key = "pts", .value = frame->pts },
                { .key = "acquired", .value = ts },
            };
            tx_metrics_push(ctx->metrics, metrics, SP_ARRAY_ELEMS(metrics));
        }

        err = sp_frame_fifo_push(entry->frames, frame);
        av_frame_free(&frame);

        if (err == AVERROR(ENOBUFS)) {
            priv->dropped_frames++;
            sp_log(entry, SP_LOG_WARN, "Dropping frame (%i dropped so far)!\n",
                priv->dropped_frames);

            SPGenericData entries[] = {
                { "dropped_frames", NULL, &priv->dropped_frames, SP_DATA_TYPE_INT},
                { 0 },
            };
            sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_STATS, entries);
        } else if (err) {
            sp_log(entry, SP_LOG_ERROR, "Unable to push frame to FIFO: %s!\n",
                   av_err2str(err));
            break;
        }
    }

fail:
    av_frame_free(&frame);

    stop_capture(priv, ctx);
    release_texture_pool(priv);

    priv->err = err;

    if (err < 0)
        sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_ERROR, NULL);

    sp_log(entry, SP_LOG_INFO, "Acquisition stopped\n");

    return NULL;
}

static int nvfbc_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx, void *ctx,
                             void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;

    IOSysEntry *entry = ctx;
    NVFBCCapture *io_priv = entry->io_priv;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        io_priv->epoch = atomic_load(event->epoch);
        pthread_create(&io_priv->pull_thread, NULL, nvfbc_capture_thread, entry);
        sp_log(entry, SP_LOG_VERBOSE, "Started capture thread\n");
        return 0;
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        atomic_store(&io_priv->quit, 1);
        pthread_join(io_priv->pull_thread, NULL);
        sp_log(entry, SP_LOG_VERBOSE, "Stopped capture thread\n");
        return 0;
    } else {
        return AVERROR(ENOTSUP);
    }
}

static int nvfbc_ioctx_ctrl(AVBufferRef *entry, SPEventType ctrl, void *arg)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    return sp_ctrl_template(iosys_entry, iosys_entry->events, 0x0,
                            nvfbc_ioctx_ctrl_cb, ctrl, arg);
}

static int nvfbc_init_io(AVBufferRef *ctx_ref, AVBufferRef *entry,
                       AVDictionary *opts)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    NVFBCCapture *cap_priv = (NVFBCCapture *)av_mallocz(sizeof(*cap_priv));

    if (!cap_priv)
        return AVERROR(ENOMEM);

    cap_priv->width = -1;
    cap_priv->height = -1;
    cap_priv->quit = 0;

    iosys_entry->ctrl = nvfbc_ioctx_ctrl;
    iosys_entry->events = sp_bufferlist_new();

    iosys_entry->io_priv = cap_priv;

    return 0;
}

static int nvfbc_set_metrics(AVBufferRef *buffer, struct tx_metrics *metrics)
{
    NVFBCCtx *ctx = (NVFBCCtx *)buffer->data;

    assert(!metrics || metrics->cb);
    ctx->metrics = metrics;

    return 0;
}

const IOSysAPI src_nvfbc = {
    .name        = "nvfbc",
    .init_sys    = nvfbc_init,
    .set_metrics = nvfbc_set_metrics,
    .init_io     = nvfbc_init_io,
    .ref_entry   = nvfbc_ref_entry,
    .ctrl        = nvfbc_ctrl,
};
