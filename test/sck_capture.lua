audio_monitor_id = nil
video_display_id = nil

function io_update_cb(identifier, entry)
    if video_display_id == nil and entry.type == "display" and entry.default then
        video_display_id = identifier
    end
    if audio_monitor_id == nil and entry.type == "monitor" and entry.default then
        audio_monitor_id = identifier
    end
end

function muxer_stats(stats)
    statusline = "Encoding, bitrate: " .. math.floor(stats.bitrate / 1000) .. " Kbps"
    tx.set_status(statusline)
end

function main(...)
    event = tx.register_io_cb(io_update_cb, "screencapturekit")
    event.destroy()

    tx.set_epoch(0)

    source_video = tx.create_io(video_display_id)

    filter_vid = tx.create_filtergraph({
            graph = "hwdownload,format=nv12,scale=w=1920:h=-2",
            priv_options = { dump_graph = false },
        })
    filter_vid.link(source_video)

    encoder_v = tx.create_encoder({
            encoder  = "hevc_videotoolbox",
            pix_fmt  = "nv12",
            options  = {
                b = 10^3 * 8000,
                realtime = "1",
                allow_sw = "1",
            },
        })
    encoder_v.link(filter_vid)

    source_audio = tx.create_io(audio_monitor_id)

    encoder_a = tx.create_encoder({
            encoder        = "libopus",
            sample_fmt     = "fltp",
            ch_layout      = "stereo",
            options        = {
                b = 10^3 * 192,
                application = "audio",
                frame_duration = 60,
                vbr = "on",
            },
        })
    encoder_a.link(source_audio)

    muxer = tx.create_muxer({
            out_url = "rec.mp4",
            priv_options = {
                dump_info = true, 
                low_latency = false
            },
        })
    muxer.link(encoder_v)
    muxer.link(encoder_a)
    muxer.schedule("stats", muxer_stats)

    tx.commit()
end
