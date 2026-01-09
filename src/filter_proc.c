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

#include <stdatomic.h>

#include <libavutil/time.h>
#include <libavutil/avstring.h>

#include <libtxproto/filter_proc.h>
#include <libtxproto/fifo_frame.h>
#include <libtxproto/utils.h>

#include "os_compat.h"
#include "ctrl_template.h"

FN_CREATING(FilterProcContext, FilterProcPad, in_pad, in_pads, num_in_pads)
FN_CREATING(FilterProcContext, FilterProcPad, out_pad, out_pads, num_out_pads)

AVBufferRef *sp_filter_proc_get_input_fifo(const FilterProcContext *ctx,
                                           const char *name)
{
    for (int i = 0; i < ctx->num_in_pads; i++)
        if (!name || !strcmp(ctx->in_pads[i]->name, name))
            return ctx->in_pads[i]->fifo;
    return NULL;
}

AVBufferRef *sp_filter_proc_get_output_fifo(const FilterProcContext *ctx,
                                            const char *name)
{
    for (int i = 0; i < ctx->num_out_pads; i++)
        if (!name || !strcmp(ctx->out_pads[i]->name, name))
            return ctx->out_pads[i]->fifo;
    return NULL;
}

static int add_pad(FilterProcContext *ctx, int is_out, int idx,
                    const char *name, int first_init)
{
    /* Check if pad already exists */
    FilterProcPad *pad = NULL;
    FilterStreamInfo info;
    if (!is_out) {
        ctx->vt.input_get_info(ctx->data, idx, &info);
        for (int n = 0; n < ctx->num_in_pads; n++) {
            if (!strcmp(ctx->in_pads[n]->name, name)) {
                pad = ctx->in_pads[n];
                break;
            }
        }
    } else {
        ctx->vt.output_get_info(ctx->data, idx, &info);
        for (int n = 0; n < ctx->num_out_pads; n++) {
            if (!strcmp(ctx->out_pads[n]->name, name)) {
                pad = ctx->out_pads[n];
                break;
            }
        }
    }

    if (pad) {
        /* Graph recreation case: reassociate an existing pad. */
        if (pad->idx != -1) {
            /* Collision due to duplicate names. */
            sp_log(ctx, SP_LOG_ERROR, "More than one pad with label \"%s\"!\n", name);
            return AVERROR(EINVAL);
        }
        if (pad->is_out != is_out || pad->type != info.type) {
            /* libavfilter graph parser behavior not deterministic. */
            sp_log(ctx, SP_LOG_ERROR, "Pad \"%s\" changed type or direction!\n", name);
            return AVERROR(EINVAL);
        }
    } else {
        if (!first_init) {
            sp_log(ctx, SP_LOG_ERROR, "Pad \"%s\" added after filter init!\n", name);
            return AVERROR(EINVAL);
        }

        pad = is_out ? create_out_pad(ctx) : create_in_pad(ctx);
        if (!pad) {
            return AVERROR(ENOMEM);
        }

        pad->fifo = is_out ? sp_frame_fifo_create(ctx, 0, 0)
                           : sp_frame_fifo_create(ctx, 8, FRAME_FIFO_BLOCK_NO_INPUT);
        pad->main = ctx;
        pad->is_out = is_out;
        pad->name = av_strdup(name);
        pad->type = info.type;
    }

    pad->idx = idx;
    return 0;
}

static int add_pads(FilterProcContext *ctx, int is_out, int first_init)
{
    int err = 0;
    char name[80];
    size_t cap = sizeof(name);
    size_t num_pads = is_out ? ctx->vt.get_output_count(ctx->data)
                             : ctx->vt.get_input_count(ctx->data);

    for (size_t idx = 0; idx < num_pads; idx++) {
        err = is_out ? ctx->vt.output_get_name(ctx->data, idx, name, cap)
                     : ctx->vt.input_get_name(ctx->data, idx, name, cap);
        if (err < 0)
            return err;

        err = add_pad(ctx, is_out, idx, name, first_init);
        if (err < 0)
            return err;
    }

    return err;
}

static int re_create_filtering(FilterProcContext *ctx, void *arg, int first_init)
{
    int err = 0;

    ctx->data = ctx->vt.alloc(arg, ctx);
    if (!ctx->data) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to allocate, no memory!\n");
        return AVERROR(ENOMEM);
    }

    err = add_pads(ctx, 0, first_init);
    if (err < 0)
        return err;

    err = add_pads(ctx, 1, first_init);
    if (err < 0)
        return err;

    return 0;
}

static int rename_ctx(FilterProcContext *ctx, const char *name)
{
    char *new_name = av_strdup(name);

    if (!new_name)
        return AVERROR(ENOMEM);

    sp_class_set_name(ctx, new_name);
    av_free(new_name);

    return 0;
}

