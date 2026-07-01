#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <sstream>
#include <cmath>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_map>
#include <linux/dma-buf.h>
#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#else
#include "linux_dma_heap_compat.h"
#endif
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <iomanip>

#include <im2d.h>
#include <im2d_buffer.h>
#include <opencv2/opencv.hpp>

#include "alsa_audio_capture.h"
#include "ffmpeg_file_source.h"
#include "rkmppdec.h"
#include "rkmppenc.h"
#include "rknn_instance.h"
#include "rkrga.h"
#include "v4l2Camera.h"
#include "yolo11.h"
#include "zlm_publisher.h"

using namespace cv;

namespace
{
// 默认摄像头设备节点与模型路径
constexpr const char* kDefaultCameraDevice = "/dev/video0";
constexpr const char* kDefaultModelPath =
    "/home/orangepi/cpp/friday_voice_speaker/models/yolov6n_85.rknn";
constexpr const char* kDefaultSecondInputPath =
    "/home/orangepi/cpp/friday_voice_speaker/src/edge_media_pipeline/test.mp4";
constexpr const char* kDefaultAudioDevice       = "";
constexpr uint32_t    kDefaultAudioSampleRate   = 8000;
constexpr uint32_t    kDefaultAudioChannels     = 1;
constexpr uint32_t    kDefaultAudioSampleBit    = 16;
constexpr size_t      kDefaultAudioPeriodFrames = 160;

// AI 推理阈值
constexpr float   kConfThresh          = 0.25f;
constexpr float   kNmsThresh           = 0.45f;
constexpr bool    kDrawDetectionText   = false;
constexpr int     kDrawBoxThickness    = 2;
constexpr auto    kPersonInferInterval = std::chrono::milliseconds(800);
constexpr uint8_t kYellowY             = 226;
constexpr uint8_t kYellowU             = 1;
constexpr uint8_t kYellowV             = 149;

// 线程重试休眠时间与队列深度配置
constexpr auto     kRetryBackoffSleep                 = std::chrono::milliseconds(2);
constexpr auto     kEncodeNoPacketSleep               = std::chrono::milliseconds(8);
constexpr size_t   kDefaultQueueDepth                 = 6;
constexpr int64_t  kDefaultAudioSyncOffsetMs          = 0;
constexpr int64_t  kAudioSyncToleranceMs              = 14;
constexpr int64_t  kAudioSyncMaxCorrectionStepMs      = 2;
constexpr int64_t  kAudioSyncFastCatchupThresholdMs   = 48;
constexpr int64_t  kAudioSyncHardCatchupThresholdMs   = 72;
constexpr int64_t  kAudioSyncFastCorrectionStepMs     = 6;
constexpr uint64_t kAudioSyncFastCatchupConfirmFrames = 2;
constexpr uint64_t kAudioSyncZeroCrossHoldFrames      = 2;
constexpr uint64_t kAudioSyncCorrectionInterval       = 4;
constexpr int64_t  kAudioSyncMaxCaptureDelayMs        = 200;
constexpr size_t   kEncodeBusyRetryLimit              = 6;
constexpr size_t   kEncodeBusyDropFatalThreshold      = 120;
constexpr size_t   kInferReuseQueueDepthThreshold     = 4;
constexpr uint32_t kInferCacheReuseFrames             = 60;

volatile sig_atomic_t g_should_exit = 0;

// 信号处理函数，用于安全捕获 Ctrl+C 退出程序
void SignalHandler(int signum)
{
    if (signum == SIGINT)
    {
        // 第一次 Ctrl+C 触发优雅退出；第二次 Ctrl+C 直接强制退出，防止卡死。
        if (g_should_exit)
        {
            _exit(130);
        }
        g_should_exit = 1;
    }
}

// 命令行参数解析结构体
struct ProgramOptions
{
    std::string camera_device = kDefaultCameraDevice;
    std::string model_path    = kDefaultModelPath;
    std::string input_url     = kDefaultSecondInputPath;
    std::string zlm_profile = "lan";
    std::string zlm_config_path;
    std::string audio_device         = kDefaultAudioDevice;
    int         audio_sync_offset_ms = static_cast<int>(kDefaultAudioSyncOffsetMs);
    bool        file_loop            = true;
    bool        enable_audio         = true;
    bool        enable_inference     = true;
    bool        rknn_cpu_input =
        false;  // 默认走 RKNN zero-copy 输入；兼容模式仅在手动传 --rknn-cpu-input 时启用
};

ProgramOptions ParseArgs(int argc, char** argv)
{
    ProgramOptions options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--camera" && i + 1 < argc)
        {
            options.camera_device = argv[++i];
        }
        else if (arg == "--model" && i + 1 < argc)
        {
            options.model_path = argv[++i];
        }
        else if (arg == "--file" && i + 1 < argc)
        {
            options.input_url = argv[++i];
        }
        else if (arg == "--input" && i + 1 < argc)
        {
            options.input_url = argv[++i];
        }
        else if (arg == "--rtsp" && i + 1 < argc)
        {
            options.input_url = argv[++i];
        }
        else if (arg == "--single-link" || arg == "--no-second-link")
        {
            options.input_url.clear();
        }
        else if (arg == "--lan-mode")
        {
            options.zlm_profile = "lan";
        }
        else if (arg == "--wan-mode")
        {
            options.zlm_profile = "wan";
        }
        else if (arg == "--zlm-profile" && i + 1 < argc)
        {
            options.zlm_profile = argv[++i];
        }
        else if (arg == "--zlm-config" && i + 1 < argc)
        {
            options.zlm_config_path = argv[++i];
        }
        else if (arg == "--audio-device" && i + 1 < argc)
        {
            options.audio_device = argv[++i];
        }
        else if (arg == "--audio-sync-offset-ms" && i + 1 < argc)
        {
            options.audio_sync_offset_ms = std::atoi(argv[++i]);
        }
        else if (arg == "--no-audio")
        {
            options.enable_audio = false;
        }
        else if (arg == "--no-infer" || arg == "--no-rknn")
        {
            options.enable_inference = false;
        }
        else if (arg == "--rknn-cpu-input")
        {
            options.rknn_cpu_input = true;
        }
        else if (arg == "--rknn-zero-copy")
        {
            options.rknn_cpu_input = false;
        }
        else if (arg == "--no-file-loop")
        {
            options.file_loop = false;
        }
    }
    return options;
}

bool FileExistsLocal(const std::string& path)
{
    if (path.empty())
    {
        return false;
    }
    std::ifstream file(path);
    return file.good();
}

std::string ResolveZlmConfigPath(const ProgramOptions& options)
{
    if (!options.zlm_config_path.empty())
    {
        return options.zlm_config_path;
    }

    if (options.zlm_profile == "lan")
    {
        if (FileExistsLocal("./config.lan.ini"))
        {
            return "./config.lan.ini";
        }
        if (FileExistsLocal("../config.lan.ini"))
        {
            return "../config.lan.ini";
        }
    }
    else
    {
        if (FileExistsLocal("./config.wan.ini"))
        {
            return "./config.wan.ini";
        }
        if (FileExistsLocal("../config.wan.ini"))
        {
            return "../config.wan.ini";
        }
    }

    return {};
}

void ApplyZlmConfigSelection(const ProgramOptions& options)
{
    const std::string config_path = ResolveZlmConfigPath(options);
    if (!config_path.empty())
    {
        ::setenv("RKMEDIA_ZLM_CONFIG", config_path.c_str(), 1);
        std::cout << "[ZLM] 启动模式: " << options.zlm_profile << ", 配置文件: " << config_path
                  << std::endl;
    }
    else
    {
        ::unsetenv("RKMEDIA_ZLM_CONFIG");
        std::cout << "[ZLM] 启动模式: " << options.zlm_profile << ", 配置文件: <fallback>"
                  << std::endl;
    }
}

uint8_t SearchALawSegment(int pcm_val)
{
    static constexpr int kSegmentEnds[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    for (uint8_t i = 0; i < 8; ++i)
    {
        if (pcm_val <= kSegmentEnds[i])
        {
            return i;
        }
    }
    return 8;
}

uint8_t LinearToALaw(int16_t sample)
{
    int     pcm_val = static_cast<int>(sample) >> 3;
    uint8_t mask    = 0xD5;
    if (pcm_val < 0)
    {
        mask    = 0x55;
        pcm_val = -pcm_val - 1;
    }

    const uint8_t seg = SearchALawSegment(pcm_val);
    if (seg >= 8)
    {
        return static_cast<uint8_t>(0x7F ^ mask);
    }

    uint8_t aval = static_cast<uint8_t>(seg << 4);
    if (seg < 2)
    {
        aval |= static_cast<uint8_t>((pcm_val >> 1) & 0x0F);
    }
    else
    {
        aval |= static_cast<uint8_t>((pcm_val >> seg) & 0x0F);
    }
    return static_cast<uint8_t>(aval ^ mask);
}

void EncodePcmToG711A(const int16_t* samples, size_t sample_count, std::vector<uint8_t>* encoded)
{
    if (!encoded)
    {
        return;
    }
    encoded->clear();
    if (!samples || sample_count == 0)
    {
        return;
    }

    encoded->resize(sample_count);
    for (size_t i = 0; i < sample_count; ++i)
    {
        (*encoded)[i] = LinearToALaw(samples[i]);
    }
}

struct AudioDenoiseState
{
    float  prev_input      = 0.0f;
    float  prev_output     = 0.0f;
    double noise_floor_abs = 12.0;
    bool   initialized     = false;
};

int16_t ClampToInt16(float value)
{
    if (value > 32767.0f)
    {
        return 32767;
    }
    if (value < -32768.0f)
    {
        return -32768;
    }
    return static_cast<int16_t>(std::lrint(value));
}

void DenoisePcmForSpeech(std::vector<int16_t>* samples, uint32_t channels, AudioDenoiseState* state)
{
    if (!samples || !state || samples->empty() || channels != 1)
    {
        return;
    }

    static constexpr float  kDcBlockAlpha             = 0.995f;
    static constexpr double kNoiseFloorInitAbs        = 10.0;
    static constexpr double kNoiseFloorUpdateRatio    = 0.04;
    static constexpr double kNoiseFrameLearnThreshold = 2.2;
    static constexpr double kHardGateFloor            = 18.0;
    static constexpr double kSoftGateFloor            = 30.0;

    std::vector<float> filtered(samples->size(), 0.0f);
    double             abs_sum  = 0.0;
    double             energy   = 0.0;
    double             peak_abs = 0.0;
    float              prev_x   = state->prev_input;
    float              prev_y   = state->prev_output;

    for (size_t i = 0; i < samples->size(); ++i)
    {
        const float x = static_cast<float>((*samples)[i]);
        const float y = x - prev_x + kDcBlockAlpha * prev_y;
        prev_x        = x;
        prev_y        = y;
        filtered[i]   = y;

        const double abs_y = std::fabs(static_cast<double>(y));
        abs_sum += abs_y;
        energy += static_cast<double>(y) * static_cast<double>(y);
        peak_abs = std::max(peak_abs, abs_y);
    }

    state->prev_input  = prev_x;
    state->prev_output = prev_y;

    const double sample_count = static_cast<double>(samples->size());
    const double abs_mean     = abs_sum / sample_count;
    const double rms          = std::sqrt(energy / sample_count);

    if (!state->initialized)
    {
        state->noise_floor_abs = std::max(abs_mean, kNoiseFloorInitAbs);
        state->initialized     = true;
    }
    else if (rms < state->noise_floor_abs * kNoiseFrameLearnThreshold)
    {
        state->noise_floor_abs = state->noise_floor_abs * (1.0 - kNoiseFloorUpdateRatio) +
                                 abs_mean * kNoiseFloorUpdateRatio;
    }

    const double hard_gate    = std::max(state->noise_floor_abs * 1.8, kHardGateFloor);
    const double soft_gate    = std::max(state->noise_floor_abs * 2.8, kSoftGateFloor);
    const bool   hard_silence = (rms < hard_gate) && (peak_abs < hard_gate * 2.2);
    const bool   soft_silence = !hard_silence && (rms < soft_gate) && (peak_abs < soft_gate * 2.4);
    const float  gain         = hard_silence ? 0.0f : (soft_silence ? 0.35f : 1.0f);
    const float  zero_threshold = static_cast<float>(std::max(state->noise_floor_abs * 1.2, 6.0));

    for (size_t i = 0; i < filtered.size(); ++i)
    {
        float value = filtered[i] * gain;
        if (std::fabs(value) < zero_threshold)
        {
            value = 0.0f;
        }
        (*samples)[i] = ClampToInt16(value);
    }
}

struct WindowMetricStats
{
    uint64_t count   = 0;
    double   mean_ms = 0.0;
    double   p90_ms  = 0.0;
    double   max_ms  = 0.0;
};

class WindowMetricCollector
{
   public:
    void AddSample(double value_ms)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.push_back(value_ms);
    }

    WindowMetricStats ConsumeAndReset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WindowMetricStats           stats;
        if (samples_.empty())
        {
            return stats;
        }

        stats.count                = static_cast<uint64_t>(samples_.size());
        std::vector<double> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());

        double sum = 0.0;
        for (double value : sorted)
        {
            sum += value;
        }

        stats.mean_ms          = sum / static_cast<double>(sorted.size());
        stats.max_ms           = sorted.back();
        const size_t p90_index = static_cast<size_t>(std::ceil(sorted.size() * 0.9)) - 1;
        stats.p90_ms           = sorted[std::min(p90_index, sorted.size() - 1)];
        samples_.clear();
        return stats;
    }

   private:
    std::mutex          mutex_;
    std::vector<double> samples_;
};

