#define COBJMACROS

#include <libavutil/fifo.h>
#include <libavutil/time.h>

#include <libplacebo/config.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/libav.h>

#include <libswscale/swscale.h>
#include <math.h>

#ifdef HAVE_D3D11
#include <libavutil/hwcontext_d3d11va.h>
#include <libplacebo/d3d11.h>
#endif // HAVE_D3D11

#include "../config.h"
#include "metrics.h"
#include "watermark.h"
#include "watermark_internal.h"
#include <libtxproto/filter_proc.h>
#include <libtxproto/fifo_frame.h>
#include <libtxproto/txproto.h>
#include <libtxproto/txproto_main.h>

#ifdef HAVE_VULKAN
#include "watermark_vulkan.h"
#endif // HAVE_VULKAN

typedef struct WatermarkInitArgs {
    const char *image_path;
    struct tx_metrics *metrics;
    TxWatermarkPosition position;
} WatermarkInitArgs;

static AVFrame *load_image_as_rgba(FilterProcContext *proc_ctx, const char *path) {
    AVFrame *out_frame = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Could not open %s\n", path);
        return NULL;
    }

    sp_log(proc_ctx, SP_LOG_INFO, "%s opened\n", path);

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (!codec) {
        sp_log(proc_ctx, SP_LOG_ERROR, "AV_CODEC_ID_PNG not supported\n");
        fclose(f);
        return NULL;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint8_t *file_bytes = NULL;
    int ret;

    if (!ctx || !pkt || !frame) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Allocation failure\n");
        goto end;
    }

    if ((ret = avcodec_open2(ctx, codec, NULL)) < 0) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Could not open codec: %s\n", av_err2str(ret));
        goto end;
    }

    fseek(f, 0, SEEK_END);
    long fsize_long = ftell(f);
    if (fsize_long <= 0) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Empty or unreadable file\n");
        goto end;
    }
    rewind(f);

    size_t fsize = (size_t)fsize_long;
    file_bytes = (uint8_t *)av_malloc(fsize);
    if (!file_bytes) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Out of memory\n");
        goto end;
    }

    size_t nread = fread(file_bytes, 1, fsize, f);
    if (nread != fsize) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Short read (%llu/%llu)\n", (unsigned long long)nread, (unsigned long long)fsize);
        goto end;
    }

    ret = av_new_packet(pkt, (int)fsize);
    if (ret < 0) {
        sp_log(proc_ctx, SP_LOG_ERROR, "av_new_packet failed: %s\n", av_err2str(ret));
        goto end;
    }
    memcpy(pkt->data, file_bytes, fsize);

    if ((ret = avcodec_send_packet(ctx, pkt)) < 0) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Error sending packet: %s\n", av_err2str(ret));
        goto end;
    }

    ret = avcodec_receive_frame(ctx, frame);
    if (ret < 0) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Error while decoding: %s\n", av_err2str(ret));
        goto end;
    }

    // Convert to RGBA
    enum AVPixelFormat dst_fmt = AV_PIX_FMT_RGBA;
    struct SwsContext *sws = sws_getContext(
        frame->width, frame->height, (enum AVPixelFormat)frame->format,
        frame->width, frame->height, dst_fmt,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    if (!sws) {
        sp_log(proc_ctx, SP_LOG_ERROR, "sws_getContext failed\n");
        goto end;
    }

    out_frame = av_frame_alloc();
    if (!out_frame) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Out of memory for output frame\n");
        sws_freeContext(sws);
        goto end;
    }

    out_frame->width = frame->width;
    out_frame->height = frame->height;
    out_frame->format = dst_fmt;

    ret = av_frame_get_buffer(out_frame, 0);
    if (ret < 0) {
        sp_log(proc_ctx, SP_LOG_ERROR, "av_frame_get_buffer failed: %s\n", av_err2str(ret));
        av_frame_free(&out_frame);
        sws_freeContext(sws);
        goto end;
    }

    int h = sws_scale(
        sws,
        (const uint8_t * const *)frame->data,
        frame->linesize,
        0,
        frame->height,
        out_frame->data,
        out_frame->linesize
    );

    sws_freeContext(sws);

    if (h != frame->height) {
        sp_log(proc_ctx, SP_LOG_ERROR, "sws_scale failed (%d/%d)\n", h, frame->height);
        av_frame_free(&out_frame);
        goto end;
    }