int sp_init_filter_proc(AVBufferRef *ctx_ref, const char *name, void *arg,
                        const FilterProcVtable *vtable)
{
    FilterProcContext *ctx = (FilterProcContext *)ctx_ref->data;
    ctx->vt = *vtable;
    int ret = rename_ctx(ctx, name);
    if (ret < 0)
        return ret;
    return re_create_filtering(ctx, arg, 1);
}

static int push_input_pads(FilterProcContext *ctx, int *flush, int opportunistically)
{
    int err = 0, ret, can_process;
    enum SPFrameFIFOFlags pull_flags;

    pull_flags = opportunistically ? FRAME_FIFO_PULL_NO_BLOCK : 0x0;

    int pads_satisfied = 0;

    for (int i = 0; i < ctx->num_in_pads; i++) {
        FilterProcPad *in_pad = ctx->in_pads[i];

        unsigned nb_req = 0;
        if (!in_pad->eos && !opportunistically) {
            nb_req = sp_frame_fifo_get_size(in_pad->fifo);
            /* If we're doing this non-opportunistically, we'll want at least
             * one frame in */
            if (!nb_req)
                nb_req = 1;
        }

        /* If we're doing it opportunistically, don't do any processing now,
         * since we'll likely pull right away after this and do the processing
         * then. Otherwise, do the processing now, unless there are many frames,
         * in which case, get them off our books as fast as possible. */
        can_process = (opportunistically || nb_req > 1) ? 0x0 : 1;

        int j;
        for (j = 0; j < nb_req; j++) {
            AVFrame *in_frame;
            ret = sp_frame_fifo_pop_flags(in_pad->fifo, &in_frame, pull_flags);
            if (opportunistically && (ret == AVERROR(EAGAIN))) {
                break;
            } else if (ret < 0) {
                sp_log(ctx, SP_LOG_ERROR, "Error pulling frame from FIFO at input pad \"%s\": %s!\n",
                       in_pad->name, av_err2str(ret));
                return ret;
            }

            if (!in_frame) {
                *flush = 1;
                can_process = 1;
                in_pad->eos = 1;
                pads_satisfied++;
                j = nb_req;
            } else {
                FormatExtraData *fe = (FormatExtraData *)in_frame->opaque_ref->data;
                sp_log(ctx, SP_LOG_TRACE, "Giving frame to input pad \"%s\", pts = %f\n",
                       in_pad->name, av_q2d(fe->time_base) * in_frame->pts);
            }

            /* Takes ownership of in_frame */
            ret = ctx->vt.on_frame(ctx->data, in_pad->idx, in_frame, !can_process);

            /* It did take ownership but it just moved the ref, it doesn't free
             * the frame as well */
            av_frame_free(&in_frame);

            if (ret == AVERROR(ENOMEM)) {
                return ret;
            } else if (ret < 0) {
                sp_log(ctx, SP_LOG_ERROR, "Error pushing frame to input pad \"%s\": %s!\n",
                       in_pad->name, av_err2str(ret));
                err = ret;
            }
        }

        /* The pad is not satisfied (our own vague term) until it has
         * received at least 1 frame, or all requested (could be none) */
        if (j == nb_req || j)
            pads_satisfied++;
    }

    return (err < 0 && !pads_satisfied) ? err : (pads_satisfied == ctx->num_in_pads);
}

