#include "packet_sink.h"

#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/buffer.h>

#include <stdatomic.h>
#include <unistd.h>

#include <libtxproto/fifo_packet.h>
#include <libtxproto/utils.h>
#include "ctrl_template.h"
#include "net.h"
#include "os_compat.h"

#define KYMUX_CODEC_H264 UINT32_C(0x68323634)
#define KYMUX_CODEC_H265 UINT32_C(0x68323635)
#define KYMUX_CODEC_AV1  UINT32_C(0x68323636)
#define KYMUX_CODEC_OPUS UINT32_C(0x6F707573)

#define KYMUX_PTS_MASK UINT64_C(0x1FFFFFFFFFFFFFFF)
#define KYMUX_FLAG_KYMEDIA UINT64_C(0x8000000000000000)
#define KYMUX_FLAG_CONFIG UINT64_C(0x4000000000000000)
#define KYMUX_FLAG_KEY UINT64_C(0x2000000000000000)

static inline bool
is_interrupted(PacketSinkContext *ctx) {
    return atomic_load_explicit(&ctx->interrupted, memory_order_relaxed);
}

static uint32_t get_kymux_codec(enum AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return KYMUX_CODEC_H264;
        case AV_CODEC_ID_H265: return KYMUX_CODEC_H265;
        case AV_CODEC_ID_AV1:  return KYMUX_CODEC_AV1;
        case AV_CODEC_ID_OPUS: return KYMUX_CODEC_OPUS;
        default: return 0;
    }
}

static int send_config(PacketSinkContext *ctx,
                       const uint8_t *extradata,
                       size_t extradata_size,
                       uint8_t rotation)
{
    unsigned char header[12];

    uint32_t kymux_codec = get_kymux_codec(ctx->enc->codec->id);
    if (!kymux_codec)
        return -1;

    // Send codec packet
    AV_WB32(header, kymux_codec);
    header[4] = rotation;
    memset(header + 5, 0, 7);
    int w = net_send_all(ctx->socket, header, sizeof(header));
    if (w == -1)
        return -1;

    // Send config packet
    uint64_t flags = KYMUX_FLAG_KYMEDIA | KYMUX_FLAG_CONFIG;
    AV_WB64(header, flags);
    AV_WB32(header + 8, extradata_size);
    w = net_send_all(ctx->socket, header, sizeof(header));
    if (w == -1)
        return -1;

    w = net_send_all(ctx->socket, extradata, extradata_size);
    if (w == -1)
        return -1;

    return 0;
}

int sp_packet_sink_set_encoding_ctx(PacketSinkContext *ctx,
                                    EncodingContext *enc) {
    ctx->enc = enc;

    if (!(enc->avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)) {
        sp_log(ctx, SP_LOG_ERROR, "Packet sink requires global header\n");
        return -1;
    }

    send_config(ctx,
                enc->avctx->extradata,
                enc->avctx->extradata_size,
                enc->rotation);

    return 0;
}

static void check_config_update(PacketSinkContext *ctx, AVPacket *in_pkt)
{
    const uint8_t *extradata;
    size_t extradata_size;
    const uint8_t *packed_dict;
    size_t packed_dict_size;
    AVDictionary *dict = NULL;
    int ret;

    // Get side data
    extradata = av_packet_get_side_data(in_pkt,
                                        AV_PKT_DATA_NEW_EXTRADATA,
                                        &extradata_size);
    if (!extradata) {
        return;
    }

    packed_dict = av_packet_get_side_data(in_pkt,
                                          AV_PKT_DATA_STRINGS_METADATA,
                                          &packed_dict_size);
    if (!packed_dict) {
        sp_log(ctx, SP_LOG_WARN, "Got new extradata but no metadata\n");
        return;
    }

    // Unpack dict
    ret = av_packet_unpack_dictionary(packed_dict, packed_dict_size, &dict);
    if (ret < 0) {
        sp_log(ctx, SP_LOG_WARN, "Fail to unpack dictionary: %s\n", av_err2str(ret));
        return;
    }

    const char *str_rotation = dict_get(dict, "rotation");;
    if (!str_rotation) {
        sp_log(ctx, SP_LOG_WARN, "Key \"rotation\" not found\n");
        goto fail;
    }

    long int rotation = strtol(str_rotation, NULL, 10);
    if (rotation < 0) {
        sp_log(ctx, SP_LOG_WARN, "Invalid rotation \"%s\"\n", str_rotation);
        goto fail;
    }

    // Send config
    send_config(ctx, extradata, extradata_size, rotation);

    return;

fail:
    av_dict_free(&dict);
}

