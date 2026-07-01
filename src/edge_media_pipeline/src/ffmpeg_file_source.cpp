#include "ffmpeg_file_source.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#if __has_include(<libavcodec/bsf.h>)
#include <libavcodec/bsf.h>
#define XIAOMAN_HAVE_FFMPEG_NEW_BSF 1
#define XIAOMAN_HAVE_FFMPEG_LEGACY_BSF 0
#else
#define XIAOMAN_HAVE_FFMPEG_NEW_BSF 0
#define XIAOMAN_HAVE_FFMPEG_LEGACY_BSF 1
#endif
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
}

#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace
{
MppCodingType MapAvCodecToMpp(AVCodecID codec_id)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_H264:
            return MPP_VIDEO_CodingAVC;
        case AV_CODEC_ID_HEVC:
            return MPP_VIDEO_CodingHEVC;
        default:
            return MPP_VIDEO_CodingUnused;
    }
}

uint32_t RationalToFps(const AVRational& fps)
{
    if (fps.num <= 0 || fps.den <= 0)
    {
        return 30;
    }

    const double value = av_q2d(fps);
    if (value <= 0.0)
    {
        return 30;
    }

    const uint32_t fps_int = static_cast<uint32_t>(value + 0.5);
    return (fps_int > 0) ? fps_int : 30;
}

int64_t TsToUs(int64_t ts, int64_t tb_num, int64_t tb_den)
{
    if (ts == AV_NOPTS_VALUE || tb_num <= 0 || tb_den <= 0)
    {
        return -1;
    }

    const AVRational src_tb{static_cast<int>(tb_num), static_cast<int>(tb_den)};
    const AVRational dst_tb{1, 1000000};
    return av_rescale_q(ts, src_tb, dst_tb);
}

bool StartsWith(const std::string& text, const char* prefix)
{
    if (!prefix)
    {
        return false;
    }

    const std::string prefix_str(prefix);
    if (text.size() < prefix_str.size())
    {
        return false;
    }
    return text.compare(0, prefix_str.size(), prefix_str) == 0;
}
}  // namespace

FfmpegFileSource::FfmpegFileSource() {}

FfmpegFileSource::~FfmpegFileSource() { Close(); }

