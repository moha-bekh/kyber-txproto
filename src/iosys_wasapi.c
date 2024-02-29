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

#define COBJMACROS
#define CONST_VTABLE

#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <windows.h>

#include <libavutil/channel_layout.h>
#include <libavutil/crc.h>
#include <libavutil/time.h>

#include "iosys_common.h"
#include "ctrl_template.h"
#include "os_compat.h"

const IOSysAPI src_wasapi;

typedef struct WasapiCtx {
    SPClass *class;

    /* Sinks list */
    SPBufferList *entries;
    SPBufferList *events;
} WasapiCtx;

typedef struct WasapiCapture {
    atomic_int quit;
    pthread_t pull_thread;
    int err;

    int64_t epoch;

    int format;
    int bits_per_sample;
    int sample_size;

    AVBufferPool *pool;
    int pool_entry_size;

    int dropped_frames;
} WasapiCapture;

static uint32_t gen_identifier(const LPWSTR s)
{
    const AVCRC *table = av_crc_get_table(AV_CRC_32_IEEE);

    uint32_t crc = UINT32_MAX;
    crc = av_crc(table, crc, (const uint8_t *)s, wcslen(s) * sizeof(*s));
    return crc;
}

static int gen_device_id(IMMDevice *dev, uint32_t *id)
{
    LPWSTR str_id;
    HRESULT hr;

    hr = IMMDevice_GetId(dev, &str_id);
    if (hr != S_OK) {
        return AVERROR_EXTERNAL;
    }

    *id = gen_identifier(str_id);
    CoTaskMemFree(str_id);

    return 0;
}

static AVChannelLayout get_channel_layout(WasapiCtx *ctx, DWORD win_layout, int nb_channels) {
    AVChannelLayout out = { 0 };

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 37, 100)
    static const struct {
        DWORD win_ch;
        enum AVChannel av_ch;
    } channels_map[] = {
        { .win_ch = SPEAKER_FRONT_LEFT, .av_ch = AV_CHAN_FRONT_LEFT },
        { .win_ch = SPEAKER_FRONT_RIGHT, .av_ch = AV_CHAN_FRONT_RIGHT },
        { .win_ch = SPEAKER_FRONT_CENTER, .av_ch = AV_CHAN_FRONT_CENTER },
        { .win_ch = SPEAKER_LOW_FREQUENCY, .av_ch = AV_CHAN_LOW_FREQUENCY },
        { .win_ch = SPEAKER_BACK_LEFT, .av_ch = AV_CHAN_BACK_LEFT },
        { .win_ch = SPEAKER_BACK_RIGHT, .av_ch = AV_CHAN_BACK_RIGHT },
        { .win_ch = SPEAKER_FRONT_LEFT_OF_CENTER, .av_ch = AV_CHAN_FRONT_LEFT_OF_CENTER },
        { .win_ch = SPEAKER_FRONT_RIGHT_OF_CENTER, .av_ch = AV_CHAN_FRONT_RIGHT_OF_CENTER },
        { .win_ch = SPEAKER_BACK_CENTER, .av_ch = AV_CHAN_TOP_BACK_CENTER },
        { .win_ch = SPEAKER_SIDE_LEFT, .av_ch = AV_CHAN_SIDE_LEFT },
        { .win_ch = SPEAKER_SIDE_RIGHT, .av_ch = AV_CHAN_SIDE_RIGHT },
        { .win_ch = SPEAKER_TOP_CENTER, .av_ch = AV_CHAN_TOP_CENTER },
        { .win_ch = SPEAKER_TOP_FRONT_LEFT, .av_ch = AV_CHAN_TOP_FRONT_LEFT },
        { .win_ch = SPEAKER_TOP_FRONT_CENTER, .av_ch = AV_CHAN_TOP_FRONT_CENTER },
        { .win_ch = SPEAKER_TOP_FRONT_RIGHT, .av_ch = AV_CHAN_TOP_FRONT_RIGHT },
        { .win_ch = SPEAKER_TOP_BACK_LEFT, .av_ch = AV_CHAN_TOP_BACK_LEFT },
        { .win_ch = SPEAKER_TOP_BACK_CENTER, .av_ch = AV_CHAN_TOP_BACK_CENTER },
        { .win_ch = SPEAKER_TOP_BACK_RIGHT, .av_ch = AV_CHAN_TOP_BACK_RIGHT },
    };

    av_channel_layout_custom_init(&out, nb_channels);
    int channel_idx = 0;

    for (size_t i = 0; i < ARRAYSIZE(channels_map); i++) {
        if (win_layout & channels_map[i].win_ch) {
            out.u.map[channel_idx].id = channels_map[i].av_ch;
            channel_idx++;
        }
    }

    av_channel_layout_retype(&out, AV_CHANNEL_ORDER_NATIVE,
                             AV_CHANNEL_LAYOUT_RETYPE_FLAG_CANONICAL);