end:
    if (file_bytes)
        av_free(file_bytes);
    if (frame)
        av_frame_free(&frame);
    if (pkt)
        av_packet_free(&pkt);
    if (ctx)
        avcodec_free_context(&ctx);
    fclose(f);
    return out_frame;
}

static void update_watermark_crop(struct WatermarkContext *ctx, AVFrame *frame, struct pl_frame *target_frame)
{
    if (!frame || !target_frame)
        return;

    const float frame_width = (float) frame->width;
    const float frame_height = (float) frame->height;
    // Preserve the original watermark size; only shrink if it would overflow the frame.
    float target_width = ctx->wm_img_width > 0 ? (float) ctx->wm_img_width : 1.0f;
    float target_height = ctx->wm_img_height > 0 ? (float) ctx->wm_img_height : 1.0f;

    if (frame_width > 0.0f && target_width > frame_width) {
        const float scale = frame_width / target_width;
        target_width = frame_width;
        target_height *= scale;
    }

    if (frame_height > 0.0f && target_height > frame_height) {
        const float scale = frame_height / target_height;
        target_height = frame_height;
        target_width *= scale;
    }

    target_width = fmaxf(target_width, 1.0f);
    target_height = fmaxf(target_height, 1.0f);

    // Clamp position values to [-1.0, 1.0]
    float pos_x = fminf(fmaxf(ctx->position.x, -1.0f), 1.0f);
    float pos_y = fminf(fmaxf(ctx->position.y, -1.0f), 1.0f);

    // Convert from [-1.0, 1.0] to [0.0, 1.0]
    float norm_x = (pos_x + 1.0f) / 2.0f;
    float norm_y = (1.0f - pos_y) / 2.0f;

    // Calculate available space for positioning
    float max_x = fmaxf(frame_width - target_width, 0.0f);
    float max_y = fmaxf(frame_height - target_height, 0.0f);

    // Linear interpolation between 0 (left/top) and max (right/bottom)
    float crop_x0 = max_x * norm_x;
    float crop_y0 = max_y * norm_y;

    const float max_x0 = fmaxf(frame_width - target_width, 0.0f);
    const float max_y0 = fmaxf(frame_height - target_height, 0.0f);

    crop_x0 = fminf(fmaxf(crop_x0, 0.0f), max_x0);
    crop_y0 = fminf(fmaxf(crop_y0, 0.0f), max_y0);

    target_frame->crop.x0 = crop_x0;
    target_frame->crop.y0 = crop_y0;
    target_frame->crop.x1 = crop_x0 + target_width;
    target_frame->crop.y1 = crop_y0 + target_height;
}

static void pl_log_cb(void *priv, enum pl_log_level level, const char *msg)
{
    struct FilterProcContext *ctx = priv;
    enum SPLogLevel sp_level;

    switch (level) {
    case PL_LOG_FATAL:  sp_level = SP_LOG_FATAL;   break;
    case PL_LOG_ERR:    sp_level = SP_LOG_ERROR;   break;
    case PL_LOG_WARN:   sp_level = SP_LOG_WARN;    break;
    case PL_LOG_INFO:   sp_level = SP_LOG_INFO;    break;
    case PL_LOG_DEBUG:  sp_level = SP_LOG_DEBUG;   break;
    case PL_LOG_TRACE:  sp_level = SP_LOG_TRACE;   break;
    default:            return;
    }

    sp_log(ctx, sp_level, "%s\n", msg);
}