int FfmpegFileSource::Open(const FfmpegFileSourceConfig& config)
{
    Close();

    if (config.input_url.empty())
    {
        std::cerr << "FFmpeg input url is empty" << std::endl;
        return -1;
    }

    config_ = config;
    av_log_set_level(AV_LOG_ERROR);

    AVDictionary* open_opts = nullptr;
    if (IsRealtimeInput())
    {
        const int64_t open_timeout_us = static_cast<int64_t>(config_.open_timeout_ms) * 1000;
        const int64_t read_timeout_us = static_cast<int64_t>(config_.read_timeout_ms) * 1000;
        if (config_.prefer_tcp_for_rtsp && StartsWith(config_.input_url, "rtsp://"))
        {
            av_dict_set(&open_opts, "rtsp_transport", "tcp", 0);
        }
        av_dict_set(&open_opts, "fflags", "nobuffer", 0);
        av_dict_set(&open_opts, "flags", "low_delay", 0);
        av_dict_set_int(&open_opts, "max_delay", 0, 0);
        av_dict_set_int(&open_opts, "stimeout", open_timeout_us, 0);
        av_dict_set_int(&open_opts, "rw_timeout", read_timeout_us, 0);
    }

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, config_.input_url.c_str(), nullptr, &open_opts) < 0 ||
        !fmt_ctx)
    {
        av_dict_free(&open_opts);
        std::cerr << "Failed to open FFmpeg input: " << config_.input_url << std::endl;
        return -1;
    }
    av_dict_free(&open_opts);

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0)
    {
        std::cerr << "Failed to read stream info from input: " << config_.input_url << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    const int video_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index < 0)
    {
        std::cerr << "No video stream found in input: " << config_.input_url << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVStream* stream = fmt_ctx->streams[video_index];
    if (!stream || !stream->codecpar)
    {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    const MppCodingType coding_type = MapAvCodecToMpp(stream->codecpar->codec_id);
    if (coding_type == MPP_VIDEO_CodingUnused)
    {
        std::cerr << "Unsupported file codec for MPP decode: "
                  << avcodec_get_name(stream->codecpar->codec_id) << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    fmt_ctx_            = fmt_ctx;
    video_stream_index_ = video_index;
    codec_id_           = stream->codecpar->codec_id;
    time_base_num_      = stream->time_base.num;
    time_base_den_      = stream->time_base.den;

    video_info_.width       = static_cast<uint32_t>(stream->codecpar->width);
    video_info_.height      = static_cast<uint32_t>(stream->codecpar->height);
    video_info_.fps         = RationalToFps(av_guess_frame_rate(fmt_ctx, stream, nullptr));
    video_info_.coding_type = coding_type;
    video_info_.valid       = (video_info_.width > 0 && video_info_.height > 0);

    if (!video_info_.valid)
    {
        std::cerr << "Invalid video geometry from FFmpeg input" << std::endl;
        Close();
        return -1;
    }

    if (CreateBitstreamFilter() != 0)
    {
        Close();
        return -1;
    }

    demux_packet_ = av_packet_alloc();
    if (!demux_packet_)
    {
        std::cerr << "Failed to allocate reusable demux packet" << std::endl;
        Close();
        return -1;
    }

    std::cout << "[INPUT] Opened FFmpeg source: " << config_.input_url
              << ", codec=" << avcodec_get_name(stream->codecpar->codec_id)
              << ", size=" << video_info_.width << "x" << video_info_.height
              << ", fps=" << video_info_.fps << std::endl;
    return 0;
}

int FfmpegFileSource::CreateBitstreamFilter()
{
    if (!fmt_ctx_ || video_stream_index_ < 0)
    {
        return -1;
    }

    const char*   bsf_name = nullptr;
    if (codec_id_ == AV_CODEC_ID_H264)
    {
        bsf_name = "h264_mp4toannexb";
    }
    else if (codec_id_ == AV_CODEC_ID_HEVC)
    {
        bsf_name = "hevc_mp4toannexb";
    }

    if (!bsf_name)
    {
        return 0;
    }

#if XIAOMAN_HAVE_FFMPEG_NEW_BSF
    AVBSFContext* bsf_ctx = nullptr;
    const AVBitStreamFilter* bsf = av_bsf_get_by_name(bsf_name);
    if (!bsf)
    {
        std::cerr << "Failed to find bitstream filter: " << bsf_name << std::endl;
        return -1;
    }

    if (av_bsf_alloc(bsf, &bsf_ctx) < 0 || !bsf_ctx)
    {
        std::cerr << "Failed to allocate bitstream filter: " << bsf_name << std::endl;
        return -1;
    }

    AVFormatContext* fmt_ctx = static_cast<AVFormatContext*>(fmt_ctx_);
    AVStream*        stream  = fmt_ctx->streams[video_stream_index_];
    if (avcodec_parameters_copy(bsf_ctx->par_in, stream->codecpar) < 0)
    {
        av_bsf_free(&bsf_ctx);
        return -1;
    }
    bsf_ctx->time_base_in = stream->time_base;

    if (av_bsf_init(bsf_ctx) < 0)
    {
        std::cerr << "Failed to init bitstream filter: " << bsf_name << std::endl;
        av_bsf_free(&bsf_ctx);
        return -1;
    }

    bsf_ctx_ = bsf_ctx;
    return 0;
#elif XIAOMAN_HAVE_FFMPEG_LEGACY_BSF
    AVBitStreamFilterContext* bsf_ctx = av_bitstream_filter_init(bsf_name);
    if (!bsf_ctx)
    {
        std::cerr << "Failed to find legacy bitstream filter: " << bsf_name << std::endl;
        return -1;
    }

    AVFormatContext* fmt_ctx = static_cast<AVFormatContext*>(fmt_ctx_);
    AVStream*        stream  = fmt_ctx->streams[video_stream_index_];
    AVCodecContext*  codec_ctx = avcodec_alloc_context3(nullptr);
    if (!codec_ctx)
    {
        av_bitstream_filter_close(bsf_ctx);
        std::cerr << "Failed to allocate legacy bitstream filter codec context" << std::endl;
        return -1;
    }

    if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0)
    {
        avcodec_free_context(&codec_ctx);
        av_bitstream_filter_close(bsf_ctx);
        std::cerr << "Failed to copy codec parameters for legacy bitstream filter"
                  << std::endl;
        return -1;
    }
    codec_ctx->time_base = stream->time_base;

    bsf_ctx_       = bsf_ctx;
    bsf_codec_ctx_ = codec_ctx;
    std::cout << "[INPUT] Using legacy FFmpeg bitstream filter: " << bsf_name << std::endl;
    return 0;
#else
    std::cerr << "FFmpeg bitstream filter API is unavailable: " << bsf_name << std::endl;
    return -1;
#endif
}

int FfmpegFileSource::SeekToStart()
{
    if (!fmt_ctx_)
    {
        return -1;
    }
    if (IsRealtimeInput())
    {
        return -1;
    }

    AVFormatContext* fmt_ctx = static_cast<AVFormatContext*>(fmt_ctx_);
    if (av_seek_frame(fmt_ctx, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD) < 0)
    {
        std::cerr << "Failed to seek media file back to start" << std::endl;
        return -1;
    }

    avformat_flush(fmt_ctx);
#if XIAOMAN_HAVE_FFMPEG_NEW_BSF
    if (bsf_ctx_)
    {
        av_bsf_flush(static_cast<AVBSFContext*>(bsf_ctx_));
    }
#elif XIAOMAN_HAVE_FFMPEG_LEGACY_BSF
    if (bsf_ctx_)
    {
        AVBitStreamFilterContext* bsf_ctx =
            static_cast<AVBitStreamFilterContext*>(bsf_ctx_);
        av_bitstream_filter_close(bsf_ctx);
        bsf_ctx_ = nullptr;
    }
    if (bsf_codec_ctx_)
    {
        AVCodecContext* codec_ctx = static_cast<AVCodecContext*>(bsf_codec_ctx_);
        avcodec_free_context(&codec_ctx);
        bsf_codec_ctx_ = nullptr;
    }
    if (CreateBitstreamFilter() != 0)
    {
        return -1;
    }
#endif
    pending_decoder_reset_    = true;
    pending_timestamp_rebase_ = true;
    return 0;
}

bool FfmpegFileSource::ConsumeDecoderResetRequest()
{
    const bool pending     = pending_decoder_reset_;
    pending_decoder_reset_ = false;
    return pending;
}

void FfmpegFileSource::RebaseLocalFileTimestamp(FrameDesc* out_desc)
{
    if (!out_desc || IsRealtimeInput())
    {
        return;
    }

    const int64_t frame_interval_us =
        (video_info_.fps > 0) ? (1000000LL / video_info_.fps) : 33333LL;
    int64_t source_ts_us = (out_desc->pts_us >= 0) ? out_desc->pts_us : out_desc->dts_us;

    if (pending_timestamp_rebase_)
    {
        if (last_output_pts_us_ >= 0)
        {
            const int64_t next_pts_us = last_output_pts_us_ + frame_interval_us;
            timestamp_offset_us_ = (source_ts_us >= 0) ? (next_pts_us - source_ts_us) : next_pts_us;
        }
        else
        {
            timestamp_offset_us_ = 0;
        }
        pending_timestamp_rebase_ = false;
    }

    if (source_ts_us >= 0)
    {
        if (out_desc->pts_us >= 0)
        {
            out_desc->pts_us += timestamp_offset_us_;
        }
        if (out_desc->dts_us >= 0)
        {
            out_desc->dts_us += timestamp_offset_us_;
        }
    }
    else
    {
        const int64_t synthesized_pts =
            (last_output_pts_us_ >= 0) ? (last_output_pts_us_ + frame_interval_us) : 0;
        out_desc->pts_us = synthesized_pts;
        out_desc->dts_us = synthesized_pts;
    }

    if (out_desc->pts_us >= 0)
    {
        out_desc->dts_us    = out_desc->pts_us;
        last_output_pts_us_ = out_desc->pts_us;
    }
    else if (out_desc->dts_us >= 0)
    {
        out_desc->pts_us    = out_desc->dts_us;
        last_output_pts_us_ = out_desc->dts_us;
    }
}
int FfmpegFileSource::ReadOnePacket(void* out_packet)
{
    if (!fmt_ctx_ || !out_packet)
    {
        return file_source_result::kFatal;
    }

    AVPacket* out = static_cast<AVPacket*>(out_packet);
    av_packet_unref(out);

    AVFormatContext* fmt_ctx      = static_cast<AVFormatContext*>(fmt_ctx_);
#if XIAOMAN_HAVE_FFMPEG_NEW_BSF
    AVBSFContext*    bsf_ctx      = static_cast<AVBSFContext*>(bsf_ctx_);
#elif XIAOMAN_HAVE_FFMPEG_LEGACY_BSF
    AVBitStreamFilterContext* bsf_ctx =
        static_cast<AVBitStreamFilterContext*>(bsf_ctx_);
    AVCodecContext* bsf_codec_ctx = static_cast<AVCodecContext*>(bsf_codec_ctx_);
#endif
    AVPacket*        demux_packet = static_cast<AVPacket*>(demux_packet_);
    if (!demux_packet)
    {
        return file_source_result::kFatal;
    }
    av_packet_unref(demux_packet);

    while (true)
    {
#if XIAOMAN_HAVE_FFMPEG_NEW_BSF
        if (bsf_ctx)
        {
            const int recv_ret = av_bsf_receive_packet(bsf_ctx, out);
            if (recv_ret == 0)
            {
                return file_source_result::kOk;
            }
            if (recv_ret != AVERROR(EAGAIN) && recv_ret != AVERROR_EOF)
            {
                std::cerr << "Failed to receive packet from bitstream filter" << std::endl;
                return file_source_result::kFatal;
            }
        }
#endif

        const int read_ret = av_read_frame(fmt_ctx, demux_packet);
        if (read_ret == AVERROR_EOF)
        {
            av_packet_unref(demux_packet);
            if (config_.loop && !IsRealtimeInput())
            {
                if (SeekToStart() != 0)
                {
                    return file_source_result::kFatal;
                }
                continue;
            }
            return file_source_result::kEof;
        }
        if (read_ret < 0)
        {
            av_packet_unref(demux_packet);
            std::cerr << "Failed to read media packet from FFmpeg input, ret=" << read_ret
                      << std::endl;
            return file_source_result::kFatal;
        }

        if (demux_packet->stream_index != video_stream_index_)
        {
            av_packet_unref(demux_packet);
            continue;
        }

#if XIAOMAN_HAVE_FFMPEG_NEW_BSF
        if (bsf_ctx)
        {
            const int send_ret = av_bsf_send_packet(bsf_ctx, demux_packet);
            av_packet_unref(demux_packet);
            if (send_ret < 0)
            {
                std::cerr << "Failed to send packet to bitstream filter" << std::endl;
                return file_source_result::kFatal;
            }
            continue;
        }
#elif XIAOMAN_HAVE_FFMPEG_LEGACY_BSF
        if (bsf_ctx)
        {
            uint8_t* filtered_data = nullptr;
            int      filtered_size = 0;
            const int filter_ret = av_bitstream_filter_filter(
                bsf_ctx,
                bsf_codec_ctx,
                nullptr,
                &filtered_data,
                &filtered_size,
                demux_packet->data,
                demux_packet->size,
                demux_packet->flags & AV_PKT_FLAG_KEY);
            if (filter_ret < 0 || !filtered_data || filtered_size <= 0)
            {
                av_packet_unref(demux_packet);
                std::cerr << "Failed to filter packet with legacy FFmpeg bitstream filter"
                          << std::endl;
                return file_source_result::kFatal;
            }

            if (av_new_packet(out, filtered_size) < 0)
            {
                if (filtered_data != demux_packet->data)
                {
                    av_free(filtered_data);
                }
                av_packet_unref(demux_packet);
                return file_source_result::kFatal;
            }
            std::memcpy(out->data, filtered_data, static_cast<size_t>(filtered_size));
            out->pts          = demux_packet->pts;
            out->dts          = demux_packet->dts;
            out->duration     = demux_packet->duration;
            out->pos          = demux_packet->pos;
            out->flags        = demux_packet->flags;
            out->stream_index = demux_packet->stream_index;

            if (filtered_data != demux_packet->data)
            {
                av_free(filtered_data);
            }
            av_packet_unref(demux_packet);
            return file_source_result::kOk;
        }
#endif

        av_packet_move_ref(out, demux_packet);
        return file_source_result::kOk;
    }
}
void FfmpegFileSource::ResetFrameDesc(FrameDesc* out_desc) const
{
    if (!out_desc)
    {
        return;
    }

    *out_desc                   = {};
    out_desc->index             = -1;
    out_desc->fd                = -1;
    out_desc->pts_us            = -1;
    out_desc->dts_us            = -1;
    out_desc->cpuSyncReadActive = false;
}

bool FfmpegFileSource::IsRealtimeInput() const
{
    return StartsWith(config_.input_url, "rtsp://") || StartsWith(config_.input_url, "rtmp://") ||
           StartsWith(config_.input_url, "http://") || StartsWith(config_.input_url, "https://") ||
           StartsWith(config_.input_url, "udp://") || StartsWith(config_.input_url, "tcp://");
}

int FfmpegFileSource::Read(FrameDesc* out_desc, std::shared_ptr<void>* out_owner)
{
    if (!out_desc || !out_owner)
    {
        return file_source_result::kFatal;
    }

    ResetFrameDesc(out_desc);
    out_owner->reset();

    AVPacket* packet = av_packet_alloc();
    if (!packet)
    {
        return file_source_result::kFatal;
    }

    const int read_ret = ReadOnePacket(packet);
    if (read_ret != file_source_result::kOk)
    {
        av_packet_free(&packet);
        return read_ret;
    }

    if (!packet->data || packet->size <= 0)
    {
        av_packet_free(&packet);
        return file_source_result::kFatal;
    }

    auto packet_owner = std::shared_ptr<void>(packet,
                                              [](void* raw)
                                              {
                                                  AVPacket* pkt = static_cast<AVPacket*>(raw);
                                                  av_packet_free(&pkt);
                                              });

    out_desc->base        = packet->data;
    out_desc->Length      = static_cast<size_t>(packet->size);
    out_desc->payloadSize = static_cast<size_t>(packet->size);
    out_desc->pts_us      = TsToUs(packet->pts, time_base_num_, time_base_den_);
    out_desc->dts_us      = TsToUs(packet->dts, time_base_num_, time_base_den_);
    if (out_desc->pts_us >= 0)
    {
        out_desc->dts_us = out_desc->pts_us;
    }
    else if (out_desc->dts_us >= 0)
    {
        out_desc->pts_us = out_desc->dts_us;
    }

    RebaseLocalFileTimestamp(out_desc);

    *out_owner = std::move(packet_owner);
    return file_source_result::kOk;
}

void FfmpegFileSource::Close()
{
    if (demux_packet_)
    {
        AVPacket* demux_packet = static_cast<AVPacket*>(demux_packet_);
        av_packet_free(&demux_packet);
        demux_packet_ = nullptr;
    }

    if (bsf_ctx_)
    {
#if XIAOMAN_HAVE_FFMPEG_NEW_BSF
        AVBSFContext* bsf_ctx = static_cast<AVBSFContext*>(bsf_ctx_);
        av_bsf_free(&bsf_ctx);
#elif XIAOMAN_HAVE_FFMPEG_LEGACY_BSF
        AVBitStreamFilterContext* bsf_ctx =
            static_cast<AVBitStreamFilterContext*>(bsf_ctx_);
        av_bitstream_filter_close(bsf_ctx);
#endif
        bsf_ctx_ = nullptr;
    }
    if (bsf_codec_ctx_)
    {
        AVCodecContext* codec_ctx = static_cast<AVCodecContext*>(bsf_codec_ctx_);
        avcodec_free_context(&codec_ctx);
        bsf_codec_ctx_ = nullptr;
    }

    if (fmt_ctx_)
    {
        AVFormatContext* fmt_ctx = static_cast<AVFormatContext*>(fmt_ctx_);
        avformat_close_input(&fmt_ctx);
        fmt_ctx_ = nullptr;
    }

    video_info_               = {};
    video_stream_index_       = -1;
    codec_id_                 = 0;
    time_base_num_            = 0;
    time_base_den_            = 0;
    pending_decoder_reset_    = false;
    pending_timestamp_rebase_ = false;
    timestamp_offset_us_      = 0;
    last_output_pts_us_       = -1;
}
