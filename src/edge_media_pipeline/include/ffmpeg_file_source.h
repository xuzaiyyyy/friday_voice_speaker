#ifndef FFMPEG_FILE_SOURCE_H
#define FFMPEG_FILE_SOURCE_H

#include <memory>
#include <string>

#include <rockchip/rk_mpi.h>

#include "pubDataType.h"

namespace file_source_result
{
inline constexpr int kOk    = 0;
inline constexpr int kEof   = 1;
inline constexpr int kFatal = -1;
}  // namespace file_source_result

struct FfmpegFileSourceConfig
{
    std::string input_url;
    bool        loop = true;
    bool        prefer_tcp_for_rtsp = true;
    int         open_timeout_ms     = 10000;
    int         read_timeout_ms     = 10000;
    int         recover_interval_ms = 2000;
    int         max_recover_count   = -1;
};

struct SourceVideoInfo
{
    uint32_t      width       = 0;
    uint32_t      height      = 0;
    uint32_t      fps         = 30;
    MppCodingType coding_type = MPP_VIDEO_CodingAVC;
    bool          valid       = false;
};

class FfmpegFileSource
{
   public:
    FfmpegFileSource();
    ~FfmpegFileSource();

    int                   Open(const FfmpegFileSourceConfig& config);
    int                   Read(FrameDesc* out_desc, std::shared_ptr<void>* out_owner);
    void                  Close();
    bool                  IsRealtimeInput() const;
    bool                  ConsumeDecoderResetRequest();
    const SourceVideoInfo& GetVideoInfo() const { return video_info_; }

   private:
    int  CreateBitstreamFilter();
    int  SeekToStart();
    int  ReadOnePacket(void* out_packet);
    void ResetFrameDesc(FrameDesc* out_desc) const;
    void RebaseLocalFileTimestamp(FrameDesc* out_desc);
    FfmpegFileSourceConfig config_     = {};
    SourceVideoInfo        video_info_ = {};
    void*                  fmt_ctx_    = nullptr;
    void*                  bsf_ctx_    = nullptr;
    void*                  bsf_codec_ctx_     = nullptr;
    void*                  demux_packet_       = nullptr;
    int                    video_stream_index_ = -1;
    int                    codec_id_           = 0;
    int64_t                time_base_num_      = 0;
    int64_t                time_base_den_      = 0;
    bool                   pending_decoder_reset_    = false;
    bool                   pending_timestamp_rebase_ = false;
    int64_t                timestamp_offset_us_      = 0;
    int64_t                last_output_pts_us_       = -1;
};

#endif  // FFMPEG_FILE_SOURCE_H