#else
    static const struct {
        DWORD win_ch;
        int av_ch;
    } channels_map[] = {
        { .win_ch = SPEAKER_FRONT_LEFT, .av_ch = AV_CH_FRONT_LEFT },
        { .win_ch = SPEAKER_FRONT_RIGHT, .av_ch = AV_CH_FRONT_RIGHT },
        { .win_ch = SPEAKER_FRONT_CENTER, .av_ch = AV_CH_FRONT_CENTER },
        { .win_ch = SPEAKER_LOW_FREQUENCY, .av_ch = AV_CH_LOW_FREQUENCY },
        { .win_ch = SPEAKER_BACK_LEFT, .av_ch = AV_CH_BACK_LEFT },
        { .win_ch = SPEAKER_BACK_RIGHT, .av_ch = AV_CH_BACK_RIGHT },
        { .win_ch = SPEAKER_FRONT_LEFT_OF_CENTER, .av_ch = AV_CH_FRONT_LEFT_OF_CENTER },
        { .win_ch = SPEAKER_FRONT_RIGHT_OF_CENTER, .av_ch = AV_CH_FRONT_RIGHT_OF_CENTER },
        { .win_ch = SPEAKER_BACK_CENTER, .av_ch = AV_CH_TOP_BACK_CENTER },
        { .win_ch = SPEAKER_SIDE_LEFT, .av_ch = AV_CH_SIDE_LEFT },
        { .win_ch = SPEAKER_SIDE_RIGHT, .av_ch = AV_CH_SIDE_RIGHT },
        { .win_ch = SPEAKER_TOP_CENTER, .av_ch = AV_CH_TOP_CENTER },
        { .win_ch = SPEAKER_TOP_FRONT_LEFT, .av_ch = AV_CH_TOP_FRONT_LEFT },
        { .win_ch = SPEAKER_TOP_FRONT_CENTER, .av_ch = AV_CH_TOP_FRONT_CENTER },
        { .win_ch = SPEAKER_TOP_FRONT_RIGHT, .av_ch = AV_CH_TOP_FRONT_RIGHT },
        { .win_ch = SPEAKER_TOP_BACK_LEFT, .av_ch = AV_CH_TOP_BACK_LEFT },
        { .win_ch = SPEAKER_TOP_BACK_CENTER, .av_ch = AV_CH_TOP_BACK_CENTER },
        { .win_ch = SPEAKER_TOP_BACK_RIGHT, .av_ch = AV_CH_TOP_BACK_RIGHT },
    };

    uint64_t map = 0;

    for (size_t i = 0; i < ARRAYSIZE(channels_map); i++) {
        if (win_layout & channels_map[i].win_ch)
            map |= channels_map[i].av_ch;
    }

    if (map)
        av_channel_layout_from_mask(&out, map);
    else
        av_channel_layout_default(&out, nb_channels);

#endif

    return out;
}

static int get_format(WasapiCtx *ctx, const WAVEFORMATEX *pwf, const WAVEFORMATEXTENSIBLE *wfe, int *format)
{
    if (IsEqualIID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM)) {
        switch (pwf->wBitsPerSample) {
            case 32:
                *format = AV_SAMPLE_FMT_S32;
                break;
            case 16:
                *format = AV_SAMPLE_FMT_S16;
                break;
            case 8:
                *format = AV_SAMPLE_FMT_U8;
                break;
            default:
                return -1;
        }
    } else if (IsEqualIID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
        if (pwf->wBitsPerSample != wfe->Samples.wValidBitsPerSample) {
            sp_log(ctx, SP_LOG_ERROR, "wBitsPerSample(%d) != wValidBitsPerSample(%d)\n",
                pwf->wBitsPerSample, wfe->Samples.wValidBitsPerSample);
            return AVERROR_EXTERNAL;
        }

        *format = AV_SAMPLE_FMT_FLT;
    } else {
        return -1;
    }

    return 0;
}

