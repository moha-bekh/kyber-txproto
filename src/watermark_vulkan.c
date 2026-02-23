#define COBJMACROS

#include <libavutil/fifo.h>
#include <libavutil/time.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>

#include <libplacebo/config.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/libav.h>

#include <libtxproto/filter_proc.h>
#include <libtxproto/txproto_main.h>

#include "watermark.h"
#include "watermark_internal.h"
#include "watermark_vulkan.h"

#if defined(__linux__) && defined(HAVE_VULKAN)
int watermark_vulkan_init(struct WatermarkContext *ctx)
{
    FilterProcContext *proc_ctx = ctx->proc_ctx;
    int ret;

    ret = av_hwdevice_ctx_create(
        &ctx->cuda_ctx,
        AV_HWDEVICE_TYPE_CUDA,
        NULL,
        NULL,
        0
    );
    if (ret < 0) {
        char errstr[0xFF];
        av_strerror(ret, errstr, sizeof(errstr));
        sp_log(proc_ctx, SP_LOG_ERROR, "av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_CUDA) failed: %s\n", errstr);
        return ret;
    }

    ret = av_hwdevice_ctx_create_derived(
        &ctx->vulkan_ctx,
        AV_HWDEVICE_TYPE_VULKAN,
        ctx->cuda_ctx,
        0
    );
    if (ret < 0) {
        char errstr[0xFF];
        av_strerror(ret, errstr, sizeof(errstr));
        sp_log(proc_ctx, SP_LOG_ERROR, "av_hwdevice_ctx_create_derived(AV_HWDEVICE_TYPE_VULKAN) failed: %s\n", errstr);
        av_buffer_unref(&ctx->cuda_ctx);
        return ret;
    }

          AVHWDeviceContext *ref_data = (AVHWDeviceContext*)ctx->vulkan_ctx->data;
        AVVulkanDeviceContext *hwctx = ref_data->hwctx;

        ctx->device = pl_vulkan_import(
            ctx->log,
            pl_vulkan_import_params(
                .instance             = hwctx->inst,
                .phys_device          = hwctx->phys_dev,
                .device               = hwctx->act_dev,
                .extensions           = hwctx->enabled_dev_extensions,
                .num_extensions       = hwctx->nb_enabled_dev_extensions,
                .queue_graphics.index = hwctx->queue_family_index,
                .queue_graphics.count = hwctx->nb_graphics_queues,
                .queue_compute.index  = hwctx->queue_family_comp_index,
                .queue_compute.count  = hwctx->nb_comp_queues,
                .queue_transfer.index = hwctx->queue_family_tx_index,
                .queue_transfer.count = hwctx->nb_tx_queues,
                .features             = &hwctx->device_features,
            )
        );
        if (!ctx->device) {
            sp_log(proc_ctx, SP_LOG_ERROR, "Failed to import Vulkan device\n");
            return -1;
        }

        ctx->format = AV_PIX_FMT_VULKAN;

    return 0;
}

int watermark_vulkan_cleanup(struct WatermarkContext *ctx)
{
    av_buffer_unref(&ctx->vulkan_ctx);
    av_buffer_unref(&ctx->cuda_ctx);

    return 0;
}

int watermark_vulkan_recreate_hwframe_ctx(
    struct WatermarkContext *ctx,
    AVBufferRef **hwframe_ctx_ref,
    AVFrame *input,
    enum AVPixelFormat sw_format,
    VkImageUsageFlags extra_usage)
{
    FilterProcContext *proc_ctx = ctx->proc_ctx;
    AVHWFramesContext *hwframe_ctx;
    AVVulkanFramesContext *vkfc;
    int err;

    if (*hwframe_ctx_ref) {
        hwframe_ctx = (AVHWFramesContext*) (*hwframe_ctx_ref)->data;

        if (hwframe_ctx->width == input->width && hwframe_ctx->height == input->height) {
            return 0;
        }
        av_buffer_unref(hwframe_ctx_ref);
    }

    *hwframe_ctx_ref = av_hwframe_ctx_alloc(ctx->vulkan_ctx);

    hwframe_ctx = (AVHWFramesContext*) (*hwframe_ctx_ref)->data;
    hwframe_ctx->format = AV_PIX_FMT_VULKAN;
    hwframe_ctx->sw_format = sw_format;
    hwframe_ctx->width = input->width;
    hwframe_ctx->height = input->height;

    vkfc = hwframe_ctx->hwctx;
    vkfc->usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | extra_usage;

    sp_log(proc_ctx, SP_LOG_INFO, "Creating Vulkan context with format %s\n", av_get_pix_fmt_name(hwframe_ctx->sw_format));
    err = av_hwframe_ctx_init(*hwframe_ctx_ref);
    if (err < 0) {
        av_buffer_unref(hwframe_ctx_ref);
        return err;
    }

    return 0;
}