struct PipelinePerfSnapshot
{
    std::string       name;
    uint64_t          source_frames       = 0;
    uint64_t          decode_failures     = 0;
    uint64_t          infer_failures      = 0;
    uint64_t          enc_push_failures   = 0;
    uint64_t          encoded_queue_drops = 0;
    uint64_t          decoded_queue_drops = 0;
    uint64_t          encode_queue_drops  = 0;
    uint64_t          infer_reuse_frames  = 0;
    uint64_t          enc_busy_drops      = 0;
    WindowMetricStats dec_ms;
    WindowMetricStats rga_ms;
    WindowMetricStats infer_ms;
    WindowMetricStats enc_in_ms;
    WindowMetricStats av_gap_abs_ms;
    size_t            encoded_q_depth        = 0;
    size_t            decoded_q_depth        = 0;
    size_t            encode_q_depth         = 0;
    uint64_t          output_frames_total    = 0;
    uint64_t          timestamp_fixups_total = 0;
    int64_t           av_gap_ms_current      = 0;
    bool              av_sync_ready          = false;
    bool              fatal                  = false;
    bool              finished               = false;
};

struct SystemPerfSnapshot
{
    double   cpu_total_pct = std::numeric_limits<double>::quiet_NaN();
    double   proc_cpu_pct  = std::numeric_limits<double>::quiet_NaN();
    uint64_t rss_kb        = 0;
    uint64_t vmhwm_kb      = 0;
    double   cpu_temp_c    = std::numeric_limits<double>::quiet_NaN();
};

std::string FormatDoubleValue(double value, int precision = 2)
{
    if (!std::isfinite(value))
    {
        return "na";
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

std::string FormatStageStats(const WindowMetricStats& stats)
{
    if (stats.count == 0)
    {
        return "na";
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << stats.mean_ms << "/" << stats.p90_ms << "/"
        << stats.max_ms;
    return oss.str();
}

template <typename T>
T ConsumeCounter(std::atomic<T>* counter)
{
    return counter ? counter->exchange(0, std::memory_order_relaxed) : T{};
}

#ifdef __linux__
struct CpuSample
{
    uint64_t total_jiffies = 0;
    uint64_t idle_jiffies  = 0;
    uint64_t proc_jiffies  = 0;
    bool     valid         = false;
};

bool ReadSystemCpuJiffies(uint64_t* out_total, uint64_t* out_idle)
{
    if (!out_total || !out_idle)
    {
        return false;
    }

    std::ifstream file("/proc/stat");
    if (!file.is_open())
    {
        return false;
    }

    std::string cpu_tag;
    uint64_t    user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
    uint64_t    irq = 0, softirq = 0, steal = 0, guest = 0, guest_nice = 0;
    if (!(file >> cpu_tag >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >>
          guest >> guest_nice))
    {
        return false;
    }

    *out_idle  = idle + iowait;
    *out_total = user + nice + system + idle + iowait + irq + softirq + steal + guest + guest_nice;
    return cpu_tag == "cpu";
}

bool ReadProcessCpuJiffies(uint64_t* out_proc_jiffies)
{
    if (!out_proc_jiffies)
    {
        return false;
    }

    std::ifstream file("/proc/self/stat");
    if (!file.is_open())
    {
        return false;
    }

    std::string line;
    std::getline(file, line);
    const size_t rparen = line.rfind(')');
    if (rparen == std::string::npos || rparen + 2 >= line.size())
    {
        return false;
    }

    std::istringstream       iss(line.substr(rparen + 2));
    std::vector<std::string> fields;
    std::string              token;
    while (iss >> token)
    {
        fields.push_back(token);
    }
    if (fields.size() <= 12)
    {
        return false;
    }

    const uint64_t utime = std::strtoull(fields[11].c_str(), nullptr, 10);
    const uint64_t stime = std::strtoull(fields[12].c_str(), nullptr, 10);
    *out_proc_jiffies    = utime + stime;
    return true;
}

bool ReadStatusValueKb(const char* key, uint64_t* out_value)
{
    if (!key || !out_value)
    {
        return false;
    }

    std::ifstream file("/proc/self/status");
    if (!file.is_open())
    {
        return false;
    }

    std::string       line;
    const std::string prefix(key);
    while (std::getline(file, line))
    {
        if (line.compare(0, prefix.size(), prefix) == 0)
        {
            std::istringstream iss(line.substr(prefix.size()));
            uint64_t           value = 0;
            if (iss >> value)
            {
                *out_value = value;
                return true;
            }
        }
    }
    return false;
}

double ReadCpuTemperatureC()
{
    const char* candidates[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp",
        "/sys/class/thermal/thermal_zone2/temp",
    };

    for (const char* path : candidates)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            continue;
        }

        double raw_temp = 0.0;
        if (file >> raw_temp)
        {
            return raw_temp / 1000.0;
        }
    }

    return std::numeric_limits<double>::quiet_NaN();
}

class SystemPerfMonitor
{
   public:
    SystemPerfMonitor()
    {
        last_tp_ = std::chrono::steady_clock::now();
        last_    = ReadSample();
        clk_tck_ = sysconf(_SC_CLK_TCK);
        if (clk_tck_ <= 0)
        {
            clk_tck_ = 100;
        }
    }

    SystemPerfSnapshot Sample()
    {
        SystemPerfSnapshot snapshot;
        const auto         now_tp  = std::chrono::steady_clock::now();
        const CpuSample    current = ReadSample();
        const double       elapsed = std::chrono::duration<double>(now_tp - last_tp_).count();

        if (elapsed > 0.0 && last_.valid && current.valid)
        {
            const uint64_t total_delta = current.total_jiffies - last_.total_jiffies;
            const uint64_t idle_delta  = current.idle_jiffies - last_.idle_jiffies;
            const uint64_t proc_delta  = current.proc_jiffies - last_.proc_jiffies;
            if (total_delta > 0)
            {
                snapshot.cpu_total_pct = 100.0 * static_cast<double>(total_delta - idle_delta) /
                                         static_cast<double>(total_delta);
            }
            snapshot.proc_cpu_pct =
                100.0 * (static_cast<double>(proc_delta) / static_cast<double>(clk_tck_)) / elapsed;
        }

        (void)ReadStatusValueKb("VmRSS:", &snapshot.rss_kb);
        (void)ReadStatusValueKb("VmHWM:", &snapshot.vmhwm_kb);
        snapshot.cpu_temp_c = ReadCpuTemperatureC();

        last_tp_ = now_tp;
        last_    = current;
        return snapshot;
    }

   private:
    CpuSample ReadSample() const
    {
        CpuSample sample;
        sample.valid = ReadSystemCpuJiffies(&sample.total_jiffies, &sample.idle_jiffies) &&
                       ReadProcessCpuJiffies(&sample.proc_jiffies);
        return sample;
    }

    CpuSample                             last_    = {};
    std::chrono::steady_clock::time_point last_tp_ = {};
    long                                  clk_tck_ = 100;
};
#else
class SystemPerfMonitor
{
   public:
    SystemPerfSnapshot Sample() const { return {}; }
};
#endif

// =========================================================================
// 核心基础设施：带有界限的防死锁队列
// =========================================================================
template <typename T>
class BoundedQueue
{
   public:
    explicit BoundedQueue(size_t max_depth) : max_depth_(max_depth) {}

    // 【关键步骤：背压控制】
    // 用于多线程间的数据传递。当队列达到最大深度时，主动丢弃最旧的帧。
    // 这保证了即便下游消费（如网络推流）卡顿，上游硬件（如摄像头/解码器）也绝不会被阻塞死锁。
    bool Push(T* item, T* dropped)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
            return false;

        if (dropped)
            *dropped = T{};

        if (max_depth_ > 0 && queue_.size() >= max_depth_)
        {
            if (dropped)
                *dropped = std::move(queue_.front());
            queue_.pop_front();
        }

        queue_.push_back(std::move(*item));
        cv_.notify_one();
        return true;
    }

    // 阻塞式获取数据
    bool Pop(T* out)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return stopped_ || !queue_.empty(); });
        if (queue_.empty())
            return false;

        *out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }

    // 清空队列并调用回调安全释放资源
    void Drain(const std::function<void(T&)>& releaser)
    {
        std::deque<T> local_queue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            local_queue.swap(queue_);
        }

        while (!local_queue.empty())
        {
            T item = std::move(local_queue.front());
            local_queue.pop_front();
            releaser(item);
        }
    }

   private:
    size_t                  max_depth_ = 0;
    bool                    stopped_   = false;
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::deque<T>           queue_;
};

// =========================================================================
// 生命周期与资源管理载体
// =========================================================================

// 统一的压缩帧载体：无缝兼容摄像头(MJPEG)和文件/网络流(H.264/HEVC)输入
struct EncodedFrameRef
{
    FrameDesc             desc = {};
    std::shared_ptr<void> owner;
    std::function<void()> release;
    bool                  decoder_reset_before_decode = false;
};

// 硬件解码后输出的 NV12 高清原始帧载体
struct DecodedFrameRef
{
    const IO_FD_t*        output = nullptr;
    std::function<void()> release;
};

// 送入硬件编码器压缩前的画面载体
struct EncodeFrameRef
{
    const IO_FD_t*        output = nullptr;
    std::function<void()> release;
};

bool HasValidEncodedFrame(const EncodedFrameRef& frame)
{
    return frame.desc.payloadSize > 0 || frame.release != nullptr || static_cast<bool>(frame.owner);
}

bool HasValidDecodedFrame(const DecodedFrameRef& frame) { return frame.output != nullptr; }

bool HasValidEncodeFrame(const EncodeFrameRef& frame) { return frame.output != nullptr; }

// 以下三个函数确保画面流转失败或被丢弃时，底层物理内存(DMABUF)能被正确还回缓冲池
void ReleaseEncodedFrame(EncodedFrameRef* frame)
{
    if (!frame)
        return;
    if (frame->release)
        frame->release();
    frame->release = nullptr;
    frame->owner.reset();
    frame->decoder_reset_before_decode = false;
    frame->desc                        = {};
}

void ReleaseDecodedFrame(DecodedFrameRef* frame)
{
    if (!frame)
        return;
    if (frame->release)
        frame->release();
    frame->release = nullptr;
    frame->output  = nullptr;
}

void ReleaseEncodeFrame(EncodeFrameRef* frame)
{
    if (!frame)
        return;
    if (frame->release)
        frame->release();
    frame->release = nullptr;
    frame->output  = nullptr;
}

namespace source_read_result
{
inline constexpr int kOk        = 0;
inline constexpr int kRetryable = 1;
inline constexpr int kEof       = 2;
inline constexpr int kFatal     = -1;
}  // namespace source_read_result

// =========================================================================
// 多态数据源抽象：隔离 V4L2 与 FFmpeg 的底层差异
// =========================================================================
class IEncodedSource
{
   public:
    virtual ~IEncodedSource() = default;

    virtual int              Init(SourceVideoInfo* out_info)              = 0;
    virtual int              ReadFrame(EncodedFrameRef* out_frame)        = 0;
    virtual const FrameDesc* GetImportFrameArray(size_t* out_count) const = 0;
    virtual bool             CanAutoRecover() const { return false; }
    virtual int              Recover() { return -1; }
    virtual int              GetRecoverIntervalMs() const { return 2000; }
    virtual int              GetMaxRecoverCount() const { return -1; }
};

// 具体的摄像头(V4L2)数据源实现
class CameraEncodedSource : public IEncodedSource
{
   public:
    explicit CameraEncodedSource(std::string device) : device_(std::move(device)) {}

    int Init(SourceVideoInfo* out_info) override
    {
        if (!out_info)
            return -1;
        if (camera_.camera_GlobalInit(device_, 0) != 0)
            return -1;

        const VideoCaptureConfig& cfg = camera_.get_active_config();
        if (!cfg.valid)
            return -1;

        out_info->width       = cfg.width;
        out_info->height      = cfg.height;
        out_info->coding_type = MPP_VIDEO_CodingMJPEG;
        out_info->fps         = 30;
        if (cfg.fps_num > 0 && cfg.fps_den > 0)
        {
            const uint32_t fps = cfg.fps_den / cfg.fps_num;
            out_info->fps      = (fps > 0) ? fps : 30;
        }
        out_info->valid = true;
        return 0;
    }

    int ReadFrame(EncodedFrameRef* out_frame) override
    {
        if (!out_frame)
            return source_read_result::kFatal;

        size_t    frame_length = camera_params::kMaxFrameSize;
        const int read_ret = camera_.camera_read_frame(&frame_length, camera_params::kMaxFrameSize);
        if (read_ret == camera_read_result::kOk)
        {
            FrameDesc* frame_desc = camera_.CurrentFrameDesc;
            if (!frame_desc)
                return source_read_result::kFatal;

            out_frame->desc    = *frame_desc;
            out_frame->release = [this, frame_desc]()
            {
                if (camera_.requeue_buffer(frame_desc) != 0)
                {
                    std::cerr << "[Camera] 重新入队采集缓冲失败" << std::endl;
                }
            };
            return source_read_result::kOk;
        }

        if (read_ret == camera_read_result::kRetryable)
            return source_read_result::kRetryable;

        if (read_ret == camera_read_result::kTimeout)
        {
            std::cerr << "[Camera] V4L2 出队超时" << std::endl;
        }
        return source_read_result::kFatal;
    }

