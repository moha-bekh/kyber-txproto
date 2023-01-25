#pragma once

#include <stdatomic.h>
#include <stdbool.h>

#include <libtxproto/bufferlist.h>
#include <libtxproto/encode.h>
#include <libtxproto/events.h>
#include <libtxproto/log.h>
#include "net.h"

typedef struct PacketSinkContext {
    SPClass *class;

    EncodingContext *enc;

    const char *uri;

    bool thread_started;
    pthread_t thread;
    atomic_bool interrupted;

    socket_t socket;

    pthread_mutex_t lock;

    SPBufferList *events;

    AVBufferRef *src_packets;
} PacketSinkContext;

AVBufferRef *sp_packet_sink_alloc(void);
int sp_packet_sink_init(AVBufferRef *ctx_ref);
int sp_packet_sink_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg);
int sp_packet_sink_set_encoding_ctx(PacketSinkContext *ctx,
                                    EncodingContext *enc);