static int get_bits_per_sample(const WAVEFORMATEX *pwf, const WAVEFORMATEXTENSIBLE *wfe, int *bits_per_sample)
{
    if (IsEqualIID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM)) {
        *bits_per_sample = wfe->Samples.wValidBitsPerSample;
    } else if (IsEqualIID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
        *bits_per_sample = pwf->wBitsPerSample;
    } else {
        return -1;
    }

    return 0;
}

static IMMDevice *find_device(WasapiCtx *ctx, uint32_t identifier)
{
    HRESULT hr;
    IMMDeviceEnumerator *e;
    IMMDeviceCollection *collection;
    IMMDevice *dev = NULL;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (LPVOID *) &e);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Fail to create device iterator: %lX\n", hr);
        return NULL;
    }

    hr = IMMDeviceEnumerator_EnumAudioEndpoints(
            e,
            eRender,
            DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED,
            &collection);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Fail to enumerate audio endpoints: %lX\n", hr);
        goto release_enumerator;
    }

    UINT count;
    hr = IMMDeviceCollection_GetCount(collection, &count);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Fail to get collection size: %lX\n", hr);
        goto release_collection;
    }

    for (UINT i = 0; i < count; i++) {
        hr = IMMDeviceCollection_Item(collection, i, &dev);
        if (hr != S_OK) {
            sp_log(ctx, SP_LOG_ERROR, "Fail to get collection item %u: %lX\n", i, hr);
            goto release_collection;
        }

        uint32_t id;

        int ret = gen_device_id(dev, &id);
        if (ret == 0 && id == identifier) {
            break;
        } else {
            IMMDevice_Release(dev);
            dev = NULL;
        }
    }

release_collection:
    IMMDeviceCollection_Release(collection);
release_enumerator:
    IMMDeviceEnumerator_Release(e);

    return dev;
}

static int handle_samples(IOSysEntry *entry, IAudioCaptureClient *capture)
{
    WasapiCapture *priv = entry->io_priv;
    WasapiCtx *ctx = entry->api_priv;

    BYTE *buf;
    UINT32 frames;
    DWORD flags = 0;
    UINT64 qpc;
    HRESULT hr;

    while (1) {
        hr = IAudioCaptureClient_GetBuffer(capture, &buf, &frames, &flags,
                                           NULL, &qpc);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            return 0;
        } else if (hr != S_OK) {
            sp_log(ctx, SP_LOG_WARN, "IAudioCaptureClient_GetBuffer() failed: %lX\n", hr);
            return AVERROR_EXTERNAL;
        }

        AVFrame *f          = av_frame_alloc();
        f->sample_rate      = entry->sample_rate;
        f->ch_layout        = entry->ch_layout;
        f->format           = priv->format;
        f->nb_samples       = frames;
        f->opaque_ref       = av_buffer_allocz(sizeof(FormatExtraData));

        f->pts              = av_gettime_relative() - priv->epoch;

        FormatExtraData *fe = (FormatExtraData *)f->opaque_ref->data;
        fe->time_base       = av_make_q(1, 1000000);
        fe->bits_per_sample = priv->bits_per_sample;

        int size = av_samples_get_buffer_size(&f->linesize[0], f->ch_layout.nb_channels,
                                              f->nb_samples, f->format, 0);
        assert(size > 0);

        if (!priv->pool || size > priv->pool_entry_size) {
            av_buffer_pool_uninit(&priv->pool);

            priv->pool = av_buffer_pool_init2(size, NULL, NULL, NULL);
            priv->pool_entry_size = size;
        }

        AVBufferRef *av_buf = av_buffer_pool_get(priv->pool);
        assert(av_buf);

        /* This assumes data is not planar */
        f->buf[0] = av_buf;
        f->data[0] = f->buf[0]->data;
        f->extended_data = f->data;

        memcpy(f->data[0], buf, frames * priv->sample_size);

        IAudioCaptureClient_ReleaseBuffer(capture, frames);

        int nb_samples = f->nb_samples;
        sp_log(entry, SP_LOG_TRACE, "Pushing frame to FIFO, pts = %f, len = %.2f ms\n",
            av_q2d(fe->time_base) * f->pts, (1000.0f * nb_samples) / f->sample_rate);
        int err = sp_frame_fifo_push(entry->frames, f);
        av_frame_free(&f);
        if (err == AVERROR(ENOBUFS)) {
            sp_log(entry, SP_LOG_WARN, "Dropping %i samples!\n", nb_samples);
        } else if (err) {
            sp_log(entry, SP_LOG_ERROR, "Unable to push frame to FIFO: %s!\n",
                av_err2str(err));
            /* Fatal error happens here */
        }
    }

    return 0;
}