    const FrameDesc* GetImportFrameArray(size_t* out_count) const override
    {
        if (out_count)
            *out_count = static_cast<size_t>(camera_.get_buffer_count());
        return camera_.FrameDescArray;
    }

   private:
    std::string device_;
    V4L2_Camera camera_;
};

// 具体的文件/网络流(FFmpeg)数据源实现
class FileEncodedSource : public IEncodedSource
{
   public:
    explicit FileEncodedSource(FfmpegFileSourceConfig config) : config_(std::move(config)) {}

    int Init(SourceVideoInfo* out_info) override
    {
        if (!out_info)
            return -1;
        if (file_source_.Open(config_) != 0)
            return -1;

        *out_info    = file_source_.GetVideoInfo();
        source_info_ = *out_info;
        initialized_ = out_info->valid;
        ResetPlaybackClock();
        return out_info->valid ? 0 : -1;
    }

    int ReadFrame(EncodedFrameRef* out_frame) override
    {
        if (!out_frame)
            return source_read_result::kFatal;

        std::shared_ptr<void> owner;
        const int             read_ret = file_source_.Read(&out_frame->desc, &owner);
        if (read_ret == file_source_result::kOk)
        {
            out_frame->owner                       = std::move(owner);
            out_frame->release                     = []() {};
            out_frame->decoder_reset_before_decode = file_source_.ConsumeDecoderResetRequest();
            MaybeThrottleForPlayback(out_frame->desc.pts_us);
            return source_read_result::kOk;
        }
        if (read_ret == file_source_result::kEof)
            return source_read_result::kEof;

        return source_read_result::kFatal;
    }

    const FrameDesc* GetImportFrameArray(size_t* out_count) const override
    {
        if (out_count)
            *out_count = 0;
        return nullptr;
    }

    bool CanAutoRecover() const override { return file_source_.IsRealtimeInput(); }

    int Recover() override
    {
        SourceVideoInfo new_info;
        file_source_.Close();
        if (file_source_.Open(config_) != 0)
            return -1;

        new_info = file_source_.GetVideoInfo();
        if (!new_info.valid)
        {
            file_source_.Close();
            return -1;
        }

        if (initialized_ &&
            (new_info.width != source_info_.width || new_info.height != source_info_.height ||
             new_info.coding_type != source_info_.coding_type))
        {
            std::cerr << "[File] 恢复后的输入分辨率或编码格式发生改变, 旧=" << source_info_.width
                      << "x" << source_info_.height << " 编码=" << source_info_.coding_type
                      << ", 新=" << new_info.width << "x" << new_info.height
                      << " 编码=" << new_info.coding_type << std::endl;
            file_source_.Close();
            return -1;
        }

        source_info_ = new_info;
        initialized_ = true;
        ResetPlaybackClock();
        return 0;
    }

    int GetRecoverIntervalMs() const override { return config_.recover_interval_ms; }
    int GetMaxRecoverCount() const override { return config_.max_recover_count; }

   private:
    void ResetPlaybackClock()
    {
        playback_clock_inited_ = false;
        playback_first_pts_us_ = -1;
        playback_last_pts_us_  = -1;
    }

    void MaybeThrottleForPlayback(int64_t pts_us)
    {
        if (file_source_.IsRealtimeInput() || pts_us < 0)
        {
            return;
        }

        const auto now_tp = std::chrono::steady_clock::now();
        if (!playback_clock_inited_)
        {
            playback_clock_inited_ = true;
            playback_first_pts_us_ = pts_us;
            playback_last_pts_us_  = pts_us;
            playback_start_tp_     = now_tp;
            return;
        }

        // 文件循环后 PTS 可能回卷，重建播放时钟基线。
        if (playback_last_pts_us_ >= 0 && pts_us + 200000 < playback_last_pts_us_)
        {
            playback_first_pts_us_ = pts_us;
            playback_start_tp_     = now_tp;
        }
        playback_last_pts_us_ = pts_us;

        int64_t relative_us = pts_us - playback_first_pts_us_;
        if (relative_us < 0)
        {
            relative_us = 0;
        }

        const auto target_tp = playback_start_tp_ + std::chrono::microseconds(relative_us);
        if (target_tp > now_tp)
        {
            auto       wait_dur = target_tp - now_tp;
            const auto max_wait = std::chrono::milliseconds(200);
            if (wait_dur > max_wait)
            {
                wait_dur = max_wait;
            }
            std::this_thread::sleep_for(wait_dur);
        }
        else if (now_tp - target_tp > std::chrono::seconds(2))
        {
            // 时间基线漂移过大时重同步，防止长时间追帧导致抖动。
            playback_first_pts_us_ = pts_us;
            playback_start_tp_     = now_tp;
        }
    }

    FfmpegFileSourceConfig                config_;
    FfmpegFileSource                      file_source_;
    SourceVideoInfo                       source_info_           = {};
    bool                                  initialized_           = false;
    bool                                  playback_clock_inited_ = false;
    int64_t                               playback_first_pts_us_ = -1;
    int64_t                               playback_last_pts_us_  = -1;
    std::chrono::steady_clock::time_point playback_start_tp_     = {};
};

// =========================================================================
// 共享的 AI 推理引擎 (并发锁优化版)
// =========================================================================
class SharedInferenceEngine
{
   public:
    struct CachedPersonBox
    {
        int left   = 0;
        int top    = 0;
        int right  = 0;
        int bottom = 0;
    };

    struct InputTensorSlot
    {
        rknn_app_context_t                           app_ctx         = {};
        IO_FD_t                                      tensor_buffer   = {};
        rknn_tensor_mem*                             tensor_mem      = nullptr;
        rga_buffer_handle_t                          rga_handle      = 0;
        bool                                         owns_app_ctx    = false;
        bool                                         use_external_fd = false;
        rknn_core_mask                               core_mask       = RKNN_NPU_CORE_AUTO;
        std::vector<uint8_t>                         input_buffer;
        std::mutex                                   slot_mutex;
        std::unordered_map<int, rga_buffer_handle_t> src_handle_cache;
        std::vector<rknn_output>                     outputs;
        std::vector<CachedPersonBox>                 cached_person_boxes;
        uint32_t                                     cached_person_ttl  = 0;
        bool                                         inference_disabled = false;
        std::chrono::steady_clock::time_point        last_infer_tp      = {};
    };

    ~SharedInferenceEngine() { ReleaseAllInputTensorSlots(); }

    int Init(const std::string& model_path, bool cpu_input_mode)
    {
        cpu_input_mode_ = cpu_input_mode;
        if (base_rknn_.Init(model_path) != 0)
        {
            return -1;
        }

        if (base_rknn_.app_ctx.io_num.n_input == 0 || !base_rknn_.app_ctx.input_attrs)
        {
            std::cerr << "[RKNN] 输入张量属性不可用" << std::endl;
            return -1;
        }

        std::cout << "[RKNN] input mode: "
                  << (cpu_input_mode_ ? "cpu-compatible (--rknn-cpu-input)"
                                      : "zero-copy rknn_set_io_mem")
                  << std::endl;
        return 0;
    }

    int RunPersonDetection(const IO_FD_t* nv12_frame, const std::string& pipeline_name)
    {
        if (!nv12_frame || !nv12_frame->base || nv12_frame->fd < 0)
            return -1;

        auto slot = GetOrCreateInputTensorSlot(pipeline_name);
        if (!slot || !slot->rga_handle)
            return -1;

        std::lock_guard<std::mutex> slot_lock(slot->slot_mutex);
        auto&                       app_ctx = cpu_input_mode_ ? base_rknn_.app_ctx : slot->app_ctx;
        object_detect_result_list   od_results{};
        if (app_ctx.rknn_ctx == 0)
        {
            return -1;
        }

        if (slot->inference_disabled)
        {
            // AI 已经降级时，继续绘制最后一次有效检测框，避免画面中框消失。
            return DrawPersonBoxesOnFrame(nv12_frame, slot->cached_person_boxes, pipeline_name);
        }

        const auto now_tp = std::chrono::steady_clock::now();
        if (slot->last_infer_tp.time_since_epoch().count() > 0 &&
            now_tp - slot->last_infer_tp < kPersonInferInterval)
        {
            if (!slot->cached_person_boxes.empty() && slot->cached_person_ttl > 0)
            {
                const int cached_ret =
                    DrawPersonBoxesOnFrame(nv12_frame, slot->cached_person_boxes, pipeline_name);
                if (cached_ret == 0 && slot->cached_person_ttl > 0)
                {
                    --slot->cached_person_ttl;
                }
                return cached_ret;
            }
            return 0;
        }

        rga_buffer_handle_t src_handle = 0;
        auto                src_it     = slot->src_handle_cache.find(nv12_frame->fd);
        if (src_it == slot->src_handle_cache.end())
        {
            src_handle = importbuffer_fd(nv12_frame->fd, nv12_frame->size);
            if (src_handle)
            {
                slot->src_handle_cache.emplace(nv12_frame->fd, src_handle);
            }
        }
        else
        {
            src_handle = src_it->second;
        }

        if (!src_handle)
        {
            std::cerr << "[" << pipeline_name << "] 导入 RGA 源文件描述符(FD)失败" << std::endl;
            return -1;
        }

        rga_buffer_t rga_src = wrapbuffer_handle(
            src_handle, static_cast<int>(nv12_frame->width), static_cast<int>(nv12_frame->height),
            RK_FORMAT_YCbCr_420_SP, static_cast<int>(nv12_frame->hor_stride),
            static_cast<int>(nv12_frame->ver_stride));

        rga_buffer_t rga_dst =
            wrapbuffer_handle(slot->rga_handle, app_ctx.model_width, app_ctx.model_height,
                              RK_FORMAT_RGB_888, app_ctx.model_width, app_ctx.model_height);

        const IM_STATUS status =
            imcvtcolor(rga_src, rga_dst, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_RGB_888);
        if (status != IM_STATUS_SUCCESS)
        {
            std::cerr << "[" << pipeline_name << "] RGA 预处理失败: " << imStrError_t(status)
                      << std::endl;
            return -1;
        }

        const uint32_t input_size =
            (app_ctx.input_attrs && app_ctx.input_attrs[0].size > 0)
                ? app_ctx.input_attrs[0].size
                : static_cast<uint32_t>(app_ctx.model_width * app_ctx.model_height *
                                        app_ctx.model_channel);
        std::unique_lock<std::mutex> inference_lock(inference_mutex_, std::defer_lock);
        if (cpu_input_mode_)
        {
            void* input_buf = slot->input_buffer.empty() ? nullptr : slot->input_buffer.data();
            if (!input_buf || input_size == 0 || slot->input_buffer.size() < input_size)
            {
                std::cerr << "[" << pipeline_name << "] 推理输入缓冲无效" << std::endl;
                return -1;
            }

            rknn_input inputs[1]   = {};
            inputs[0].index        = 0;
            inputs[0].buf          = input_buf;
            inputs[0].size         = input_size;
            inputs[0].type         = RKNN_TENSOR_UINT8;
            inputs[0].fmt          = RKNN_TENSOR_NHWC;
            inputs[0].pass_through = 0;

            inference_lock.lock();
            if (rknn_inputs_set(app_ctx.rknn_ctx, 1, inputs) < 0)
            {
                std::cerr << "[" << pipeline_name << "] rknn_inputs_set 失败" << std::endl;
                return -1;
            }
        }
        else if (slot->tensor_mem)
        {
            const int sync_ret =
                rknn_mem_sync(app_ctx.rknn_ctx, slot->tensor_mem, RKNN_MEMORY_SYNC_TO_DEVICE);
            if (sync_ret < 0)
            {
                std::cerr << "[" << pipeline_name << "] rknn_mem_sync(TO_DEVICE) 失败: " << sync_ret
                          << std::endl;
                slot->inference_disabled = true;
                return -1;
            }
        }

        if (rknn_run(app_ctx.rknn_ctx, nullptr) < 0)
        {
            std::cerr << "[" << pipeline_name << "] rknn_run 失败" << std::endl;
            return -1;
        }

        if (slot->outputs.size() != app_ctx.io_num.n_output)
        {
            slot->outputs.assign(app_ctx.io_num.n_output, rknn_output{});
            for (uint32_t i = 0; i < app_ctx.io_num.n_output; ++i)
            {
                slot->outputs[i].want_float = 1;
            }
        }

        if (rknn_outputs_get(app_ctx.rknn_ctx, app_ctx.io_num.n_output, slot->outputs.data(),
                             nullptr) < 0)
        {
            std::cerr << "[" << pipeline_name << "] rknn_outputs_get 失败" << std::endl;
            return -1;
        }

        letterbox_t letter_box = {0, 0, 1.0f};
        post_process(&app_ctx, slot->outputs.data(), &letter_box, kConfThresh, kNmsThresh,
                     &od_results);
        rknn_outputs_release(app_ctx.rknn_ctx, app_ctx.io_num.n_output, slot->outputs.data());

        slot->last_infer_tp       = now_tp;
        slot->cached_person_boxes = BuildPersonBoxes(nv12_frame, app_ctx, od_results);
        slot->cached_person_ttl   = slot->cached_person_boxes.empty() ? 0 : kInferCacheReuseFrames;
        return DrawPersonBoxesOnFrame(nv12_frame, slot->cached_person_boxes, pipeline_name);
    }