static int drain_output_pad(FilterProcContext *ctx, FilterProcPad *out_pad,
                            AVFrame **tmp_frame, int *flush)
{
    int ret, input_pushed = 0;
    AVFrame *filt_frame = *tmp_frame;
    int can_process = *flush ? 0 : 1;

    do {
        /* Pull frame */
        ret = ctx->vt.pull_frame(ctx->data, out_pad->idx, filt_frame, !can_process);

        /* Check for EAGAIN or errors */
        if (!input_pushed && ret == AVERROR(EAGAIN)) {
            ret = push_input_pads(ctx, flush, 1);

            /* The function above returns 0 if not all pads have been satisfied.
             * If so, no point in trying again. */
            if (ret <= 0)
                return ret;

            /* The flush status may have changed. */
            can_process = *flush ? 0 : 1;
            input_pushed++;
            continue;
        } else if (ret < 0) {
            break;
        }

        input_pushed = 0;

        av_buffer_unref(&filt_frame->opaque_ref);
        filt_frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));
        if (!filt_frame->opaque_ref)
            return AVERROR(ENOMEM);

        FilterStreamInfo pad_info;
        ctx->vt.output_get_info(ctx->data, out_pad->idx, &pad_info);

        FormatExtraData *fe = (FormatExtraData *)filt_frame->opaque_ref->data;
        fe->time_base = pad_info.time_base;
        if (pad_info.type == AVMEDIA_TYPE_VIDEO)
            fe->avg_frame_rate  = pad_info.frame_rate;
        else if (pad_info.type == AVMEDIA_TYPE_AUDIO)
            fe->bits_per_sample = av_get_bytes_per_sample(pad_info.format) * 8;

        sp_log(ctx, SP_LOG_TRACE, "Pushing frame to FIFO from output pad \"%s\", pts = %f\n",
               out_pad->name, av_q2d(fe->time_base) * filt_frame->pts);

        ret = sp_frame_fifo_push(out_pad->fifo, filt_frame);
        av_frame_free(tmp_frame);
        if (ret == AVERROR(ENOMEM))
            return ret;

        if (ret == AVERROR(ENOBUFS)) {
            out_pad->dropped_frames++;
            sp_log(ctx, SP_LOG_WARN,
                   "Dropping filtered frame from output pad \"%s\" (%i dropped)!\n",
                   out_pad->name, out_pad->dropped_frames);
        } else if (ret < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Error pushing frame to FIFO on output pad \"%s\": %s\n",
                   out_pad->name, av_err2str(ret));
            return ret;
        }
        
        *tmp_frame = filt_frame = av_frame_alloc();
        if (!filt_frame)
            return AVERROR(ENOMEM);

    } while (1);

    /* Probably overkill, but why take a chance? */
    if ((ret == AVERROR_EOF) || ((ret == AVERROR(EAGAIN)) && *flush)) {
        sp_log(ctx, SP_LOG_VERBOSE, "Output pad \"%s\" flushed!\n", out_pad->name);
        ret = AVERROR_EOF;
        out_pad->eos = 1;
    } else if (ret != AVERROR(EAGAIN)) {
        sp_log(ctx, SP_LOG_ERROR, "Error pulling filtered frame from output pad \"%s\": %s!\n",
               out_pad->name, av_err2str(ret));
    }

    return ret;
}

static void *filtering_thread(void *data)
{
    int err = 0, flushing = 0;
    FilterProcContext *ctx = data;

    sp_set_thread_name_self(sp_class_get_name(ctx));

    sp_log(ctx, SP_LOG_VERBOSE, "Filter initialized!\n");
    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_INIT, NULL);

    AVFrame *filt_frame = av_frame_alloc();

    while (1) {
        pthread_mutex_lock(&ctx->lock);

        err = push_input_pads(ctx, &flushing, 0);
        if (err < 0)
            goto fail;

        int pads_flushed = 0;
        int pads_errors = 0;
        for (int i = 0; i < ctx->num_out_pads; i++) {
            FilterProcPad *out_pad = ctx->out_pads[i];

            if (out_pad->eos) {
                pads_flushed++;
                continue;
            }

            err = drain_output_pad(ctx, out_pad, &filt_frame, &flushing);
          
            if (err == AVERROR(ENOMEM))
                goto fail;
            else if (err == AVERROR_EOF)
                pads_flushed++;
            else if (err == AVERROR(EAGAIN))
                continue;
            else if (err < 0)
                pads_errors++;
        }

        pthread_mutex_unlock(&ctx->lock);

        /* Yes, I'm being clever. */
        if ((pads_flushed + pads_errors) == ctx->num_out_pads)
            break;
    }

    if (err >= 0 || (err == AVERROR_EOF))
        sp_log(ctx, SP_LOG_DEBUG, "Filter flushed!\n");
    else
        sp_log(ctx, SP_LOG_ERROR, "Filter errors: %s!\n", av_err2str(err));

    av_frame_free(&filt_frame);

    if (ctx->send_eos) {
        int tmp = err;
        sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_EOS, &tmp);
        if (tmp != 0) {
            for (int i = 0; i < ctx->num_out_pads; i++)
                sp_frame_fifo_push(ctx->out_pads[i]->fifo, NULL);
        }
    }

    return NULL;

fail:
    {
        int tmp = err;
        sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_EOS, &tmp);
        if (tmp != 0) {
            for (int i = 0; i < ctx->num_out_pads; i++)
                sp_frame_fifo_push(ctx->out_pads[i]->fifo, NULL);
        }
    }

    av_frame_free(&filt_frame);
    pthread_mutex_unlock(&ctx->lock);
    return NULL;
}

static int sp_filter_init_graph(FilterProcContext *ctx)
{
    int err = 0;

    err = sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_CONFIG, NULL);
    if (err < 0)
        return err;

    sp_log(ctx, SP_LOG_INFO, "Configure %p %p %p\n", ctx->vt.configure, ctx->data, ctx);
    err = ctx->vt.configure(ctx->data);
    if (err < 0)
        return err;

    sp_log(ctx, SP_LOG_DEBUG, "Filter configured!\n");

    return err;
}