static void *wasapi_capture_thread(void *s)
{
    IOSysEntry *entry = s;
    WasapiCapture *priv = entry->io_priv;
    WasapiCtx *ctx = entry->api_priv;
    IAudioClient *client = NULL;
    IAudioCaptureClient *capture = NULL;
    WAVEFORMATEX *pwf = NULL;
    HANDLE event = NULL;
    HRESULT hr;
    int ret;

    sp_set_thread_name_self(sp_class_get_name(entry));
    sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_CONFIG | SP_EVENT_ON_INIT, NULL);

    /* Find requested device */
    IMMDevice *dev = find_device(ctx, entry->identifier);
    if (!dev) {
        sp_log(ctx, SP_LOG_ERROR, "Requested device hasn't been found\n");
        ret = AVERROR_EXTERNAL;
        goto error;
    }

    /* Open device and fetch the missing parts of its audio format */
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (LPVOID *) &client);
    IMMDevice_Release(dev);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to activate device: %lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto error;
    }

    hr = IAudioClient_GetMixFormat(client, &pwf);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to get mix format: %lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto error;
    }

    if (pwf->wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
        sp_log(ctx, SP_LOG_ERROR, "Only WAVE_FORMAT_EXTENSIBLE is supported");
        ret = AVERROR_EXTERNAL;
        goto error;
    }

    const WAVEFORMATEXTENSIBLE *wfe = (const WAVEFORMATEXTENSIBLE *)pwf;

    priv->sample_size = pwf->nBlockAlign;

    ret = get_format(ctx, pwf, wfe, &priv->format);
    if (ret) {
        sp_log(ctx, SP_LOG_ERROR, "Fail to get audio format");
        goto error;
    }

    ret = get_bits_per_sample(pwf, wfe, &priv->bits_per_sample);
    if (ret) {
        sp_log(ctx, SP_LOG_ERROR, "Fail to get bits per sample count");
        goto error;
    }

    sp_log(ctx, SP_LOG_INFO, "Channels: %d, Sample rate: %d, Bits per sample %d\n",
        entry->channels,
        entry->sample_rate,
        priv->bits_per_sample);

    /*
     * Initialize audio device
     *
     * MSDN: For a shared-mode stream that uses event-driven buffering,
     * the caller must set both hnsPeriodicity and hnsBufferDuration
     * to 0. */
    hr = IAudioClient_Initialize(
        client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_LOOPBACK,
        0,
        0,
        pwf,
        NULL);

    CoTaskMemFree(pwf);
    pwf = NULL;

    /* Setup notifications */
    event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!event) {
        sp_log(ctx, SP_LOG_ERROR, "CreateEvent() failed: %lX\n", GetLastError());
        ret = AVERROR_EXTERNAL;
        goto error;
    }

    hr = IAudioClient_SetEventHandle(client, event);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "SetEventHandle() failed: %lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto error;
    }

    /* Start capture */
    hr = IAudioClient_GetService(client, &IID_IAudioCaptureClient, (LPVOID *) &capture);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to get CaptureClient: %lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto error;
    }

    hr = IAudioClient_Start(client);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to start capture: %lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto error;
    }

    while (WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0) {
        ret = handle_samples(entry, capture);
        if (ret < 0) {
            break;
        }
    }

error:
    if (capture)
        IAudioCaptureClient_Release(capture);

    if (event)
        CloseHandle(event);

    if (pwf)
        CoTaskMemFree(pwf);

    if (client)
        IAudioClient_Release(client);

    return NULL;
}

static int wasapi_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    int err = 0;
    WasapiCtx *ctx = (WasapiCtx *)ctx_ref->data;

    if (ctrl & SP_EVENT_CTRL_NEW_EVENT) {
        AVBufferRef *event = (AVBufferRef *)arg;
        char *fstr = sp_event_flags_to_str_buf(event);
        sp_log(ctx, SP_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
        av_free(fstr);

        if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
            /* Bring up the new event to speed with current affairs */
            SPBufferList *tmp_event = sp_bufferlist_new();
            sp_eventlist_add(ctx, tmp_event, event, 1);

            AVBufferRef *obj = NULL;
            while ((obj = sp_bufferlist_iter_ref(ctx->entries))) {
                sp_eventlist_dispatch(obj->data, tmp_event,
                                      (SPEventType) (SP_EVENT_ON_CHANGE | SP_EVENT_TYPE_SOURCE), obj->data);
                av_buffer_unref(&obj);
            }

            sp_bufferlist_free(&tmp_event);
        }

        /* Add it to the list now to receive events dynamically */
        err = sp_eventlist_add(ctx, ctx->events, event, 1);
        if (err < 0)
            return err;
    }

    return 0;
}

