#pragma once

#include "../config.h"
#include <libavutil/buffer.h>
#include <libavutil/fifo.h>
#include <libtxproto/filter_proc.h>
#include <libtxproto/txproto.h>

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>

#ifdef HAVE_D3D11
#include <libplacebo/d3d11.h>
#endif

struct WatermarkContext {
    FilterProcContext *proc_ctx;
    char *image_path;

    // frame input and output fifos
    AVFifo *in_frames;
    AVFifo *out_frames;
    int format;
    int sw_format;
    bool is_bgr;
    AVRational frame_rate;

    // libplacebo context
    pl_log log;

#if defined(HAVE_D3D11)
    pl_d3d11 device;
#elif defined(HAVE_VULKAN)
    pl_vulkan device;
    AVBufferRef *cuda_ctx;
    AVBufferRef *vulkan_ctx;
    AVBufferRef *vulkan_hwframe_ctx;
    AVBufferRef *vulkan_hwframe_nv12_ctx;
#else
    void *device;  // placeholder for builds without backends
#endif

    pl_renderer renderer;

    // watermark
    AVFrame *wm_avframe;
    pl_tex wm_tex[4];
    struct pl_frame *wm_frame;

    uint32_t wm_img_width;
    uint32_t wm_img_height;
    TxWatermarkPosition position;

    // metrics
    struct tx_metrics *metrics;
};
