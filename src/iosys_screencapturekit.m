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

#define SCK_ENABLE_AUDIO 1

#include "libtxproto/log.h"
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/graphics/IOGraphicsLib.h>
#import <os/log.h>

#import <stdatomic.h>

#import <libavutil/time.h>
#import <libavutil/pixdesc.h>
#import <libavutil/crc.h>
#import <libavutil/frame.h>
#import <libavutil/pixfmt.h>
#import <libavutil/imgutils.h>
#import <libavutil/hwcontext.h>
#import <libavutil/hwcontext_videotoolbox.h>
#import <libavutil/channel_layout.h>
#import <libavutil/samplefmt.h>

#import "iosys_common.h"
#import <libtxproto/utils.h>
#import "ctrl_template.h"
#import "utils.h"
#import "os_compat.h"
#import "../config.h"

#import <stdlib.h>
#import <pthread.h>
#import <unistd.h>

const IOSysAPI src_screencapturekit;

@class SCKVideoDelegate;
@class SCKAudioDelegate;

typedef struct SCKCtx {
    SPClass *class;
    
    os_log_t os_log;

    SCShareableContent *contentToShare;
    pthread_mutex_t content_lock;

    SPBufferList *entries;
    SPBufferList *events;

} SCKCtx;

typedef struct SCKCapture {
    os_log_t os_log;
    atomic_int quit;

    SCStream *stream;
    SCStreamConfiguration *streamConfiguration;
    id captureDelegate;

    // Hardware device context, intialized in sck_init_io
    AVBufferRef *hw_device_context;

    // Hardware frame context, initialized on-demand once we
    // get the first frame and actually know about the needed
    // size and pixel format.
    // It is cached and attached to all subsequent frames that
    // match the properties. In case of mismatch, a new one is
    // created.
    AVBufferRef *hw_frames_ctx;

    SPRotation rotation;
    AVRational framerate;

    int64_t epoch;

    int dropped_frames;

    AVBufferPool *audio_pool;
    int audio_pool_entry_size;
} SCKCapture;

static inline int64_t sck_now() {
    CMTime now = CMClockGetTime(CMClockGetHostTimeClock());
    return (int64_t)(CMTimeGetSeconds(now) * 1e6);
}

static int sck_stopCapture(IOSysEntry *entry);
static void destroy_entry(void *opaque, uint8_t *data);

static NSString *sck_display_name(CGDirectDisplayID displayID, NSString *uuid)
{
    NSString *localized = nil;
    for (NSScreen *screen in [NSScreen screens]) {
        CGDirectDisplayID screenDisplayID = kCGNullDirectDisplay;
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        if (@available(macOS 26.0, *)) {
            screenDisplayID = screen.CGDirectDisplayID;
        } else
#endif
        {
            NSNumber *screenID = [screen.deviceDescription objectForKey:@"NSScreenNumber"];
            if (screenID)
                screenDisplayID = [screenID unsignedIntValue];
        }
        if (screenDisplayID == displayID) {
            localized = screen.localizedName;
            break;
        }
    }
    if (localized.length > 0)
        return [NSString stringWithFormat:@"%@ [0x%08X]", localized, displayID];
    return uuid;
}

static SPRotation convert_rotation(uint32_t displayID)
{
    int rotation = CGDisplayRotation(displayID);
    switch (rotation) {
        case 90:
            return ROTATION_ROTATE90;
        case 180:
            return ROTATION_ROTATE180;
        case 270:
            return ROTATION_ROTATE270;

        default:
            return ROTATION_IDENTITY;
    }
}

static void destroy_entry(void *opaque, uint8_t *data)
{
    IOSysEntry *entry = (IOSysEntry *)data;
    sp_log(entry, SP_LOG_VERBOSE, "Destroying entry\n");
    sck_stopCapture(entry);
    SCKCapture *capture = (SCKCapture *)entry->io_priv;
    if (capture) {
        av_buffer_unref(&capture->hw_device_context);
        av_free(capture);
        entry->io_priv = NULL;
    }
    sp_class_free(entry);
    av_free(entry);
}

static int init_hwframe_ctx(SCKCapture *capture, int w, int h,
                            enum AVPixelFormat sw_fmt)
{
    av_buffer_unref(&capture->hw_frames_ctx);

    capture->hw_frames_ctx = av_hwframe_ctx_alloc(capture->hw_device_context);
    if (!capture->hw_frames_ctx)
        return AVERROR(ENOMEM);

    AVHWFramesContext *cfg = (AVHWFramesContext *)capture->hw_frames_ctx->data;
    cfg->format = AV_PIX_FMT_VIDEOTOOLBOX;
    cfg->sw_format = sw_fmt;
    cfg->width = w;
    cfg->height = h;

    int ret = av_hwframe_ctx_init(capture->hw_frames_ctx);
    if (ret < 0) {
        av_buffer_unref(&capture->hw_frames_ctx);
        return ret;
    }
    return 0;
}

