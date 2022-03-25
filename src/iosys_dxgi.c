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

#define COBJMACROS

#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <windows.h>

#include <libavformat/avformat.h>
#include <libavutil/crc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#include "iosys_common.h"
#include "os_compat.h"
#include "utils.h"

#include "ctrl_template.h"

/**
 * Limitation of the implementation if txproto is runned as a standard user:
 * Secure Desktop (UAC and Ctrl+Alt+Del for example) can't be accessed without
 * some security tweaks. txproto won't be able to acquire new frames in this
 * case.
 *
 *
 * When called from a standard user process, IDXGIOutput1::DuplicateOutput()
 * can't access to UAC: E_ACCESSDENIED is returned.
 *
 * Ref: https://docs.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-duplicateoutput#return-value.
 *
 *
 * The solution is to run the process at LOCAL_SYSTEM. To do this, a solution is
 * to create a Windows service. However, there is another limitation in this
 * specific scenario: IDXGIAdapter::EnumOutputs() returns DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
 * if called in a Session 0 process.
 *
 * Ref: https://docs.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiadapter-enumoutputs.
 *
 *
 * It is possible to spawn a process from the service, using CreateProcessAsUser(),
 * that will inherit from the service access to Secure Desktop, but that is
 * runned as a standard user:
 * - Get the service access token, and duplicate it.
 * - Update the TokenSessionId of the token. It should match the attached physical
 *   console. This id can be fetched with WTSGetActiveConsoleSessionId().
 * - CreateProcessAsUser() can now be called with the updated token.
 *
 * Refs:
 * - https://github.com/markjandrews/tightvnc/blob/baae5b00b7903087c52c53027c8d14d218ddc46c/win-system/CurrentConsoleProcess.cpp#L43
 * - https://stackoverflow.com/questions/5334642
 * - https://stackoverflow.com/questions/43520385
 */

const IOSysAPI src_dxgi;

typedef struct DxgiCtx {
    SPClass *class;

    ID3D11Device *d3d11_device;
    IUnknown *d3d11_device_base;
    ID3D11DeviceContext *d3d11_device_ctx;
    IDXGIAdapter *dxgi_adapter;

    atomic_int quit;
    pthread_t source_update;

    /* Sinks list */
    SPBufferList *entries;
    SPBufferList *events;
} DxgiCtx;

typedef struct DxgiCapture {
    atomic_int quit;
    pthread_t pull_thread;
    int err;

    /* Hardware frames */
    AVBufferRef *hw_device_context;
    AVBufferRef *hw_frames_ctx;
    int width;
    int height;
    SPRotation rotation;
    AVRational framerate;

    /* Windows capture */
    HDESK current_desk;
    IDXGIOutputDuplication *output_duplication;

    int64_t epoch;

    int dropped_frames;
} DxgiCapture;

static uint32_t gen_identifier(const char *s)
{
    const AVCRC *table = av_crc_get_table(AV_CRC_32_IEEE);

    uint32_t crc = UINT32_MAX;
    crc = av_crc(table, crc, (const uint8_t *)s, strlen(s));
    return crc;
}

static SPRotation convert_rotation(DXGI_MODE_ROTATION dxgi_rotation)
{
    switch (dxgi_rotation) {
        case DXGI_MODE_ROTATION_ROTATE90:
            return ROTATION_ROTATE90;

        case DXGI_MODE_ROTATION_ROTATE180:
            return ROTATION_ROTATE180;

        case DXGI_MODE_ROTATION_ROTATE270:
            return ROTATION_ROTATE270;

        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY:
        default:
            return ROTATION_IDENTITY;
    }
}

static void close_d3ddevice(DxgiCtx *ctx)
{
    if (ctx->dxgi_adapter) {
        IDXGIAdapter_Release(ctx->dxgi_adapter);
        ctx->dxgi_adapter = NULL;
    }

    if (ctx->d3d11_device_base) {
        IUnknown_Release(ctx->d3d11_device_base);
        ctx->d3d11_device_base = NULL;
    }

    if (ctx->d3d11_device) {
        ID3D11Device_Release(ctx->d3d11_device);
        ctx->d3d11_device = NULL;
    }

    if (ctx->d3d11_device_ctx) {
        ID3D11DeviceContext_Release(ctx->d3d11_device_ctx);
        ctx->d3d11_device_ctx = NULL;
    }
}