static int filter_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx,
                               void *_ctx, void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;
    FilterProcContext *ctx = _ctx;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        if (!sp_eventlist_has_dispatched(ctx->events, SP_EVENT_ON_CONFIG)) {
            int ret = sp_filter_init_graph(ctx);
            if (ret < 0)
                return ret;
        }
        ctx->epoch = atomic_load(event->epoch);
        if (!ctx->filter_thread)
            pthread_create(&ctx->filter_thread, NULL, filtering_thread, ctx);
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        if (ctx->filter_thread) {
            for (int i = 0; i < ctx->num_in_pads; i++)
                sp_frame_fifo_push(ctx->in_pads[i]->fifo, NULL);
            pthread_join(ctx->filter_thread, NULL);
            ctx->filter_thread = 0;
        }
    } else if (event->ctrl & SP_EVENT_CTRL_OPTS) {
        pthread_mutex_lock(&ctx->lock);
        const char *tmp_val = NULL;
        if ((tmp_val = dict_get(event->opts, "fifo_size"))) {
            long int len = strtol(tmp_val, NULL, 10);
            if (len < 0) {
                sp_log(ctx, SP_LOG_ERROR, "Invalid fifo size \"%s\"!\n", tmp_val);
            } else {
                for (int i = 0; i < ctx->num_in_pads; i++)
                    sp_frame_fifo_set_max_queued(ctx->in_pads[i]->fifo, len);
            }
        }
        if ((tmp_val = dict_get(event->opts, "send_eos"))) {
            /* anything other than "false" or 0 is truthy */
            ctx->send_eos = strcmp(tmp_val, "false") && strtol(tmp_val, NULL, 10) != 0;
        }
        pthread_mutex_unlock(&ctx->lock);
    } else if (event->ctrl & SP_EVENT_CTRL_COMMAND) {
        if (ctx->vt.on_command) {
            const AVDictionaryEntry *e = NULL;

            while ((e = av_dict_get(event->cmd, "", e, AV_DICT_IGNORE_SUFFIX))) {
                if (!strcmp(e->key, "sp_filter_target"))
                    continue;

                int ret = ctx->vt.on_command(ctx->data, e->key, e->value);
                sp_log(ctx,
                       ret == AVERROR(EINVAL) ? SP_LOG_ERROR : SP_LOG_VERBOSE,
                       "Filter processor response to command %s=%s: %s\n",
                       e->key, e->value, av_err2str(ret));
            }
        }
    } else {
        return AVERROR(ENOTSUP);
    }

    return 0;
}

int sp_filter_ctrl_proc(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    FilterProcContext *ctx = (FilterProcContext *)ctx_ref->data;
    return sp_ctrl_template(ctx, ctx->events, SP_EVENT_CTRL_COMMAND,
                            filter_ioctx_ctrl_cb, ctrl, arg);
}

static void filter_proc_free(void *opaque, uint8_t *data)
{
    FilterProcContext *ctx = (FilterProcContext *)data;

    for (int i = 0; i < ctx->num_in_pads; i++)
        sp_frame_fifo_unmirror_all(ctx->in_pads[i]->fifo);

    for (int i = 0; i < ctx->num_out_pads; i++)
        sp_frame_fifo_unmirror_all(ctx->out_pads[i]->fifo);

    if (ctx->filter_thread) {
        for (int i = 0; i < ctx->num_in_pads; i++)
            sp_frame_fifo_push(ctx->in_pads[i]->fifo, NULL);
        pthread_join(ctx->filter_thread, NULL);
    }

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, ctx);
    sp_bufferlist_free(&ctx->events);

    ctx->vt.free(ctx->data);
    ctx->data = NULL;

    for (int i = 0; i < ctx->num_in_pads; i++) {
        av_buffer_unref(&ctx->in_pads[i]->fifo);
        av_free(ctx->in_pads[i]->name);
        av_free(ctx->in_pads[i]);
    }
    av_free(ctx->in_pads);

    for (int i = 0; i < ctx->num_out_pads; i++) {
        av_buffer_unref(&ctx->out_pads[i]->fifo);
        av_free(ctx->out_pads[i]->name);
        av_free(ctx->out_pads[i]);
    }
    av_free(ctx->out_pads);

    pthread_mutex_unlock(&ctx->lock);
    pthread_mutex_destroy(&ctx->lock);

    sp_class_free(ctx);
    av_free(ctx);
}

AVBufferRef *sp_filter_alloc_proc(void)
{
    FilterProcContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    int err = sp_class_alloc(ctx, "user_proc", SP_TYPE_FILTER_PROC, NULL);
    if (err < 0) {
        av_free(ctx);
        return NULL;
    }

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            filter_proc_free, NULL, 0);
    if (!ctx_ref) {
        sp_class_free(ctx);
        av_free(ctx);
        return NULL;
    }

    pthread_mutex_init(&ctx->lock, NULL);
    ctx->events = sp_bufferlist_new();
    ctx->send_eos = 1;

    return ctx_ref;
}