static AVBufferRef *get_hwframe_ctx_ref(SCKCapture *capture, int w, int h,
                                        enum AVPixelFormat sw_fmt)
{
    if (capture->hw_frames_ctx) {
        AVHWFramesContext *cfg = (AVHWFramesContext *)capture->hw_frames_ctx->data;
        if (cfg->width == w && cfg->height == h && cfg->sw_format == sw_fmt)
            return av_buffer_ref(capture->hw_frames_ctx);
    }

    int ret = init_hwframe_ctx(capture, w, h, sw_fmt);
    if (ret < 0) {
        sp_log(capture, SP_LOG_ERROR,
               "Failed to (re)init AVHWFramesContext: %s\n", av_err2str(ret));
        return NULL;
    }
    return av_buffer_ref(capture->hw_frames_ctx);
}

static void cvpixelbuffer_free(void *opaque, uint8_t *buf)
{
    sp_assert(buf == opaque);
    CVPixelBufferRelease((CVPixelBufferRef)opaque);
}

static AVFrame *avframe_from_video_samplebuffer(SCKCapture *capture, CMSampleBufferRef sb)
{
    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
    if (!pb) {
        os_log(capture->os_log,
                   "%s : can't get image buffer\n", __FUNCTION__);
        return NULL;
    }

    size_t w = CVPixelBufferGetWidth(pb);
    size_t h = CVPixelBufferGetHeight(pb);
    OSType osfmt = CVPixelBufferGetPixelFormatType(pb);
    enum AVPixelFormat ffmt = av_map_videotoolbox_format_to_pixfmt(osfmt);

    AVBufferRef *hw_ctx = get_hwframe_ctx_ref(capture, (int)w, (int)h, ffmt);
    if (!hw_ctx) {
        os_log(capture->os_log,
                   "%s : can't get hwframe ctx\n", __FUNCTION__);
        return NULL;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        os_log(capture->os_log,
                   "%s : can't alloc avframe\n", __FUNCTION__);
        av_buffer_unref(&hw_ctx);
        return NULL;
    }

    frame->hw_frames_ctx = hw_ctx;
    frame->format = AV_PIX_FMT_VIDEOTOOLBOX;
    frame->width = (int)w;
    frame->height = (int)h;
    frame->data[3] = (uint8_t *)pb;
    frame->buf[0] = av_buffer_create((uint8_t *)pb, sizeof(CVPixelBufferRef),
                                     cvpixelbuffer_free, (void *)pb,
                                     AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0]) {
        os_log(capture->os_log,
                   "%s : can't create avbuffer\n", __FUNCTION__);
        av_frame_free(&frame);
        return NULL;
    }
    CVBufferRetain(pb);

    CMTime pts_cm = CMSampleBufferGetPresentationTimeStamp(sb);
    frame->pts = (int64_t)(CMTimeGetSeconds(pts_cm) * 1e6) - capture->epoch;

    frame->time_base = AV_TIME_BASE_Q;

    frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));
    if (!frame->opaque_ref) {
        os_log(capture->os_log,
                   "%s : can't create extradata\n", __FUNCTION__);
        av_frame_free(&frame);
        return NULL;
    }
    FormatExtraData *fe = (FormatExtraData *)frame->opaque_ref->data;
    fe->time_base = AV_TIME_BASE_Q;
    fe->avg_frame_rate = capture->framerate;

    return frame;
}

static void sck_process_video_buffer(IOSysEntry *entry, CMSampleBufferRef sb)
{
    SCKCapture *capture = (SCKCapture *)entry->io_priv;
    if (!sb || atomic_load(&capture->quit))
        return;

    if (!CMSampleBufferGetImageBuffer(sb)) {
        return;
    }

    AVFrame *frame = avframe_from_video_samplebuffer(capture, sb);

    if (!frame) {
        return;
    }

    os_log(capture->os_log, "Video [PTS %lld]: captured", frame->pts);
    int err = sp_frame_fifo_push(entry->frames, frame);
    int64_t pts = frame->pts;

    if (err == AVERROR(ENOBUFS)) {
        capture->dropped_frames++;
        sp_log(entry, SP_LOG_WARN,
                "Dropping video frame (PTS %lld, %d dropped)\n",
                pts, capture->dropped_frames);
        SPGenericData stats[] = {
            { "dropped_frames", NULL, &capture->dropped_frames, SP_DATA_TYPE_INT },
            { 0 },
        };
        sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_STATS, stats);
    } else if (err) {
        sp_log(entry, SP_LOG_ERROR,
                "Push video frame failed: %s\n", av_err2str(err));
    }
    
    av_frame_free(&frame);
}

@interface SCKVideoDelegate : NSObject <SCStreamOutput, SCStreamDelegate>
@property struct IOSysEntry *entry;
@end

@implementation SCKVideoDelegate

- (void)stream:(SCStream *)stream
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        ofType:(SCStreamOutputType)type
{
    if (self.entry && type == SCStreamOutputTypeScreen)
        sck_process_video_buffer(self.entry, sampleBuffer);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    if (!error) return;
    NSString *msg = [NSString stringWithFormat:@"code %ld: %@",
                     error.code, error.localizedDescription];
    if (self.entry)
        sp_log(self.entry, SP_LOG_ERROR,
               "Video SCStream stopped: %s\n", [msg UTF8String]);
    else
        NSLog(@"Video SCStream stopped: %@", msg);
}

