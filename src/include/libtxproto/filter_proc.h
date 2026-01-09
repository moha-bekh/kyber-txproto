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

#pragma once

#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>

#include <libtxproto/utils.h>
#include <libtxproto/log.h>

typedef struct FilterProcPad {
    struct FilterProcContext *main;
    char *name;
    int is_out;
    size_t idx;

    enum AVMediaType type;

    /* Input only */
    int eos;

    /* Output only */
    int dropped_frames;

    AVBufferRef *fifo;
} FilterProcPad;

typedef struct FilterStreamInfo {
    enum AVMediaType type;
    AVRational time_base;
    AVRational frame_rate;
    int format;
} FilterStreamInfo;

typedef struct FilterProcContext FilterProcContext;

typedef struct FilterProcVtable {
    void *(*alloc)(void*, FilterProcContext *ctx);
    void (*free)(void*);

    int (*configure)(void*);

    size_t (*get_input_count)(void*);
    size_t (*get_output_count)(void*);

    int (*input_get_name)(void*, size_t idx, char *buf, size_t buf_capacity);
    int (*output_get_name)(void*, size_t idx, char *buf, size_t buf_capacity);

    int (*input_get_info)(void*, size_t idx, FilterStreamInfo *info);
    int (*output_get_info)(void*, size_t idx, FilterStreamInfo *info);

    int (*on_command)(void*, const char *key, const char *value);

    /**
     * Called when a frame is pushed on an input
     *
     * Must take ownership of the frame reference with `av_frame_move_ref`.
     *
     * in_idx - the input on which the frame is being pushed
     * no_process - if set, don't immediately process the frame; buffer it
     *              instead
     */
    int (*on_frame)(void*, size_t in_idx, AVFrame*, int no_process);

    /**
     * Pull a frame from an output.
     *
     * out_idx - the output from which to pull the frame
     * no_process - if set, return `AVERROR(EAGAIN)` when no frame is ready yet
     */
    int (*pull_frame)(void*, size_t out_idx, AVFrame*, int no_process);
} FilterProcVtable;

typedef struct FilterProcContext {
    SPClass *class;

    SPBufferList *events;
    FilterProcVtable vt;
    void *data;

    int send_eos;

    /* Pads - inputs */
    FilterProcPad **in_pads;
    int num_in_pads;

    /* Pads - outputs */
    FilterProcPad **out_pads;
    int num_out_pads;

    /* I/O thread */
    pthread_t filter_thread;
    pthread_mutex_t lock;

    int64_t epoch;
} FilterProcContext;

/* Allocate a filtering context */
AVBufferRef *sp_filter_alloc_proc(void);

/**
 * Initializes a single filter, such as a scaling filter
 * name - custom name used for logging
 * filt - the name of the filter (ownership transferred, so must be av_malloc'd)
 * opts - an AVDictionary with the options for the filter, ownership goes to
 *        FilterProcContext
 * graph_opts - options for the filtergraph (threads, thread_type, etc, look:
 *              avfiltergraph.c), ownership goes to FilterProcContext
 * derive_device - derive a hardware device from the input frames and use it
 */
int sp_init_filter_proc(AVBufferRef *ctx_ref, const char *name, void *arg,
                        const FilterProcVtable *vtable);

/**
 * Returns the FIFO of the specified input pad.
 *
 * The returned `AVBufferRef` is not owned, and must not be freed.
 */
AVBufferRef *sp_filter_proc_get_input_fifo(const FilterProcContext *ctx,
                                           const char *name);

/**
 * Returns the FIFO of the specified output pad.
 *
 * The returned `AVBufferRef` is not owned, and must not be freed.
 */
AVBufferRef *sp_filter_proc_get_output_fifo(const FilterProcContext *ctx,
                                            const char *name);

/**
 * Filter control.
 */
int sp_filter_ctrl_proc(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg);