    int TryRenderCachedPersonDetections(const IO_FD_t* nv12_frame, const std::string& pipeline_name)
    {
        if (!nv12_frame || !nv12_frame->base || nv12_frame->fd < 0)
        {
            return -1;
        }

        auto slot = GetOrCreateInputTensorSlot(pipeline_name);
        if (!slot)
        {
            return -1;
        }

        std::lock_guard<std::mutex> slot_lock(slot->slot_mutex);
        if (slot->cached_person_boxes.empty() || slot->cached_person_ttl == 0)
        {
            return 0;
        }

        const int ret =
            DrawPersonBoxesOnFrame(nv12_frame, slot->cached_person_boxes, pipeline_name);
        if (ret == 0 && slot->cached_person_ttl > 0)
        {
            --slot->cached_person_ttl;
        }
        return ret == 0 ? 1 : -1;
    }

   private:
    std::vector<CachedPersonBox> BuildPersonBoxes(const IO_FD_t*                   nv12_frame,
                                                  const rknn_app_context_t&        app_ctx,
                                                  const object_detect_result_list& od_results) const
    {
        std::vector<CachedPersonBox> boxes;
        if (!nv12_frame || app_ctx.model_width <= 0 || app_ctx.model_height <= 0)
        {
            return boxes;
        }

        const float x_factor =
            static_cast<float>(nv12_frame->width) / static_cast<float>(app_ctx.model_width);
        const float y_factor =
            static_cast<float>(nv12_frame->height) / static_cast<float>(app_ctx.model_height);

        for (int i = 0; i < od_results.count; ++i)
        {
            const object_detect_result* det = &od_results.results[i];
            CachedPersonBox             box;
            box.left   = std::max(0, std::min(static_cast<int>(det->box.left * x_factor),
                                              static_cast<int>(nv12_frame->width) - 1));
            box.top    = std::max(0, std::min(static_cast<int>(det->box.top * y_factor),
                                              static_cast<int>(nv12_frame->height) - 1));
            box.right  = std::max(0, std::min(static_cast<int>(det->box.right * x_factor),
                                              static_cast<int>(nv12_frame->width) - 1));
            box.bottom = std::max(0, std::min(static_cast<int>(det->box.bottom * y_factor),
                                              static_cast<int>(nv12_frame->height) - 1));
            boxes.emplace_back(box);
        }
        return boxes;
    }