/* Open D3D device and enable multithreading on it */
static int open_d3ddevice(DxgiCtx *ctx)
{
    ID3D10Multithread* device_multithread;
    IDXGIDevice* dxgi_device;
    HRESULT hr;

    /* Open Direct3D device */
    const D3D_DRIVER_TYPE driver_types[] = {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };

    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_1
    };

    for (UINT i = 0; i < ARRAYSIZE(driver_types); ++i) {
        hr = D3D11CreateDevice(
            NULL,
            driver_types[i],
            NULL, 0,
            feature_levels, ARRAYSIZE(feature_levels),
            D3D11_SDK_VERSION,
            &ctx->d3d11_device,
            NULL,
            &ctx->d3d11_device_ctx);
        if (SUCCEEDED(hr)) {
            break;
        }
    }

    if (FAILED(hr)) {
        return AVERROR_EXTERNAL;
    }

    hr = ID3D11Device_QueryInterface(ctx->d3d11_device,
                                     &IID_IUnknown,
                                     (void **)&ctx->d3d11_device_base);
    if (FAILED(hr)) {
        goto fail;
    }

    /* Enable multithreading */
    hr = ID3D11Device_QueryInterface(ctx->d3d11_device,
                                     &IID_ID3D10Multithread,
                                     (void **)&device_multithread);
    if (FAILED(hr)) {
        goto fail;
    }

    ID3D10Multithread_SetMultithreadProtected(device_multithread, TRUE);
    ID3D10Multithread_Release(device_multithread);

    /* Get Dxgi adapter */
    hr = ID3D11Device_QueryInterface(ctx->d3d11_device,
                                     &IID_IDXGIDevice,
                                     (void **)&dxgi_device);
    if (FAILED(hr)) {
        sp_log(ctx, SP_LOG_ERROR, "ID3D11Device_QueryInterface failed: 0x%lX\n", hr);
        goto fail;
    }

    hr = IDXGIDevice_GetParent(dxgi_device,
                               &IID_IDXGIAdapter,
                               (void **)&ctx->dxgi_adapter);
    IDXGIDevice_Release(dxgi_device);
    if (FAILED(hr)) {
        sp_log(ctx, SP_LOG_ERROR, "IDXGIDevice_GetParent failed: 0x%lX\n", hr);
        goto fail;
    }

    return 0;

fail:
    close_d3ddevice(ctx);

    return AVERROR_EXTERNAL;
}

static void destroy_entry(void *opaque, uint8_t *data)
{
    IOSysEntry *entry = (IOSysEntry *)data;

    sp_class_free(entry);
    av_free(entry);
}

static int update_entries(DxgiCtx *ctx)
{
    HRESULT hr;

    for (UINT i = 0;; i++) {
        IOSysEntry *entry = NULL;
        int change = 0;
        int new_entry = 0;

        IDXGIOutput *output;
        hr = IDXGIAdapter_EnumOutputs(ctx->dxgi_adapter, i, &output);
        if (FAILED(hr)) {
            break;
        }

        DXGI_OUTPUT_DESC desc;
        hr = IDXGIOutput_GetDesc(output, &desc);
        if (FAILED(hr)) {
            sp_log(ctx, SP_LOG_ERROR, "IDXGIOutput_GetDesc failed: 0x%lX\n", hr);

            IDXGIOutput_Release(output);
            return AVERROR_EXTERNAL;
        }

        char name[ARRAYSIZE(desc.DeviceName)];
        wcstombs(name, desc.DeviceName, sizeof(name));
        uint32_t id = gen_identifier(name);

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

        int x = desc.DesktopCoordinates.left;
        int y = desc.DesktopCoordinates.top;
        int width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        int height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        SPRotation rotation = convert_rotation(desc.Rotation);

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
        entry->is_default = i == 0;

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

        IDXGIOutput_Release(output);
        output = NULL;
    }

    return 0;
}