@end

static enum AVSampleFormat asbd_to_av_sample_fmt(const AudioStreamBasicDescription *asbd)
{
    if (!(asbd->mFormatFlags & kAudioFormatFlagIsFloat))
        return AV_SAMPLE_FMT_NONE;

    BOOL planar = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    if (asbd->mBitsPerChannel == 32) return planar ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;
    if (asbd->mBitsPerChannel == 64) return planar ? AV_SAMPLE_FMT_DBLP : AV_SAMPLE_FMT_DBL;
    return AV_SAMPLE_FMT_NONE;
}

static AVFrame *avframe_from_audio_samplebuffer(SCKCapture *capture,
                                                IOSysEntry *entry,
                                                CMSampleBufferRef sb)
{
    CMFormatDescriptionRef fmtDesc = CMSampleBufferGetFormatDescription(sb);
    if (!fmtDesc) {
        sp_log(entry, SP_LOG_ERROR, "Audio CMSampleBuffer has no format description\n");
        return NULL;
    }
    const AudioStreamBasicDescription *asbd =
        CMAudioFormatDescriptionGetStreamBasicDescription(fmtDesc);
    if (!asbd) {
        sp_log(entry, SP_LOG_ERROR, "Cannot get ASBD from format description\n");
        return NULL;
    }

    enum AVSampleFormat src_fmt = asbd_to_av_sample_fmt(asbd);
    if (src_fmt == AV_SAMPLE_FMT_NONE) {
        sp_log(entry, SP_LOG_ERROR,
               "Unsupported SCK audio format (flags=0x%x bits=%u)\n",
               (unsigned)asbd->mFormatFlags, (unsigned)asbd->mBitsPerChannel);
        return NULL;
    }

    int nb_ch = (int)asbd->mChannelsPerFrame;
    int rate = (int)asbd->mSampleRate;
    int nb_samples = (int)CMSampleBufferGetNumSamples(sb);
    int bits = (int)asbd->mBitsPerChannel;

    if (nb_ch <= 0 || rate <= 0 || nb_samples <= 0) {
        sp_log(entry, SP_LOG_ERROR,
               "Invalid audio params ch=%d rate=%d samples=%d\n",
               nb_ch, rate, nb_samples);
        return NULL;
    }

    // AudioBufferList has a flexible mBuffers[] tail, so its size depends on
    // the channel layout reported by the sample buffer (1 buffer interleaved,
    // N buffers planar). First call with a NULL list to retrieve the exact
    // allocation size; we then malloc and fill it in the second call below.
    size_t bufListSize = 0;
    OSStatus ret = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sb, &bufListSize, NULL, 0,
        kCFAllocatorDefault, kCFAllocatorDefault,
        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
        NULL);

    if (ret != noErr) {
        sp_log(entry, SP_LOG_ERROR,
               "GetAudioBufferListWithRetainedBlockBuffer (size query) failed: %d\n",
               (int)ret);
        return NULL;
    }

    AudioBufferList *bufList = (AudioBufferList *)av_malloc(bufListSize);
    if (!bufList) {
        sp_log(entry, SP_LOG_ERROR, "OOM allocating AudioBufferList (%zu bytes)\n",
               bufListSize);
        return NULL;
    }

    // Second call: populate the now-sized list and retain the backing
    // CMBlockBuffer so the audio data stays valid for the lifetime of bufList.
    CMBlockBufferRef blockBuf = NULL;
    ret = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sb, NULL, bufList, bufListSize,
        kCFAllocatorDefault, kCFAllocatorDefault,
        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
        &blockBuf);

    if (ret != noErr) {
        sp_log(entry, SP_LOG_ERROR,
               "GetAudioBufferListWithRetainedBlockBuffer failed: %d\n", (int)ret);
        av_free(bufList);
        return NULL;
    }

    BOOL is_planar = (bufList->mNumberBuffers == (uint32_t)nb_ch);
    BOOL is_interleaved = (bufList->mNumberBuffers == 1);

    if (!is_planar && !is_interleaved) {
        sp_log(entry, SP_LOG_WARN,
               "Unexpected mNumberBuffers=%u for %d channels — skipping\n",
               bufList->mNumberBuffers, nb_ch);
        CFRelease(blockBuf);
        av_free(bufList);
        return NULL;
    }

    int bps = bits / 8;
    uint32_t want_buf_bytes = is_planar
        ? (uint32_t)(nb_samples * bps)
        : (uint32_t)(nb_samples * nb_ch * bps);
    for (uint32_t i = 0; i < bufList->mNumberBuffers; i++) {
        if (bufList->mBuffers[i].mDataByteSize < want_buf_bytes) {
            sp_log(entry, SP_LOG_WARN,
                   "Audio buffer[%u] underrun: got %u bytes, want %u\n",
                   i, bufList->mBuffers[i].mDataByteSize, want_buf_bytes);
            CFRelease(blockBuf);
            av_free(bufList);
            return NULL;
        }
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        CFRelease(blockBuf);
        av_free(bufList);
        return NULL;
    }

    frame->format = src_fmt;
    frame->sample_rate = rate;
    frame->nb_samples = nb_samples;
    av_channel_layout_default(&frame->ch_layout, nb_ch);

    int total_size = nb_samples * nb_ch * bps;

    if (!capture->audio_pool || total_size > capture->audio_pool_entry_size) {
        av_buffer_pool_uninit(&capture->audio_pool);
        capture->audio_pool = av_buffer_pool_init2(total_size, NULL, NULL, NULL);
        capture->audio_pool_entry_size = total_size;
    }
    AVBufferRef *pool_buf = av_buffer_pool_get(capture->audio_pool);
    if (!pool_buf) {
        av_frame_free(&frame);
        CFRelease(blockBuf);
        av_free(bufList);
        return NULL;
    }
    frame->buf[0] = pool_buf;

    if (is_planar) {
        int plane_size = nb_samples * bps;
        frame->extended_data = (uint8_t **)av_malloc_array(nb_ch, sizeof(uint8_t *));
        if (!frame->extended_data) {
            av_frame_free(&frame);
            CFRelease(blockBuf);
            av_free(bufList);
            return NULL;
        }
        for (int c = 0; c < nb_ch; c++) {
            uint8_t *p = pool_buf->data + c * plane_size;
            frame->extended_data[c] = p;
            if (c < AV_NUM_DATA_POINTERS)
                frame->data[c] = p;
            memcpy(p, bufList->mBuffers[c].mData, (size_t)plane_size);
        }
        frame->linesize[0] = plane_size;
    } else {
        frame->data[0] = pool_buf->data;
        frame->extended_data = frame->data;
        frame->linesize[0] = total_size;
        memcpy(pool_buf->data, bufList->mBuffers[0].mData, (size_t)total_size);
    }

    CFRelease(blockBuf);
    av_free(bufList);

    CMTime pts_cm = CMSampleBufferGetPresentationTimeStamp(sb);
    frame->pts = (int64_t)(CMTimeGetSeconds(pts_cm) * 1e6) - capture->epoch;
    frame->duration = av_rescale_q(nb_samples, (AVRational){1, rate}, AV_TIME_BASE_Q);

    frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));
    if (!frame->opaque_ref) {
        os_log(capture->os_log,
                   "%s : can't create extradata\n", __FUNCTION__);
        av_frame_free(&frame);
        return NULL;
    }
    FormatExtraData *fe = (FormatExtraData *)frame->opaque_ref->data;
    fe->time_base = AV_TIME_BASE_Q;
    fe->bits_per_sample = bits;

    return frame;
}