    int DrawPersonBoxesOnFrame(const IO_FD_t* nv12_frame, const std::vector<CachedPersonBox>& boxes,
                               const std::string& pipeline_name) const
    {
        if (!nv12_frame || !nv12_frame->base || nv12_frame->fd < 0 || boxes.empty())
        {
            return 0;
        }

        cv::Mat y_channel(static_cast<int>(nv12_frame->height), static_cast<int>(nv12_frame->width),
                          CV_8UC1, nv12_frame->base, static_cast<size_t>(nv12_frame->hor_stride));
        uint8_t* uv_base = static_cast<uint8_t*>(nv12_frame->base) +
                           static_cast<size_t>(nv12_frame->hor_stride) *
                               static_cast<size_t>(nv12_frame->ver_stride);
        cv::Mat  uv_channel(static_cast<int>(nv12_frame->height / 2),
                            static_cast<int>(nv12_frame->width / 2), CV_8UC2, uv_base,
                            static_cast<size_t>(nv12_frame->hor_stride));

        struct dma_buf_sync sync = {0};
        sync.flags               = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
        if (ioctl(nv12_frame->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
        {
            std::cerr << "[" << pipeline_name << "] DMA Sync Start failed: " << std::strerror(errno)
                      << std::endl;
        }

        for (const auto& box : boxes)
        {
            cv::rectangle(y_channel, Point(box.left, box.top), Point(box.right, box.bottom),
                          cv::Scalar(kYellowY), kDrawBoxThickness);

            const int uv_left = std::max(0, box.left / 2);
            const int uv_top  = std::max(0, box.top / 2);
            const int uv_right =
                std::min(static_cast<int>(nv12_frame->width / 2) - 1, box.right / 2);
            const int uv_bottom =
                std::min(static_cast<int>(nv12_frame->height / 2) - 1, box.bottom / 2);
            if (uv_right > uv_left && uv_bottom > uv_top)
            {
                cv::rectangle(uv_channel, Point(uv_left, uv_top), Point(uv_right, uv_bottom),
                              cv::Scalar(kYellowU, kYellowV), std::max(1, kDrawBoxThickness / 2));
            }

            if (kDrawDetectionText)
            {
                cv::putText(y_channel, "person", Point(box.left, std::max(0, box.top - 10)),
                            cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(kYellowY), 2);
            }
        }

        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
        if (ioctl(nv12_frame->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
        {
            std::cerr << "[" << pipeline_name << "] DMA Sync End failed: " << std::strerror(errno)
                      << std::endl;
        }
        return 0;
    }

   private:
    std::shared_ptr<InputTensorSlot> GetOrCreateInputTensorSlot(const std::string& pipeline_name)
    {
        const std::string slot_key = pipeline_name.empty() ? "default" : pipeline_name;

        std::lock_guard<std::mutex> lock(slot_map_mutex_);
        auto                        it = input_slots_.find(slot_key);
        if (it != input_slots_.end())
        {
            return it->second;
        }

        auto slot = std::make_shared<InputTensorSlot>();
        if (CreateInputTensorSlot(slot.get(), input_slots_.size()) != 0)
        {
            return nullptr;
        }

        input_slots_[slot_key] = slot;
        std::cout << "[RKNN] pipeline=" << slot_key << " ";
        if (cpu_input_mode_)
        {
            std::cout << "使用兼容模式: shared context + CPU input buffer";
        }
        else if (slot->owns_app_ctx)
        {
            std::cout << "使用零拷贝模式: duplicated context + rknn_set_io_mem";
        }
        else
        {
            std::cout << "使用零拷贝模式: primary context + rknn_set_io_mem";
        }
        std::cout << std::endl;
        return slot;
    }

    int CreateInputTensorSlot(InputTensorSlot* slot, size_t slot_index)
    {
        if (!slot)
            return -1;

        if (!cpu_input_mode_)
        {
            if (slot_index == 0)
            {
                // RK3566 上 duplicated context + zero-copy input 容易在 rknn_run 阶段触发
                // runtime 内部崩溃。第一路直接复用主 context，仍然保持 RGA -> RKNN
                // tensor memory 的零拷贝输入路径。
                slot->app_ctx      = base_rknn_.app_ctx;
                slot->owns_app_ctx = false;
            }
            else if (InitSlotAppContext(slot, slot_index) != 0)
            {
                ReleaseInputTensorSlot(slot);
                return -1;
            }
        }

        const auto&  app_ctx    = cpu_input_mode_ ? base_rknn_.app_ctx : slot->app_ctx;
        const size_t input_size = static_cast<size_t>(app_ctx.model_width) *
                                  static_cast<size_t>(app_ctx.model_height) *
                                  static_cast<size_t>(app_ctx.model_channel);
        if (input_size == 0)
        {
            std::cerr << "[RKNN] 输入张量大小非法" << std::endl;
            ReleaseInputTensorSlot(slot);
            return -1;
        }

        if (cpu_input_mode_)
        {
            slot->input_buffer.assign(input_size, 0);
            slot->rga_handle =
                importbuffer_virtualaddr(slot->input_buffer.data(), static_cast<int>(input_size));
        }
        else
        {
            const uint32_t logical_tensor_size =
                (app_ctx.input_attrs && app_ctx.input_attrs[0].size > 0)
                    ? app_ctx.input_attrs[0].size
                    : static_cast<uint32_t>(input_size);
            const uint32_t tensor_size =
                (app_ctx.input_attrs && app_ctx.input_attrs[0].size_with_stride > 0)
                    ? app_ctx.input_attrs[0].size_with_stride
                    : logical_tensor_size;
            slot->tensor_mem = rknn_create_mem(slot->app_ctx.rknn_ctx, tensor_size);
            if (!slot->tensor_mem)
            {
                std::cerr << "[RKNN] 创建零拷贝输入张量内存失败" << std::endl;
                ReleaseInputTensorSlot(slot);
                return -1;
            }

            rknn_tensor_attr input_attr = slot->app_ctx.input_attrs[0];
            input_attr.index            = 0;
            input_attr.type             = RKNN_TENSOR_UINT8;
            input_attr.fmt              = RKNN_TENSOR_NHWC;
            input_attr.size             = logical_tensor_size;
            input_attr.size_with_stride = tensor_size;
            input_attr.pass_through     = 0;
            if (rknn_set_io_mem(slot->app_ctx.rknn_ctx, slot->tensor_mem, &input_attr) < 0)
            {
                std::cerr << "[RKNN] rknn_set_io_mem 输入绑定失败" << std::endl;
                ReleaseInputTensorSlot(slot);
                return -1;
            }

            if (slot->tensor_mem->fd >= 0)
            {
                slot->rga_handle =
                    importbuffer_fd(slot->tensor_mem->fd, static_cast<int>(slot->tensor_mem->size));
            }
            else if (slot->tensor_mem->virt_addr)
            {
                slot->rga_handle = importbuffer_virtualaddr(
                    slot->tensor_mem->virt_addr, static_cast<int>(slot->tensor_mem->size));
            }
        }

        if (!slot->rga_handle)
        {
            std::cerr << "[RKNN] 导入推理输入内存到 RGA 失败" << std::endl;
            ReleaseInputTensorSlot(slot);
            return -1;
        }

        return 0;
    }

    int InitSlotAppContext(InputTensorSlot* slot, size_t slot_index)
    {
        if (!slot || base_rknn_.app_ctx.rknn_ctx == 0)
            return -1;

        if (rknn_dup_context(&base_rknn_.app_ctx.rknn_ctx, &slot->app_ctx.rknn_ctx) < 0 ||
            slot->app_ctx.rknn_ctx == 0)
        {
            std::cerr << "[RKNN] rknn_dup_context 失败" << std::endl;
            return -1;
        }
        slot->owns_app_ctx = true;

        slot->core_mask = PickCoreMask(slot_index);
        if (slot->core_mask != RKNN_NPU_CORE_AUTO)
        {
            const int core_ret = rknn_set_core_mask(slot->app_ctx.rknn_ctx, slot->core_mask);
            if (core_ret < 0)
            {
                std::cerr << "[RKNN] 设置 NPU core mask 失败, ret=" << core_ret
                          << ", pipeline_index=" << slot_index << std::endl;
                slot->core_mask = RKNN_NPU_CORE_AUTO;
            }
        }

        if (rknn_query(slot->app_ctx.rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &slot->app_ctx.io_num,
                       sizeof(slot->app_ctx.io_num)) < 0)
        {
            std::cerr << "[RKNN] 查询 duplicated context 的 IO 数量失败" << std::endl;
            ReleaseSlotAppContext(slot);
            return -1;
        }

        slot->app_ctx.input_attrs = static_cast<rknn_tensor_attr*>(
            std::calloc(slot->app_ctx.io_num.n_input, sizeof(rknn_tensor_attr)));
        slot->app_ctx.output_attrs = static_cast<rknn_tensor_attr*>(
            std::calloc(slot->app_ctx.io_num.n_output, sizeof(rknn_tensor_attr)));
        if ((slot->app_ctx.io_num.n_input > 0 && !slot->app_ctx.input_attrs) ||
            (slot->app_ctx.io_num.n_output > 0 && !slot->app_ctx.output_attrs))
        {
            std::cerr << "[RKNN] duplicated context 属性内存分配失败" << std::endl;
            ReleaseSlotAppContext(slot);
            return -1;
        }

        for (uint32_t i = 0; i < slot->app_ctx.io_num.n_input; ++i)
        {
            slot->app_ctx.input_attrs[i].index = i;
            if (rknn_query(slot->app_ctx.rknn_ctx, RKNN_QUERY_INPUT_ATTR,
                           &slot->app_ctx.input_attrs[i], sizeof(rknn_tensor_attr)) < 0)
            {
                std::cerr << "[RKNN] 查询 duplicated context 输入属性失败, index=" << i
                          << std::endl;
                ReleaseSlotAppContext(slot);
                return -1;
            }
        }
        for (uint32_t i = 0; i < slot->app_ctx.io_num.n_output; ++i)
        {
            slot->app_ctx.output_attrs[i].index = i;
            if (rknn_query(slot->app_ctx.rknn_ctx, RKNN_QUERY_OUTPUT_ATTR,
                           &slot->app_ctx.output_attrs[i], sizeof(rknn_tensor_attr)) < 0)
            {
                std::cerr << "[RKNN] 查询 duplicated context 输出属性失败, index=" << i
                          << std::endl;
                ReleaseSlotAppContext(slot);
                return -1;
            }
        }

        slot->app_ctx.model_channel = base_rknn_.app_ctx.model_channel;
        slot->app_ctx.model_width   = base_rknn_.app_ctx.model_width;
        slot->app_ctx.model_height  = base_rknn_.app_ctx.model_height;
        slot->app_ctx.is_quant      = base_rknn_.app_ctx.is_quant;
        return 0;
    }

    rknn_core_mask PickCoreMask(size_t slot_index) const
    {
        switch (slot_index % 3)
        {
            case 0:
                return RKNN_NPU_CORE_AUTO;
            case 1:
                return RKNN_NPU_CORE_AUTO;
            case 2:
                return RKNN_NPU_CORE_AUTO;
            default:
                return RKNN_NPU_CORE_AUTO;
        }
    }

    void ReleaseSlotAppContext(InputTensorSlot* slot)
    {
        if (!slot)
            return;

        if (!slot->owns_app_ctx)
        {
            slot->app_ctx   = {};
            slot->core_mask = RKNN_NPU_CORE_AUTO;
            return;
        }

        if (slot->app_ctx.input_attrs)
        {
            std::free(slot->app_ctx.input_attrs);
            slot->app_ctx.input_attrs = nullptr;
        }
        if (slot->app_ctx.output_attrs)
        {
            std::free(slot->app_ctx.output_attrs);
            slot->app_ctx.output_attrs = nullptr;
        }
        if (slot->app_ctx.rknn_ctx != 0)
        {
            rknn_destroy(slot->app_ctx.rknn_ctx);
            slot->app_ctx.rknn_ctx = 0;
        }
        slot->app_ctx.io_num.n_input  = 0;
        slot->app_ctx.io_num.n_output = 0;
        slot->app_ctx.model_channel   = 0;
        slot->app_ctx.model_width     = 0;
        slot->app_ctx.model_height    = 0;
        slot->app_ctx.is_quant        = false;
        slot->core_mask               = RKNN_NPU_CORE_AUTO;
        slot->owns_app_ctx            = false;
    }

    void ReleaseInputTensorSlot(InputTensorSlot* slot)
    {
        if (!slot)
            return;

        std::lock_guard<std::mutex> slot_lock(slot->slot_mutex);

        if (slot->rga_handle)
        {
            releasebuffer_handle(slot->rga_handle);
            slot->rga_handle = 0;
        }
        for (auto& kv : slot->src_handle_cache)
        {
            if (kv.second)
            {
                releasebuffer_handle(kv.second);
            }
        }
        slot->src_handle_cache.clear();
        slot->outputs.clear();

        if (slot->tensor_mem && slot->app_ctx.rknn_ctx)
        {
            rknn_destroy_mem(slot->app_ctx.rknn_ctx, slot->tensor_mem);
            slot->tensor_mem = nullptr;
        }

        if (slot->use_external_fd)
        {
            ReleaseDmaBufFD(&slot->tensor_buffer);
        }
        else
        {
            slot->tensor_buffer = {};
        }
        slot->use_external_fd = false;
        slot->input_buffer.clear();
        slot->input_buffer.shrink_to_fit();

        ReleaseSlotAppContext(slot);
    }

    void ReleaseAllInputTensorSlots()
    {
        std::lock_guard<std::mutex> lock(slot_map_mutex_);
        for (auto& kv : input_slots_)
        {
            ReleaseInputTensorSlot(kv.second.get());
        }
        input_slots_.clear();
    }

    int AllocDmaBufFD(IO_FD_t* output, size_t size)
    {
        if (!output || size == 0)
        {
            return -1;
        }

        ReleaseDmaBufFD(output);

        int heap = open(mpp_common::kSystemDmaHeapPath, O_RDWR | O_CLOEXEC);
        if (heap < 0)
        {
            std::cerr << "[RKNN] Failed to open dma_heap(" << mpp_common::kSystemDmaHeapPath
                      << "): " << std::strerror(errno) << std::endl;
            return -1;
        }

        dma_heap_allocation_data req{};
        req.len      = size;
        req.fd_flags = O_RDWR | O_CLOEXEC;

        if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req) < 0)
        {
            std::cerr << "[RKNN] DMA heap allocation failed: " << std::strerror(errno) << std::endl;
            close(heap);
            return -1;
        }
        close(heap);

        void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
        if (addr == MAP_FAILED)
        {
            std::cerr << "[RKNN] mmap dma-buf failed: " << std::strerror(errno) << std::endl;
            close(req.fd);
            return -1;
        }

        output->fd         = req.fd;
        output->base       = addr;
        output->size       = size;
        output->format     = 0;
        output->width      = 0;
        output->height     = 0;
        output->hor_stride = 0;
        output->ver_stride = 0;
        output->pts_us     = -1;
        output->dts_us     = -1;
        return 0;
    }

    void ReleaseDmaBufFD(IO_FD_t* output)
    {
        if (!output)
            return;

        if (output->base && output->size > 0)
        {
            (void)munmap(output->base, output->size);
        }
        if (output->fd >= 0)
        {
            (void)close(output->fd);
            output->fd = -1;
        }
        output->base       = nullptr;
        output->size       = 0;
        output->format     = 0;
        output->width      = 0;
        output->height     = 0;
        output->hor_stride = 0;
        output->ver_stride = 0;
        output->pts_us     = -1;
        output->dts_us     = -1;
    }

    std::mutex                                                        slot_map_mutex_;
    std::mutex                                                        inference_mutex_;
    bool                                                              cpu_input_mode_ = false;
    RknnInstance                                                      base_rknn_;
    std::unordered_map<std::string, std::shared_ptr<InputTensorSlot>> input_slots_;
};
// 流水线配置信息
struct PipelineConfig
{
    std::string name;
    std::string stream_name;
    size_t      encoded_queue_depth = kDefaultQueueDepth;
    size_t      decoded_queue_depth = kDefaultQueueDepth;
    size_t      encode_queue_depth  = kDefaultQueueDepth;
    bool        enable_audio        = false;
    std::string audio_device;
    int         audio_sync_offset_ms = static_cast<int>(kDefaultAudioSyncOffsetMs);
};

// =========================================================================
// 核心架构：工业级全栈处理流水线（Pipeline）
// 彻底将每路视频的状态机、硬件实例、工作线程封锁在容器内部，实现纯物理隔离
// =========================================================================
class VideoPipeline
{
   public:
    VideoPipeline(PipelineConfig config, std::unique_ptr<IEncodedSource> source,
                  SharedInferenceEngine* inference, std::atomic_bool* global_stop)
        : config_(std::move(config)),
          source_(std::move(source)),
          inference_(inference),
          global_stop_(global_stop),
          encoded_queue_(config_.encoded_queue_depth),
          decoded_queue_(config_.decoded_queue_depth),
          encode_queue_(config_.encode_queue_depth)
    {}

    ~VideoPipeline()
    {
        Stop();
        Join();
        encoded_queue_.Drain([](EncodedFrameRef& frame) { ReleaseEncodedFrame(&frame); });
        decoded_queue_.Drain([](DecodedFrameRef& frame) { ReleaseDecodedFrame(&frame); });
        encode_queue_.Drain([](EncodeFrameRef& frame) { ReleaseEncodeFrame(&frame); });
    }

    // 初始化本条流水线涉及到的所有独立硬件加速器
    int Init()
    {
        if (!source_ || !global_stop_)
            return -1;

        if (source_->Init(&video_info_) != 0 || !video_info_.valid)
        {
            std::cerr << "[" << config_.name << "] 数据源初始化失败" << std::endl;
            return -1;
        }

        if (decoder_.DecInit(video_info_.coding_type) != 0)
        {
            std::cerr << "[" << config_.name << "] MPP 硬件解码器初始化失败" << std::endl;
            return -1;
        }
        if (decoder_.DecConfigWidthHeight(video_info_.width, video_info_.height) != 0)
            return -1;

        size_t           import_count = 0;
        const FrameDesc* import_array = source_->GetImportFrameArray(&import_count);
        if (decoder_.DecAllocBuffer(import_array, import_count) != 0)
        {
            std::cerr << "[" << config_.name << "] 硬件解码器分配缓冲失败" << std::endl;
            return -1;
        }

        if (rga_.RgaInit() != 0)
        {
            std::cerr << "[" << config_.name << "] RGA 图形引擎初始化失败" << std::endl;
            return -1;
        }

        if (encoder_.EncInit(MPP_VIDEO_CodingAVC) != 0)
        {
            std::cerr << "[" << config_.name << "] MPP 硬件编码器初始化失败" << std::endl;
            return -1;
        }
        if (encoder_.EncConfigWidthHeight(video_info_.width, video_info_.height, 0, 0,
                                          video_info_.fps) != 0)
        {
            return -1;
        }

        ZlmPublishConfig publish_cfg;
        publish_cfg.stream = config_.stream_name;
        if (config_.enable_audio)
        {
            AlsaAudioCapture::Config audio_config;
            bool                     audio_device_auto = false;
            std::string              audio_device_note;
            if (config_.audio_device.empty() || config_.audio_device == "auto")
            {
                audio_device_auto   = true;
                audio_config.device = AlsaAudioCapture::AutoSelectCaptureDevice(&audio_device_note);
            }
            else
            {
                audio_config.device = config_.audio_device;
                audio_device_note   = "手动指定 ALSA 采集设备: " + audio_config.device;
            }
            audio_config.sample_rate   = kDefaultAudioSampleRate;
            audio_config.channels      = kDefaultAudioChannels;
            audio_config.sample_bit    = kDefaultAudioSampleBit;
            audio_config.period_frames = static_cast<snd_pcm_uframes_t>(kDefaultAudioPeriodFrames);

            audio_capture_ = std::make_unique<AlsaAudioCapture>(audio_config);
            if (audio_capture_->Init() == 0)
            {
                audio_enabled_                = true;
                publish_cfg.enable_audio      = true;
                publish_cfg.audio_sample_rate = audio_config.sample_rate;
                publish_cfg.audio_channels    = audio_config.channels;
                publish_cfg.audio_sample_bit  = audio_config.sample_bit;
                std::cout << "[" << config_.name
                          << "] ALSA 音频采集已启用, device=" << audio_config.device
                          << ", rate=" << audio_config.sample_rate
                          << ", channels=" << audio_config.channels
                          << ", select=" << (audio_device_auto ? "auto" : "manual") << std::endl;
                if (!audio_device_note.empty())
                {
                    std::cout << "[" << config_.name << "] " << audio_device_note << std::endl;
                }
            }
            else
            {
                audio_capture_.reset();
                std::cerr << "[" << config_.name
                          << "] ALSA 音频采集初始化失败，继续以纯视频模式运行" << std::endl;
            }
        }

        if (publisher_.Init(publish_cfg) != 0)
        {
            std::cerr << "[" << config_.name << "] ZLM 极速推流引擎初始化失败" << std::endl;
            return -1;
        }
        publisher_.SetExpectedFps(video_info_.fps);

        std::cout << "[" << config_.name << "] RTSP 拉流地址: " << publisher_.GetRtspUrl()
                  << std::endl;
        std::cout << "[" << config_.name
                  << "] WebRTC 播放页: " << publisher_.GetWebRtcPlayerPageUrl() << std::endl;
        std::cout << "[" << config_.name << "] WebRTC API: " << publisher_.GetWebRtcApiUrl()
                  << std::endl;
        return 0;
    }

    // 一键拉起 5 大协同线程
    void Start()
    {
        source_thread_        = std::thread(&VideoPipeline::SourceLoop, this);
        decode_thread_        = std::thread(&VideoPipeline::DecodeLoop, this);
        process_thread_       = std::thread(&VideoPipeline::ProcessLoop, this);
        encode_input_thread_  = std::thread(&VideoPipeline::EncodeInputLoop, this);
        encode_output_thread_ = std::thread(&VideoPipeline::EncodeOutputLoop, this);
        if (audio_enabled_ && audio_capture_)
        {
            audio_thread_ = std::thread(&VideoPipeline::AudioLoop, this);
        }
    }

    void Stop()
    {
        local_stop_.store(true);
        encoded_queue_.Stop();
        decoded_queue_.Stop();
        encode_queue_.Stop();
        if (audio_capture_)
        {
            audio_capture_->Close();
        }
    }

    void Join()
    {
        if (source_thread_.joinable())
            source_thread_.join();
        if (decode_thread_.joinable())
            decode_thread_.join();
        if (process_thread_.joinable())
            process_thread_.join();
        if (encode_input_thread_.joinable())
            encode_input_thread_.join();
        if (encode_output_thread_.joinable())
            encode_output_thread_.join();
        if (audio_thread_.joinable())
            audio_thread_.join();
    }

    bool HasFatalError() const { return fatal_error_.load(); }
    bool IsFinished() const { return finished_.load(); }

    PipelinePerfSnapshot ConsumePerfSnapshot()
    {
        PipelinePerfSnapshot snapshot;
        snapshot.name                   = config_.name;
        snapshot.source_frames          = ConsumeCounter(&source_frame_count_);
        snapshot.decode_failures        = ConsumeCounter(&decode_failures_);
        snapshot.infer_failures         = ConsumeCounter(&infer_failures_);
        snapshot.enc_push_failures      = ConsumeCounter(&enc_push_failures_);
        snapshot.encoded_queue_drops    = ConsumeCounter(&encoded_queue_drops_);
        snapshot.decoded_queue_drops    = ConsumeCounter(&decoded_queue_drops_);
        snapshot.encode_queue_drops     = ConsumeCounter(&encode_queue_drops_);
        snapshot.infer_reuse_frames     = ConsumeCounter(&infer_reuse_frames_);
        snapshot.enc_busy_drops         = ConsumeCounter(&enc_busy_drops_);
        snapshot.dec_ms                 = dec_stage_ms_.ConsumeAndReset();
        snapshot.rga_ms                 = rga_stage_ms_.ConsumeAndReset();
        snapshot.infer_ms               = infer_stage_ms_.ConsumeAndReset();
        snapshot.enc_in_ms              = enc_input_stage_ms_.ConsumeAndReset();
        snapshot.av_gap_abs_ms          = av_gap_abs_ms_.ConsumeAndReset();
        snapshot.encoded_q_depth        = encoded_queue_.Size();
        snapshot.decoded_q_depth        = decoded_queue_.Size();
        snapshot.encode_q_depth         = encode_queue_.Size();
        snapshot.output_frames_total    = publisher_.GetOutputFrameCount();
        snapshot.timestamp_fixups_total = publisher_.GetTimestampFallbackCount();
        snapshot.av_gap_ms_current      = av_gap_ms_current_.load(std::memory_order_relaxed);
        snapshot.av_sync_ready          = av_sync_ready_.load(std::memory_order_relaxed);
        snapshot.fatal                  = fatal_error_.load();
        snapshot.finished               = finished_.load();
        return snapshot;
    }

   private:
    bool ShouldStop() const { return local_stop_.load() || global_stop_->load(); }

    void RequestFatalStop(const std::string& reason)
    {
        if (!fatal_error_.exchange(true))
        {
            std::cerr << "[WARN] [" << config_.name << "] 流水线发生致命错误: " << reason
                      << std::endl;
        }
        Stop();  // 通知内部所有线程退出
    }

    // =========================================================================
    // 线程 1：源采集 (SourceLoop)
    // 责任：仅负责向底层(V4L2/FFmpeg)索要裸流压缩包，然后无脑扔进队列。
    // =========================================================================
    void SourceLoop()
    {
        while (!ShouldStop())
        {
            EncodedFrameRef encoded;
            const int       read_ret = source_->ReadFrame(&encoded);
            if (read_ret == source_read_result::kRetryable)
            {
                std::this_thread::sleep_for(kRetryBackoffSleep);
                continue;
            }
            if (read_ret == source_read_result::kEof)
                break;

            if (read_ret != source_read_result::kOk)
            {
                if (source_->CanAutoRecover())
                {
                    bool      recovered    = false;
                    int       attempts     = 0;
                    const int max_attempts = source_->GetMaxRecoverCount();
                    const int sleep_ms     = std::max(200, source_->GetRecoverIntervalMs());

                    while (!ShouldStop())
                    {
                        ++attempts;
                        std::cerr << "[WARN] [" << config_.name
                                  << "] 源读取失败，正在尝试断线重连 #" << attempts << std::endl;

                        if (source_->Recover() == 0)
                        {
                            std::cout << "[" << config_.name << "] 数据源已恢复" << std::endl;
                            recovered = true;
                            break;
                        }

                        if (max_attempts >= 0 && attempts >= max_attempts)
                            break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                    }

                    if (recovered)
                        continue;
                }

                RequestFatalStop("数据源读取失败");
                break;
            }

            source_frame_count_.fetch_add(1, std::memory_order_relaxed);

            EncodedFrameRef dropped;
            if (!encoded_queue_.Push(&encoded, &dropped))
            {
                ReleaseEncodedFrame(&encoded);
                break;
            }
            if (HasValidEncodedFrame(dropped))
            {
                encoded_queue_drops_.fetch_add(1, std::memory_order_relaxed);
            }
            ReleaseEncodedFrame(&dropped);
        }

        encoded_queue_.Stop();
    }

    // =========================================================================
    // 线程 2：硬件解码 (DecodeLoop)
    // 责任：将带有压缩格式数据的 FD 塞给 VPU 解码器，等待其吐出纯净的 NV12 DMABUF 句柄。
    // =========================================================================
    void DecodeLoop()
    {
        EncodedFrameRef encoded;
        while (encoded_queue_.Pop(&encoded))
        {
            if (ShouldStop())
            {
                ReleaseEncodedFrame(&encoded);
                continue;
            }

            if (encoded.decoder_reset_before_decode)
            {
                std::cout << "[" << config_.name << "] 检测到本地文件回卷，已重建时间轴"
                          << std::endl;
            }

            const auto decode_begin_tp = std::chrono::steady_clock::now();
            const int  decode_ret      = decoder_.MppDecode(&encoded.desc);
            dec_stage_ms_.AddSample(std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - decode_begin_tp)
                                        .count());
            if (decode_ret != 0)
            {
                decode_failures_.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "[" << config_.name
                          << "] 硬件解码失败, payload=" << encoded.desc.payloadSize << std::endl;
                ReleaseEncodedFrame(&encoded);
                continue;
            }

            const IO_FD_t* decoded_output = decoder_.CurrentOutputDesc;
            if (!decoded_output)
            {
                ReleaseEncodedFrame(&encoded);
                continue;
            }

            DecodedFrameRef decoded;
            decoded.output  = decoded_output;
            decoded.release = [this, decoded_output]()
            {
                if (decoder_.DecQueueOutputForRecycle(decoded_output) != 0)
                {
                    std::cerr << "[" << config_.name
                              << "] 回收解码输出缓冲池盘子失败, fd=" << decoded_output->fd
                              << std::endl;
                }
            };

            ReleaseEncodedFrame(&encoded);

            DecodedFrameRef dropped;
            if (!decoded_queue_.Push(&decoded, &dropped))
            {
                ReleaseDecodedFrame(&decoded);
                break;
            }
            if (HasValidDecodedFrame(dropped))
            {
                decoded_queue_drops_.fetch_add(1, std::memory_order_relaxed);
            }
            ReleaseDecodedFrame(&dropped);
        }

        decoded_queue_.Stop();
    }

    // =========================================================================
    // 线程 3：核心处理 (ProcessLoop)
    // 责任：架构解耦与防死锁（RGA Copy切断依赖） -> 跨流水线调用 NPU 找人 -> 画框
    // =========================================================================
    void ProcessLoop()
    {
        DecodedFrameRef decoded;
        while (decoded_queue_.Pop(&decoded))
        {
            if (ShouldStop())
            {
                ReleaseDecodedFrame(&decoded);
                continue;
            }

            // 【架构精髓：利用 RGA 瞬间硬件 Copy 以释放上游解码器的输出盘子】
            const auto     rga_begin_tp = std::chrono::steady_clock::now();
            const IO_FD_t* rga_output   = nullptr;
            size_t         retry_cnt    = 0;
            while (!ShouldStop())
            {
                rga_output = rga_.Copy(decoded.output);
                if (rga_output)
                    break;

                ++retry_cnt;
                if (retry_cnt > 100)
                {
                    RequestFatalStop("RGA 硬件 Copy 连续失败超过 100 次");
                    break;
                }
                std::this_thread::sleep_for(kRetryBackoffSleep);
            }

            rga_stage_ms_.AddSample(std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - rga_begin_tp)
                                        .count());

            // 立刻归还解码器私有内存，防止解码环节因网络推流慢而卡死
            ReleaseDecodedFrame(&decoded);
            if (!rga_output)
                continue;

            if (!encoder_io_ready_)
            {
                // 将 RGA 专属的缓冲池注册给下游的编码器实现硬件透传
                if (encoder_.AllocBufferForIO(rga_.output_pool_,
                                              resource_limits::kRgaOutputBufferCount) != 0)
                {
                    EncodeFrameRef to_release;
                    to_release.output  = rga_output;
                    to_release.release = [this, rga_output]()
                    { (void)rga_.QueueOutputToRecycle(rga_output); };
                    ReleaseEncodeFrame(&to_release);
                    RequestFatalStop("硬件编码器导入 RGA 零拷贝缓冲池失败");
                    continue;
                }
                encoder_io_ready_ = true;
            }

            if (inference_)
            {
                // 将 FD 交给全局并发管理的 AI
                // 引擎去找人、画框；在重载时优先复用最近一帧结果以稳住实时性。
                const size_t decoded_backlog = decoded_queue_.Size();
                const size_t encode_backlog  = encode_queue_.Size();
                const bool   prefer_cached_infer =
                    decoded_backlog > 0 || encode_backlog > 0 ||
                    decoded_backlog >= kInferReuseQueueDepthThreshold ||
                    encode_backlog >= kInferReuseQueueDepthThreshold;

                const auto infer_begin_tp = std::chrono::steady_clock::now();
                int        infer_ret      = 0;
                bool       reused_infer   = false;
                if (prefer_cached_infer)
                {
                    const int reuse_ret =
                        inference_->TryRenderCachedPersonDetections(rga_output, config_.name);
                    if (reuse_ret > 0)
                    {
                        reused_infer = true;
                        infer_reuse_frames_.fetch_add(1, std::memory_order_relaxed);
                    }
                    else if (reuse_ret < 0)
                    {
                        infer_ret = reuse_ret;
                    }
                }
                if (!reused_infer && infer_ret == 0)
                {
                    infer_ret = inference_->RunPersonDetection(rga_output, config_.name);
                }
                infer_stage_ms_.AddSample(std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - infer_begin_tp)
                                              .count());
                if (infer_ret != 0)
                {
                    infer_failures_.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[" << config_.name << "] NPU 推理出错，但保留原画面继续推流防中断"
                              << std::endl;
                }
            }

            EncodeFrameRef encode_frame;
            encode_frame.output  = rga_output;
            encode_frame.release = [this, rga_output]()
            {
                if (rga_.QueueOutputToRecycle(rga_output) != 0)
                {
                    std::cerr << "[" << config_.name
                              << "] 回收 RGA 输出缓冲盘子失败, fd=" << rga_output->fd << std::endl;
                }
            };

            EncodeFrameRef dropped;
            if (!encode_queue_.Push(&encode_frame, &dropped))
            {
                ReleaseEncodeFrame(&encode_frame);
                break;
            }
            if (HasValidEncodeFrame(dropped))
            {
                encode_queue_drops_.fetch_add(1, std::memory_order_relaxed);
            }
            ReleaseEncodeFrame(&dropped);
        }