static void *
packet_sink_thread(void *arg) {
    PacketSinkContext *ctx = arg;

    sp_set_thread_name_self(sp_class_get_name(ctx));
    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_INIT, NULL);

    for (;;) {
        if (is_interrupted(ctx)) {
            goto end;
        }

        AVPacket *in_pkt = sp_packet_fifo_pop(ctx->src_packets);
        if (!in_pkt) {
            // a NULL packet has been pushed on stop, it's interrupted
            goto end;
        }

        check_config_update(ctx, in_pkt);

        unsigned char header[12];

        uint64_t pts_and_flags = KYMUX_FLAG_KYMEDIA;
        pts_and_flags |= in_pkt->pts & KYMUX_PTS_MASK;

        if (in_pkt->flags & AV_PKT_FLAG_KEY)
            pts_and_flags |= KYMUX_FLAG_KEY;

        AV_WB64(header, pts_and_flags);
        AV_WB32(header + 8, in_pkt->size);

        int w = net_send_all(ctx->socket, header, sizeof(header));
        if (w != sizeof(header)) {
            sp_log(ctx, SP_LOG_ERROR, "Could not write to socket");
            goto end;
        }

        w = net_send_all(ctx->socket, in_pkt->data, in_pkt->size);
        if (w != in_pkt->size) {
            sp_log(ctx, SP_LOG_ERROR, "Could not write to socket");
            goto end;
        }

        av_packet_free(&in_pkt);
    }

end:
    return NULL;
}

static void
packet_sink_free(void *opaque, uint8_t *data) {
    PacketSinkContext *ctx = (PacketSinkContext *) data;

    if (ctx->thread_started) {
        sp_packet_fifo_push(ctx->src_packets, NULL);
        pthread_join(ctx->thread, NULL);
    }

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, NULL);
    sp_bufferlist_free(&ctx->events);
    av_buffer_unref(&ctx->src_packets);
    pthread_mutex_destroy(&ctx->lock);
}

AVBufferRef *
sp_packet_sink_alloc(void) {
    PacketSinkContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            packet_sink_free, NULL, 0);
    if (!ctx_ref) {
        av_free(&ctx);
        return NULL;
    }

    int err = sp_class_alloc(ctx, "packet_sink", SP_TYPE_PACKET_SINK, NULL);
    if (err < 0)
        goto error;

    pthread_mutex_init(&ctx->lock, NULL);
    ctx->src_packets =
        sp_packet_fifo_create(ctx, 256, PACKET_FIFO_BLOCK_NO_INPUT);
    if (!ctx->src_packets)
        goto error;

    ctx->events = sp_bufferlist_new();
    if (!ctx->events)
        goto error;

    atomic_init(&ctx->interrupted, false);

    return ctx_ref;

error:
    av_buffer_unref(&ctx_ref);
    return NULL;
}