static void destroy_entry(void *opaque, uint8_t *data)
{
    IOSysEntry *entry = (IOSysEntry *)data;

    sp_class_free(entry);
    av_free(entry);
}

static int fill_entry(WasapiCtx *ctx, IOSysEntry *entry, IMMDevice *dev, uint32_t default_id)
{
    IAudioClient *client = NULL;
    WAVEFORMATEX *pwf = NULL;
    int format;
    HRESULT hr;
    int ret;

    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (LPVOID *) &client);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to activate device: %lX\n", hr);
        goto error;
    }

    /* Get format */
    hr = IAudioClient_GetMixFormat(client, &pwf);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to get mix format: %lX\n", hr);
        goto error;
    }

    if (pwf->wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
        sp_log(ctx, SP_LOG_WARN, "Only WAVE_FORMAT_EXTENSIBLE is supported\n");
        /* Only WAVE_FORMAT_EXTENSIBLE is currently supported */
        goto error;
    }

    const WAVEFORMATEXTENSIBLE *wfe = (const WAVEFORMATEXTENSIBLE *)pwf;

    ret = get_format(ctx, pwf, wfe, &format);
    if (ret < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to format\n");
        goto error;
    }

    /* Get device name */
    IPropertyStore *pProps;
    hr = IMMDevice_OpenPropertyStore(dev, STGM_READ, &pProps);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to open property store: %lX\n", hr);
        goto error;
    }

    PROPVARIANT varName;
    PropVariantInit(&varName);

    hr = IPropertyStore_GetValue(pProps, &PKEY_Device_FriendlyName, &varName);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to get mix format: %lX\n", hr);
        IPropertyStore_Release(pProps);
        goto error;
    }

    size_t len = wcslen(varName.pwszVal);
    char *name = calloc(1, len + 1);
    wcstombs(name, varName.pwszVal, len);

    PropVariantClear(&varName);
    IPropertyStore_Release(pProps);

    /* Fill entry */
    sp_class_alloc(entry, NULL, SP_TYPE_AUDIO_BIDIR, ctx);
    sp_class_set_name(entry, name);
    free(name);

    ret = gen_device_id(dev, &entry->identifier);
    if (ret < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to generate device id\n");
        IPropertyStore_Release(pProps);
        goto error;
    }

    entry->api_id = entry->identifier;
    entry->type = SP_IO_TYPE_AUDIO_MONITOR;
    entry->frames = sp_frame_fifo_create(entry, 0, 0);
    entry->api_priv = ctx;

    entry->is_default = entry->identifier == default_id;

    entry->sample_rate = pwf->nSamplesPerSec;
    entry->volume = 1.0;
    entry->sample_fmt = format;

    entry->ch_layout = get_channel_layout(ctx, wfe->dwChannelMask, pwf->nChannels);

    CoTaskMemFree(pwf);
    IAudioClient_Release(client);

    return 0;

error:
    if (pwf)
        CoTaskMemFree(pwf);

    if (client)
        IAudioClient_Release(client);

    return AVERROR_EXTERNAL;
}

static int enumerate_entries(WasapiCtx *ctx)
{
    IMMDeviceEnumerator *e;
    IMMDevice *dev;
    IMMDeviceCollection *collection = NULL;
    HRESULT hr;
    int ret;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (LPVOID *) &e);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Fail to create device iterator: %lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, eRender, eConsole, &dev);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Fail to get default endpoint: %lX\n", hr);
        goto release_enumerator;
    }

    uint32_t default_id;
    ret = gen_device_id(dev, &default_id);
    IMMDevice_Release(dev);
    if (ret < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Fail to get device id\n");
        goto release_enumerator;
    }

    hr = IMMDeviceEnumerator_EnumAudioEndpoints(
            e,
            eRender,
            DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED,
            &collection);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Fail to enumerate audio endpoints: %lX\n", hr);
        goto release_enumerator;
    }

    UINT count;
    hr = IMMDeviceCollection_GetCount(collection, &count);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Fail to get collection size: %lX\n", hr);
        goto release_collection;
    }

    for (UINT i = 0; i < count; i++) {
        hr = IMMDeviceCollection_Item(collection, i, &dev);
        if (hr != S_OK) {
            sp_log(ctx, SP_LOG_ERROR, "Fail to get collection item %u: %lX\n", i, hr);
            goto release_collection;
        }

        IOSysEntry *entry = av_mallocz(sizeof(*entry));
        ret = fill_entry(ctx, entry, dev, default_id);
        IMMDevice_Release(dev);
        if (ret < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Fail to fill entry\n");
            continue;
        }

        AVBufferRef *buf = av_buffer_create((uint8_t *)entry, sizeof(*entry),
                                            destroy_entry, NULL, 0);

        sp_bufferlist_append_noref(ctx->entries, buf);
    }