int watermark_vulkan_process_video_frame(struct WatermarkContext *ctx, AVFrame **input)
{
    FilterProcContext *proc_ctx = ctx->proc_ctx;
    int err;
    AVFrame *in_f = *input;

    if (!in_f)
        return 0;

    /* Hardware mapping and transferring */
    if ((in_f->hw_frames_ctx || ctx->vulkan_hwframe_ctx) &&
        (in_f->format != ctx->format)) {
        const AVPixFmtDescriptor *id = av_pix_fmt_desc_get(in_f->format);
        const AVPixFmtDescriptor *ed = av_pix_fmt_desc_get(ctx->format);

        if (in_f->hw_frames_ctx && ctx->vulkan_hwframe_ctx) { /* Map */
            AVFrame *mapped_frame = av_frame_alloc();
            if (!mapped_frame)
                return AVERROR(ENOMEM);

            mapped_frame->format = ctx->format;

            /* Set frame hardware context referencce */
            mapped_frame->hw_frames_ctx = av_buffer_ref(ctx->vulkan_hwframe_ctx);
            if (!mapped_frame->hw_frames_ctx)
                return AVERROR(ENOMEM);

            err = av_hwframe_map(mapped_frame, in_f, AV_HWFRAME_MAP_READ);
            if (err < 0) {
                sp_log(proc_ctx, SP_LOG_ERROR, "Error mapping: %s!\n", av_err2str(err));
                return err;
            }

            /* Copy properties */
            av_frame_copy_props(mapped_frame, in_f);

            /* Replace original frame with mapped */
            av_frame_free(&in_f);
            in_f = mapped_frame;
        } else if ((id->flags ^ ed->flags) & AV_PIX_FMT_FLAG_HWACCEL) {
            AVFrame *tx_frame = av_frame_alloc();
            if (!tx_frame)
                return AVERROR(ENOMEM);

            tx_frame->width = in_f->width;
            tx_frame->height = in_f->height;

            if (ed->flags & AV_PIX_FMT_FLAG_HWACCEL) {
                tx_frame->format = ctx->format;

                err = av_hwframe_get_buffer(ctx->vulkan_hwframe_ctx, tx_frame, 0);
                if (err < 0) {
                    sp_log(proc_ctx, SP_LOG_ERROR, "Error allocating frame: %s!\n", av_err2str(err));
                    return err;
                }
            } else {
                AVHWFramesContext *in_hwfc = (AVHWFramesContext *)in_f->hw_frames_ctx->data;
                tx_frame->format = in_hwfc->sw_format;

                err = av_frame_get_buffer(tx_frame, 4096);
                if (err < 0) {
                    sp_log(proc_ctx, SP_LOG_ERROR, "Error allocating frame: %s!\n", av_err2str(err));
                    return err;
                }
            }

            err = av_hwframe_transfer_data(tx_frame, in_f, 0);
            if (err < 0) {
                sp_log(proc_ctx, SP_LOG_ERROR, "Error transferring: %s!\n", av_err2str(err));
                return err;
            }

            av_frame_copy_props(tx_frame, in_f);
            av_frame_free(&in_f);
            in_f = tx_frame;
        }
    }

    /* Both or neither must have a hardware reference now */
    sp_assert(!((!!in_f->hw_frames_ctx) ^ (!!ctx->vulkan_hwframe_ctx)));

    *input = in_f;

    return 0;
}

int watermark_vulkan_convert_frame_to_nv12(struct WatermarkContext *ctx, struct pl_frame *in_frame, AVFrame *in_avframe, AVFrame **out_avframe)
{
    FilterProcContext *proc_ctx = ctx->proc_ctx;
    AVFrame *av_frame_nv12 = NULL;
    struct pl_frame in_frame_nv12;
    pl_tex in_tex_nv12[4];
    bool in_frame_nv12_mapped = false;
    int ret = 0;

    ret = watermark_vulkan_recreate_hwframe_ctx(ctx, &ctx->vulkan_hwframe_nv12_ctx, in_avframe, AV_PIX_FMT_NV12, 0);
    if (ret < 0) {
        sp_log(proc_ctx, SP_LOG_WARN, "watermark_vulkan_recreate_hwframe_ctx(AV_PIX_FMT_NV12) failed\n");
        return ret;
    }

    av_frame_nv12 = av_frame_alloc();
    if (!av_frame_nv12) {
        sp_log(proc_ctx, SP_LOG_ERROR, "av_frame_alloc() failed\n");
        return AVERROR(ENOMEM);
    }

    av_frame_nv12->width = in_avframe->width;
    av_frame_nv12->height = in_avframe->height;
    av_frame_nv12->format = AV_PIX_FMT_VULKAN;

    ret = av_hwframe_get_buffer(ctx->vulkan_hwframe_nv12_ctx, av_frame_nv12, 0);
    if (ret < 0) {
        sp_log(proc_ctx, SP_LOG_WARN, "av_hwframe_get_buffer() failed\n");
        goto fail;
    }

    av_frame_copy_props(av_frame_nv12, in_avframe);

    bool ok = pl_map_avframe_ex(
        ctx->device->gpu,
        &in_frame_nv12,
        pl_avframe_params(
            .frame = av_frame_nv12,
            .tex   = in_tex_nv12,
        )
    );
    if (!ok) {
        sp_log(proc_ctx, SP_LOG_WARN, "pl_map_avframe_ex() failed\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    in_frame_nv12_mapped = true;

    struct pl_render_params render_params = pl_render_default_params;
    in_frame->crop.x0 = 0.0;
    in_frame->crop.y0 = 0.0;
    in_frame->crop.x1 = in_avframe->width;
    in_frame->crop.y1 = in_avframe->height;

    ok = pl_render_image(
        ctx->renderer,
        in_frame,
        &in_frame_nv12,
        &render_params
    );
    if (!ok) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Error when rendering NV12\n");
    }

    pl_unmap_avframe(ctx->device->gpu, &in_frame_nv12);
    in_frame_nv12_mapped = false;

    *out_avframe = av_frame_nv12;
    return 0;

fail:
    if (in_frame_nv12_mapped) {
        pl_unmap_avframe(ctx->device->gpu, &in_frame_nv12);
    }
    av_frame_free(&av_frame_nv12);
    return ret;
}

#endif /* __linux__ && HAVE_VULKAN */