static void sck_process_audio_buffer(IOSysEntry *entry, CMSampleBufferRef sb)
{
    SCKCapture *capture = (SCKCapture *)entry->io_priv;
    if (!sb || atomic_load(&capture->quit))
        return;

    AVFrame *frame = avframe_from_audio_samplebuffer(capture, entry, sb);

    if (!frame) {
        return;
    }

    int nb_samples = frame->nb_samples;
    sp_log(entry, SP_LOG_TRACE,
           "Pushing audio frame to FIFO, pts=%f, len=%.2f ms\n",
           (double)frame->pts / 1e6,
           (double)frame->duration / 1e3);

    int err = sp_frame_fifo_push(entry->frames, frame);

    if (err == AVERROR(ENOBUFS)) {
        capture->dropped_frames++;
        sp_log(entry, SP_LOG_WARN,
               "Dropping audio (%d samples, %d dropped so far)\n",
               nb_samples, capture->dropped_frames);
    } else if (err) {
        sp_log(entry, SP_LOG_ERROR,
               "Push audio frame failed: %s\n", av_err2str(err));
    }
    av_frame_free(&frame);
}

API_AVAILABLE(macos(13.0))
@interface SCKAudioDelegate : NSObject <SCStreamOutput, SCStreamDelegate>
@property struct IOSysEntry *entry;
@end

@implementation SCKAudioDelegate

- (void)stream:(SCStream *)stream
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        ofType:(SCStreamOutputType)type
{
    if (self.entry && type == SCStreamOutputTypeAudio)
        sck_process_audio_buffer(self.entry, sampleBuffer);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    if (!error) return;
    NSString *msg = [NSString stringWithFormat:@"code %ld: %@",
                     error.code, error.localizedDescription];
    if (self.entry)
        sp_log(self.entry, SP_LOG_ERROR,
               "Audio SCStream stopped: %s\n", [msg UTF8String]);
    else
        NSLog(@"Audio SCStream stopped: %@", msg);
}

@end

