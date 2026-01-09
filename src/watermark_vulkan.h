#pragma once

#include <libavutil/frame.h>
#include <vulkan/vulkan.h>

struct WatermarkContext;
struct pl_frame;

int watermark_vulkan_init(struct WatermarkContext *ctx);
int watermark_vulkan_cleanup(struct WatermarkContext *ctx);
int watermark_vulkan_recreate_hwframe_ctx(struct WatermarkContext *ctx, AVBufferRef **hwframe_ctx_ref, AVFrame *input, enum AVPixelFormat sw_format, VkImageUsageFlags extra_usage);
int watermark_vulkan_process_video_frame(struct WatermarkContext *ctx, AVFrame **input);
int watermark_vulkan_convert_frame_to_nv12(struct WatermarkContext *ctx, struct pl_frame *in_frame, AVFrame *in_avframe, AVFrame **out_avframe);
