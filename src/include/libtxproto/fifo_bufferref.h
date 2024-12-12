#pragma once

#include <libavutil/buffer.h>

enum SPBufferRefFIFOFlags {
    BUFFERREF_FIFO_BLOCK_MAX_OUTPUT = (1 << 0),
    BUFFERREF_FIFO_BLOCK_NO_INPUT   = (1 << 1),
    BUFFERREF_FIFO_PULL_NO_BLOCK    = (1 << 2),
};

#define FRENAME(x) BUFFERREF_FIFO_ ## x
#define RENAME(x)  sp_bufferref_ ##x
#define FNAME      enum SPBufferRefFIFOFlags
#define TYPE       AVBufferRef

/* Create */
AVBufferRef *RENAME(fifo_create)(void *opaque, int max_queued, FNAME block_flags); /* -1 = INF, 0 = none */
AVBufferRef *RENAME(fifo_ref)(AVBufferRef *src, int max_queued, FNAME block_flags);

/* Query */
int RENAME(fifo_is_full)(AVBufferRef *src);
int RENAME(fifo_get_size)(AVBufferRef *src);
int RENAME(fifo_get_max_size)(AVBufferRef *src);

/* Modify */
void RENAME(fifo_set_max_queued)(AVBufferRef *dst, int max_queued);
void RENAME(fifo_set_block_flags)(AVBufferRef *dst, FNAME block_flags);
int  RENAME(fifo_string_to_block_flags)(FNAME *dst, const char *in_str);

/* Up/downstreaming */
int RENAME(fifo_mirror)(AVBufferRef *dst, AVBufferRef *src);
int RENAME(fifo_unmirror)(AVBufferRef *dst, AVBufferRef *src);
int RENAME(fifo_unmirror_all)(AVBufferRef *dst);

/* I/O */
int   RENAME(fifo_push)(AVBufferRef *dst, TYPE *in);
TYPE *RENAME(fifo_pop)(AVBufferRef *src);
int   RENAME(fifo_pop_flags)(AVBufferRef *src, TYPE **ret, FNAME flags);
TYPE *RENAME(fifo_peek)(AVBufferRef *src);

#undef TYPE
#undef FNAME
#undef RENAME
#undef FRENAME