static int proc_process(struct WatermarkContext *ctx)
{
    FilterProcContext *proc_ctx = ctx->proc_ctx;
    AVFrame *av_frame;
    int ret = 0;

    while (av_fifo_read(ctx->in_frames, &av_frame, 1) >= 0) {
        bool frame_sent = false;
        bool drop_frame = false;
        bool fatal_error = false;
        bool in_frame_mapped = false;

        // Push watermarking metric
        if (ctx->metrics && av_frame) {
            const tx_metrics_entry metrics[] = {
                { .key = "pts", .value = av_frame->pts },
                { .key = "watermarking", .value = av_gettime_relative() },
            };
            tx_metrics_push(ctx->metrics, metrics, SP_ARRAY_ELEMS(metrics));
        }

#if defined(HAVE_VULKAN)
        ret = watermark_vulkan_recreate_hwframe_ctx(ctx, &ctx->vulkan_hwframe_ctx, av_frame, av_frame->format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (ret < 0) {
            sp_log(proc_ctx, SP_LOG_WARN, "watermark_vulkan_recreate_hwframe_ctx(VK_HWFRAME_CTX_DEFAULT) failed\n");
            fatal_error = true;
            goto loop_cleanup;
        }

        ret = watermark_vulkan_process_video_frame(ctx, &av_frame);
        if (ret < 0) {
            sp_log(proc_ctx, SP_LOG_WARN, "watermark_vulkan_process_video_frame() failed\n");
            fatal_error = true;
            goto loop_cleanup;
        }
#endif

        struct pl_frame in_frame;
        pl_tex in_tex[4];

        bool ok = pl_map_avframe_ex(
            ctx->device->gpu,
            &in_frame,
            pl_avframe_params(
                .frame = av_frame,
                .tex   = in_tex,
            )
        );
        if (!ok) {
            sp_log(proc_ctx, SP_LOG_WARN, "pl_map_avframe_ex() failed\n");
            drop_frame = true;
            goto loop_cleanup;
        }
        in_frame_mapped = true;

        // Render overlay
        update_watermark_crop(ctx, av_frame, &in_frame);

        struct pl_render_params render_params = pl_render_default_params;
        render_params.border = PL_CLEAR_SKIP;
        render_params.background = PL_CLEAR_SKIP;
        render_params.blend_params = &pl_alpha_overlay;

        ok = pl_render_image(
            (pl_renderer)ctx->renderer,
            (const struct pl_frame *)ctx->wm_frame,
            &in_frame,
            &render_params
        );
        if (!ok) {
            sp_log(proc_ctx, SP_LOG_ERROR, "Error when rendering watermark\n");
        }

#ifdef HAVE_VULKAN
        // Convert to NV12
        AVFrame *av_frame_nv12 = NULL;
        ret = watermark_vulkan_convert_frame_to_nv12(ctx, &in_frame, av_frame, &av_frame_nv12);
        pl_unmap_avframe(ctx->device->gpu, &in_frame);
        in_frame_mapped = false;
        if (ret < 0) {
            fatal_error = true;
            goto loop_cleanup;
        }

        av_frame_free(&av_frame);
        av_frame = av_frame_nv12;
#else
        pl_unmap_avframe(ctx->device->gpu, &in_frame);
        in_frame_mapped = false;
#endif

        ret = av_fifo_write(ctx->out_frames, &av_frame, 1);
        if (ret < 0) {
            sp_log(proc_ctx, SP_LOG_WARN, "av_fifo_write(out_frames) failed: %d\n", ret);
            drop_frame = true;
            goto loop_cleanup;
        }

        // Push watermarked metric
        if (ctx->metrics && av_frame) {
            const tx_metrics_entry metrics[] = {
                { .key = "pts", .value = av_frame->pts },
                { .key = "watermarked", .value = av_gettime_relative() },
            };
            tx_metrics_push(ctx->metrics, metrics, SP_ARRAY_ELEMS(metrics));
        }

        frame_sent = true;

loop_cleanup:
        if (in_frame_mapped) {
            pl_unmap_avframe(ctx->device->gpu, &in_frame);
        }

        if (frame_sent) {
            break;
        }

        av_frame_free(&av_frame);

        if (fatal_error) {
            break;
        }

        if (drop_frame) {
            continue;
        }

        break;
    }

    return ret < 0 ? ret : 0;
}

static int proc_configure(void *raw)
{
    struct WatermarkContext *ctx = raw;
    FilterProcContext *proc_ctx = ctx->proc_ctx;
    AVFrame *frame;
    int ret;

    if (proc_ctx->num_in_pads != 1) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Expected exactly one input pad\n");
        return -1;
    }

    frame = sp_frame_fifo_peek(proc_ctx->in_pads[0]->fifo);
    if (!frame) {
        sp_log(proc_ctx, SP_LOG_WARN, "Couldn't get config frame\n");
        return -1;
    }

    if (frame->opaque_ref) {
        FormatExtraData *fe = (FormatExtraData *)frame->opaque_ref->data;
        if (fe->avg_frame_rate.num && fe->avg_frame_rate.den) {
            ctx->frame_rate = fe->avg_frame_rate;
        }
    }

    ctx->log = (void *)pl_log_create(PL_API_VER, pl_log_params(
        .log_level = PL_LOG_INFO,
        .log_cb = pl_log_cb,
        .log_priv = proc_ctx,
    ));

    switch (frame->format) {
#if defined(HAVE_D3D11)
    case AV_PIX_FMT_D3D11: {
        if (!frame->hw_frames_ctx) {
            sp_log(proc_ctx, SP_LOG_WARN, "Not an hardware frame\n");
            av_frame_free(&frame);
            return -1;
        }

        AVHWFramesContext *hw_frames_ctx_cfg = (AVHWFramesContext *) frame->hw_frames_ctx->data;
        AVD3D11VADeviceContext *d3d11va_context = (AVD3D11VADeviceContext *) hw_frames_ctx_cfg->device_ctx->hwctx;
        ID3D11Device *device = d3d11va_context->device;

        struct pl_d3d11_params params = {
            .device = device,
        };

        ctx->device = pl_d3d11_create(ctx->log, &params);
        if (!ctx->device) {
            sp_log(proc_ctx, SP_LOG_ERROR, "Failed to create D3D11 device\n");
            av_frame_free(&frame);
            return -1;
        }
        ctx->format = AV_PIX_FMT_D3D11;
        break;
    }
#endif // HAVE_D3D11
    default: {
#if defined(HAVE_VULKAN)
        ret = watermark_vulkan_init(ctx);
        if (ret < 0) {
            sp_log(proc_ctx, SP_LOG_ERROR, "watermark_vulkan_init() failed\n");
            av_frame_free(&frame);
            return ret;
        }
        ctx->sw_format = frame->format;
        break;
#else
        sp_log(proc_ctx, SP_LOG_ERROR, "Unexpected format: %s\n", av_get_pix_fmt_name(frame->format));
        av_frame_free(&frame);
        return -1;
#endif // HAVE_VULKAN
    }}

    ctx->renderer = pl_renderer_create(ctx->log, ctx->device->gpu);
    if (!ctx->renderer) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Failed to create renderer\n");
        av_frame_free(&frame);
        goto fail;
    }

    av_frame_free(&frame);

    // Load watermark
    ctx->wm_avframe = load_image_as_rgba(proc_ctx, ctx->image_path);
    if (!ctx->wm_avframe) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Failed to load watermark image\n");
        goto fail;
    }

    struct pl_frame *wm_frame = av_mallocz(sizeof(struct pl_frame));
    if (!wm_frame) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Failed to allocate watermark frame\n");
        goto fail;
    }

    bool ok = pl_map_avframe_ex(
        ctx->device->gpu,
        wm_frame,
        pl_avframe_params(
            .frame = ctx->wm_avframe,
            .tex   = ctx->wm_tex,
        )
    );
    if (!ok) {
        sp_log(proc_ctx, SP_LOG_ERROR, "pl_map_avframe_ex() failed for watermark\n");
        av_free(wm_frame);
        goto fail;
    }

    ctx->wm_frame = wm_frame;
    ctx->wm_img_width = ctx->wm_avframe->width;
    ctx->wm_img_height = ctx->wm_avframe->height;

    return 0;