static void *source_update_thread(void *s)
{
    DxgiCtx *ctx = s;

    while (!atomic_load(&ctx->quit)) {
        update_entries(ctx);
        av_usleep(1000000);
    }

    return 0;
}

static int dxgi_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    int err = 0;
    DxgiCtx *ctx = (DxgiCtx *)ctx_ref->data;

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

static void dxgi_uninit(void *opaque, uint8_t *data)
{
    DxgiCtx *ctx = (DxgiCtx *)data;

    /* Stop updating */
    atomic_store(&ctx->quit, 1);
    pthread_join(ctx->source_update, NULL);

    close_d3ddevice(ctx);

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, ctx);
    sp_bufferlist_free(&ctx->entries);

    sp_class_free(ctx);
    av_free(ctx);
}

static int dxgi_init(AVBufferRef **s)
{
    int err = 0;
    DxgiCtx *ctx = av_mallocz(sizeof(*ctx));

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            dxgi_uninit, NULL, 0);
    if (!ctx_ref) {
        av_free(ctx);
        return AVERROR(ENOMEM);
    }

    ctx->entries = sp_bufferlist_new();
    if (!ctx->entries) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->events = sp_bufferlist_new();
    if (!ctx->events) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = sp_class_alloc(ctx, src_dxgi.name, SP_TYPE_CONTEXT, NULL);
    if (err < 0)
        goto fail;

    err = open_d3ddevice(ctx);
    if (err < 0)
        goto fail;

    ctx->quit = ATOMIC_VAR_INIT(0);
    pthread_create(&ctx->source_update, NULL, source_update_thread, ctx);

    *s = ctx_ref;

    return 0;

fail:
    av_buffer_unref(&ctx_ref);

    return err;
}

static AVBufferRef *dxgi_ref_entry(AVBufferRef *ctx_ref, uint32_t identifier)
{
    DxgiCtx *ctx = (DxgiCtx *)ctx_ref->data;
    return sp_bufferlist_pop(ctx->entries, sp_bufferlist_iosysentry_by_id, &identifier);
}

static IDXGIOutput1 *get_dxgi_output(DxgiCtx *ctx, IOSysEntry *entry)
{
    HRESULT hr;

    for (UINT i = 0;; i++) {
        IDXGIOutput *output;
        hr = IDXGIAdapter_EnumOutputs(ctx->dxgi_adapter, i, &output);
        if (FAILED(hr)) {
            return NULL;
        }

        DXGI_OUTPUT_DESC desc;
        hr = IDXGIOutput_GetDesc(output, &desc);
        if (FAILED(hr)) {
            IDXGIOutput_Release(output);
            return NULL;
        }

        char name[ARRAYSIZE(desc.DeviceName)];
        wcstombs(name, desc.DeviceName, sizeof(name));
        uint32_t id = gen_identifier(name);

        if (entry->identifier == id) {
            IDXGIOutput1 *dxgi_output1;

            hr = IDXGIOutput_QueryInterface(output,
                                            &IID_IDXGIOutput1,
                                            (void **)&dxgi_output1);
            IDXGIOutput_Release(output);
            output = NULL;
            if (FAILED(hr)) {
                return NULL;
            } else {
                return dxgi_output1;
            }
        } else {
            IDXGIOutput_Release(output);
            output = NULL;
        }
    }

    return NULL;
}

static void release_texture_pool(DxgiCapture *priv)
{
    if (priv->hw_frames_ctx) {
        av_buffer_unref(&priv->hw_frames_ctx);
    }

    if (priv->hw_device_context) {
        av_buffer_unref(&priv->hw_device_context);
    }

    priv->width = -1;
    priv->height = -1;
}

