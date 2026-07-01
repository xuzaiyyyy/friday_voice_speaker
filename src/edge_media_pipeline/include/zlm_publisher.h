#ifndef ZLM_PUBLISHER_H
#define ZLM_PUBLISHER_H

#include <atomic>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "mk_mediakit.h"
#include "rkmppenc.h"

struct ZlmPublishConfig
{
    uint16_t    rtsp_port         = 8554;
    uint16_t    http_port         = 8000;
    uint16_t    rtc_port          = 8001;
    bool        enable_audio      = false;
    uint32_t    audio_sample_rate = 8000;
    uint32_t    audio_channels    = 1;
    uint32_t    audio_sample_bit  = 16;
    std::string vhost             = "__defaultVhost__";
    std::string app               = "live";
    std::string stream            = "camera";
};

class ZlmPublisher
{
   public:
    ZlmPublisher();
    ~ZlmPublisher();

    int         Init(const ZlmPublishConfig& cfg = {});
    void        SetExpectedFps(uint32_t fps);
    int         InputPacketChunk(const EncPacketView& pkt);
    int         InputAudioFrame(const void* data, size_t len, uint64_t dts_ms);
    void        Close();
    std::string GetRtspUrl() const;
    std::string GetWebRtcApiUrl() const;
    std::string GetWebRtcPlayerPageUrl() const;
    uint64_t    GetOutputFrameCount() const
    {
        return OutputFrameCount_.load(std::memory_order_relaxed);
    }
    uint64_t GetTimestampFallbackCount() const
    {
        return TimestampFallbackCount_.load(std::memory_order_relaxed);
    }
    bool HasVideoTimeline() const
    {
        return VideoTimelineReady_.load(std::memory_order_relaxed);
    }
    uint64_t GetCurrentVideoDtsMs() const
    {
        return VideoTimelineDtsMs_.load(std::memory_order_relaxed);
    }
    uint64_t GetEstimatedVideoDtsMs() const;

   private:
    struct NaluRange
    {
        const uint8_t* data = nullptr;
        size_t         len  = 0;
    };

    bool NormalizeTimestamp(const EncPacketView& pkt, uint64_t* out_dts_ms, uint64_t* out_pts_ms);
    bool IsAnnexBStartCode(const uint8_t* data, size_t len) const;
    bool FindAnnexBPrefix(const uint8_t* data, size_t len, size_t pos, size_t* prefix_len) const;
    bool SplitAnnexBNalus(const uint8_t* data, size_t len, std::vector<NaluRange>* out_nalus) const;
    int  AcquireSharedServer(const ZlmPublishConfig& cfg);
    void ReleaseSharedServer();

    mk_media         Media_         = nullptr;
    ZlmPublishConfig PublishConfig_ = {};
    uint16_t         RtspPort_      = 0;
    uint16_t         HttpPort_      = 0;
    uint16_t         RtcPort_       = 0;

    uint64_t              LastDtsUs_ = 0;
    uint64_t              LastPtsUs_ = 0;
    uint64_t              LastDtsMs_ = 0;
    uint64_t              LastPtsMs_ = 0;
    std::atomic<uint64_t> VideoTimelineDtsMs_{0};
    std::atomic<uint64_t> VideoTimelineTickMs_{0};
    std::atomic<uint64_t> OutputFrameCount_{0};
    std::atomic<uint64_t> TimestampFallbackCount_{0};
    std::atomic<bool>     VideoTimelineReady_{false};
    uint64_t              FrameIntervalUs_  = 1000000 / 30;
    bool                  Initialized_      = false;
    bool                  SharedServerHeld_ = false;
    bool                  HasLastTimestamp_ = false;
    std::mutex            MediaInputMutex_;
};

#endif  // ZLM_PUBLISHER_H