fail:
    if (ctx->renderer) {
        pl_renderer_destroy((pl_renderer *)&ctx->renderer);
    }
#if defined(HAVE_D3D11)
    pl_d3d11_destroy(&ctx->device);
#elif defined(HAVE_VULKAN)
    pl_vulkan_destroy(&ctx->device);
#endif
    return -1;
}

static void *proc_alloc(void *raw_arg, FilterProcContext *proc_ctx)
{
    WatermarkInitArgs *args = (WatermarkInitArgs *)raw_arg;
    struct WatermarkContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Failed to allocate context\n");
        goto err;
    }

    ctx->image_path = av_strdup(args->image_path);
    if (!ctx->image_path) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Failed to allocate image path\n");
        goto err;
    }

    ctx->proc_ctx = proc_ctx;
    ctx->metrics = args->metrics;
    ctx->position = args->position;
    ctx->in_frames = av_fifo_alloc2(8, sizeof(AVFrame *), AV_FIFO_FLAG_AUTO_GROW);
    if (!ctx->in_frames) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Failed to allocate input frames fifo\n");
        goto err;
    }

    ctx->out_frames = av_fifo_alloc2(8, sizeof(AVFrame *), AV_FIFO_FLAG_AUTO_GROW);
    if (!ctx->out_frames) {
        sp_log(proc_ctx, SP_LOG_ERROR, "Failed to allocate output frames fifo\n");
        goto err;
    }

    // Free the temporary init args
    av_free(args);

    return ctx;

