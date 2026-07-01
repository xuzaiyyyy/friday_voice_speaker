#include "alsa_audio_capture.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <iostream>

namespace
{
constexpr unsigned int kCaptureWaitTimeoutMs = 100;
constexpr unsigned int kCaptureLatencyUs     = 80000;

int64_t FramesToMs(snd_pcm_sframes_t frames, uint32_t sample_rate)
{
    if (frames <= 0 || sample_rate == 0)
    {
        return 0;
    }
    return static_cast<int64_t>((static_cast<uint64_t>(frames) * 1000ULL + sample_rate / 2) /
                                sample_rate);
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string NormalizeDeviceDesc(const char* desc)
{
    if (!desc)
    {
        return "";
    }
    std::string value(desc);
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

int DevicePriority(const AlsaAudioCapture::DeviceInfo& info)
{
    const std::string text = ToLowerAscii(info.name + " " + info.desc);
    int               score = 0;
    if (text.find("webcamera") != std::string::npos || text.find("webcam") != std::string::npos)
    {
        score += 120;
    }
    if (text.find("usb audio") != std::string::npos || text.find("usb") != std::string::npos)
    {
        score += 100;
    }
    if (text.find("microphone") != std::string::npos || text.find("mic") != std::string::npos)
    {
        score += 80;
    }
    if (text.find("camera") != std::string::npos)
    {
        score += 60;
    }
    if (text.find("capture") != std::string::npos || text.find("input") != std::string::npos)
    {
        score += 20;
    }
    return score;
}
}  // namespace

AlsaAudioCapture::AlsaAudioCapture(Config config) : Config_(std::move(config)) {}

AlsaAudioCapture::~AlsaAudioCapture()
{
    Close();
}

std::vector<AlsaAudioCapture::DeviceInfo> AlsaAudioCapture::EnumerateCaptureDevices()
{
    std::vector<DeviceInfo> devices;
    void**                  hints = nullptr;
    const int               ret   = snd_device_name_hint(-1, "pcm", &hints);
    if (ret < 0 || !hints)
    {
        return devices;
    }

    for (void** item = hints; *item; ++item)
    {
        char* name = snd_device_name_get_hint(*item, "NAME");
        char* desc = snd_device_name_get_hint(*item, "DESC");
        char* ioid = snd_device_name_get_hint(*item, "IOID");

        const bool is_capture = (!ioid || std::strcmp(ioid, "Input") == 0);
        if (name && is_capture)
        {
            DeviceInfo info;
            info.name = name;
            info.desc = NormalizeDeviceDesc(desc);
            info.ioid = ioid ? ioid : "";
            devices.emplace_back(std::move(info));
        }

        if (name)
        {
            free(name);
        }
        if (desc)
        {
            free(desc);
        }
        if (ioid)
        {
            free(ioid);
        }
    }

    snd_device_name_free_hint(hints);
    return devices;
}

std::string AlsaAudioCapture::AutoSelectCaptureDevice(std::string* summary)
{
    const auto devices = EnumerateCaptureDevices();
    if (devices.empty())
    {
        if (summary)
        {
            *summary = "自动选择未发现可用采集设备，回退到 default";
        }
        return "default";
    }

    const DeviceInfo* best = &devices.front();
    int               best_score = DevicePriority(*best);
    for (const auto& device : devices)
    {
        const int score = DevicePriority(device);
        if (score > best_score)
        {
            best       = &device;
            best_score = score;
        }
    }

    if (summary)
    {
        *summary = "自动选择 ALSA 采集设备: " + best->name;
        if (!best->desc.empty())
        {
            *summary += " (" + best->desc + ")";
        }
    }
    return best->name;
}

int AlsaAudioCapture::Init()
{
    Close();

    int ret = snd_pcm_open(&PcmHandle_, Config_.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (ret < 0)
    {
        std::cerr << "[audio] 打开 ALSA 采集设备失败: " << snd_strerror(ret)
                  << ", device=" << Config_.device << std::endl;
        PcmHandle_ = nullptr;
        return -1;
    }

    UseMmap_ = true;
    ret      = snd_pcm_set_params(PcmHandle_, SND_PCM_FORMAT_S16_LE,
                                  SND_PCM_ACCESS_MMAP_INTERLEAVED, Config_.channels,
                                  Config_.sample_rate, 1, kCaptureLatencyUs);
    if (ret < 0)
    {
        snd_pcm_close(PcmHandle_);
        PcmHandle_ = nullptr;

        ret = snd_pcm_open(&PcmHandle_, Config_.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
        if (ret < 0)
        {
            std::cerr << "[audio] 重新打开 ALSA 采集设备失败: " << snd_strerror(ret)
                      << std::endl;
            PcmHandle_ = nullptr;
            return -1;
        }

        UseMmap_ = false;
        ret      = snd_pcm_set_params(PcmHandle_, SND_PCM_FORMAT_S16_LE,
                                      SND_PCM_ACCESS_RW_INTERLEAVED, Config_.channels,
                                      Config_.sample_rate, 1, kCaptureLatencyUs);
        if (ret < 0)
        {
            std::cerr << "[audio] 配置 ALSA 采集参数失败: " << snd_strerror(ret) << std::endl;
            Close();
            return -1;
        }
    }

    ret = snd_pcm_prepare(PcmHandle_);
    if (ret < 0)
    {
        std::cerr << "[audio] 预热 ALSA 采集设备失败: " << snd_strerror(ret) << std::endl;
        Close();
        return -1;
    }

    snd_pcm_uframes_t buffer_frames = 0;
    snd_pcm_uframes_t period_frames = 0;
    if (snd_pcm_get_params(PcmHandle_, &buffer_frames, &period_frames) == 0)
    {
        ConfiguredLatencyMs_ = FramesToMs(static_cast<snd_pcm_sframes_t>(buffer_frames), Config_.sample_rate);
    }
    else
    {
        ConfiguredLatencyMs_ = static_cast<int64_t>(kCaptureLatencyUs / 1000);
    }

    if (UseMmap_)
    {
        ret = snd_pcm_start(PcmHandle_);
        if (ret < 0)
        {
            std::cerr << "[audio] 启动 ALSA mmap 采集失败: " << snd_strerror(ret) << std::endl;
            Close();
            return -1;
        }
    }

    return 0;
}

void AlsaAudioCapture::Close()
{
    if (PcmHandle_)
    {
        snd_pcm_drop(PcmHandle_);
        snd_pcm_close(PcmHandle_);
        PcmHandle_ = nullptr;
    }
    ConfiguredLatencyMs_ = 0;
}

int64_t AlsaAudioCapture::GetCaptureDelayMs() const
{
    if (!PcmHandle_)
    {
        return ConfiguredLatencyMs_;
    }

    snd_pcm_sframes_t delay_frames = 0;
    const int         ret          = snd_pcm_delay(PcmHandle_, &delay_frames);
    if (ret < 0)
    {
        return ConfiguredLatencyMs_;
    }

    const int64_t delay_ms = FramesToMs(delay_frames, Config_.sample_rate);
    return delay_ms > 0 ? delay_ms : ConfiguredLatencyMs_;
}

int AlsaAudioCapture::CapturePcmFrame(std::vector<int16_t>* pcm_samples)
{
    if (!pcm_samples || !PcmHandle_)
    {
        return -1;
    }

    const size_t sample_count = static_cast<size_t>(Config_.period_frames) * Config_.channels;
    pcm_samples->assign(sample_count, 0);
    return CaptureFrames(pcm_samples->data(), Config_.period_frames);
}

int AlsaAudioCapture::CaptureFrames(int16_t* dst, snd_pcm_uframes_t total_frames)
{
    if (!dst || !PcmHandle_)
    {
        return -1;
    }

    snd_pcm_uframes_t captured      = 0;
    uint64_t          wait_timeouts = 0;
    while (captured < total_frames)
    {
        const int wait_ret = snd_pcm_wait(PcmHandle_, kCaptureWaitTimeoutMs);
        if (wait_ret == 0)
        {
            ++wait_timeouts;
            if (wait_timeouts <= 5 || (wait_timeouts % 50) == 0)
            {
                std::cerr << "[audio] 等待 ALSA 采集数据超时，第" << wait_timeouts
                          << "次, device=" << Config_.device
                          << ", mode=" << (UseMmap_ ? "mmap" : "rw") << std::endl;
            }
            continue;
        }
        if (wait_ret < 0)
        {
            const int recover_ret = snd_pcm_recover(PcmHandle_, wait_ret, 1);
            if (recover_ret < 0)
            {
                std::cerr << "[audio] 等待 ALSA 采集数据失败: " << snd_strerror(wait_ret)
                          << std::endl;
                return -1;
            }
            continue;
        }

        const snd_pcm_uframes_t need = total_frames - captured;
        snd_pcm_sframes_t       read_ret;
        if (UseMmap_)
        {
            read_ret = snd_pcm_mmap_readi(
                PcmHandle_,
                dst + captured * static_cast<snd_pcm_uframes_t>(Config_.channels),
                need);
        }
        else
        {
            read_ret = snd_pcm_readi(PcmHandle_,
                                     dst + captured * static_cast<snd_pcm_uframes_t>(Config_.channels),
                                     need);
        }

        if (read_ret == -EAGAIN)
        {
            continue;
        }
        if (read_ret < 0)
        {
            const int recover_ret = snd_pcm_recover(PcmHandle_, static_cast<int>(read_ret), 1);
            if (recover_ret < 0)
            {
                std::cerr << "[audio] ALSA 采集失败: " << snd_strerror(static_cast<int>(read_ret))
                          << std::endl;
                return -1;
            }
            continue;
        }

        captured += static_cast<snd_pcm_uframes_t>(read_ret);
    }

    return 0;
}