static int allocate_texture_pool(DxgiCapture *priv, ID3D11Device* device, int width, int height)
{
    int err;

    /* Init av_hwdevice.
     *
     * According to documentation: Deallocating the AVHWDeviceContext will always release this interface.
     * ID3D11Device_AddRef() must be called.
     */
    priv->hw_device_context = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);

    AVD3D11VADeviceContext *d3d11va_context = (((AVHWDeviceContext *) priv->hw_device_context->data)->hwctx);
    ID3D11Device_AddRef(device);
    d3d11va_context->device = device;

    err = av_hwdevice_ctx_init(priv->hw_device_context);
    if (err < 0) {
        goto fail;
    }

    /* Init av_hwframe_ctx */
    priv->hw_frames_ctx = av_hwframe_ctx_alloc(priv->hw_device_context);
    if (!priv->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    AVHWFramesContext *hw_frames_ctx_cfg = (AVHWFramesContext *)priv->hw_frames_ctx->data;
    hw_frames_ctx_cfg->format = AV_PIX_FMT_D3D11;
    hw_frames_ctx_cfg->sw_format = AV_PIX_FMT_BGRA;
    hw_frames_ctx_cfg->width = width;
    hw_frames_ctx_cfg->height = height;
    hw_frames_ctx_cfg->initial_pool_size = 6;

    err = av_hwframe_ctx_init(priv->hw_frames_ctx);
    if (err < 0) {
        goto fail;
    }

    return 0;

fail:
    release_texture_pool(priv);

    return err;
}

static void stop_capture(DxgiCapture *priv)
{
    if (priv->output_duplication) {
        IDXGIOutputDuplication_Release(priv->output_duplication);
        priv->output_duplication = NULL;
    }

    if (priv->current_desk) {
        CloseDesktop(priv->current_desk);
        priv->current_desk = NULL;
    }
}

static int start_capture(DxgiCapture *priv, DxgiCtx *ctx, IDXGIOutput1 *dxgi_output)
{
    BOOL err;
    HRESULT hr;

    /* Current desktop can change during runtime. This typically happens
     * when UAC (Secure Dekstop) is triggered.
     *
     * SetThreadDesktop() must be correctly configured to grab Secure Desktop. */
    priv->current_desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!priv->current_desk) {
        sp_log(priv, SP_LOG_WARN, "Fail to open input desktop\n");
        return AVERROR_EXTERNAL;
    }

    err = SetThreadDesktop(priv->current_desk);
    if (!err) {
        goto fail;
    }

    /* DuplicateOutput() can be rejected if the current user isn't allowed
     * to grab Secure Desktop. */
    hr = IDXGIOutput1_DuplicateOutput(dxgi_output,
                                      ctx->d3d11_device_base,
                                      &priv->output_duplication);
    if (FAILED(hr)) {
        sp_log(priv, SP_LOG_WARN, "DuplicateOutput() failed: %lX\n", hr);
        goto fail;
    }

    /* Fill rotation and framerate */
    DXGI_OUTDUPL_DESC desc;
    IDXGIOutputDuplication_GetDesc(priv->output_duplication, &desc);

    priv->rotation = convert_rotation(desc.Rotation);

    priv->framerate = av_make_q(desc.ModeDesc.RefreshRate.Numerator,
                                desc.ModeDesc.RefreshRate.Denominator);

    sp_log(priv, SP_LOG_INFO, "Duplication started. Resolution: %dx%d\n",
           desc.ModeDesc.Width, desc.ModeDesc.Height);

    return 0;

fail:
    stop_capture(priv);

    return AVERROR_EXTERNAL;
}

static int check_resolution_change(DxgiCapture *priv,
                                   DxgiCtx *ctx,
                                   ID3D11Resource *tex)
{
    ID3D11Texture2D *tex2d;
    D3D11_TEXTURE2D_DESC desc;
    HRESULT hr;
    int err;

    hr = ID3D11Texture2D_QueryInterface(tex,
                                        &IID_ID3D11Texture2D,
                                        (void **)&tex2d);
    if (FAILED(hr)) {
        return AVERROR_EXTERNAL;
    }

    ID3D11Texture2D_GetDesc(tex2d, &desc);
    ID3D11Texture2D_Release(tex2d);
    tex2d = NULL;

    if (desc.Width != priv->width || desc.Height != priv->height) {
        release_texture_pool(priv);

        // Rotation and framerate are fetched juste after DuplicateOutput().
        // The texture size can differ from the the DXGIOutput resolution
        // when a rotation is applied (90°/270°).
        priv->width = desc.Width;
        priv->height = desc.Height;

        err = allocate_texture_pool(priv,
                                    ctx->d3d11_device,
                                    priv->width,
                                    priv->height);
        if (err < 0) {
            return err;
        }
    }

    return 0;
}

