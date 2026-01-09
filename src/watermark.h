#pragma once

#include <libavutil/buffer.h>
#include <libtxproto/txproto.h>

/* Opaque WatermarkContext - defined in watermark.c */
typedef struct WatermarkContext WatermarkContext;

AVBufferRef *watermark_filter_create(TXMainContext *ctx, const char *image_path, TxWatermarkPosition position);
