#ifndef ALSA_AUDIO_CAPTURE_H
#define ALSA_AUDIO_CAPTURE_H

#include <alsa/asoundlib.h>

#include <cstdint>
#include <string>
#include <vector>

class AlsaAudioCapture
{
   public:
    struct Config
    {
        std::string       device        = "";
        uint32_t          sample_rate   = 8000;
        uint32_t          channels      = 1;
        uint32_t          sample_bit    = 16;
        snd_pcm_uframes_t period_frames = 160;
    };

    struct DeviceInfo
    {
        std::string name;
        std::string desc;
        std::string ioid;
    };

    AlsaAudioCapture() = default;
    explicit AlsaAudioCapture(Config config);
    ~AlsaAudioCapture();

    static std::vector<DeviceInfo> EnumerateCaptureDevices();
    static std::string             AutoSelectCaptureDevice(std::string* summary = nullptr);

    int  Init();
    void Close();
    bool IsReady() const { return PcmHandle_ != nullptr; }
    const Config& GetConfig() const { return Config_; }
    int64_t GetCaptureDelayMs() const;
    int CapturePcmFrame(std::vector<int16_t>* pcm_samples);

   private:
    int CaptureFrames(int16_t* dst, snd_pcm_uframes_t total_frames);

    Config     Config_{};
    snd_pcm_t* PcmHandle_           = nullptr;
    bool       UseMmap_             = false;
    int64_t    ConfiguredLatencyMs_ = 0;
};

#endif  // ALSA_AUDIO_CAPTURE_H