        encode_queue_.Stop();
    }

    // =========================================================================
    // 线程 4：硬件编码输入 (EncodeInputLoop)
    // 责任：将已画好目标框的纯净 NV12 高清画面喂给独立 VPU 实例，请求压缩为 H.264
    // =========================================================================
    void EncodeInputLoop()
    {
        EncodeFrameRef encode_frame;
        size_t         consecutive_busy_drops = 0;
        while (encode_queue_.Pop(&encode_frame))
        {
            if (ShouldStop())
            {
                ReleaseEncodeFrame(&encode_frame);
                continue;
            }

            const auto enc_input_begin_tp = std::chrono::steady_clock::now();
            bool       push_ok            = false;
            for (size_t retry = 0; retry < kEncodeBusyRetryLimit && !ShouldStop(); ++retry)
            {
                if (encoder_.EncodePushFrame(encode_frame.output) == 0)
                {
                    push_ok = true;
                    break;
                }
                std::this_thread::sleep_for(kRetryBackoffSleep);
            }

            enc_input_stage_ms_.AddSample(std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - enc_input_begin_tp)
                                              .count());
            if (!push_ok)
            {
                enc_busy_drops_.fetch_add(1, std::memory_order_relaxed);
                ReleaseEncodeFrame(&encode_frame);
                ++consecutive_busy_drops;
                if (consecutive_busy_drops >= kEncodeBusyDropFatalThreshold)
                {
                    enc_push_failures_.fetch_add(1, std::memory_order_relaxed);
                    RequestFatalStop("硬件编码器持续繁忙，丢帧超过阈值");
                    break;
                }
                continue;
            }

            consecutive_busy_drops = 0;
            ReleaseEncodeFrame(&encode_frame);
        }

        encode_input_done_.store(true);
    }

    // =========================================================================
    // 线程 5：推流派发 (EncodeOutputLoop)
    // 责任：接收 VPU 吐出的 H.264 NALU 数据段，组装并抛入进程内嵌的 ZLMediaKit 服务器
    // =========================================================================
    void EncodeOutputLoop()
    {
        auto                 last_stat_tp     = std::chrono::steady_clock::now();
        uint64_t             last_frame_count = 0;
        std::vector<uint8_t> pending_au;
        int64_t              pending_dts_us        = -1;
        int64_t              pending_pts_us        = -1;
        bool                 pending_eos           = false;
        size_t               idle_after_input_done = 0;

        auto flush_pending_au = [&]()
        {
            if (pending_au.empty())
                return;

            EncPacketView au_view;
            au_view.data         = pending_au.data();
            au_view.len          = pending_au.size();
            au_view.dts_us       = pending_dts_us;
            au_view.pts_us       = pending_pts_us;
            au_view.eos          = pending_eos;
            au_view.is_partition = false;
            au_view.is_eoi       = true;
            au_view.is_extra     = false;
            au_view.handle       = nullptr;

            if (publisher_.InputPacketChunk(au_view) != 0)
            {
                std::cerr << "[" << config_.name << "] 向 ZLM 网络引擎推送 AU 数据失败"
                          << std::endl;
            }

            pending_au.clear();
            pending_dts_us = -1;
            pending_pts_us = -1;
            pending_eos    = false;
        };

        while (true)
        {
            const bool stop_now   = ShouldStop();
            const bool input_done = encode_input_done_.load();

            EncPacketView pkt_view;
            const int     get_packet_ret = encoder_.EncoderGetPacket(&pkt_view);
            if (get_packet_ret == mpp_enc_packet_result::kError)
            {
                if (stop_now && input_done)
                    break;

                RequestFatalStop("从硬件编码器获取 H.264 输出包失败");
                break;
            }
            if (get_packet_ret == mpp_enc_packet_result::kNoPacket)
            {
                if (input_done)
                {
                    ++idle_after_input_done;
                    if (stop_now || idle_after_input_done > 10)
                    {
                        flush_pending_au();
                        break;
                    }
                }
                std::this_thread::sleep_for(kEncodeNoPacketSleep);
                continue;
            }
            idle_after_input_done = 0;

            if (!pkt_view.is_partition)
            {
                if (!pending_au.empty())
                {
                    flush_pending_au();
                }

                if (publisher_.InputPacketChunk(pkt_view) != 0)
                {
                    std::cerr << "[" << config_.name << "] 向 ZLM 网络引擎推送 AU 数据失败"
                              << std::endl;
                }

                if (encoder_.EncoderReleasePacket(&pkt_view) != 0)
                {
                    RequestFatalStop("释放 MPP 编码器缓存包结构失败");
                    break;
                }
            }
            else
            {
                if (pending_au.empty())
                {
                    pending_dts_us = pkt_view.dts_us;
                    pending_pts_us = pkt_view.pts_us;
                    pending_eos    = pkt_view.eos;
                }
                else
                {
                    pending_eos = pending_eos || pkt_view.eos;
                    if (pending_dts_us < 0 && pkt_view.dts_us >= 0)
                        pending_dts_us = pkt_view.dts_us;
                    if (pending_pts_us < 0 && pkt_view.pts_us >= 0)
                        pending_pts_us = pkt_view.pts_us;
                }

                const uint8_t* packet_bytes = static_cast<const uint8_t*>(pkt_view.data);
                pending_au.insert(pending_au.end(), packet_bytes, packet_bytes + pkt_view.len);

                const bool packet_is_eoi = pkt_view.is_eoi;
                const bool packet_is_eos = pkt_view.eos;

                if (encoder_.EncoderReleasePacket(&pkt_view) != 0)
                {
                    RequestFatalStop("释放 MPP 编码器缓存包结构失败");
                    break;
                }

                if (packet_is_eoi || packet_is_eos)
                    flush_pending_au();
            }

            const auto now_tp = std::chrono::steady_clock::now();
            if (now_tp - last_stat_tp >= std::chrono::seconds(5))
            {
                // 1. 计算精确的流逝时间（秒）
                double elapsed_sec = std::chrono::duration<double>(now_tp - last_stat_tp).count();

                // 2. 获取当前总帧数
                uint64_t current_frames = publisher_.GetOutputFrameCount();

                // 3. 计算实时帧率：(当前总帧数 - 上次总帧数) / 经过的秒数
                double real_time_fps = (current_frames - last_frame_count) / elapsed_sec;

                // 4. 打印格式化的结果
                std::cout << "[" << config_.name << "] 实时推流: " << std::fixed
                          << std::setprecision(1) << real_time_fps << " FPS"
                          << " (累计: " << current_frames
                          << "帧, TS修复: " << publisher_.GetTimestampFallbackCount() << "次)"
                          << std::endl;

                // 5. 更新历史记录，用于下一次计算
                last_stat_tp     = now_tp;
                last_frame_count = current_frames;
            }
        }

        finished_.store(true);
    }

    void AudioLoop()
    {
        if (!audio_capture_ || !audio_enabled_)
        {
            return;
        }

        std::cout << "[" << config_.name << "] 音频线程启动" << std::endl;
        std::cout << "[" << config_.name << "] 音频轻量去噪已启用, mode=dc-block+noise-gate"
                  << std::endl;

        const auto&    audio_config = audio_capture_->GetConfig();
        const uint32_t sample_rate =
            audio_config.sample_rate ? audio_config.sample_rate : kDefaultAudioSampleRate;
        const uint32_t channels = audio_config.channels ? audio_config.channels : 1;

        std::vector<int16_t> pcm_samples;
        std::vector<uint8_t> g711_frame;
        uint64_t             audio_capture_failures       = 0;
        uint64_t             audio_samples_sent           = 0;
        uint64_t             audio_base_dts_ms            = 0;
        uint64_t             audio_correction_frame_acc   = 0;
        uint64_t             audio_fast_catchup_hits      = 0;
        uint64_t             audio_zero_cross_hold_frames = 0;
        int                  last_av_gap_sign             = 0;
        int64_t              last_audio_dts_ms            = -1;
        int64_t              smoothed_av_gap_ms           = 0;
        bool                 smoothed_av_gap_ready        = false;
        const int64_t extra_sync_offset_ms  = static_cast<int64_t>(config_.audio_sync_offset_ms);
        bool          audio_timeline_locked = false;
        bool          logged_first_capture  = false;
        bool          logged_first_encode   = false;
        bool          logged_first_push     = false;
        bool          logged_wait_timeline  = false;
        while (!ShouldStop())
        {
            const int capture_ret = audio_capture_->CapturePcmFrame(&pcm_samples);
            if (capture_ret != 0)
            {
                ++audio_capture_failures;
                if (ShouldStop())
                {
                    break;
                }
                if (audio_capture_failures <= 5 || (audio_capture_failures % 50) == 0)
                {
                    std::cerr << "[" << config_.name << "] 音频采集失败，第"
                              << audio_capture_failures << "次" << std::endl;
                }
                std::this_thread::sleep_for(kRetryBackoffSleep);
                continue;
            }
            if (!logged_first_capture)
            {
                logged_first_capture = true;
                std::cout << "[" << config_.name
                          << "] 首次采集到音频样本, samples=" << pcm_samples.size() << std::endl;
            }

            DenoisePcmForSpeech(&pcm_samples, channels, &audio_denoise_);
            EncodePcmToG711A(pcm_samples.data(), pcm_samples.size(), &g711_frame);
            if (g711_frame.empty())
            {
                continue;
            }
            if (!logged_first_encode)
            {
                logged_first_encode = true;
                std::cout << "[" << config_.name
                          << "] 首次编码 G711A 成功, bytes=" << g711_frame.size() << std::endl;
            }

            const uint64_t frame_samples_per_channel =
                static_cast<uint64_t>(pcm_samples.size() / channels);
            if (frame_samples_per_channel == 0)
            {
                continue;
            }

            if (!audio_timeline_locked)
            {
                if (!publisher_.HasVideoTimeline())
                {
                    if (!logged_wait_timeline)
                    {
                        logged_wait_timeline = true;
                        std::cout << "[" << config_.name << "] 等待视频时间轴就绪后再对齐音频"
                                  << std::endl;
                    }
                    continue;
                }

                const int64_t capture_delay_ms = std::clamp<int64_t>(
                    audio_capture_->GetCaptureDelayMs(), 0, kAudioSyncMaxCaptureDelayMs);
                const int64_t total_offset_ms = capture_delay_ms + extra_sync_offset_ms;
                const int64_t current_video_dts_ms =
                    static_cast<int64_t>(publisher_.GetEstimatedVideoDtsMs());
                audio_base_dts_ms =
                    current_video_dts_ms > total_offset_ms
                        ? static_cast<uint64_t>(current_video_dts_ms - total_offset_ms)
                        : 0;
                audio_timeline_locked = true;
                av_sync_ready_.store(true, std::memory_order_relaxed);
                std::cout << "[" << config_.name
                          << "] 音频时间轴已对齐到视频, base_dts_ms=" << audio_base_dts_ms
                          << ", sample_rate=" << sample_rate << ", channels=" << channels
                          << ", capture_delay_ms=" << capture_delay_ms
                          << ", extra_offset_ms=" << extra_sync_offset_ms << std::endl;
            }

            uint64_t dts_ms = audio_base_dts_ms + (audio_samples_sent * 1000ULL) / sample_rate;
            audio_samples_sent += frame_samples_per_channel;
            if (last_audio_dts_ms >= 0 && dts_ms <= static_cast<uint64_t>(last_audio_dts_ms))
            {
                dts_ms = static_cast<uint64_t>(last_audio_dts_ms + 1);
            }
            if (publisher_.HasVideoTimeline())
            {
                const int64_t video_dts_ms =
                    static_cast<int64_t>(publisher_.GetEstimatedVideoDtsMs());
                const int64_t av_gap_ms = static_cast<int64_t>(dts_ms) - video_dts_ms;
                av_gap_ms_current_.store(av_gap_ms, std::memory_order_relaxed);
                const double av_gap_abs_ms =
                    static_cast<double>(av_gap_ms >= 0 ? av_gap_ms : -av_gap_ms);
                av_gap_abs_ms_.AddSample(av_gap_abs_ms);

                if (!smoothed_av_gap_ready)
                {
                    smoothed_av_gap_ms    = av_gap_ms;
                    smoothed_av_gap_ready = true;
                }
                else
                {
                    smoothed_av_gap_ms = (smoothed_av_gap_ms * 3 + av_gap_ms) / 4;
                }

                ++audio_correction_frame_acc;
                const int64_t abs_av_gap_ms       = std::llabs(av_gap_ms);
                const int     current_av_gap_sign = (av_gap_ms > 0) - (av_gap_ms < 0);
                if (current_av_gap_sign != 0 && last_av_gap_sign != 0 &&
                    current_av_gap_sign != last_av_gap_sign)
                {
                    audio_zero_cross_hold_frames = kAudioSyncZeroCrossHoldFrames;
                }
                if (current_av_gap_sign != 0)
                {
                    last_av_gap_sign = current_av_gap_sign;
                }

                const bool in_zero_cross_hold = audio_zero_cross_hold_frames > 0 &&
                                                abs_av_gap_ms < kAudioSyncFastCatchupThresholdMs;

                if (abs_av_gap_ms >= kAudioSyncFastCatchupThresholdMs && !in_zero_cross_hold)
                {
                    ++audio_fast_catchup_hits;
                }
                else
                {
                    audio_fast_catchup_hits = 0;
                }

                int64_t correction_step = 0;
                if (!in_zero_cross_hold &&
                    (abs_av_gap_ms >= kAudioSyncHardCatchupThresholdMs ||
                     (audio_fast_catchup_hits >= kAudioSyncFastCatchupConfirmFrames &&
                      abs_av_gap_ms >= kAudioSyncFastCatchupThresholdMs)))
                {
                    audio_correction_frame_acc = 0;
                    audio_fast_catchup_hits    = 0;
                    const int64_t basis_gap_ms = abs_av_gap_ms >= kAudioSyncHardCatchupThresholdMs
                                                     ? av_gap_ms
                                                     : smoothed_av_gap_ms;
                    correction_step =
                        std::clamp<int64_t>(basis_gap_ms / 4, -kAudioSyncFastCorrectionStepMs,
                                            kAudioSyncFastCorrectionStepMs);
                }
                else if (audio_correction_frame_acc >= kAudioSyncCorrectionInterval)
                {
                    audio_correction_frame_acc = 0;
                    if (!in_zero_cross_hold &&
                        std::llabs(smoothed_av_gap_ms) > kAudioSyncToleranceMs)
                    {
                        correction_step = std::clamp<int64_t>(smoothed_av_gap_ms / 8,
                                                              -kAudioSyncMaxCorrectionStepMs,
                                                              kAudioSyncMaxCorrectionStepMs);
                    }
                }

                if (audio_zero_cross_hold_frames > 0)
                {
                    --audio_zero_cross_hold_frames;
                }

                if (correction_step > 0)
                {
                    audio_base_dts_ms =
                        audio_base_dts_ms > static_cast<uint64_t>(correction_step)
                            ? audio_base_dts_ms - static_cast<uint64_t>(correction_step)
                            : 0;
                }
                else if (correction_step < 0)
                {
                    audio_base_dts_ms += static_cast<uint64_t>(-correction_step);
                }
            }
            last_audio_dts_ms = static_cast<int64_t>(dts_ms);
            if (publisher_.InputAudioFrame(g711_frame.data(), g711_frame.size(), dts_ms) != 0)
            {
                std::cerr << "[" << config_.name << "] 音频推流失败，保留视频链路继续运行"
                          << std::endl;
                std::this_thread::sleep_for(kRetryBackoffSleep);
                continue;
            }
            if (!logged_first_push)
            {
                logged_first_push = true;
                std::cout << "[" << config_.name
                          << "] 首次音频帧送入 ZLM 成功, bytes=" << g711_frame.size()
                          << ", dts_ms=" << dts_ms << std::endl;
            }
        }
    }

    PipelineConfig                  config_;
    std::unique_ptr<IEncodedSource> source_;
    SharedInferenceEngine*          inference_   = nullptr;
    std::atomic_bool*               global_stop_ = nullptr;
    SourceVideoInfo                 video_info_  = {};

    // 每个流水线都会独占这 4 个极度消耗状态机和硬件指令的组件实体
    MppDecInstance                    decoder_;
    RgaInstance                       rga_;
    MppEncInstance                    encoder_;
    ZlmPublisher                      publisher_;
    std::unique_ptr<AlsaAudioCapture> audio_capture_;

    bool encoder_io_ready_ = false;
    bool audio_enabled_    = false;

    std::atomic<int64_t> av_gap_ms_current_{0};
    std::atomic_bool     av_sync_ready_{false};
    std::atomic_bool     local_stop_{false};
    std::atomic_bool     fatal_error_{false};
    std::atomic_bool     finished_{false};
    std::atomic_bool     encode_input_done_{false};

    std::atomic<uint64_t> source_frame_count_{0};
    std::atomic<uint64_t> decode_failures_{0};
    std::atomic<uint64_t> infer_failures_{0};
    std::atomic<uint64_t> enc_push_failures_{0};
    std::atomic<uint64_t> encoded_queue_drops_{0};
    std::atomic<uint64_t> decoded_queue_drops_{0};
    std::atomic<uint64_t> encode_queue_drops_{0};
    std::atomic<uint64_t> infer_reuse_frames_{0};
    std::atomic<uint64_t> enc_busy_drops_{0};
    WindowMetricCollector dec_stage_ms_;
    WindowMetricCollector rga_stage_ms_;
    WindowMetricCollector infer_stage_ms_;
    WindowMetricCollector enc_input_stage_ms_;
    WindowMetricCollector av_gap_abs_ms_;
    AudioDenoiseState     audio_denoise_;

    BoundedQueue<EncodedFrameRef> encoded_queue_;
    BoundedQueue<DecodedFrameRef> decoded_queue_;
    BoundedQueue<EncodeFrameRef>  encode_queue_;

    std::thread source_thread_;
    std::thread decode_thread_;
    std::thread process_thread_;
    std::thread encode_input_thread_;
    std::thread encode_output_thread_;
    std::thread audio_thread_;
};
}  // namespace