static bool parse_uri(PacketSinkContext *ctx,
                      const char *uri,
                      uint32_t *out_host,
                      int *out_port,
                      uint16_t *out_endpoint) {
    char proto[8];
    char str_host[16];
    uint32_t host;
    int port;
    char path[32];
    uint64_t endpoint;
    bool ok;

    av_url_split(
        proto, sizeof(proto),
        NULL, 0,
        str_host, sizeof(str_host),
        &port,
        path, sizeof(path),
        uri);

    // Validate scheme
    if (strcmp(proto, "kymux")) {
        sp_log(ctx, SP_LOG_WARN, "URI %s has an invalid protocol\n", ctx->uri);
        return false;
    }

    // Get and parse host
    ok = net_parse_ipv4(str_host, &host);
    if (!ok) {
        sp_log(ctx, SP_LOG_WARN, "URI %s has an invalid host\n", ctx->uri);
        return false;
    }

    // Validate port
    if (port < 0) {
        sp_log(ctx, SP_LOG_WARN, "URI %s has no port\n", ctx->uri);
        return false;
    }

    // Extract endpoint
    if (path[0] == '\0') {
        sp_log(ctx, SP_LOG_WARN, "URI %s has no valid path\n", ctx->uri);
        return false;
    }

    const char *endpoint_str = path + 1; // The first char is `\\`
    char *endptr;
    endpoint = strtoull(endpoint_str, &endptr, 16);
    if (endptr == endpoint_str || *endptr != '\0' || endpoint > UINT64_C(0xFFFF)) {
        sp_log(ctx, SP_LOG_WARN, "URI %s has no valid endpoint. Tried to parse %s\n", ctx->uri, path + 1);
        return false;
    }

    // Return values
    *out_host = host;
    *out_port = port;
    *out_endpoint = endpoint;

    return true;
}

int
sp_packet_sink_init(AVBufferRef *ctx_ref) {
    PacketSinkContext *ctx = (PacketSinkContext *) ctx_ref->data;
    bool ok;

    sp_class_set_name(ctx, "packet sink");
    sp_set_thread_name_self(sp_class_get_name(ctx));

    // Parse URI
    assert(ctx->uri);
    sp_log(ctx, SP_LOG_INFO, "connecting to URI: %s\n", ctx->uri);

    uint32_t host;
    int port;
    uint16_t endpoint;

    ok = parse_uri(ctx, ctx->uri, &host, &port, &endpoint);
    if (!ok) {
        return -1;
    }

#ifdef __WIN32
    uint16_t net_endpoint = htons(endpoint);
#else
    uint16_t net_endpoint = htobe16(endpoint);
#endif

    // Connect to kymux
    ctx->socket = net_socket();
    if (ctx->socket == INVALID_SOCKET) {
        return -1;
    }

    ok = net_connect(ctx->socket, host, port);
    if (!ok) {
        net_close(ctx->socket);
        return -1;
    }

    if (ctx->socket == INVALID_SOCKET) {
        return -1;
    }

    net_send_all(ctx->socket, &net_endpoint, sizeof(net_endpoint));

    uint8_t sync;
    net_recv_all(ctx->socket, &sync, sizeof(sync));

    sp_log(ctx, SP_LOG_INFO, "connected to %s\n", ctx->uri);

    return 0;
}

static int packet_sink_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx,
                                     void *_ctx, void *dep_ctx, void *data) {
    SPCtrlTemplateCbCtx *event = callback_ctx;
    PacketSinkContext *ctx = _ctx;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        if (!ctx->thread_started) {
            if (!sp_eventlist_has_dispatched(ctx->events, SP_EVENT_ON_CONFIG))
                sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_CONFIG, NULL);
            int ret = pthread_create(&ctx->thread, NULL, packet_sink_thread, ctx);
            if (ret) {
                return 1;
            }
            ctx->thread_started = true;
        }
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        if (ctx->thread_started) {
            sp_packet_fifo_push(ctx->src_packets, NULL);
            atomic_store_explicit(&ctx->interrupted, true, memory_order_relaxed);
            // TODO interrupt any blocking socket I/O in the background thread
            pthread_join(ctx->thread, NULL);
            if (ctx->socket != INVALID_SOCKET) {
                net_close(ctx->socket);
                ctx->socket = INVALID_SOCKET;
            }
            ctx->thread_started = false;
        }
    } else if (event->ctrl & SP_EVENT_CTRL_OPTS) {
        // nothing to do
    } else if (event->ctrl & SP_EVENT_CTRL_FLUSH) {
        // nothing to do
    } else {
        return AVERROR(ENOTSUP);
    }

    return 0;
}
int
sp_packet_sink_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg) {
    PacketSinkContext *ctx = (PacketSinkContext *) ctx_ref->data;
    return sp_ctrl_template(ctx, ctx->events, 0x0, packet_sink_ioctx_ctrl_cb,
                            ctrl, arg);
}