err:
    if (ctx) {
        av_freep(&ctx->image_path);
        av_fifo_freep2(&ctx->in_frames);
        av_fifo_freep2(&ctx->out_frames);
        av_free(ctx);
    }
    av_free(args);
    return NULL;
}

static void proc_free(void *raw)
{
    struct WatermarkContext *ctx = raw;

    // drain fifo to free all frames
    while (av_fifo_can_read(ctx->in_frames)) {
        AVFrame *frame;
        av_fifo_read(ctx->in_frames, &frame, 1);
        av_frame_free(&frame);
    }
    av_fifo_freep2(&ctx->in_frames);

    // drain fifo to free all frames
    while (av_fifo_can_read(ctx->out_frames)) {
        AVFrame *frame;
        av_fifo_read(ctx->out_frames, &frame, 1);
        av_frame_free(&frame);
    }
    av_fifo_freep2(&ctx->out_frames);

    if (ctx->renderer) {
        pl_renderer_destroy((pl_renderer *)&ctx->renderer);
    }

    if (ctx->wm_frame) {
        pl_unmap_avframe(ctx->device->gpu, ctx->wm_frame);
        av_free(ctx->wm_frame);
        ctx->wm_frame = NULL;
    }

    if (ctx->wm_avframe) {
        av_frame_free(&ctx->wm_avframe);
    }

#if defined(HAVE_D3D11)
    pl_d3d11_destroy(&ctx->device);
#elif defined(HAVE_VULKAN)
    pl_vulkan_destroy(&ctx->device);
    av_buffer_unref(&ctx->vulkan_hwframe_nv12_ctx);
    av_buffer_unref(&ctx->vulkan_hwframe_ctx);
    watermark_vulkan_cleanup(ctx);
#endif

    if (ctx->log) {
        pl_log_destroy((pl_log *)&ctx->log);
    }

    av_freep(&ctx->image_path);
    av_freep(&ctx);
}

static size_t proc_get_input_count(void *raw)
{
    return 1;
}

static size_t proc_get_output_count(void *raw)
{
    return 1;
}

static int proc_input_get_name(void *raw, size_t idx, char *buf, size_t buf_capacity)
{
    switch (idx) {
    case 0:
        snprintf(buf, buf_capacity, "source");
        break;
    default:
        return AVERROR(EINVAL);
    }

    return 0;
}

static int proc_output_get_name(void *raw, size_t idx, char *buf, size_t buf_capacity)
{
    switch (idx) {
    case 0:
        snprintf(buf, buf_capacity, "out");
        break;
    default:
        return AVERROR(EINVAL);
    }

    return 0;
}

static int proc_input_get_info(void *raw, size_t idx, FilterStreamInfo *info)
{
    struct WatermarkContext *ctx = raw;

    switch (idx) {
    case 0:
        info->type = AVMEDIA_TYPE_VIDEO;
        info->frame_rate = ctx->frame_rate;
        info->format = ctx->sw_format;
        info->time_base = AV_TIME_BASE_Q;
        break;

    default:
        return AVERROR(EINVAL);
    }

    return 0;
}