static int sck_stopCapture(IOSysEntry *entry)
{
    SCKCapture *capture = (SCKCapture *)entry->io_priv;
    if (!capture) return 0;

    atomic_store(&capture->quit, 1);

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block NSError *stopErr = nil;

    if (capture->stream) {
        [capture->stream stopCaptureWithCompletionHandler:^(NSError *e) {
            stopErr = e;
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        if (stopErr) {
            sp_log(entry, SP_LOG_ERROR, "stopCapture failed: %s\n",
                   [[stopErr localizedFailureReason] UTF8String]);
        }

        SCStreamOutputType outType = (entry->type == SP_IO_TYPE_VIDEO_DISPLAY)
                                     ? SCStreamOutputTypeScreen
                                     : SCStreamOutputTypeAudio;
        NSError *removeError = nil;
        [capture->stream removeStreamOutput:capture->captureDelegate
                                       type:outType
                                      error:&removeError];
        if (removeError)
            sp_log(entry, SP_LOG_WARN, "removeStreamOutput failed: %s\n",
                   [[removeError localizedFailureReason] UTF8String]);

        capture->stream = nil;
    }

    capture->captureDelegate = nil;
    capture->streamConfiguration = nil;
    capture->os_log = nil;

    av_buffer_unref(&capture->hw_frames_ctx);
    av_buffer_pool_uninit(&capture->audio_pool);

    return 0;
}

static SCContentFilter *sck_make_filter(IOSysEntry *entry, SCKCtx *ctx)
{
    pthread_mutex_lock(&ctx->content_lock);
    SCShareableContent *contentSnap = ctx->contentToShare;
    pthread_mutex_unlock(&ctx->content_lock);

    SCDisplay *display = nil;
    for (SCDisplay *d in contentSnap.displays)
        if (d.displayID == entry->api_id) { display = d; break; }
    if (!display) {
        sp_log(entry, SP_LOG_FATAL, "Display %d not found\n", entry->api_id);
        return nil;
    }

    SCRunningApplication *selfApp = nil;
    NSString *bundleID = [[NSBundle mainBundle] bundleIdentifier];
    for (SCRunningApplication *app in contentSnap.applications) {
        if ([app.bundleIdentifier isEqualToString:bundleID]) {
            selfApp = app;
            break;
        }
    }
    NSArray *excluded = selfApp ? @[selfApp] : @[];
    return [[SCContentFilter alloc]
            initWithDisplay:display
            excludingApplications:excluded
            exceptingWindows:@[]];
}

static int sck_run_start_completion(SCStream *stream, IOSysEntry *entry)
{
    __block NSError *startErr = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [stream startCaptureWithCompletionHandler:^(NSError *e) {
        startErr = e;
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

    if (startErr) {
        sp_log(entry, SP_LOG_FATAL, "startCapture failed: %s\n",
               [[startErr localizedFailureReason] UTF8String]);
        return -1;
    }
    return 0;
}

static int sck_startVideoCapture(IOSysEntry *entry)
{
    SCKCapture *capture = (SCKCapture *)entry->io_priv;
    SCKCtx *ctx = (SCKCtx *)entry->api_priv;
    SCContentFilter *contentFilter = nil;
    SCKVideoDelegate *delegate = nil;
    dispatch_queue_t queue = NULL;
    NSError *error = nil;

    sp_set_thread_name_self(sp_class_get_name(entry));
    sp_eventlist_dispatch(entry, entry->events,
                          SP_EVENT_ON_CONFIG | SP_EVENT_ON_INIT, NULL);

    capture->os_log = os_log_create("org.ffmpeg.txproto", "sck-video");

    contentFilter = sck_make_filter(entry, ctx);
    if (!contentFilter)
        goto fail;

    capture->streamConfiguration = [[SCStreamConfiguration alloc] init];
    [capture->streamConfiguration setShowsCursor:NO];
    [capture->streamConfiguration setQueueDepth:3];
    [capture->streamConfiguration setCapturesAudio:NO];

    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(entry->api_id);
    if (mode) {
        [capture->streamConfiguration setWidth:CGDisplayModeGetPixelWidth(mode)];
        [capture->streamConfiguration setHeight:CGDisplayModeGetPixelHeight(mode)];
        CGDisplayModeRelease(mode);
    }

    delegate = [[SCKVideoDelegate alloc] init];
    delegate.entry = entry;
    capture->captureDelegate = delegate;

    capture->stream = [[SCStream alloc] initWithFilter:contentFilter
                                      configuration:capture->streamConfiguration
                                           delegate:delegate];

    queue = dispatch_queue_create("org.ffmpeg.txproto.sck-video",
                                  DISPATCH_QUEUE_SERIAL);

    BOOL ok = [capture->stream addStreamOutput:delegate
                                       type:SCStreamOutputTypeScreen
                         sampleHandlerQueue:queue
                                      error:&error];
    if (!ok) {
        sp_log(entry, SP_LOG_FATAL,
               "addStreamOutput (video) failed: %s\n",
               [[error localizedFailureReason] UTF8String]);
        goto fail;
    }

    if (sck_run_start_completion(capture->stream, entry) < 0)
        goto fail;

    sp_log(entry, SP_LOG_DEBUG, "Video capture started for display %d\n",
           entry->api_id);
    return 0;

fail:
    sck_stopCapture(entry);
    return -1;
}

static int sck_startAudioCapture(IOSysEntry *entry)
{
    SCKCapture *capture = (SCKCapture *)entry->io_priv;

    if (@available(macOS 13.0, *)) {
        SCKCtx *ctx = (SCKCtx *)entry->api_priv;
        SCContentFilter *contentFilter = nil;
        SCKAudioDelegate *delegate = nil;
        dispatch_queue_t queue = NULL;
        NSError *error = nil;

        sp_set_thread_name_self(sp_class_get_name(entry));
        sp_eventlist_dispatch(entry, entry->events,
                              SP_EVENT_ON_CONFIG | SP_EVENT_ON_INIT, NULL);

        capture->os_log = os_log_create("org.ffmpeg.txproto", "sck-audio");

        contentFilter = sck_make_filter(entry, ctx);
        if (!contentFilter)
            goto fail;

        capture->streamConfiguration = [[SCStreamConfiguration alloc] init];
        [capture->streamConfiguration setShowsCursor:NO];
        [capture->streamConfiguration setQueueDepth:1];
        [capture->streamConfiguration setWidth:2];
        [capture->streamConfiguration setHeight:2];
        [capture->streamConfiguration setMinimumFrameInterval:
            CMTimeMake(1, 1)]; /* 1 fps — just enough to keep SCK happy */
#if defined(__MAC_15_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_15_0
        if (@available(macOS 15.0, *))
            [capture->streamConfiguration setCaptureResolution:
                SCCaptureResolutionNominal];
#endif
        [capture->streamConfiguration setCapturesAudio:YES];
        [capture->streamConfiguration setExcludesCurrentProcessAudio:YES];
        [capture->streamConfiguration setSampleRate:48000];
        [capture->streamConfiguration setChannelCount:2];

        delegate = [[SCKAudioDelegate alloc] init];
        delegate.entry = entry;
        capture->captureDelegate = delegate;

        capture->stream = [[SCStream alloc] initWithFilter:contentFilter
                                          configuration:capture->streamConfiguration
                                               delegate:delegate];

        queue = dispatch_queue_create("org.ffmpeg.txproto.sck-audio",
                                      DISPATCH_QUEUE_SERIAL);
        BOOL ok = [capture->stream addStreamOutput:delegate
                                           type:SCStreamOutputTypeAudio
                             sampleHandlerQueue:queue
                                          error:&error];
        if (!ok) {
            sp_log(entry, SP_LOG_FATAL,
                   "addStreamOutput (audio) failed: %s\n",
                   [[error localizedFailureReason] UTF8String]);
            goto fail;
        }

        if (sck_run_start_completion(capture->stream, entry) < 0)
            goto fail;

        sp_log(entry, SP_LOG_DEBUG, "Audio capture started for display %d\n",
               entry->api_id);
        return 0;

    fail:
        sck_stopCapture(entry);
        return -1;
    }

    sp_log(entry, SP_LOG_FATAL, "Audio capture requires macOS 13.0 or later\n");
    return -1;
}

static int sck_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx,
                              void *ctx, void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;
    IOSysEntry *entry = ctx;
    SCKCapture *capture = (SCKCapture *)entry->io_priv;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        atomic_store(&capture->quit, 0);
        capture->epoch = sck_now();
        if (entry->type == SP_IO_TYPE_VIDEO_DISPLAY)
            return sck_startVideoCapture(entry);
        if (entry->type == SP_IO_TYPE_AUDIO_MONITOR)
            return sck_startAudioCapture(entry);
        return AVERROR(EINVAL);
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        return sck_stopCapture(entry);
    }
    return AVERROR(ENOTSUP);
}

static int sck_ioctx_ctrl(AVBufferRef *entry_ref, SPEventType ctrl, void *arg)
{
    IOSysEntry *entry = (IOSysEntry *)entry_ref->data;
    return sp_ctrl_template(entry, entry->events, 0x0, sck_ioctx_ctrl_cb, ctrl, arg);
}

static int sck_init_io(AVBufferRef *ctx_ref, AVBufferRef *entry_ref,
                       AVDictionary *opts)
{
    int ret;
    IOSysEntry *entry = (IOSysEntry *)entry_ref->data;

    SCKCapture *capture = av_mallocz(sizeof(*capture));
    if (!capture)
        return AVERROR(ENOMEM);

    capture->quit = ATOMIC_VAR_INIT(0);

    if (entry->type == SP_IO_TYPE_VIDEO_DISPLAY) {
        capture->hw_device_context =
            av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
        if (!capture->hw_device_context) {
            sp_log(entry, SP_LOG_ERROR,
                   "AVHWDeviceContext alloc failed\n");
            av_free(capture);
            return AVERROR(ENOMEM);
        }

        ret = av_hwdevice_ctx_init(capture->hw_device_context);
        if (ret < 0) {
            sp_log(entry, SP_LOG_ERROR,
                   "AVHWDeviceContext init failed: %s\n", av_err2str(ret));
            av_buffer_unref(&capture->hw_device_context);
            av_free(capture);
            return ret;
        }

        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(entry->api_id);
        if (mode) {
            capture->framerate = av_d2q(CGDisplayModeGetRefreshRate(mode), 10000);
            CGDisplayModeRelease(mode);
        }
        capture->rotation = convert_rotation(entry->api_id);

        sp_log(entry, SP_LOG_VERBOSE,
               "Video init_io OK (display %d)\n", entry->api_id);

    } else if (entry->type == SP_IO_TYPE_AUDIO_MONITOR) {
#ifdef SCK_ENABLE_AUDIO
        sp_log(entry, SP_LOG_VERBOSE,
               "Audio init_io OK (display %d)\n", entry->api_id);
#else
        av_free(capture);
        return AVERROR(EINVAL);
#endif
    } else {
        sp_log(entry, SP_LOG_ERROR,
               "sck_init_io: unsupported entry type %d\n", entry->type);
        av_free(capture);
        return AVERROR(EINVAL);
    }

    entry->io_priv = capture;
    entry->ctrl = sck_ioctx_ctrl;
    entry->events = sp_bufferlist_new();

    return 0;
}

static int update_entries(SCKCtx *ctx)
{
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block SCShareableContent *result = nil;
    __block NSError *resErr = nil;

    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent *c,
                                                                     NSError *e) {
        result = c;
        resErr = e;
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

    if (resErr) {
        NSLog(@"SCK getShareableContent error: %@",
              [resErr localizedFailureReason]);
        return AVERROR_EXTERNAL;
    }
    pthread_mutex_lock(&ctx->content_lock);

    ctx->contentToShare = result;

    pthread_mutex_unlock(&ctx->content_lock);

    for (SCDisplay *display in result.displays) {
        uint32_t displayID = display.displayID;
        int x = (int)display.frame.origin.x;
        int y = (int)display.frame.origin.y;
        int w = (int)display.width;
        int h = (int)display.height;

        CFUUIDRef uuidRef = CGDisplayCreateUUIDFromDisplayID(displayID);
        NSString *uuid = @"";
        if (uuidRef) {
            uuid = (__bridge_transfer NSString *)CFUUIDCreateString(kCFAllocatorDefault,
                                                          uuidRef);
            CFRelease(uuidRef);
        }

        uint32_t vid_id = sp_iosys_gen_identifier(ctx, displayID, SP_IO_TYPE_VIDEO_DISPLAY);
        uint32_t aud_id = sp_iosys_gen_identifier(ctx, displayID, SP_IO_TYPE_AUDIO_MONITOR);

        NSString *display_name = sck_display_name(displayID, uuid);

        SPRotation rotation = convert_rotation(displayID);

        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
        AVRational fr = (AVRational){ 0, 1 };
        int scale = 1;
        if (mode) {
            fr = av_d2q(CGDisplayModeGetRefreshRate(mode), 10000);
            scale = (int)(CGDisplayModeGetPixelWidth(mode) / display.width);
            CGDisplayModeRelease(mode);
        }

        {
            AVBufferRef *ref = sp_bufferlist_ref(ctx->entries,
                                                 sp_bufferlist_iosysentry_by_id,
                                                 &vid_id);
            IOSysEntry *entry;
            BOOL is_new = NO;
            BOOL did_change = NO;

            if (!ref) {
                entry = av_mallocz(sizeof(*entry));
                sp_class_alloc(entry, NULL, SP_TYPE_VIDEO_BIDIR, ctx);
                sp_class_set_name(entry, [display_name UTF8String]);
                entry->identifier = vid_id;
                entry->api_id = displayID;
                entry->type = SP_IO_TYPE_VIDEO_DISPLAY;
                entry->frames = sp_frame_fifo_create(entry, 0, 0);
                entry->api_priv = ctx;
                is_new = YES;
            } else {
                entry = (IOSysEntry *)ref->data;
            }

            did_change = (entry->x != x) || (entry->y != y) ||
                         (entry->width != w) || (entry->height != h) ||
                         (entry->rotation != rotation);

            entry->x = x; entry->y = y; entry->width = w; entry->height = h;
            entry->rotation = rotation;
            entry->framerate = fr;
            entry->scale = scale;
            entry->is_default = CGDisplayIsMain(displayID);

            if (is_new) {
                AVBufferRef *buf = av_buffer_create((uint8_t *)entry, sizeof(*entry),
                                                    destroy_entry, NULL, 0);
                sp_bufferlist_append_noref(ctx->entries, buf);
            } else {
                if (did_change)
                    sp_eventlist_dispatch(entry, ctx->events,
                                          SP_EVENT_ON_CHANGE |
                                          SP_EVENT_TYPE_SOURCE |
                                          SP_EVENT_TYPE_SINK, entry);
                av_buffer_unref(&ref);
            }
        }
#ifdef SCK_ENABLE_AUDIO
        {
            AVBufferRef *ref = sp_bufferlist_ref(ctx->entries,
                                                 sp_bufferlist_iosysentry_by_id,
                                                 &aud_id);
            if (!ref) {
                IOSysEntry *entry = av_mallocz(sizeof(*entry));
                sp_class_alloc(entry, NULL, SP_TYPE_AUDIO_BIDIR, ctx);

                NSString *aname = [NSString stringWithFormat:@"%@ (audio)", display_name];
                sp_class_set_name(entry, [aname UTF8String]);

                entry->identifier = aud_id;
                entry->api_id = displayID;
                entry->type = SP_IO_TYPE_AUDIO_MONITOR;
                entry->frames = sp_frame_fifo_create(entry, 0, 0);
                entry->api_priv = ctx;
                entry->sample_rate = 48000;
                entry->sample_fmt = AV_SAMPLE_FMT_FLTP;
                entry->channels = 2;
                av_channel_layout_default(&entry->ch_layout, 2);
                entry->is_default = CGDisplayIsMain(displayID);

                AVBufferRef *buf = av_buffer_create((uint8_t *)entry, sizeof(*entry),
                                                    destroy_entry, NULL, 0);
                sp_bufferlist_append_noref(ctx->entries, buf);
            } else {
                IOSysEntry *entry = (IOSysEntry *)ref->data;
                entry->is_default = CGDisplayIsMain(displayID);
                av_buffer_unref(&ref);
            }
        }
#endif // #ifdef SCK_ENABLE_AUDIO
    }

    return 0;
}

static void sck_display_reconfig_cb(CGDirectDisplayID display,
                                    CGDisplayChangeSummaryFlags flags,
                                    void *userInfo)
{
    SCKCtx *ctx = (SCKCtx *)userInfo;

    if (flags & (kCGDisplayAddFlag | kCGDisplayRemoveFlag |
                 kCGDisplayEnabledFlag | kCGDisplayDisabledFlag |
                 kCGDisplayMovedFlag | kCGDisplaySetModeFlag))
        update_entries(ctx);
}

static int sck_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    int err = 0;
    SCKCtx *ctx = (SCKCtx *)ctx_ref->data;

    if (ctrl & SP_EVENT_CTRL_NEW_EVENT) {
        AVBufferRef *event = (AVBufferRef *)arg;
        char *fstr = sp_event_flags_to_str_buf(event);
        sp_log(ctx, SP_LOG_DEBUG, "New event (%s)\n", fstr);
        av_free(fstr);

        if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
            SPBufferList *tmp = sp_bufferlist_new();
            sp_eventlist_add(ctx, tmp, event, 1);
            update_entries(ctx);

            AVBufferRef *obj = NULL;
            while ((obj = sp_bufferlist_iter_ref(ctx->entries))) {
                sp_eventlist_dispatch(obj->data, tmp,
                                      SP_EVENT_ON_CHANGE | SP_EVENT_TYPE_SOURCE,
                                      obj->data);
                av_buffer_unref(&obj);
            }
            sp_bufferlist_free(&tmp);
        }

        err = sp_eventlist_add(ctx, ctx->events, event, 1);
    }

    return err;
}