static void *dxgi_capture_thread(void *s)
{
    IOSysEntry *entry = s;
    DxgiCapture *priv = entry->io_priv;
    DxgiCtx *ctx = entry->api_priv;
    IDXGIOutput1 *dxgi_output = NULL;
    AVFrame* frame = NULL;
    HRESULT hr;
    int err = 0;

    sp_set_thread_name_self(sp_class_get_name(entry));
    sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_CONFIG | SP_EVENT_ON_INIT, NULL);

    dxgi_output = get_dxgi_output(ctx, entry);
    if (!dxgi_output) {
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    while (!atomic_load(&priv->quit)) {
        /* Capture may be restarted multiple time during a session in some cases:
         * - Fullscreen switch
         * - Desktop switch
         * - Resolution switch
         *
         * Capture restart can fail if the current user isn't allowed to access Secure
         * Desktop. Restart retries are done because the user is likely to quit Secure
         * Desktop. In this case, capture restart will succeed. */
        if (!priv->output_duplication) {
            err = start_capture(priv, ctx, dxgi_output);
            if (err < 0) {
                av_usleep(100000);
                continue;
            }
        }

        /* Frame acquisition */
        sp_log(entry, SP_LOG_TRACE, "Request frame acquisition\n");

        IDXGIResource *acquired_resource;
        DXGI_OUTDUPL_FRAME_INFO frame_info;
        memset(&frame_info, 0, sizeof(frame_info));
        hr = IDXGIOutputDuplication_AcquireNextFrame(priv->output_duplication,
                                                     100, /* 100 ms */
                                                     &frame_info,
                                                     &acquired_resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        } else if (hr == DXGI_ERROR_ACCESS_LOST) {
            sp_log(entry, SP_LOG_WARN, "Acquisition access lost\n");
            stop_capture(priv);
            continue;
        } else if (FAILED(hr)) {
            sp_log(entry, SP_LOG_WARN, "AcquireNextFrame() failed: %lX\n", hr);
            err = AVERROR_EXTERNAL;
            goto fail;
        }

        ID3D11Resource *acquired_tex;
        hr = IDXGIResource_QueryInterface(acquired_resource,
                                          &IID_ID3D11Resource,
                                          (void **)&acquired_tex);
        IDXGIResource_Release(acquired_resource);
        acquired_resource = NULL;
        if (FAILED(hr)) {
            err = AVERROR_EXTERNAL;
            goto fail;
        }

        /* Check for resolution change */
        err = check_resolution_change(priv, ctx, acquired_tex);
        if (err < 0) {
            goto fail;
        }

        /* Allocate AVFrame/D3D11 texture */
        frame = av_frame_alloc();

        err = av_hwframe_get_buffer(priv->hw_frames_ctx, frame, 0);
        if (err < 0) {
            sp_log(entry, SP_LOG_WARN, "av_hwframe_get_buffer() failed: %d\n", err);

            av_frame_free(&frame);
            IDXGIResource_Release(acquired_tex);

            hr = IDXGIOutputDuplication_ReleaseFrame(priv->output_duplication);
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                sp_log(entry, SP_LOG_WARN, "Acquisition access lost\n");
                stop_capture(priv);
            } else if (FAILED(hr)) {
               sp_log(entry, SP_LOG_ERROR, "IDXGIOutputDuplication_ReleaseFrame() failed\n");
            }

            continue;
        }

        ID3D11Texture2D *target_tex = (ID3D11Texture2D *)frame->data[0];
        intptr_t target_tex_idx = (intptr_t)frame->data[1];

        /* Copy and release texture */
        sp_log(entry, SP_LOG_TRACE, "Grab image\n");

        ID3D11Resource *target_ressource;
        hr = ID3D11Texture2D_QueryInterface(target_tex,
                                            &IID_ID3D11Resource,
                                            (void **)&target_ressource);
        if (FAILED(hr)) {
            err = AVERROR_EXTERNAL;
            goto fail;
        }

        ID3D11DeviceContext_CopySubresourceRegion(ctx->d3d11_device_ctx,
                                                  target_ressource, target_tex_idx,
                                                  0, 0, 0,
                                                  acquired_tex, 0,
                                                  NULL);

        ID3D11Resource_Release(target_ressource);

        IDXGIResource_Release(acquired_tex);
        acquired_tex = NULL;

        hr = IDXGIOutputDuplication_ReleaseFrame(priv->output_duplication);
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            sp_log(entry, SP_LOG_WARN, "Acquisition access lost\n");
            av_frame_free(&frame);
            stop_capture(priv);
            continue;
        } else if (FAILED(hr)) {
            sp_log(entry, SP_LOG_ERROR, "IDXGIOutputDuplication_ReleaseFrame() failed\n");
        }

        /* Fill metadata */
        frame->pts        = av_gettime_relative() - priv->epoch;
        frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));

        FormatExtraData *fe = (FormatExtraData *)frame->opaque_ref->data;
        fe->time_base       = AV_TIME_BASE_Q;
        fe->avg_frame_rate  = priv->framerate;
        fe->rotation        = priv->rotation;

        sp_log(entry, SP_LOG_TRACE, "Pushing frame to FIFO, pts = %f (%I64d)\n",
               av_q2d(fe->time_base) * frame->pts, frame->pts);

        /* We don't do this check at the start on since there's still some chance
        * whatever's consuming the FIFO will be done by now. */
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

        /* /!\ Hack /!\
        *
        * IDXGIOutputDuplication_AcquireNextFrame() shouldn't be called directly.
        * When called, nvenc API will be blocked. To workaround this, wait as much
        * as possible to get a new frame.
        *
        * Ref: https://docs.nvidia.com/video-technologies/video-codec-sdk/nvenc-video-encoder-api-prog-guide/#threading-model
        */
        IDXGIOutput1_WaitForVBlank(dxgi_output);
    }