release_collection:
    IMMDeviceCollection_Release(collection);
release_enumerator:
    IMMDeviceEnumerator_Release(e);

    return 0;
}

static void wasapi_uninit(void *opaque, uint8_t *data)
{
    WasapiCtx *ctx = (WasapiCtx *)data;

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, ctx);
    sp_bufferlist_free(&ctx->entries);

    sp_class_free(ctx);
    av_free(ctx);
}

static int wasapi_init(AVBufferRef **s)
{
    WasapiCtx *ctx = av_mallocz(sizeof(*ctx));
    HRESULT hr;
    int err = 0;

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            wasapi_uninit, NULL, 0);
    if (!ctx_ref) {
        av_free(ctx);
        return AVERROR(ENOMEM);
    }

    ctx->entries = sp_bufferlist_new();
    if (!ctx->entries) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->events = sp_bufferlist_new();
    if (!ctx->events) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = sp_class_alloc(ctx, src_wasapi.name, SP_TYPE_CONTEXT, NULL);
    if (err < 0)
        goto fail;

    /* Enumerate entries */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr != S_OK) {
        sp_log(ctx, SP_LOG_ERROR, "CoInitializeEx() failed: %lX\n", hr);
        goto fail;
    }

    err = enumerate_entries(ctx);
    if (err < 0)
        goto fail;

    *s = ctx_ref;

    return 0;

fail:
    av_buffer_unref(&ctx_ref);

    return err;
}

static AVBufferRef *wasapi_ref_entry(AVBufferRef *ctx_ref, uint32_t identifier)
{
    WasapiCtx *ctx = (WasapiCtx *)ctx_ref->data;
    return sp_bufferlist_pop(ctx->entries, sp_bufferlist_iosysentry_by_id, &identifier);
}

static int wasapi_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx, void *ctx,
                             void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;

    IOSysEntry *entry = ctx;
    WasapiCapture *io_priv = entry->io_priv;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        io_priv->epoch = atomic_load(event->epoch);
        pthread_create(&io_priv->pull_thread, NULL, wasapi_capture_thread, entry);
        sp_log(entry, SP_LOG_VERBOSE, "Started capture thread\n");
        return 0;
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        atomic_store(&io_priv->quit, 1);
        pthread_join(io_priv->pull_thread, NULL);
        sp_log(entry, SP_LOG_VERBOSE, "Stopped capture thread\n");
        return 0;
    } else {
        return AVERROR(ENOTSUP);
    }
}

static int wasapi_ioctx_ctrl(AVBufferRef *entry, SPEventType ctrl, void *arg)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    return sp_ctrl_template(iosys_entry, iosys_entry->events, 0x0,
                            wasapi_ioctx_ctrl_cb, ctrl, arg);
}

static int wasapi_init_io(AVBufferRef *ctx_ref, AVBufferRef *entry,
                       AVDictionary *opts)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    WasapiCapture *cap_priv = (WasapiCapture *)av_mallocz(sizeof(*cap_priv));

    if (!cap_priv)
        return AVERROR(ENOMEM);

    cap_priv->quit = ATOMIC_VAR_INIT(0);

    iosys_entry->ctrl = wasapi_ioctx_ctrl;
    iosys_entry->events = sp_bufferlist_new();

    iosys_entry->io_priv = cap_priv;

    return 0;
}

const IOSysAPI src_wasapi = {
    .name      = "wasapi",
    .init_sys  = wasapi_init,
    .init_io   = wasapi_init_io,
    .ref_entry = wasapi_ref_entry,
    .ctrl      = wasapi_ctrl,
};