static void sck_uninit(void *opaque, uint8_t *data)
{
    SCKCtx *ctx = (SCKCtx *)data;

    CGDisplayRemoveReconfigurationCallback(sck_display_reconfig_cb, ctx);

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, ctx);
    sp_bufferlist_free(&ctx->entries);
    sp_bufferlist_free(&ctx->events);

    pthread_mutex_destroy(&ctx->content_lock);

    ctx->contentToShare = nil;
    ctx->os_log = nil;

    sp_class_free(ctx);
    av_free(ctx);
}

static int sck_init(AVBufferRef **s)
{
    int err = 0;
    SCKCtx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx) return AVERROR(ENOMEM);

    ctx->os_log = os_log_create("org.ffmpeg.txproto",
                                 "sck-ctx");

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            sck_uninit, NULL, 0);
    if (!ctx_ref) { av_free(ctx); return AVERROR(ENOMEM); }

    ctx->entries = sp_bufferlist_new();
    if (!ctx->entries) { err = AVERROR(ENOMEM); goto fail; }

    ctx->events = sp_bufferlist_new();
    if (!ctx->events) { err = AVERROR(ENOMEM); goto fail; }

    err = sp_class_alloc(ctx, src_screencapturekit.name, SP_TYPE_CONTEXT, NULL);
    if (err < 0) goto fail;

    pthread_mutex_init(&ctx->content_lock, NULL);

    update_entries(ctx);
    CGDisplayRegisterReconfigurationCallback(sck_display_reconfig_cb, ctx);

    *s = ctx_ref;
    return 0;

fail:
    av_buffer_unref(&ctx_ref);
    return err;
}

static AVBufferRef *sck_ref_entry(AVBufferRef *ctx_ref, uint32_t identifier)
{
    SCKCtx *ctx = (SCKCtx *)ctx_ref->data;
    return sp_bufferlist_ref(ctx->entries, sp_bufferlist_iosysentry_by_id,
                             &identifier);
}

const IOSysAPI src_screencapturekit = {
    .name      = "screencapturekit",
    .init_sys  = sck_init,
    .init_io   = sck_init_io,
    .ref_entry = sck_ref_entry,
    .ctrl      = sck_ctrl,
};