fail:
    av_frame_free(&frame);

    stop_capture(priv);
    release_texture_pool(priv);

    if (dxgi_output)
        IDXGIOutput1_Release(dxgi_output);

    priv->err = err;
    return NULL;
}

static int dxgi_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx, void *ctx,
                             void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;

    IOSysEntry *entry = ctx;
    DxgiCapture *io_priv = entry->io_priv;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        io_priv->epoch = atomic_load(event->epoch);
        pthread_create(&io_priv->pull_thread, NULL, dxgi_capture_thread, entry);
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

static int dxgi_ioctx_ctrl(AVBufferRef *entry, SPEventType ctrl, void *arg)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    return sp_ctrl_template(iosys_entry, iosys_entry->events, 0x0,
                            dxgi_ioctx_ctrl_cb, ctrl, arg);
}

static int dxgi_init_io(AVBufferRef *ctx_ref, AVBufferRef *entry,
                       AVDictionary *opts)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    DxgiCapture *cap_priv = (DxgiCapture *)av_mallocz(sizeof(*cap_priv));

    if (!cap_priv)
        return AVERROR(ENOMEM);

    cap_priv->width = -1;
    cap_priv->height = -1;
    cap_priv->quit = ATOMIC_VAR_INIT(0);

    iosys_entry->ctrl = dxgi_ioctx_ctrl;
    iosys_entry->events = sp_bufferlist_new();

    iosys_entry->io_priv = cap_priv;

    return 0;
}

const IOSysAPI src_dxgi = {
    .name      = "dxgi",
    .init_sys  = dxgi_init,
    .init_io   = dxgi_init_io,
    .ref_entry = dxgi_ref_entry,
    .ctrl      = dxgi_ctrl,
};