static int proc_output_get_info(void *raw, size_t idx, FilterStreamInfo *info)
{
    struct WatermarkContext *ctx = raw;

    switch (idx) {
    case 0:
        info->type = AVMEDIA_TYPE_VIDEO;
        info->frame_rate = ctx->frame_rate;
        info->format = ctx->format;
        info->time_base = AV_TIME_BASE_Q;
        break;

    default:
        return AVERROR(EINVAL);
    }

    return 0;
}

static int on_source_frame(struct WatermarkContext *ctx, AVFrame *frame)
{
    int ret;

    if (!frame) {
        // EOF! write a NULL to the fifo
        return av_fifo_write(ctx->in_frames, &frame, 1);
    }

    AVFrame *frame_clone = av_frame_clone(frame);
    if (!frame_clone)
        return AVERROR(ENOMEM);

    ret = av_fifo_write(ctx->in_frames, &frame_clone, 1);
    if (ret < 0) {
        av_frame_free(&frame_clone);
        return ret;
    }

    return 0;
}

static int proc_on_frame(void *raw, size_t in_idx, AVFrame *frame, int no_process)
{
    struct WatermarkContext *ctx = raw;
    int ret;

    switch (in_idx) {
    case 0:
        ret = on_source_frame(ctx, frame);
        if (ret < 0) {
            sp_log(ctx->proc_ctx, SP_LOG_WARN, "on_source_frame() failed: %d\n", ret);
            return ret;
        }
        break;

    default:
        return AVERROR(EINVAL);
    }

    if (!no_process) {
        ret = proc_process(ctx);
        if (ret < 0) {
            sp_log(ctx, SP_LOG_WARN, "proc_process() failed: %d\n", ret);
            return ret;
        }
    }

    return 0;
}

static int proc_pull_frame(void *raw, size_t out_idx, AVFrame *out_frame, int no_process)
{
    struct WatermarkContext *ctx = raw;
    int ret;

    if (out_idx >= 1) {
        return AVERROR(EINVAL);
    }

    if (av_fifo_can_read(ctx->out_frames)) {
        AVFrame *frame;

        av_fifo_read(ctx->out_frames, &frame, 1);
        av_frame_unref(out_frame);
        if (frame) {
            av_frame_move_ref(out_frame, frame);
            av_frame_free(&frame);
            return 0;
        }

        return AVERROR_EOF;
    }

    if (no_process)
        return AVERROR(EAGAIN);

    ret = proc_process(ctx);
    if (ret < 0) {
        sp_log(ctx, SP_LOG_WARN, "proc_process() failed: %d\n", ret);
        return ret;
    }

    if (av_fifo_can_read(ctx->out_frames)) {
        AVFrame *frame;

        av_fifo_read(ctx->out_frames, &frame, 1);
        av_frame_unref(out_frame);
        if (frame) {
            av_frame_move_ref(out_frame, frame);
            av_frame_free(&frame);
            return 0;
        }

        return AVERROR_EOF;
    }

    return AVERROR(EAGAIN);
}

static const FilterProcVtable WATERMARK_FILTER_VTABLE = {
    .alloc = &proc_alloc,
    .free = &proc_free,
    .configure = &proc_configure,
    .get_input_count = &proc_get_input_count,
    .get_output_count = &proc_get_output_count,
    .input_get_name = &proc_input_get_name,
    .output_get_name = &proc_output_get_name,
    .input_get_info = &proc_input_get_info,
    .output_get_info = &proc_output_get_info,
    .on_command = NULL,
    .on_frame = &proc_on_frame,
    .pull_frame = &proc_pull_frame,
};

AVBufferRef *watermark_filter_create(TXMainContext *ctx, const char *image_path, TxWatermarkPosition position)
{
    WatermarkInitArgs *args = av_mallocz(sizeof(WatermarkInitArgs));
    if (!args)
        return NULL;

    args->image_path = image_path;
    args->metrics = tx_get_metrics(ctx);
    args->position = position;

    AVBufferRef *filter = tx_filter_proc_create(
        ctx,
        "watermark filter",
        &WATERMARK_FILTER_VTABLE,
        args,
        NULL
    );

    if (!filter) {
        av_free(args);
    }

    return filter;
}