// =========================================================================
// Main 调度函数：面向对象启动器
// =========================================================================
int main(int argc, char** argv)
{
    // 1.信号注册（优雅退出）
    std::signal(SIGINT, SignalHandler);

    // 2.解析命令行参数
    const ProgramOptions options = ParseArgs(argc, argv);
    ApplyZlmConfigSelection(options);
    // 如果 input_url 不为空，则启用第二路；否则禁用。
    const bool enable_file_pipeline = !options.input_url.empty();
    if (!enable_file_pipeline)
    {
        std::cout << "[INFO] 第二路已禁用(--single-link/--no-second-link)，系统将以单路(摄像头)模式运行"
                  << std::endl;
    }
    else
    {
        std::cout << "[INFO] 双路推流已启用: camera=" << options.camera_device
                  << ", file/input=" << options.input_url << std::endl;
    }

    // 3.全局共享的 AI 推理引擎 (单例化)
    SharedInferenceEngine  inference;
    SharedInferenceEngine* inference_ptr = nullptr;
    if (options.enable_inference)
    {
        if (inference.Init(options.model_path, options.rknn_cpu_input) != 0)
        {
            std::cerr << "RKNN AI 模型载入与 NPU 初始化失败，自动降级为纯视频推流: "
                      << options.model_path << std::endl;
        }
        else
        {
            inference_ptr = &inference;
        }
    }
    else
    {
        std::cout << "[RKNN] 人体识别已禁用(--no-infer)，保留纯视频推流" << std::endl;
    }

    // 4. 全局停机同步标志
    std::atomic_bool global_stop{false};

    // 5. 实例化多态数据源 (智能指针管理)
    auto camera_source = std::make_unique<CameraEncodedSource>(
        options.camera_device);  // 实例化CameraEncodedSource对象
    auto file_source = std::make_unique<FileEncodedSource>(FfmpegFileSourceConfig{
        options.input_url, options.file_loop, true, 10000, 10000});  // 实例化FileEncodedSource对象

    // =========================================================================
    // 实例化两条“物理平行”的零拷贝处理车道
    // 两路视频将带着各自的进程标识与网络端口，跑满 RK3588 的算力上限
    // =========================================================================
    // 6. 拼装流水线 (依赖注入)
    // 每个流水线内部包括多个线程，将准备好的零散资源塞进这两条流水线里
    VideoPipeline camera_pipeline(
        PipelineConfig{"camera", "camera", kDefaultQueueDepth, kDefaultQueueDepth,
                       kDefaultQueueDepth, options.enable_audio, options.audio_device,
                       options.audio_sync_offset_ms},
        std::move(camera_source), inference_ptr,
        &global_stop);  // 第一个camera是流水线的名称（流水线id），第二个camera交给底层 ZLMediaKit
                        // 的 stream_name，最终可以通过 rtsp://IP/live/camera 被外部拉取视频流
    VideoPipeline file_pipeline(
        PipelineConfig{"file", "file", kDefaultQueueDepth, kDefaultQueueDepth, kDefaultQueueDepth,
                       false, "", static_cast<int>(kDefaultAudioSyncOffsetMs)},
        std::move(file_source), inference_ptr, &global_stop);

    bool camera_failed  = false;
    bool file_failed    = !enable_file_pipeline;
    bool camera_started = false;
    bool file_started   = false;

    // 分离启动，一路失败绝对不能拖死另一路的安防推流
    // 7. 分离式启动链路 1
    if (camera_pipeline.Init() != 0)
    {
        camera_failed = true;
        std::cerr << "[WARN] [camera] 链路1 (摄像头) 初始化失败，但不会影响链路2继续运行"
                  << std::endl;
    }
    else
    {
        camera_pipeline.Start();
        camera_started = true;
    }

    // 8. 分离式启动链路 2
    if (enable_file_pipeline)
    {
        if (file_pipeline.Init() != 0)
        {
            file_failed = true;
            std::cerr << "[WARN] [file] 链路2 (视频/网络) 初始化失败，但不会影响链路1继续运行"
                      << std::endl;
        }
        else
        {
            file_pipeline.Start();
            file_started = true;
        }
    }

    // 9. 极限判定：两条链路全部失效才退出
    if (camera_failed && file_failed)
    {
        std::cerr << "[FATAL] 所有多媒体链路均不可用，进程安全退出" << std::endl;
        return 1;
    }

    const bool  monitor_camera_pipeline = camera_started;
    const bool  monitor_file_pipeline   = enable_file_pipeline && file_started;
    std::thread perf_monitor_thread(
        [&]()
        {
            SystemPerfMonitor system_monitor;
            auto              last_log_tp     = std::chrono::steady_clock::now();
            uint64_t          last_camera_out = 0;
            uint64_t          last_file_out   = 0;

            auto print_pipeline_snapshot = [&](const PipelinePerfSnapshot& snapshot,
                                               uint64_t* last_output_frames, double elapsed_sec)
            {
                if (!last_output_frames || elapsed_sec <= 0.0)
                {
                    return;
                }

                const double in_fps = static_cast<double>(snapshot.source_frames) / elapsed_sec;
                const double out_fps =
                    static_cast<double>(snapshot.output_frames_total - *last_output_frames) /
                    elapsed_sec;
                *last_output_frames = snapshot.output_frames_total;

                std::cout << "[MON][" << snapshot.name
                          << "] in_fps=" << FormatDoubleValue(in_fps, 1)
                          << ", out_fps=" << FormatDoubleValue(out_fps, 1)
                          << ", dec_ms(avg/p90/max)=" << FormatStageStats(snapshot.dec_ms)
                          << ", rga_ms(avg/p90/max)=" << FormatStageStats(snapshot.rga_ms)
                          << ", infer_ms(avg/p90/max)=" << FormatStageStats(snapshot.infer_ms)
                          << ", enc_in_ms(avg/p90/max)=" << FormatStageStats(snapshot.enc_in_ms);
                if (snapshot.av_sync_ready)
                {
                    std::cout << ", av_gap_ms(cur/avg/p90/maxabs)=" << snapshot.av_gap_ms_current
                              << "/" << FormatStageStats(snapshot.av_gap_abs_ms);
                }
                else
                {
                    std::cout << ", av_gap_ms=na";
                }
                std::cout << ", q_depth=" << snapshot.encoded_q_depth << "/"
                          << snapshot.decoded_q_depth << "/" << snapshot.encode_q_depth
                          << ", q_drop=" << snapshot.encoded_queue_drops << "/"
                          << snapshot.decoded_queue_drops << "/" << snapshot.encode_queue_drops
                          << ", fail(dec/infer/enc)=" << snapshot.decode_failures << "/"
                          << snapshot.infer_failures << "/" << snapshot.enc_push_failures
                          << ", rt_opt(reuse/enc_drop)=" << snapshot.infer_reuse_frames << "/"
                          << snapshot.enc_busy_drops
                          << ", out_total=" << snapshot.output_frames_total
                          << ", ts_fix_total=" << snapshot.timestamp_fixups_total;
                if (snapshot.fatal)
                {
                    std::cout << ", fatal=1";
                }
                if (snapshot.finished)
                {
                    std::cout << ", finished=1";
                }
                std::cout << std::endl;
            };

            auto emit_monitor_log = [&](bool force)
            {
                const auto now_tp    = std::chrono::steady_clock::now();
                const auto elapsed_s = std::chrono::duration<double>(now_tp - last_log_tp).count();
                if ((!force && elapsed_s < 5.0) || elapsed_s <= 0.0 || (force && elapsed_s < 1.0))
                {
                    return;
                }

                const SystemPerfSnapshot sys_snapshot = system_monitor.Sample();
                std::cout << "[MON][SYS] cpu_total_pct="
                          << FormatDoubleValue(sys_snapshot.cpu_total_pct, 1)
                          << ", cpu_proc_pct=" << FormatDoubleValue(sys_snapshot.proc_cpu_pct, 1)
                          << ", rss_mb="
                          << FormatDoubleValue(static_cast<double>(sys_snapshot.rss_kb) / 1024.0, 1)
                          << ", vmhwm_mb="
                          << FormatDoubleValue(static_cast<double>(sys_snapshot.vmhwm_kb) / 1024.0,
                                               1)
                          << ", cpu_temp_c=" << FormatDoubleValue(sys_snapshot.cpu_temp_c, 1)
                          << std::endl;

                if (monitor_camera_pipeline)
                {
                    print_pipeline_snapshot(camera_pipeline.ConsumePerfSnapshot(), &last_camera_out,
                                            elapsed_s);
                }
                if (monitor_file_pipeline)
                {
                    print_pipeline_snapshot(file_pipeline.ConsumePerfSnapshot(), &last_file_out,
                                            elapsed_s);
                }
                last_log_tp = now_tp;
            };

            while (!global_stop.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                emit_monitor_log(false);
            }
            emit_monitor_log(true);
        });

    // 主线程挂起充当监控狗（WatchDog）
    // 10. 主线程陷入极低功耗的监视循环
    while (!global_stop.load())  //.load() (加载/读取)这个变量当前的值
    {
        // 捕获到 Ctrl+C
        if (g_should_exit)
        {
            global_stop.store(true);  //.store(true) (存储/写入)把这个变量的值修改为 true
            break;
        }

        // 11. 巡检链路 1 的健康状态
        if (camera_started && camera_pipeline.HasFatalError())
        {
            camera_failed  = true;
            camera_started = false;
            std::cerr << "[WARN] [camera] 链路1遭遇致命崩溃已停机保护，链路2若健康则继续推流"
                      << std::endl;
        }
        else if (camera_started && camera_pipeline.IsFinished())
        {
            camera_failed  = true;
            camera_started = false;
            std::cerr << "[INFO] [camera] 链路1已跑完数据，正常结束" << std::endl;
        }

        // 12. 巡检链路 2 的健康状态
        if (file_started && file_pipeline.HasFatalError())
        {
            file_failed  = true;
            file_started = false;
            std::cerr << "[WARN] [file] 链路2遭遇致命崩溃已停机保护，链路1若健康则继续推流"
                      << std::endl;
        }
        else if (file_started && file_pipeline.IsFinished())
        {
            file_failed  = true;
            file_started = false;
            std::cerr << "[INFO] [file] 链路2已跑完数据，正常结束" << std::endl;
        }

        // 13. 如果两路都阵亡/结束，触发全局停机
        if (camera_failed && file_failed)
        {
            std::cerr << "[INFO] 所有的流水线均已结束，监控狗即将退出主进程" << std::endl;
            global_stop.store(true);
            break;
        }

        // 14. 极低功耗休眠
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (perf_monitor_thread.joinable())
    {
        perf_monitor_thread.join();
    }

    // =========================================================================
    // 资源优雅退出：按序通知销毁各个流水线内部的线程池，防止内核引发段错误
    // =========================================================================
    camera_pipeline.Stop();
    file_pipeline.Stop();
    camera_pipeline.Join();
    file_pipeline.Join();
    return 0;
}
