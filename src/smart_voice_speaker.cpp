#include <fcntl.h>
#include <glob.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app_state.h"
#include "app_config.h"
#include "app_utils.h"
#include "assistant_client.h"
#include "command_router.h"
#include "music_player.h"

#ifdef SMART_SPEAKER_USE_ALSA_PCM
#include <alsa/asoundlib.h>
#endif

#ifdef SMART_SPEAKER_USE_OPENCV
#ifdef SMART_SPEAKER_USE_RKNN_PERSON
#include "person_detector.h"
#endif
#ifdef SMART_SPEAKER_USE_RKNN_EMOTION
#include "emotion_detector.h"
#endif
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#endif

#ifdef SMART_SPEAKER_USE_SHERPA_ONNX_C_API
#include <sherpa-onnx/c-api/c-api.h>
#endif

using namespace xiaoman;

namespace
{
constexpr const char *kAssistantName = "星期五";
constexpr const char *kDefaultMicDevice = "plughw:2,0";
constexpr const char *kDefaultCameraDev = "/dev/video0";
constexpr const char *kDefaultLcdSpiDev = "/dev/spidev3.0";
constexpr const char *kDefaultUiCommandFifo = "/tmp/friday_voice_speaker_ui.fifo";
constexpr const char *kDefaultDesktopControlFifo = "/tmp/friday_voice_speaker_desktop.fifo";
constexpr int kSampleRate = 16000;
constexpr int kWakeRecordSeconds = 3;
constexpr int kWakeMaxListenSeconds = 12;
constexpr int kDialogueRecordSeconds = 8;
constexpr int kDialogueMaxListenSeconds = 15;
constexpr int kPreRollMs = 300;
constexpr int kMinVoiceMs = 650;
constexpr int kAsrMinInputMs = 1200;
constexpr int kSilenceStopMs = 900;
constexpr int kVoiceConfirmMs = 120;
constexpr float kVoiceStartRms = 0.012f;
constexpr float kVoiceStopRms = 0.006f;
constexpr float kVoiceMinAvgRms = 0.006f;
constexpr float kVoiceMinPeakRms = 0.018f;
constexpr float kVoiceNoiseRatio = 3.0f;
constexpr int kLcdDcGpio = 128;
constexpr int kLcdResGpio = 130;
constexpr int kDefaultLcdWidth = 240;
constexpr int kDefaultLcdHeight = 240;
constexpr uint8_t kDefaultLcdMadctl = 0x68;
constexpr uint32_t kLcdSpiHz = 40U * 1000U * 1000U;
constexpr int kCameraWidth = 640;
constexpr int kCameraHeight = 480;
constexpr int kCameraFps = 15;
constexpr int kVisionJpegQuality = 75;
constexpr int kVisionCachedFrameMaxAgeMs = 2500;
constexpr int kPersonDetectIntervalMs = 350;
constexpr int kEmotionDetectIntervalMs = 500;
constexpr const char *kDefaultEmojiStates = "正常,微笑,眨眼,兴奋,睡觉,苏醒";
constexpr int kJpgEmojiIntervalMs = 50;
constexpr int kGeometryFaceIntervalMs = 350;

void set_current_thread_name(const char *name)
{
#ifdef __linux__
    (void)::prctl(PR_SET_NAME, name, 0, 0, 0);
#else
    (void)name;
#endif
}

int parse_int_auto_base(const std::string &text, int default_value)
{
    const std::string value = trim(text);
    if (value.empty())
    {
        return default_value;
    }
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 0);
    if (errno != 0 || end == value.c_str() || *end != '\0' ||
        parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max())
    {
        return default_value;
    }
    return static_cast<int>(parsed);
}

int env_int_auto_base(std::initializer_list<const char *> names, int default_value)
{
    return parse_int_auto_base(env_first(names), default_value);
}

float parse_float(const std::string &text, float default_value)
{
    const std::string value = trim(text);
    if (value.empty())
    {
        return default_value;
    }
    char *end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value.c_str(), &end);
    if (errno != 0 || end == value.c_str() || *end != '\0' || !std::isfinite(parsed))
    {
        return default_value;
    }
    return parsed;
}

float env_float(std::initializer_list<const char *> names, float default_value)
{
    return parse_float(env_first(names), default_value);
}

std::string desktop_control_fifo_path()
{
    std::string path = trim(env_first({"XIAOMAN_DESKTOP_CONTROL_FIFO", "NEWBOT_DESKTOP_CONTROL_FIFO"}));
    if (path.empty())
    {
        path = kDefaultDesktopControlFifo;
    }
    return path;
}

void send_desktop_control_command(const std::string &command)
{
    const std::string trimmed = trim(command);
    if (trimmed.empty())
    {
        return;
    }

    const std::string path = desktop_control_fifo_path();
    const int fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        if (errno != ENOENT && errno != ENXIO)
        {
            std::cerr << "warning: desktop control fifo open failed: " << path
                      << ": " << std::strerror(errno) << "\n";
        }
        return;
    }

    const std::string payload = trimmed + "\n";
    const ssize_t written = ::write(fd, payload.data(), payload.size());
    ::close(fd);
    if (written != static_cast<ssize_t>(payload.size()))
    {
        std::cerr << "warning: desktop control fifo write failed: " << path
                  << ": " << std::strerror(errno) << "\n";
    }
}

struct PcmAudio
{
    int sample_rate = kSampleRate;
    std::vector<float> samples;
    float avg_rms = 0.0f;
    float peak_rms = 0.0f;
};

struct RecordOptions
{
    int record_seconds = kDialogueRecordSeconds;
    int max_listen_seconds = kDialogueMaxListenSeconds;
    bool interrupt_on_manual_wake = false;
};

struct VoiceGateConfig
{
    float start_rms = kVoiceStartRms;
    float stop_rms = kVoiceStopRms;
    float min_avg_rms = kVoiceMinAvgRms;
    float min_peak_rms = kVoiceMinPeakRms;
    float noise_ratio = kVoiceNoiseRatio;
    int pre_roll_ms = kPreRollMs;
    int min_voice_ms = kMinVoiceMs;
    int silence_stop_ms = kSilenceStopMs;
    int confirm_ms = kVoiceConfirmMs;
    bool debug = false;
};

struct CameraConfig
{
    std::string dev = kDefaultCameraDev;
    int width = kCameraWidth;
    int height = kCameraHeight;
    int fps = kCameraFps;
    bool mjpeg = true;
};

struct WakeButtonConfig
{
    int gpio = -1;
    bool active_low = true;
    int poll_ms = 20;
    int debounce_ms = 180;
};

struct LcdGeometry
{
    std::string driver = "gc9a01";
    int width = kDefaultLcdWidth;
    int height = kDefaultLcdHeight;
    int x_offset = 0;
    int y_offset = 0;
    uint8_t madctl = kDefaultLcdMadctl;
    uint32_t spi_hz = kLcdSpiHz;
    int dc_gpio = kLcdDcGpio;
    int reset_gpio = kLcdResGpio;
    int backlight_gpio = -1;
    bool backlight_active_high = true;

    size_t pixel_count() const
    {
        return static_cast<size_t>(width) * static_cast<size_t>(height);
    }

    size_t rgb565_byte_count() const
    {
        return pixel_count() * 2;
    }

    bool valid() const
    {
        return width > 0 && height > 0 && width <= 1024 && height <= 1024;
    }
};

struct CpuTimes
{
    uint64_t idle = 0;
    uint64_t total = 0;
};

struct SystemStats
{
    double cpu_percent = 0.0;
    double one_core_equiv_percent = 0.0;
    double cpu_temp_c = 0.0;
    double mem_percent = 0.0;
    double load_1m = 0.0;
    int cpu_mhz = 0;
    int cpu_cores = 0;
    uint64_t uptime_seconds = 0;
    bool temp_valid = false;
    bool mem_valid = false;
};

VoiceGateConfig load_voice_gate_config()
{
    VoiceGateConfig cfg;
    cfg.start_rms = std::max(0.0001f, env_float({"XIAOMAN_VOICE_START_RMS", "NEWBOT_VOICE_START_RMS"}, cfg.start_rms));
    cfg.stop_rms = std::max(0.0001f, env_float({"XIAOMAN_VOICE_STOP_RMS", "NEWBOT_VOICE_STOP_RMS"}, cfg.stop_rms));
    cfg.min_avg_rms = std::max(0.0001f, env_float({"XIAOMAN_VOICE_MIN_AVG_RMS", "NEWBOT_VOICE_MIN_AVG_RMS"}, cfg.min_avg_rms));
    cfg.min_peak_rms = std::max(0.0001f, env_float({"XIAOMAN_VOICE_MIN_PEAK_RMS", "NEWBOT_VOICE_MIN_PEAK_RMS"}, cfg.min_peak_rms));
    cfg.noise_ratio = std::max(1.0f, env_float({"XIAOMAN_VOICE_NOISE_RATIO", "NEWBOT_VOICE_NOISE_RATIO"}, cfg.noise_ratio));
    cfg.pre_roll_ms = std::max(0, env_int({"XIAOMAN_VOICE_PRE_ROLL_MS", "NEWBOT_VOICE_PRE_ROLL_MS"}, cfg.pre_roll_ms));
    cfg.min_voice_ms = std::max(100, env_int({"XIAOMAN_VOICE_MIN_MS", "NEWBOT_VOICE_MIN_MS"}, cfg.min_voice_ms));
    cfg.silence_stop_ms = std::max(100, env_int({"XIAOMAN_VOICE_SILENCE_STOP_MS", "NEWBOT_VOICE_SILENCE_STOP_MS"}, cfg.silence_stop_ms));
    cfg.confirm_ms = std::max(20, env_int({"XIAOMAN_VOICE_CONFIRM_MS", "NEWBOT_VOICE_CONFIRM_MS"}, cfg.confirm_ms));
    cfg.debug = env_bool({"XIAOMAN_VOICE_DEBUG", "NEWBOT_VOICE_DEBUG"}, cfg.debug);
    if (cfg.stop_rms > cfg.start_rms)
    {
        cfg.stop_rms = cfg.start_rms * 0.5f;
    }
    return cfg;
}

LcdGeometry load_lcd_geometry()
{
    LcdGeometry geometry;
    const std::string driver = to_lower_ascii(trim(env_first({"XIAOMAN_LCD_DRIVER", "NEWBOT_LCD_DRIVER"})));
    if (!driver.empty())
    {
        geometry.driver = driver;
    }
    geometry.width = env_int({"XIAOMAN_LCD_WIDTH", "NEWBOT_LCD_WIDTH"}, geometry.width);
    geometry.height = env_int({"XIAOMAN_LCD_HEIGHT", "NEWBOT_LCD_HEIGHT"}, geometry.height);
    geometry.x_offset = env_int({"XIAOMAN_LCD_X_OFFSET", "NEWBOT_LCD_X_OFFSET"}, geometry.x_offset);
    geometry.y_offset = env_int({"XIAOMAN_LCD_Y_OFFSET", "NEWBOT_LCD_Y_OFFSET"}, geometry.y_offset);
    geometry.madctl = static_cast<uint8_t>(
        env_int_auto_base({"XIAOMAN_LCD_MADCTL", "NEWBOT_LCD_MADCTL"}, geometry.madctl) & 0xff);
    geometry.spi_hz = static_cast<uint32_t>(
        std::max(1000000, env_int({"XIAOMAN_LCD_SPI_HZ", "NEWBOT_LCD_SPI_HZ"}, static_cast<int>(geometry.spi_hz))));
    geometry.dc_gpio = env_int({"XIAOMAN_LCD_DC_GPIO", "NEWBOT_LCD_DC_GPIO"}, geometry.dc_gpio);
    geometry.reset_gpio = env_int({"XIAOMAN_LCD_RESET_GPIO", "NEWBOT_LCD_RESET_GPIO"}, geometry.reset_gpio);
    geometry.backlight_gpio = env_int({"XIAOMAN_LCD_BACKLIGHT_GPIO", "NEWBOT_LCD_BACKLIGHT_GPIO"}, geometry.backlight_gpio);
    geometry.backlight_active_high =
        env_bool({"XIAOMAN_LCD_BACKLIGHT_ACTIVE_HIGH", "NEWBOT_LCD_BACKLIGHT_ACTIVE_HIGH"}, geometry.backlight_active_high);

    if (!geometry.valid())
    {
        std::cerr << "warning: invalid LCD geometry "
                  << geometry.width << "x" << geometry.height
                  << ", fallback to " << kDefaultLcdWidth << "x" << kDefaultLcdHeight << "\n";
        geometry.width = kDefaultLcdWidth;
        geometry.height = kDefaultLcdHeight;
    }
    return geometry;
}

bool read_cpu_times(CpuTimes *times)
{
    std::ifstream in("/proc/stat");
    std::string cpu;
    uint64_t user = 0;
    uint64_t nice = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t iowait = 0;
    uint64_t irq = 0;
    uint64_t softirq = 0;
    uint64_t steal = 0;
    if (!(in >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) || cpu != "cpu")
    {
        return false;
    }
    times->idle = idle + iowait;
    times->total = user + nice + system + idle + iowait + irq + softirq + steal;
    return times->total > 0;
}

double compute_cpu_percent(const CpuTimes &previous, const CpuTimes &current)
{
    if (current.total <= previous.total || current.idle < previous.idle)
    {
        return 0.0;
    }
    const uint64_t total_delta = current.total - previous.total;
    const uint64_t idle_delta = current.idle - previous.idle;
    if (total_delta == 0)
    {
        return 0.0;
    }
    return 100.0 * static_cast<double>(total_delta - idle_delta) / static_cast<double>(total_delta);
}

int read_cpu_core_count()
{
    static int cached_count = 0;
    if (cached_count > 0)
    {
        return cached_count;
    }
    std::ifstream in("/proc/stat");
    std::string key;
    int count = 0;
    while (in >> key)
    {
        if (key.size() > 3 &&
            key.rfind("cpu", 0) == 0 &&
            std::all_of(key.begin() + 3, key.end(), [](unsigned char ch)
                        { return std::isdigit(ch); }))
        {
            ++count;
        }
        std::string rest;
        std::getline(in, rest);
    }
    if (count <= 0)
    {
        count = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    }
    cached_count = count;
    return cached_count;
}

bool read_first_number_from_file(const std::string &path, double *value)
{
    std::ifstream in(path);
    return static_cast<bool>(in >> *value);
}

bool read_cpu_temperature(double *temp_c)
{
    double best = -1000.0;
    bool found = false;
    for (int i = 0; i < 16; ++i)
    {
        const std::string path = "/sys/class/thermal/thermal_zone" + std::to_string(i) + "/temp";
        double raw = 0.0;
        if (!read_first_number_from_file(path, &raw))
        {
            continue;
        }
        const double celsius = raw > 1000.0 ? raw / 1000.0 : raw;
        if (!found || celsius > best)
        {
            best = celsius;
            found = true;
        }
    }
    if (found)
    {
        *temp_c = best;
    }
    return found;
}

bool read_memory_percent(double *mem_percent)
{
    std::ifstream in("/proc/meminfo");
    std::string key;
    uint64_t value = 0;
    std::string unit;
    uint64_t total_kb = 0;
    uint64_t available_kb = 0;
    while (in >> key >> value >> unit)
    {
        if (key == "MemTotal:")
        {
            total_kb = value;
        }
        else if (key == "MemAvailable:")
        {
            available_kb = value;
        }
    }
    if (total_kb == 0 || available_kb > total_kb)
    {
        return false;
    }
    *mem_percent = 100.0 * static_cast<double>(total_kb - available_kb) / static_cast<double>(total_kb);
    return true;
}

double read_load_1m()
{
    double value = 0.0;
    read_first_number_from_file("/proc/loadavg", &value);
    return value;
}

int read_cpu_mhz()
{
    double khz = 0.0;
    if (read_first_number_from_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", &khz))
    {
        return static_cast<int>(std::round(khz / 1000.0));
    }

    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line))
    {
        const std::string prefix = "cpu MHz";
        if (line.rfind(prefix, 0) == 0)
        {
            const size_t colon = line.find(':');
            if (colon != std::string::npos)
            {
                try
                {
                    return static_cast<int>(std::round(std::stod(trim(line.substr(colon + 1)))));
                }
                catch (...)
                {
                    return 0;
                }
            }
        }
    }
    return 0;
}

uint64_t read_uptime_seconds()
{
    double seconds = 0.0;
    if (!read_first_number_from_file("/proc/uptime", &seconds))
    {
        return 0;
    }
    return static_cast<uint64_t>(seconds);
}

SystemStats read_system_stats(CpuTimes *previous_cpu, bool *has_previous_cpu)
{
    SystemStats stats;

    CpuTimes current_cpu;
    if (read_cpu_times(&current_cpu))
    {
        if (*has_previous_cpu)
        {
            stats.cpu_percent = compute_cpu_percent(*previous_cpu, current_cpu);
        }
        *previous_cpu = current_cpu;
        *has_previous_cpu = true;
    }

    stats.temp_valid = read_cpu_temperature(&stats.cpu_temp_c);
    stats.mem_valid = read_memory_percent(&stats.mem_percent);
    stats.load_1m = read_load_1m();
    stats.cpu_mhz = read_cpu_mhz();
    stats.cpu_cores = read_cpu_core_count();
    stats.one_core_equiv_percent = stats.cpu_percent * std::max(1, stats.cpu_cores);
    stats.uptime_seconds = read_uptime_seconds();
    return stats;
}

std::mutex g_command_mutex;
std::atomic<bool> g_manual_wake_requested{false};
std::atomic<bool> g_dialogue_active{false};
std::atomic<bool> g_command_active{false};
std::atomic<bool> g_keyboard_dialogue_active{false};

enum class KeyboardEventType
{
    Command,
    TextWake,
};

struct KeyboardEvent
{
    KeyboardEventType type = KeyboardEventType::Command;
    std::string text;
};

std::mutex g_keyboard_mutex;
std::deque<KeyboardEvent> g_keyboard_events;
std::atomic<bool> g_keyboard_event_pending{false};

void enqueue_keyboard_event(KeyboardEvent event)
{
    {
        std::lock_guard<std::mutex> lock(g_keyboard_mutex);
        g_keyboard_events.push_back(std::move(event));
        g_keyboard_event_pending = true;
    }
}

bool pop_keyboard_event(KeyboardEvent *event)
{
    std::lock_guard<std::mutex> lock(g_keyboard_mutex);
    if (g_keyboard_events.empty())
    {
        g_keyboard_event_pending = false;
        return false;
    }
    *event = std::move(g_keyboard_events.front());
    g_keyboard_events.pop_front();
    g_keyboard_event_pending = !g_keyboard_events.empty();
    return true;
}

bool has_keyboard_event_pending()
{
    return g_keyboard_event_pending.load();
}

int keyboard_dialogue_timeout_seconds()
{
    return std::max(5, env_int({"XIAOMAN_KEYBOARD_DIALOGUE_TIMEOUT_SECONDS",
                                "NEWBOT_KEYBOARD_DIALOGUE_TIMEOUT_SECONDS"},
                               30));
}

struct KeyboardDialogueState
{
    bool active = false;
    std::chrono::steady_clock::time_point deadline{};
};

struct UiCommandPipeConfig
{
    bool enabled = false;
    std::string path = kDefaultUiCommandFifo;
};

struct DesktopConfig
{
    bool autostart = false;
    std::string command;
};

void close_keyboard_dialogue(KeyboardDialogueState *state)
{
    state->active = false;
    g_keyboard_dialogue_active = false;
}

void refresh_keyboard_dialogue_deadline(KeyboardDialogueState *state)
{
    state->deadline = std::chrono::steady_clock::now() +
                      std::chrono::seconds(keyboard_dialogue_timeout_seconds());
}

void start_keyboard_dialogue(KeyboardDialogueState *state)
{
    state->active = true;
    g_keyboard_dialogue_active = true;
    refresh_keyboard_dialogue_deadline(state);
    g_ui_state = static_cast<int>(UiState::Wake);
    speak_text(next_wake_reply());
    std::cout << "keyboard: text dialogue active, type your message\n";
    g_ui_state = static_cast<int>(UiState::Idle);
}

bool keyboard_dialogue_expired(const KeyboardDialogueState &state)
{
    return state.active && std::chrono::steady_clock::now() >= state.deadline;
}

void request_manual_wake(const char *source)
{
    if (g_dialogue_active.load() || g_command_active.load() || has_keyboard_event_pending())
    {
        std::cout << source << ": assistant is busy\n";
        return;
    }
    if (!g_manual_wake_requested.exchange(true))
    {
        std::cout << source << ": wake requested\n";
    }
}

bool consume_manual_wake_request()
{
    return g_manual_wake_requested.exchange(false);
}

#ifdef SMART_SPEAKER_USE_SHERPA_ONNX_C_API
std::mutex g_asr_mutex;
const SherpaOnnxOfflineRecognizer *g_recognizer = nullptr;
std::string g_recognizer_model_dir;
#endif

void update_audio_stats(PcmAudio *audio)
{
    audio->avg_rms = 0.0f;
    audio->peak_rms = 0.0f;
    if (audio->samples.empty())
    {
        return;
    }

    double sum_square = 0.0;
    constexpr size_t kBlockFrames = 320;
    for (float sample : audio->samples)
    {
        sum_square += static_cast<double>(sample) * static_cast<double>(sample);
    }
    audio->avg_rms = static_cast<float>(
        std::sqrt(sum_square / static_cast<double>(audio->samples.size())));

    for (size_t offset = 0; offset < audio->samples.size(); offset += kBlockFrames)
    {
        const size_t count = std::min(kBlockFrames, audio->samples.size() - offset);
        double block_sum = 0.0;
        for (size_t i = 0; i < count; ++i)
        {
            const float sample = audio->samples[offset + i];
            block_sum += static_cast<double>(sample) * static_cast<double>(sample);
        }
        const float block_rms = static_cast<float>(
            std::sqrt(block_sum / std::max(1.0, static_cast<double>(count))));
        audio->peak_rms = std::max(audio->peak_rms, block_rms);
    }
}

bool passes_voice_energy_gate(const PcmAudio &audio, const VoiceGateConfig &gate)
{
    const size_t min_samples = static_cast<size_t>(audio.sample_rate) *
                               static_cast<size_t>(gate.min_voice_ms) / 1000;
    return audio.samples.size() >= min_samples &&
           audio.avg_rms >= gate.min_avg_rms &&
           audio.peak_rms >= gate.min_peak_rms;
}

std::string normalize_noise_text(std::string text)
{
    text = to_lower_ascii(trim(text));
    const std::array<std::string, 16> drops{{
        " ", "\t", "\r", "\n", "，", "。", "！", "？",
        "、", ",", ".", "!", "?", "：", ":", "　",
    }};
    for (const std::string &drop : drops)
    {
        size_t pos = 0;
        while ((pos = text.find(drop, pos)) != std::string::npos)
        {
            text.erase(pos, drop.size());
        }
    }
    return text;
}

bool is_probable_asr_noise_text(const std::string &text)
{
    const std::string normalized = normalize_noise_text(text);
    if (normalized.empty())
    {
        return true;
    }
    const std::array<const char *, 10> noise_texts{{
        "嗯",
        "啊",
        "呃",
        "额",
        "oppo",
        "没对嗯",
        "没问对",
        "没有没没有",
        "没有没有",
        "没没有",
    }};
    return std::any_of(noise_texts.begin(), noise_texts.end(), [&](const char *noise)
                       { return normalized == noise; });
}

#ifdef SMART_SPEAKER_USE_OPENCV
struct CameraFrame
{
    cv::Mat image;
    std::chrono::steady_clock::time_point captured_at;
    uint64_t sequence = 0;
};

std::mutex g_camera_mutex;
std::condition_variable g_camera_cv;
std::shared_ptr<const CameraFrame> g_latest_camera_frame;
uint64_t g_camera_sequence = 0;
std::atomic<int64_t> g_camera_active_until_ms{0};

#if defined(SMART_SPEAKER_USE_RKNN_PERSON) || defined(SMART_SPEAKER_USE_RKNN_EMOTION)
struct VisionSnapshot
{
    uint64_t camera_sequence = 0;
    bool emotion_mode = false;
    std::chrono::steady_clock::time_point processed_at;
#ifdef SMART_SPEAKER_USE_RKNN_PERSON
    std::vector<serial_motor::PersonDetection> person_detections;
#endif
#ifdef SMART_SPEAKER_USE_RKNN_EMOTION
    std::vector<serial_motor::EmotionDetection> emotion_detections;
#endif
};

std::mutex g_vision_mutex;
std::shared_ptr<const VisionSnapshot> g_latest_vision_snapshot;
#endif

int64_t steady_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void request_camera_frames_for(int duration_ms)
{
    const int64_t deadline = steady_now_ms() + std::max(0, duration_ms);
    int64_t current = g_camera_active_until_ms.load();
    while (deadline > current &&
           !g_camera_active_until_ms.compare_exchange_weak(current, deadline))
    {
    }
}

bool camera_capture_needed(bool always_on)
{
    return always_on ||
           g_lcd_camera_mode.load() ||
           steady_now_ms() < g_camera_active_until_ms.load();
}
#endif

#ifdef SMART_SPEAKER_USE_ALSA_PCM
bool record_pcm_alsa(const std::string &device, const RecordOptions &options, PcmAudio *audio_out)
{
    const VoiceGateConfig gate = load_voice_gate_config();
    snd_pcm_t *handle = nullptr;
    const int open_err = snd_pcm_open(&handle, device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (open_err < 0)
    {
        std::cerr << "warning: ALSA open capture device failed: " << snd_strerror(open_err) << "\n";
        return false;
    }

    int err = snd_pcm_set_params(handle,
                                 SND_PCM_FORMAT_S16_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED,
                                 1,
                                 kSampleRate,
                                 1,
                                 500000);
    if (err < 0)
    {
        std::cerr << "warning: ALSA set params failed: " << snd_strerror(err) << "\n";
        snd_pcm_close(handle);
        return false;
    }

    audio_out->sample_rate = kSampleRate;
    audio_out->samples.clear();
    audio_out->avg_rms = 0.0f;
    audio_out->peak_rms = 0.0f;
    audio_out->samples.reserve(static_cast<size_t>(options.record_seconds) * kSampleRate);

    constexpr snd_pcm_uframes_t kFramesPerRead = 320;
    std::vector<int16_t> buffer(kFramesPerRead);

    const size_t max_voice_frames = static_cast<size_t>(options.record_seconds) * kSampleRate;
    const size_t max_listen_frames = static_cast<size_t>(options.max_listen_seconds) * kSampleRate;
    const size_t pre_roll_frames = static_cast<size_t>(kSampleRate * gate.pre_roll_ms / 1000);
    const size_t min_voice_frames = static_cast<size_t>(kSampleRate * gate.min_voice_ms / 1000);
    const size_t silence_stop_frames = static_cast<size_t>(kSampleRate * gate.silence_stop_ms / 1000);
    const size_t trigger_confirm_frames = static_cast<size_t>(kSampleRate * gate.confirm_ms / 1000);
    std::vector<float> pre_roll(pre_roll_frames);
    size_t pre_roll_pos = 0;
    size_t pre_roll_count = 0;

    bool triggered = false;
    bool has_noise_floor = false;
    float noise_floor_rms = 0.0f;
    size_t listened_frames = 0;
    size_t silence_frames = 0;
    size_t candidate_voice_frames = 0;

    auto normalized_sample = [](int16_t sample) -> float
    {
        return static_cast<float>(sample) / 32768.0f;
    };
    auto append_to_preroll = [&](const int16_t *data, size_t count)
    {
        if (pre_roll.empty())
        {
            return;
        }
        for (size_t i = 0; i < count; ++i)
        {
            pre_roll[pre_roll_pos] = normalized_sample(data[i]);
            pre_roll_pos = (pre_roll_pos + 1) % pre_roll.size();
            pre_roll_count = std::min(pre_roll_count + 1, pre_roll.size());
        }
    };
    auto copy_preroll_to_output = [&]()
    {
        audio_out->samples.clear();
        audio_out->samples.reserve(max_voice_frames);
        const size_t begin = (pre_roll_pos + pre_roll.size() - pre_roll_count) % pre_roll.size();
        for (size_t i = 0; i < pre_roll_count; ++i)
        {
            audio_out->samples.push_back(pre_roll[(begin + i) % pre_roll.size()]);
        }
    };
    auto append_to_output = [&](const int16_t *data, size_t count)
    {
        const size_t writable = std::min(count, max_voice_frames - std::min(audio_out->samples.size(), max_voice_frames));
        for (size_t i = 0; i < writable; ++i)
        {
            audio_out->samples.push_back(normalized_sample(data[i]));
        }
    };

    bool interrupted = false;
    while (listened_frames < max_listen_frames && g_running)
    {
        if (options.interrupt_on_manual_wake &&
            (g_manual_wake_requested.load() || g_command_active.load() || has_keyboard_event_pending()))
        {
            interrupted = true;
            break;
        }
        const snd_pcm_uframes_t frames_to_read =
            static_cast<snd_pcm_uframes_t>(std::min<size_t>(max_listen_frames - listened_frames, kFramesPerRead));
        const snd_pcm_sframes_t frames_read = snd_pcm_readi(handle, buffer.data(), frames_to_read);

        if (frames_read == -EPIPE)
        {
            snd_pcm_prepare(handle);
            continue;
        }
        if (frames_read < 0)
        {
            err = snd_pcm_recover(handle, static_cast<int>(frames_read), 0);
            if (err < 0)
            {
                std::cerr << "warning: ALSA read failed: " << snd_strerror(err) << "\n";
                snd_pcm_close(handle);
                return false;
            }
            continue;
        }
        if (frames_read == 0)
        {
            continue;
        }

        listened_frames += static_cast<size_t>(frames_read);
        double sum_square = 0.0;
        for (snd_pcm_sframes_t i = 0; i < frames_read; ++i)
        {
            const float sample = normalized_sample(buffer[static_cast<size_t>(i)]);
            sum_square += static_cast<double>(sample) * static_cast<double>(sample);
        }
        const float rms = static_cast<float>(
            std::sqrt(sum_square / std::max(1.0, static_cast<double>(frames_read))));

        if (!triggered)
        {
            append_to_preroll(buffer.data(), static_cast<size_t>(frames_read));
            const float dynamic_start_rms = has_noise_floor
                                                ? std::max(gate.start_rms, noise_floor_rms * gate.noise_ratio)
                                                : gate.start_rms;
            if (rms >= dynamic_start_rms)
            {
                candidate_voice_frames += static_cast<size_t>(frames_read);
            }
            else
            {
                candidate_voice_frames = 0;
                if (!has_noise_floor)
                {
                    noise_floor_rms = rms;
                    has_noise_floor = true;
                }
                else
                {
                    noise_floor_rms = noise_floor_rms * 0.95f + rms * 0.05f;
                }
            }
            if (candidate_voice_frames >= trigger_confirm_frames)
            {
                triggered = true;
                silence_frames = 0;
                copy_preroll_to_output();
            }
            continue;
        }

        append_to_output(buffer.data(), static_cast<size_t>(frames_read));
        if (rms >= gate.stop_rms)
        {
            silence_frames = 0;
        }
        else
        {
            silence_frames += static_cast<size_t>(frames_read);
        }
        if (audio_out->samples.size() >= max_voice_frames)
        {
            break;
        }
        if (audio_out->samples.size() >= min_voice_frames && silence_frames >= silence_stop_frames)
        {
            if (silence_frames < audio_out->samples.size())
            {
                audio_out->samples.resize(audio_out->samples.size() - silence_frames);
            }
            break;
        }
    }

    if (interrupted)
    {
        snd_pcm_drop(handle);
    }
    else
    {
        snd_pcm_drain(handle);
    }
    snd_pcm_close(handle);
    if (interrupted)
    {
        return false;
    }
    update_audio_stats(audio_out);
    if (gate.debug)
    {
        std::cout << "voice gate: triggered=" << triggered
                  << " samples=" << audio_out->samples.size()
                  << " avg_rms=" << audio_out->avg_rms
                  << " peak_rms=" << audio_out->peak_rms
                  << " noise_rms=" << noise_floor_rms << "\n";
    }
    return triggered && audio_out->samples.size() >= min_voice_frames;
}
#else
bool record_pcm_alsa(const std::string &, const RecordOptions &, PcmAudio *)
{
    std::cerr << "warning: ALSA PCM support is not enabled. Install libasound2-dev and rebuild.\n";
    return false;
}
#endif

#ifdef SMART_SPEAKER_USE_SHERPA_ONNX_C_API
bool file_has_data(const std::string &path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

std::string debug_cwd()
{
    const std::vector<std::string> dirs = cwd_and_parents(1);
    return dirs.empty() ? std::string("unknown") : dirs.front();
}

struct SherpaOfflineStreamDeleter
{
    void operator()(const SherpaOnnxOfflineStream *stream) const
    {
        if (stream != nullptr)
        {
            SherpaOnnxDestroyOfflineStream(stream);
        }
    }
};

struct SherpaOfflineResultDeleter
{
    void operator()(const SherpaOnnxOfflineRecognizerResult *result) const
    {
        if (result != nullptr)
        {
            SherpaOnnxDestroyOfflineRecognizerResult(result);
        }
    }
};

const SherpaOnnxOfflineRecognizer *create_recognizer(const std::string &model_dir)
{
    const std::string model_path = join_path(model_dir, "model.int8.onnx");
    const std::string tokens_path = join_path(model_dir, "tokens.txt");
    if (!file_has_data(model_path))
    {
        std::cerr << "warning: ASR model file is missing or empty: " << model_path
                  << " model_dir=" << model_dir
                  << " cwd=" << debug_cwd() << "\n";
        return nullptr;
    }
    if (!file_has_data(tokens_path))
    {
        std::cerr << "warning: ASR tokens file is missing or empty: " << tokens_path
                  << " model_dir=" << model_dir
                  << " cwd=" << debug_cwd() << "\n";
        return nullptr;
    }

    SherpaOnnxOfflineRecognizerConfig config;
    std::memset(&config, 0, sizeof(config));
    config.feat_config.sample_rate = kSampleRate;
    config.feat_config.feature_dim = 80;
    config.model_config.paraformer.model = model_path.c_str();
    config.model_config.tokens = tokens_path.c_str();
    const unsigned int cpu_count = std::max(1u, std::thread::hardware_concurrency());
    config.model_config.num_threads =
        env_int({"XIAOMAN_ASR_THREADS", "NEWBOT_ASR_THREADS"}, static_cast<int>(std::min(2u, cpu_count)));
    config.model_config.debug = 0;
    config.model_config.provider = "cpu";
    config.decoding_method = "greedy_search";
    config.max_active_paths = 4;
    try
    {
        return SherpaOnnxCreateOfflineRecognizer(&config);
    }
    catch (const std::exception &e)
    {
        std::cerr << "warning: sherpa-onnx recognizer init failed: " << e.what()
                  << " model=" << model_path
                  << " tokens=" << tokens_path << "\n";
        return nullptr;
    }
    catch (...)
    {
        std::cerr << "warning: sherpa-onnx recognizer init failed with unknown error"
                  << " model=" << model_path
                  << " tokens=" << tokens_path << "\n";
        return nullptr;
    }
}

const SherpaOnnxOfflineRecognizer *ensure_recognizer_locked(const std::string &model_dir)
{
    if (g_recognizer != nullptr && g_recognizer_model_dir == model_dir)
    {
        return g_recognizer;
    }
    if (g_recognizer != nullptr)
    {
        SherpaOnnxDestroyOfflineRecognizer(g_recognizer);
        g_recognizer = nullptr;
        g_recognizer_model_dir.clear();
    }
    g_recognizer = create_recognizer(model_dir);
    if (g_recognizer != nullptr)
    {
        g_recognizer_model_dir = model_dir;
    }
    return g_recognizer;
}

bool asr_samples(const std::string &model_dir, const PcmAudio &audio, std::string *text_out)
{
    try
    {
        text_out->clear();
        if (audio.samples.empty())
        {
            return false;
        }

        const size_t min_asr_samples = static_cast<size_t>(audio.sample_rate) * kAsrMinInputMs / 1000;
        std::vector<float> padded_samples;
        const float *samples = audio.samples.data();
        size_t sample_count = audio.samples.size();
        if (sample_count < min_asr_samples)
        {
            padded_samples = audio.samples;
            padded_samples.resize(min_asr_samples, 0.0f);
            samples = padded_samples.data();
            sample_count = padded_samples.size();
        }

        std::lock_guard<std::mutex> lock(g_asr_mutex);
        const SherpaOnnxOfflineRecognizer *recognizer = ensure_recognizer_locked(model_dir);
        if (recognizer == nullptr)
        {
            std::cerr << "warning: failed to create sherpa-onnx recognizer\n";
            return false;
        }

        std::unique_ptr<const SherpaOnnxOfflineStream, SherpaOfflineStreamDeleter> stream(
            SherpaOnnxCreateOfflineStream(recognizer));
        if (stream == nullptr)
        {
            return false;
        }

        SherpaOnnxAcceptWaveformOffline(stream.get(), audio.sample_rate, samples, static_cast<int32_t>(sample_count));
        SherpaOnnxDecodeOfflineStream(recognizer, stream.get());
        std::unique_ptr<const SherpaOnnxOfflineRecognizerResult, SherpaOfflineResultDeleter> result(
            SherpaOnnxGetOfflineStreamResult(stream.get()));
        if (result != nullptr && result->text != nullptr)
        {
            *text_out = trim(result->text);
        }
        return !text_out->empty();
    }
    catch (const std::exception &e)
    {
        std::cerr << "warning: sherpa-onnx decode failed: " << e.what() << "\n";
        text_out->clear();
        return false;
    }
    catch (...)
    {
        std::cerr << "warning: sherpa-onnx decode failed with unknown error\n";
        text_out->clear();
        return false;
    }
}

void destroy_recognizer()
{
    std::lock_guard<std::mutex> lock(g_asr_mutex);
    if (g_recognizer != nullptr)
    {
        SherpaOnnxDestroyOfflineRecognizer(g_recognizer);
        g_recognizer = nullptr;
    }
}
#else
bool asr_samples(const std::string &, const PcmAudio &, std::string *)
{
    std::cerr << "warning: sherpa-onnx C API is not enabled. Set SHERPA_ONNX_ROOT or copy tools/voice_test libs.\n";
    return false;
}

void destroy_recognizer() {}
#endif

class GpioPin
{
public:
    explicit GpioPin(int gpio) : gpio_(gpio)
    {
        base_path_ = "/sys/class/gpio/gpio" + std::to_string(gpio_);
        if (!path_exists(base_path_))
        {
            std::ofstream export_file("/sys/class/gpio/export");
            export_file << gpio_;
            export_file.flush();
            usleep(100000);
        }
        if (!path_exists(base_path_))
        {
            throw std::runtime_error("GPIO export failed: gpio" + std::to_string(gpio_));
        }
        std::ofstream direction(base_path_ + "/direction");
        direction << "out";
        direction.flush();
        value_fd_ = ::open((base_path_ + "/value").c_str(), O_WRONLY);
        if (value_fd_ < 0)
        {
            throw std::runtime_error("open gpio value failed: " + std::string(std::strerror(errno)));
        }
    }

    ~GpioPin()
    {
        if (value_fd_ >= 0)
        {
            ::close(value_fd_);
        }
    }

    void set(bool high)
    {
        const char value = high ? '1' : '0';
        if (::lseek(value_fd_, 0, SEEK_SET) < 0 || ::write(value_fd_, &value, 1) != 1)
        {
            throw std::runtime_error("write gpio failed: " + std::string(std::strerror(errno)));
        }
    }

private:
    int gpio_ = -1;
    int value_fd_ = -1;
    std::string base_path_;
};

class GpioInputPin
{
public:
    explicit GpioInputPin(int gpio) : gpio_(gpio)
    {
        base_path_ = "/sys/class/gpio/gpio" + std::to_string(gpio_);
        if (!path_exists(base_path_))
        {
            std::ofstream export_file("/sys/class/gpio/export");
            export_file << gpio_;
            export_file.flush();
            usleep(100000);
        }
        if (!path_exists(base_path_))
        {
            throw std::runtime_error("GPIO export failed: gpio" + std::to_string(gpio_));
        }
        std::ofstream direction(base_path_ + "/direction");
        direction << "in";
        direction.flush();
        value_fd_ = ::open((base_path_ + "/value").c_str(), O_RDONLY);
        if (value_fd_ < 0)
        {
            throw std::runtime_error("open gpio input failed: " + std::string(std::strerror(errno)));
        }
    }

    ~GpioInputPin()
    {
        if (value_fd_ >= 0)
        {
            ::close(value_fd_);
        }
    }

    bool high() const
    {
        char value = '1';
        if (::lseek(value_fd_, 0, SEEK_SET) < 0 || ::read(value_fd_, &value, 1) != 1)
        {
            throw std::runtime_error("read gpio input failed: " + std::string(std::strerror(errno)));
        }
        return value == '1';
    }

private:
    int gpio_ = -1;
    int value_fd_ = -1;
    std::string base_path_;
};

class SpiDevice
{
public:
    SpiDevice(const std::string &dev, uint32_t speed_hz) : speed_hz_(speed_hz)
    {
        fd_ = ::open(dev.c_str(), O_RDWR);
        if (fd_ < 0)
        {
            const int open_errno = errno;
            std::string message = "open " + dev + " failed: " + std::strerror(open_errno);
            if (open_errno == EACCES || open_errno == EPERM)
            {
                message += ". Run with sudo -E, or grant the current user access to SPI/GPIO devices.";
            }
            throw std::runtime_error(message);
        }
        uint8_t mode = SPI_MODE_0;
        uint8_t bits = 8;
        if (::ioctl(fd_, SPI_IOC_WR_MODE, &mode) < 0 ||
            ::ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
            ::ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz_) < 0)
        {
            throw std::runtime_error("SPI setup failed: " + std::string(std::strerror(errno)));
        }
    }

    ~SpiDevice()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }

    void write(const uint8_t *data, size_t size)
    {
        constexpr size_t kChunkSize = 4096;
        size_t offset = 0;
        while (offset < size)
        {
            const size_t chunk = std::min(kChunkSize, size - offset);
            spi_ioc_transfer tr{};
            tr.tx_buf = reinterpret_cast<uintptr_t>(data + offset);
            tr.len = static_cast<uint32_t>(chunk);
            tr.speed_hz = speed_hz_;
            tr.bits_per_word = 8;
            if (::ioctl(fd_, SPI_IOC_MESSAGE(1), &tr) < 0)
            {
                throw std::runtime_error("SPI transfer failed: " + std::string(std::strerror(errno)));
            }
            offset += chunk;
        }
    }

    void write_byte(uint8_t data)
    {
        write(&data, 1);
    }

private:
    int fd_ = -1;
    uint32_t speed_hz_ = 0;
};

class LcdDisplay
{
public:
    LcdDisplay(const std::string &spi_dev, LcdGeometry geometry)
        : geometry_(std::move(geometry)),
          spi_(spi_dev, geometry_.spi_hz),
          dc_(geometry_.dc_gpio),
          res_(geometry_.reset_gpio)
    {
        if (geometry_.backlight_gpio >= 0)
        {
            backlight_ = std::make_unique<GpioPin>(geometry_.backlight_gpio);
            set_backlight(false);
        }
        dc_.set(true);
        res_.set(true);
        reset();
        init();
        clear();
        set_backlight(true);
    }

    void display_rgb565(const std::vector<uint16_t> &frame)
    {
        if (frame.size() != geometry_.pixel_count())
        {
            return;
        }
        tx_bytes_.resize(geometry_.rgb565_byte_count());
        uint8_t *out = tx_bytes_.data();
        for (uint16_t pixel : frame)
        {
            *out++ = static_cast<uint8_t>(pixel >> 8);
            *out++ = static_cast<uint8_t>(pixel & 0xff);
        }
        write_frame_bytes(tx_bytes_.data(), tx_bytes_.size());
    }

    void display_rgb565_bytes(const std::vector<uint8_t> &frame)
    {
        if (frame.size() != geometry_.rgb565_byte_count())
        {
            return;
        }
        write_frame_bytes(frame.data(), frame.size());
    }

    void clear()
    {
        tx_bytes_.assign(geometry_.rgb565_byte_count(), 0);
        write_frame_bytes(tx_bytes_.data(), tx_bytes_.size());
    }

private:
    void write_frame_bytes(const uint8_t *data, size_t size)
    {
        set_region(0, 0, geometry_.width - 1, geometry_.height - 1);
        spi_.write(data, size);
    }

    void reset()
    {
        res_.set(true);
        usleep(50000);
        res_.set(false);
        usleep(50000);
        res_.set(true);
        usleep(120000);
    }

    void set_backlight(bool on)
    {
        if (!backlight_)
        {
            return;
        }
        backlight_->set(on ? geometry_.backlight_active_high : !geometry_.backlight_active_high);
    }

    void command(uint8_t index, std::initializer_list<uint8_t> data = {})
    {
        dc_.set(false);
        spi_.write_byte(index);
        dc_.set(true);
        for (uint8_t value : data)
        {
            spi_.write_byte(value);
        }
    }

    void set_region(int x_start, int y_start, int x_end, int y_end)
    {
        x_start += geometry_.x_offset;
        x_end += geometry_.x_offset;
        y_start += geometry_.y_offset;
        y_end += geometry_.y_offset;
        command(0x2a, {static_cast<uint8_t>(x_start >> 8), static_cast<uint8_t>(x_start & 0xff),
                       static_cast<uint8_t>(x_end >> 8), static_cast<uint8_t>(x_end & 0xff)});
        command(0x2b, {static_cast<uint8_t>(y_start >> 8), static_cast<uint8_t>(y_start & 0xff),
                       static_cast<uint8_t>(y_end >> 8), static_cast<uint8_t>(y_end & 0xff)});
        command(0x2c);
    }

    void init()
    {
        if (geometry_.driver == "st7789")
        {
            init_st7789();
        }
        else
        {
            init_gc9a01();
        }
    }

    void init_st7789()
    {
        command(0x36, {geometry_.madctl});
        command(0x3A, {0x05});
        command(0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33});
        command(0xB7, {0x35});
        command(0xBB, {0x19});
        command(0xC0, {0x2C});
        command(0xC2, {0x01});
        command(0xC3, {0x12});
        command(0xC4, {0x20});
        command(0xC6, {0x0F});
        command(0xD0, {0xA4, 0xA1});
        command(0xE0, {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23});
        command(0xE1, {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23});
        command(0x21);
        command(0x11);
        usleep(120000);
        command(0x29);
        usleep(50000);
    }

    void init_gc9a01()
    {
        command(0xEF);
        command(0xEB, {0x14});
        command(0xFE);
        command(0xEF);
        command(0xEB, {0x14});
        command(0x84, {0x40});
        command(0x85, {0xFF});
        command(0x86, {0xFF});
        command(0x87, {0xFF});
        command(0x88, {0x0A});
        command(0x89, {0x21});
        command(0x8A, {0x00});
        command(0x8B, {0x80});
        command(0x8C, {0x01});
        command(0x8D, {0x01});
        command(0x8E, {0xFF});
        command(0x8F, {0xFF});
        command(0xB6, {0x00, 0x20});
        command(0x36, {geometry_.madctl});
        command(0x3A, {0x05});
        command(0x90, {0x08, 0x08, 0x08, 0x08});
        command(0xBD, {0x06});
        command(0xBC, {0x00});
        command(0xFF, {0x60, 0x01, 0x04});
        command(0xC3, {0x13});
        command(0xC4, {0x13});
        command(0xC9, {0x22});
        command(0xBE, {0x11});
        command(0xE1, {0x10, 0x0E});
        command(0xDF, {0x21, 0x0C, 0x02});
        command(0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A});
        command(0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F});
        command(0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A});
        command(0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F});
        command(0xED, {0x1B, 0x0B});
        command(0xAE, {0x77});
        command(0xCD, {0x63});
        command(0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03});
        command(0xE8, {0x34});
        command(0x62, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70});
        command(0x63, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70});
        command(0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07});
        command(0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00});
        command(0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98});
        command(0x74, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00});
        command(0x98, {0x3E, 0x07});
        command(0x35);
        command(0x21);
        command(0x11);
        usleep(120000);
        command(0x29);
        usleep(20000);
    }

    LcdGeometry geometry_;
    SpiDevice spi_;
    GpioPin dc_;
    GpioPin res_;
    std::unique_ptr<GpioPin> backlight_;
    std::vector<uint8_t> tx_bytes_;
};

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void set_pixel(std::vector<uint16_t> &frame, const LcdGeometry &geometry, int x, int y, uint16_t color)
{
    if (x >= 0 && x < geometry.width && y >= 0 && y < geometry.height)
    {
        frame[static_cast<size_t>(y * geometry.width + x)] = color;
    }
}

void fill_circle(std::vector<uint16_t> &frame, const LcdGeometry &geometry, int cx, int cy, int radius, uint16_t color)
{
    const int r2 = radius * radius;
    for (int y = cy - radius; y <= cy + radius; ++y)
    {
        for (int x = cx - radius; x <= cx + radius; ++x)
        {
            const int dx = x - cx;
            const int dy = y - cy;
            if (dx * dx + dy * dy <= r2)
            {
                set_pixel(frame, geometry, x, y, color);
            }
        }
    }
}

void fill_ellipse(std::vector<uint16_t> &frame, const LcdGeometry &geometry, int cx, int cy, int rx, int ry, uint16_t color)
{
    const int rx2 = rx * rx;
    const int ry2 = ry * ry;
    const int limit = rx2 * ry2;
    for (int y = cy - ry; y <= cy + ry; ++y)
    {
        for (int x = cx - rx; x <= cx + rx; ++x)
        {
            const int dx = x - cx;
            const int dy = y - cy;
            if (dx * dx * ry2 + dy * dy * rx2 <= limit)
            {
                set_pixel(frame, geometry, x, y, color);
            }
        }
    }
}

void draw_line(std::vector<uint16_t> &frame, const LcdGeometry &geometry, int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    for (int i = 0; i <= std::max(1, steps); ++i)
    {
        fill_circle(frame, geometry, x0 + dx * i / std::max(1, steps), y0 + dy * i / std::max(1, steps), thickness, color);
    }
}

void draw_arc(std::vector<uint16_t> &frame, const LcdGeometry &geometry, int cx, int cy, int radius, int start_degree, int end_degree, int thickness, uint16_t color)
{
    constexpr double kPi = 3.14159265358979323846;
    for (int degree = start_degree; degree <= end_degree; ++degree)
    {
        const double rad = degree * kPi / 180.0;
        fill_circle(frame,
                    geometry,
                    cx + static_cast<int>(std::cos(rad) * radius),
                    cy + static_cast<int>(std::sin(rad) * radius),
                    thickness,
                    color);
    }
}

std::array<uint8_t, 7> glyph5x7(char ch)
{
    switch (ch)
    {
    case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
    case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I': return {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case 'J': return {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
    case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
    case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
    case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    case '6': return {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};
    case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    case ':': return {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
    case '%': return {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13};
    case '/': return {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
    case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    case '_': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};
    case '+': return {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
    case ' ': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    default: return {0x1F, 0x11, 0x02, 0x04, 0x04, 0x00, 0x04};
    }
}

void draw_char(std::vector<uint16_t> &frame,
               const LcdGeometry &geometry,
               int x,
               int y,
               char ch,
               int scale,
               uint16_t color)
{
    const std::array<uint8_t, 7> rows = glyph5x7(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    for (int row = 0; row < 7; ++row)
    {
        for (int col = 0; col < 5; ++col)
        {
            if ((rows[static_cast<size_t>(row)] & (1 << (4 - col))) == 0)
            {
                continue;
            }
            for (int yy = 0; yy < scale; ++yy)
            {
                for (int xx = 0; xx < scale; ++xx)
                {
                    set_pixel(frame, geometry, x + col * scale + xx, y + row * scale + yy, color);
                }
            }
        }
    }
}

void draw_text(std::vector<uint16_t> &frame,
               const LcdGeometry &geometry,
               int x,
               int y,
               const std::string &text,
               int scale,
               uint16_t color)
{
    int cursor = x;
    const int advance = 6 * scale;
    for (char ch : text)
    {
        draw_char(frame, geometry, cursor, y, ch, scale, color);
        cursor += advance;
    }
}

void fill_rect(std::vector<uint16_t> &frame,
               const LcdGeometry &geometry,
               int x,
               int y,
               int width,
               int height,
               uint16_t color)
{
    for (int yy = y; yy < y + height; ++yy)
    {
        for (int xx = x; xx < x + width; ++xx)
        {
            set_pixel(frame, geometry, xx, yy, color);
        }
    }
}

void draw_rect(std::vector<uint16_t> &frame,
               const LcdGeometry &geometry,
               int x,
               int y,
               int width,
               int height,
               uint16_t color)
{
    draw_line(frame, geometry, x, y, x + width - 1, y, 1, color);
    draw_line(frame, geometry, x, y + height - 1, x + width - 1, y + height - 1, 1, color);
    draw_line(frame, geometry, x, y, x, y + height - 1, 1, color);
    draw_line(frame, geometry, x + width - 1, y, x + width - 1, y + height - 1, 1, color);
}

std::string format_one_decimal(double value, const char *suffix = "")
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << value << suffix;
    return out.str();
}

std::string format_uptime(uint64_t seconds)
{
    const uint64_t hours = seconds / 3600;
    const uint64_t minutes = (seconds / 60) % 60;
    std::ostringstream out;
    out << hours << "H";
    if (minutes < 10)
    {
        out << "0";
    }
    out << minutes << "M";
    return out.str();
}

void draw_bar(std::vector<uint16_t> &frame,
              const LcdGeometry &geometry,
              int x,
              int y,
              int width,
              int height,
              double percent,
              uint16_t fill_color)
{
    const uint16_t border = rgb565(80, 110, 130);
    const uint16_t background = rgb565(20, 28, 42);
    draw_rect(frame, geometry, x, y, width, height, border);
    fill_rect(frame, geometry, x + 2, y + 2, std::max(0, width - 4), std::max(0, height - 4), background);
    const double clamped = std::max(0.0, std::min(100.0, percent));
    const int fill_width = static_cast<int>((width - 4) * clamped / 100.0);
    if (fill_width > 0)
    {
        fill_rect(frame, geometry, x + 2, y + 2, fill_width, std::max(0, height - 4), fill_color);
    }
}

void draw_system_stats_panel(const LcdGeometry &geometry,
                             const SystemStats &stats,
                             std::vector<uint16_t> *frame_out)
{
    frame_out->assign(geometry.pixel_count(), rgb565(0, 0, 0));
    std::vector<uint16_t> &frame = *frame_out;

    const uint16_t yellow = rgb565(255, 230, 70);
    const uint16_t white = rgb565(235, 245, 255);
    const uint16_t cyan = rgb565(40, 230, 255);
    const uint16_t red = rgb565(255, 80, 55);
    const uint16_t green = rgb565(80, 255, 90);
    const uint16_t blue = rgb565(80, 130, 210);
    const uint16_t gray = rgb565(90, 100, 120);

    std::string status_text = "STATUS OK";
    uint16_t status_color = green;
    if (stats.temp_valid && stats.cpu_temp_c >= 80.0)
    {
        status_text = "STATUS DANGER";
        status_color = red;
    }
    else if (stats.temp_valid && stats.cpu_temp_c >= 60.0)
    {
        status_text = "STATUS HOT";
        status_color = yellow;
    }

    if (geometry.width <= 200 && geometry.height >= 260)
    {
        const int margin = std::max(8, geometry.width / 18);
        const int label_scale = 2;
        const int value_scale = 3;
        const int mini_scale = 1;
        const int bar_width = geometry.width - margin * 2;
        int y = std::max(8, geometry.height / 32);

        draw_text(frame, geometry, margin, y, "ORANGE PI 3B", label_scale, yellow);
        y += 26;
        draw_text(frame, geometry, margin, y, "SYS STATUS", label_scale, white);
        y += 34;

        draw_text(frame, geometry, margin, y, "CPU LOAD", label_scale, white);
        y += 20;
        draw_text(frame, geometry, margin, y, format_one_decimal(stats.cpu_percent, "%"), value_scale, cyan);
        y += 27;
        draw_bar(frame, geometry, margin, y, bar_width, 16, stats.cpu_percent, blue);
        y += 34;

        draw_text(frame, geometry, margin, y, "CPU TEMP", label_scale, white);
        y += 20;
        draw_text(frame, geometry, margin, y, stats.temp_valid ? format_one_decimal(stats.cpu_temp_c, "C") : "--.-C", value_scale, red);
        y += 34;

        draw_text(frame, geometry, margin, y, "RAM USE", label_scale, white);
        y += 20;
        draw_text(frame, geometry, margin, y, stats.mem_valid ? format_one_decimal(stats.mem_percent, "%") : "--.-%", label_scale, green);
        y += 20;
        draw_bar(frame, geometry, margin, y, bar_width, 12, stats.mem_percent, green);
        y += 25;

        draw_text(frame, geometry, margin, y, "1C " + format_one_decimal(stats.one_core_equiv_percent, "%"), mini_scale, cyan);
        if (stats.cpu_mhz > 0)
        {
            draw_text(frame, geometry, margin + 78, y, std::to_string(stats.cpu_mhz) + "MHZ", mini_scale, gray);
        }
        y += 12;
        draw_text(frame, geometry, margin, y, "LOAD " + format_one_decimal(stats.load_1m), mini_scale, gray);
        draw_text(frame, geometry, margin + 78, y, "UP " + format_uptime(stats.uptime_seconds), mini_scale, gray);

        draw_text(frame, geometry, margin, geometry.height - 20, status_text, status_text.size() > 10 ? mini_scale : label_scale, status_color);
        return;
    }

    const int margin = std::max(6, geometry.width / 14);
    const int small_scale = std::max(1, geometry.width / 120);
    const int value_scale = std::max(2, geometry.width / 86);
    const int line_gap = std::max(8, geometry.height / 36);
    int y = margin;

    draw_text(frame, geometry, margin, y, "ORANGE PI 3B", small_scale, yellow);
    y += 9 * small_scale + line_gap;
    draw_text(frame, geometry, margin, y, "XIAOMAN STATUS", small_scale, white);
    y += 12 * small_scale + line_gap;

    draw_text(frame, geometry, margin, y, "CPU LOAD", small_scale, white);
    y += 9 * small_scale;
    draw_text(frame, geometry, margin, y, format_one_decimal(stats.cpu_percent, "%"), value_scale, cyan);
    y += 9 * value_scale + 5;
    draw_bar(frame, geometry, margin, y, geometry.width - margin * 2, std::max(8, geometry.height / 36), stats.cpu_percent, blue);
    y += std::max(8, geometry.height / 36) + line_gap;

    draw_text(frame, geometry, margin, y, "CPU TEMP", small_scale, white);
    y += 9 * small_scale;
    draw_text(frame, geometry, margin, y, stats.temp_valid ? format_one_decimal(stats.cpu_temp_c, "C") : "--.-C", value_scale, red);
    y += 9 * value_scale + line_gap;

    draw_text(frame, geometry, margin, y, "RAM USE", small_scale, white);
    y += 9 * small_scale;
    draw_text(frame, geometry, margin, y, stats.mem_valid ? format_one_decimal(stats.mem_percent, "%") : "--.-%", small_scale + 1, green);
    y += 9 * (small_scale + 1) + 4;
    draw_bar(frame, geometry, margin, y, geometry.width - margin * 2, std::max(7, geometry.height / 42), stats.mem_percent, green);
    y += std::max(7, geometry.height / 42) + line_gap;

    const std::string load_text = "LOAD " + format_one_decimal(stats.load_1m);
    draw_text(frame, geometry, margin, y, load_text, small_scale, cyan);
    y += 9 * small_scale + 3;

    draw_text(frame, geometry, margin, y, "1C " + format_one_decimal(stats.one_core_equiv_percent, "%"), small_scale, gray);
    y += 9 * small_scale + 3;

    if (stats.cpu_mhz > 0)
    {
        draw_text(frame, geometry, margin, y, "FREQ " + std::to_string(stats.cpu_mhz) + "MHZ", small_scale, gray);
        y += 9 * small_scale + 3;
    }

    draw_text(frame, geometry, margin, y, "UP " + format_uptime(stats.uptime_seconds), small_scale, gray);
    y += 9 * small_scale + line_gap;

    draw_text(frame, geometry, margin, std::min(y, geometry.height - 10 * small_scale), status_text, small_scale, status_color);
}

void draw_status_face(const LcdGeometry &geometry, UiState state, size_t tick, std::vector<uint16_t> *frame_out)
{
    frame_out->assign(geometry.pixel_count(), rgb565(4, 10, 18));
    std::vector<uint16_t> &frame = *frame_out;
    constexpr int kVirtualSize = 240;
    const int side = std::max(1, std::min(geometry.width, geometry.height));
    const int origin_x = (geometry.width - side) / 2;
    const int origin_y = (geometry.height - side) / 2;
    auto sx = [&](int x)
    {
        return origin_x + x * side / kVirtualSize;
    };
    auto sy = [&](int y)
    {
        return origin_y + y * side / kVirtualSize;
    };
    auto ss = [&](int value)
    {
        return std::max(1, value * side / kVirtualSize);
    };

    const uint16_t ring = rgb565(18, 84, 112);
    const uint16_t face = rgb565(8, 24, 36);
    const uint16_t cyan = rgb565(0, 226, 255);
    const uint16_t amber = rgb565(255, 190, 40);
    const uint16_t green = rgb565(70, 230, 130);
    const uint16_t red = rgb565(255, 80, 80);
    const uint16_t white = rgb565(232, 255, 255);
    fill_circle(frame, geometry, sx(120), sy(120), ss(112), ring);
    fill_circle(frame, geometry, sx(120), sy(120), ss(105), face);

    uint16_t color = cyan;
    if (state == UiState::Listen || state == UiState::Wake)
    {
        color = green;
    }
    else if (state == UiState::Think)
    {
        color = amber;
    }
    else if (state == UiState::Error)
    {
        color = red;
    }

    const int pulse = static_cast<int>((tick % 6) - 3);
    if (state == UiState::Idle)
    {
        fill_ellipse(frame, geometry, sx(82), sy(98), ss(18), ss(22), cyan);
        fill_ellipse(frame, geometry, sx(158), sy(98), ss(18), ss(22), cyan);
        draw_line(frame, geometry, sx(92), sy(158), sx(148), sy(158), ss(3), cyan);
    }
    else if (state == UiState::Listen || state == UiState::Wake)
    {
        fill_circle(frame, geometry, sx(82), sy(98), ss(24 + std::abs(pulse)), color);
        fill_circle(frame, geometry, sx(158), sy(98), ss(24 + std::abs(pulse)), color);
        draw_arc(frame, geometry, sx(120), sy(132), ss(48), 22, 158, ss(4), color);
    }
    else if (state == UiState::Think)
    {
        draw_line(frame, geometry, sx(62), sy(100), sx(102), sy(100), ss(4), color);
        draw_line(frame, geometry, sx(138), sy(100), sx(178), sy(100), ss(4), color);
        fill_circle(frame, geometry, sx(95 + static_cast<int>(tick % 4) * 16), sy(160), ss(6), color);
    }
    else if (state == UiState::Speak)
    {
        fill_ellipse(frame, geometry, sx(82), sy(98), ss(18), ss(24), color);
        fill_ellipse(frame, geometry, sx(158), sy(98), ss(18), ss(24), color);
        fill_ellipse(frame, geometry, sx(120), sy(158), ss(28), ss(12 + std::abs(pulse) * 2), color);
    }
    else if (state == UiState::Camera)
    {
        fill_ellipse(frame, geometry, sx(82), sy(98), ss(18), ss(24), color);
        fill_ellipse(frame, geometry, sx(158), sy(98), ss(18), ss(24), color);
        draw_line(frame, geometry, sx(74), sy(160), sx(166), sy(160), ss(4), white);
        draw_line(frame, geometry, sx(74), sy(188), sx(166), sy(188), ss(4), white);
        draw_line(frame, geometry, sx(74), sy(160), sx(74), sy(188), ss(4), white);
        draw_line(frame, geometry, sx(166), sy(160), sx(166), sy(188), ss(4), white);
    }
    else
    {
        draw_line(frame, geometry, sx(62), sy(80), sx(102), sy(120), ss(5), red);
        draw_line(frame, geometry, sx(102), sy(80), sx(62), sy(120), ss(5), red);
        draw_line(frame, geometry, sx(138), sy(80), sx(178), sy(120), ss(5), red);
        draw_line(frame, geometry, sx(178), sy(80), sx(138), sy(120), ss(5), red);
        draw_line(frame, geometry, sx(92), sy(166), sx(148), sy(166), ss(4), red);
    }
}

#ifdef SMART_SPEAKER_USE_OPENCV
void bgr_mat_to_rgb565_bytes(const cv::Mat &input,
                             const LcdGeometry &geometry,
                             std::vector<uint8_t> *frame,
                             cv::Mat *resize_scratch = nullptr,
                             cv::Mat *color_scratch = nullptr,
                             cv::Mat *canvas_scratch = nullptr)
{
    if (input.empty())
    {
        frame->clear();
        return;
    }

    cv::Mat local_resize;
    cv::Mat local_color;
    cv::Mat local_canvas;
    cv::Mat image;
    if (input.cols != geometry.width || input.rows != geometry.height)
    {
        const double scale = std::min(geometry.width / static_cast<double>(input.cols),
                                      geometry.height / static_cast<double>(input.rows));
        const int resized_width = std::max(1, static_cast<int>(std::round(input.cols * scale)));
        const int resized_height = std::max(1, static_cast<int>(std::round(input.rows * scale)));
        const int offset_x = (geometry.width - resized_width) / 2;
        const int offset_y = (geometry.height - resized_height) / 2;

        cv::Mat &resized = resize_scratch != nullptr ? *resize_scratch : local_resize;
        cv::resize(input, resized, cv::Size(resized_width, resized_height));

        cv::Mat &canvas = canvas_scratch != nullptr ? *canvas_scratch : local_canvas;
        canvas.create(geometry.height, geometry.width, input.type());
        canvas.setTo(cv::Scalar::all(0));
        resized.copyTo(canvas(cv::Rect(offset_x, offset_y, resized_width, resized_height)));
        image = canvas;
    }
    else
    {
        image = input;
    }
    if (image.channels() == 1)
    {
        cv::Mat &converted = color_scratch != nullptr ? *color_scratch : local_color;
        cv::cvtColor(image, converted, cv::COLOR_GRAY2BGR);
        image = converted;
    }
    else if (image.channels() == 4)
    {
        cv::Mat &converted = color_scratch != nullptr ? *color_scratch : local_color;
        cv::cvtColor(image, converted, cv::COLOR_BGRA2BGR);
        image = converted;
    }
    else if (image.channels() != 3)
    {
        frame->clear();
        return;
    }

    frame->resize(geometry.rgb565_byte_count());
    uint8_t *out = frame->data();
    for (int y = 0; y < geometry.height; ++y)
    {
        const cv::Vec3b *row = image.ptr<cv::Vec3b>(y);
        for (int x = 0; x < geometry.width; ++x)
        {
            const cv::Vec3b &bgr = row[x];
            const uint16_t pixel = rgb565(bgr[2], bgr[1], bgr[0]);
            *out++ = static_cast<uint8_t>(pixel >> 8);
            *out++ = static_cast<uint8_t>(pixel & 0xff);
        }
    }
}

std::string basename_of(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos)
    {
        return path;
    }
    return path.substr(slash + 1);
}

int numeric_file_stem(const std::string &path)
{
    std::string name = basename_of(path);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
    {
        name = name.substr(0, dot);
    }
    try
    {
        return std::stoi(name);
    }
    catch (...)
    {
        return 0;
    }
}

struct JpgEmojiState
{
    std::string name;
    std::vector<std::vector<uint8_t>> frames;
    int hold_frames = 0;
};

std::vector<std::string> list_jpg_files(const std::string &folder, int frame_step)
{
    glob_t result{};
    std::vector<std::string> files;
    const std::string pattern = join_path(folder, "*.jpg");

    if (::glob(pattern.c_str(), 0, nullptr, &result) == 0)
    {
        for (size_t i = 0; i < result.gl_pathc; ++i)
        {
            files.emplace_back(result.gl_pathv[i]);
        }
    }
    ::globfree(&result);

    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b)
              {
                  const int ai = numeric_file_stem(a);
                  const int bi = numeric_file_stem(b);
                  if (ai != bi)
                  {
                      return ai < bi;
                  }
                  return a < b;
              });

    if (frame_step <= 1)
    {
        return files;
    }

    std::vector<std::string> sampled;
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (i % static_cast<size_t>(frame_step) == 0)
        {
            sampled.push_back(files[i]);
        }
    }
    return sampled;
}

std::vector<std::string> emoji_state_folders(const std::string &image_root, const std::string &state)
{
    if (state == "眨眼")
    {
        return {
            join_path(join_path(image_root, state), "单次眨眼偶发"),
            join_path(join_path(image_root, state), "快速双眨眼偶发"),
        };
    }

    if (state == "正常" || state == "睡觉" || state == "苏醒" || state == "微笑")
    {
        return {join_path(image_root, state)};
    }

    return {
        join_path(join_path(image_root, state), state + "_1进入姿势"),
        join_path(join_path(image_root, state), state + "_2可循环动作"),
        join_path(join_path(image_root, state), state + "_3回正"),
    };
}

int jpg_frame_step_for_state(const std::string &state)
{
    if (state == "兴奋")
    {
        return 8;
    }
    if (state == "睡觉" || state == "苏醒" || state == "正常" || state == "微笑")
    {
        return 1;
    }
    return 3;
}

int jpg_hold_frames_for_state(const std::string &state)
{
    if (state == "正常" || state == "微笑" || state == "睡觉")
    {
        return 16;
    }
    if (state == "苏醒")
    {
        return 8;
    }
    return 4;
}

std::vector<uint8_t> load_jpg_frame_rgb565_bytes(const std::string &path, const LcdGeometry &geometry)
{
    cv::Mat image = cv::imread(path, cv::IMREAD_COLOR);
    if (image.empty())
    {
        throw std::runtime_error("cannot read emoji image: " + path);
    }
    std::vector<uint8_t> frame;
    bgr_mat_to_rgb565_bytes(image, geometry, &frame);
    if (frame.size() != geometry.rgb565_byte_count())
    {
        throw std::runtime_error("cannot convert emoji image: " + path);
    }
    return frame;
}

class JpgEmojiPlayer
{
public:
    JpgEmojiPlayer(const std::string &image_root, const std::string &states_text, LcdGeometry geometry)
        : geometry_(std::move(geometry))
    {
        if (!dir_exists(image_root))
        {
            throw std::runtime_error("emoji image root not found: " + image_root);
        }

        std::vector<std::string> states = split_csv(states_text);
        if (states.empty())
        {
            states = split_csv(kDefaultEmojiStates);
        }

        for (const std::string &state_name : states)
        {
            JpgEmojiState state;
            state.name = state_name;
            state.hold_frames = jpg_hold_frames_for_state(state_name);
            const int frame_step = jpg_frame_step_for_state(state_name);

            for (const std::string &folder : emoji_state_folders(image_root, state_name))
            {
                if (!dir_exists(folder))
                {
                    continue;
                }
                const std::vector<std::string> files = list_jpg_files(folder, frame_step);
                for (const std::string &file : files)
                {
                    state.frames.push_back(load_jpg_frame_rgb565_bytes(file, geometry_));
                }
            }

            if (!state.frames.empty())
            {
                states_.push_back(std::move(state));
            }
        }

        if (states_.empty())
        {
            throw std::runtime_error("no emoji JPG frames loaded from: " + image_root);
        }

        size_t frame_count = 0;
        for (const JpgEmojiState &state : states_)
        {
            frame_count += state.frames.size();
        }
        std::cout << "JPG emoji frames loaded: " << frame_count
                  << " frames from " << image_root << "\n";
    }

    void show_next_frame(LcdDisplay &lcd)
    {
        JpgEmojiState &state = states_[state_index_];
        if (hold_frames_remaining_ > 0)
        {
            --hold_frames_remaining_;
            if (hold_frames_remaining_ == 0)
            {
                state_index_ = (state_index_ + 1) % states_.size();
            }
            return;
        }

        lcd.display_rgb565_bytes(state.frames[frame_index_]);
        ++frame_index_;
        if (frame_index_ >= state.frames.size())
        {
            frame_index_ = 0;
            hold_frames_remaining_ = state.hold_frames;
            if (hold_frames_remaining_ == 0)
            {
                state_index_ = (state_index_ + 1) % states_.size();
            }
        }
    }

private:
    LcdGeometry geometry_;
    std::vector<JpgEmojiState> states_;
    size_t state_index_ = 0;
    size_t frame_index_ = 0;
    int hold_frames_remaining_ = 0;
};

bool is_integer_text(const std::string &text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch)
                                        { return std::isdigit(ch); });
}

bool open_camera(const CameraConfig &cfg, cv::VideoCapture *cap, std::string *error)
{
    if (is_integer_text(cfg.dev))
    {
        cap->open(std::stoi(cfg.dev), cv::CAP_V4L2);
    }
    else
    {
        cap->open(cfg.dev, cv::CAP_V4L2);
    }
    if (!cap->isOpened())
    {
        *error = "open failed";
        return false;
    }
    if (cfg.mjpeg)
    {
        cap->set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    }
    cap->set(cv::CAP_PROP_FRAME_WIDTH, cfg.width);
    cap->set(cv::CAP_PROP_FRAME_HEIGHT, cfg.height);
    cap->set(cv::CAP_PROP_FPS, cfg.fps);
    return true;
}

void remember_camera_frame(cv::Mat frame)
{
    if (frame.empty())
    {
        return;
    }
    auto published = std::make_shared<CameraFrame>();
    published->image = std::move(frame);
    published->captured_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_camera_mutex);
        published->sequence = ++g_camera_sequence;
        g_latest_camera_frame = std::move(published);
    }
    g_camera_cv.notify_all();
}

std::shared_ptr<const CameraFrame> get_recent_camera_frame(int max_age_ms = kVisionCachedFrameMaxAgeMs)
{
    std::lock_guard<std::mutex> lock(g_camera_mutex);
    if (!g_latest_camera_frame || g_latest_camera_frame->image.empty())
    {
        return nullptr;
    }
    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - g_latest_camera_frame->captured_at)
                            .count();
    if (age_ms > max_age_ms)
    {
        return nullptr;
    }
    return g_latest_camera_frame;
}

uint64_t latest_camera_sequence()
{
    std::lock_guard<std::mutex> lock(g_camera_mutex);
    return g_latest_camera_frame ? g_latest_camera_frame->sequence : 0;
}

std::shared_ptr<const CameraFrame> wait_for_new_camera_frame(uint64_t last_sequence,
                                                            int max_age_ms,
                                                            std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(g_camera_mutex);
    g_camera_cv.wait_for(lock, timeout, [&]()
                         {
                             return !g_running ||
                                    (g_latest_camera_frame &&
                                     !g_latest_camera_frame->image.empty() &&
                                     g_latest_camera_frame->sequence != last_sequence);
                         });
    if (!g_latest_camera_frame || g_latest_camera_frame->image.empty())
    {
        return nullptr;
    }
    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - g_latest_camera_frame->captured_at)
                            .count();
    if (age_ms > max_age_ms)
    {
        return nullptr;
    }
    return g_latest_camera_frame;
}

#if defined(SMART_SPEAKER_USE_RKNN_PERSON) || defined(SMART_SPEAKER_USE_RKNN_EMOTION)
void publish_vision_snapshot(std::shared_ptr<VisionSnapshot> snapshot)
{
    if (!snapshot)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_vision_mutex);
        g_latest_vision_snapshot = std::move(snapshot);
    }
}

std::shared_ptr<const VisionSnapshot> get_recent_vision_snapshot(int max_age_ms)
{
    std::lock_guard<std::mutex> lock(g_vision_mutex);
    if (!g_latest_vision_snapshot)
    {
        return nullptr;
    }
    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - g_latest_vision_snapshot->processed_at)
                            .count();
    if (age_ms > max_age_ms)
    {
        return nullptr;
    }
    return g_latest_vision_snapshot;
}
#endif

bool try_get_recent_camera_frame(cv::Mat *frame, int max_age_ms = kVisionCachedFrameMaxAgeMs)
{
    const std::shared_ptr<const CameraFrame> latest = get_recent_camera_frame(max_age_ms);
    if (!latest)
    {
        return false;
    }
    *frame = latest->image;
    return true;
}

bool wait_for_camera_frame_for_request(cv::Mat *frame)
{
    const int warmup_ms =
        std::max(200, env_int({"XIAOMAN_CAMERA_WARMUP_MS", "NEWBOT_CAMERA_WARMUP_MS"}, 2500));
    request_camera_frames_for(warmup_ms);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_ms);
    uint64_t last_sequence = 0;
    while (g_running && std::chrono::steady_clock::now() < deadline)
    {
        const std::shared_ptr<const CameraFrame> latest =
            wait_for_new_camera_frame(last_sequence,
                                      kVisionCachedFrameMaxAgeMs,
                                      std::chrono::milliseconds(120));
        if (latest && !latest->image.empty())
        {
            *frame = latest->image;
            return true;
        }
        last_sequence = latest_camera_sequence();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return try_get_recent_camera_frame(frame);
}

std::string encode_frame_to_jpeg_base64(const cv::Mat &frame)
{
    if (frame.empty())
    {
        throw std::runtime_error("cannot encode empty camera frame");
    }
    std::vector<unsigned char> jpeg;
    std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, kVisionJpegQuality};
    if (!cv::imencode(".jpg", frame, jpeg, params) || jpeg.empty())
    {
        throw std::runtime_error("camera frame JPEG encode failed");
    }
    return base64_encode(reinterpret_cast<const uint8_t *>(jpeg.data()), jpeg.size());
}

std::string camera_preview_file_path()
{
    std::string path = trim(env_first({"XIAOMAN_CAMERA_PREVIEW_FILE", "NEWBOT_CAMERA_PREVIEW_FILE"}));
    if (path.empty())
    {
        path = "/tmp/friday_voice_speaker_camera.jpg";
    }
    return path;
}

void publish_camera_preview_file(const cv::Mat &frame)
{
    if (!g_lcd_camera_mode.load() || frame.empty())
    {
        return;
    }

    static int64_t last_publish_ms = 0;
    const int interval_ms =
        std::max(80, env_int({"XIAOMAN_CAMERA_PREVIEW_INTERVAL_MS", "NEWBOT_CAMERA_PREVIEW_INTERVAL_MS"}, 160));
    const int64_t now_ms = steady_now_ms();
    if (now_ms - last_publish_ms < interval_ms)
    {
        return;
    }
    last_publish_ms = now_ms;

    cv::Mat preview = frame.clone();
#if defined(SMART_SPEAKER_USE_RKNN_PERSON) || defined(SMART_SPEAKER_USE_RKNN_EMOTION)
    const std::shared_ptr<const VisionSnapshot> vision = get_recent_vision_snapshot(
        std::max(200, env_int({"XIAOMAN_VISION_OVERLAY_MAX_AGE_MS", "NEWBOT_VISION_OVERLAY_MAX_AGE_MS"}, 1000)));
    const bool emotion_active = g_emotion_mode.load();
    if (vision && vision->emotion_mode == emotion_active)
    {
#ifdef SMART_SPEAKER_USE_RKNN_PERSON
        if (!vision->emotion_mode && !vision->person_detections.empty())
        {
            serial_motor::draw_person_detections(preview, vision->person_detections);
        }
#endif
#ifdef SMART_SPEAKER_USE_RKNN_EMOTION
        if (vision->emotion_mode && !vision->emotion_detections.empty())
        {
            serial_motor::draw_emotion_detections(preview, vision->emotion_detections);
        }
#endif
    }
#endif

    const std::string label = g_emotion_mode.load() ? "Emotion" : "Camera";
    cv::putText(preview, label, cv::Point(14, 34), cv::FONT_HERSHEY_SIMPLEX, 0.9,
                cv::Scalar(0, 255, 255), 2, cv::LINE_AA);

    const std::string path = camera_preview_file_path();
    const std::string tmp_path = path + ".tmp";
    const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY,
                                  std::max(40, std::min(95, env_int({"XIAOMAN_CAMERA_PREVIEW_JPEG_QUALITY",
                                                                      "NEWBOT_CAMERA_PREVIEW_JPEG_QUALITY"},
                                                                     72)))};
    static int warning_count = 0;
    static bool logged_success = false;
    try
    {
        std::vector<unsigned char> jpeg;
        if (!cv::imencode(".jpg", preview, jpeg, params) || jpeg.empty())
        {
            if (warning_count++ < 3)
            {
                std::cerr << "warning: camera preview JPEG encode failed\n";
            }
            return;
        }

        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            if (warning_count++ < 3)
            {
                std::cerr << "warning: camera preview open failed: " << tmp_path << "\n";
            }
            return;
        }
        out.write(reinterpret_cast<const char *>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        out.close();
        if (!out)
        {
            if (warning_count++ < 3)
            {
                std::cerr << "warning: camera preview write failed: " << tmp_path << "\n";
            }
            return;
        }

        ::chmod(tmp_path.c_str(), 0666);
        if (::rename(tmp_path.c_str(), path.c_str()) != 0)
        {
            if (warning_count++ < 3)
            {
                std::cerr << "warning: camera preview rename failed: " << tmp_path
                          << " -> " << path << ": " << std::strerror(errno) << "\n";
            }
            return;
        }
        ::chmod(path.c_str(), 0666);
        if (!logged_success)
        {
            logged_success = true;
            std::cout << "camera preview file=" << path << "\n";
        }
    }
    catch (const std::exception &e)
    {
        if (warning_count++ < 3)
        {
            std::cerr << "warning: camera preview publish failed: " << e.what() << "\n";
        }
    }
}

class BackgroundCamera
{
public:
    explicit BackgroundCamera(CameraConfig cfg) : cfg_(std::move(cfg)) {}

    ~BackgroundCamera()
    {
        stop();
    }

    void start()
    {
        if (running_.exchange(true))
        {
            return;
        }
        worker_ = std::thread([this]
                              { run(); });
    }

    void stop()
    {
        running_ = false;
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

private:
    void run()
    {
        set_current_thread_name("xiaoman-camera");
        const bool always_on = env_bool({"XIAOMAN_CAMERA_ALWAYS_ON", "NEWBOT_CAMERA_ALWAYS_ON"}, false);
        const int idle_sleep_ms =
            std::max(50, env_int({"XIAOMAN_CAMERA_IDLE_SLEEP_MS", "NEWBOT_CAMERA_IDLE_SLEEP_MS"}, 250));
        while (running_)
        {
            if (!camera_capture_needed(always_on))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(idle_sleep_ms));
                continue;
            }
            try
            {
                cv::VideoCapture cap;
                std::string error;
                if (!open_camera(cfg_, &cap, &error))
                {
                    throw std::runtime_error(error);
                }
                std::cout << "camera opened: " << cfg_.dev << "\n";
                const int capture_interval_ms = env_int(
                    {"XIAOMAN_CAMERA_CAPTURE_INTERVAL_MS", "NEWBOT_CAMERA_CAPTURE_INTERVAL_MS"},
                    cfg_.fps > 0 ? std::max(1, 1000 / cfg_.fps) : 40);
                while (running_ && camera_capture_needed(always_on))
                {
                    cv::Mat frame;
                    if (!cap.read(frame) || frame.empty())
                    {
                        throw std::runtime_error("camera read failed");
                    }
                    publish_camera_preview_file(frame);
                    remember_camera_frame(std::move(frame));
                    std::this_thread::sleep_for(std::chrono::milliseconds(capture_interval_ms));
                }
            }
            catch (const std::exception &e)
            {
                if (running_)
                {
                    std::cerr << "camera warning: " << e.what() << ", retrying\n";
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        }
    }

    CameraConfig cfg_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

#if defined(SMART_SPEAKER_USE_RKNN_PERSON) || defined(SMART_SPEAKER_USE_RKNN_EMOTION)
class VisionWorker
{
public:
    void start()
    {
        if (running_.exchange(true))
        {
            return;
        }
        worker_ = std::thread([this]
                              { run(); });
    }

    void stop()
    {
        running_ = false;
        g_camera_cv.notify_all();
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    ~VisionWorker()
    {
        stop();
    }

private:
    void run()
    {
        set_current_thread_name("xiaoman-vision");
        const int idle_sleep_ms =
            std::max(50, env_int({"XIAOMAN_VISION_IDLE_SLEEP_MS", "NEWBOT_VISION_IDLE_SLEEP_MS"}, 200));
        const int frame_wait_ms =
            std::max(10, env_int({"XIAOMAN_VISION_FRAME_WAIT_MS", "NEWBOT_VISION_FRAME_WAIT_MS"}, 80));

#ifdef SMART_SPEAKER_USE_RKNN_PERSON
        std::unique_ptr<serial_motor::PersonDetector> person_detector;
        std::vector<serial_motor::PersonDetection> person_detections;
        const int person_detect_interval_ms =
            std::max(50, env_int({"XIAOMAN_PERSON_DETECT_INTERVAL_MS", "NEWBOT_PERSON_DETECT_INTERVAL_MS"}, kPersonDetectIntervalMs));
        auto last_person_detect = std::chrono::steady_clock::now() - std::chrono::milliseconds(person_detect_interval_ms);
        auto last_person_print = std::chrono::steady_clock::now();
        uint64_t last_person_sequence = 0;
        bool person_detector_tried = false;
        bool person_unavailable_printed = false;
#endif

#ifdef SMART_SPEAKER_USE_RKNN_EMOTION
        std::unique_ptr<serial_motor::EmotionDetector> emotion_detector;
        std::vector<serial_motor::EmotionDetection> emotion_detections;
        const int emotion_detect_interval_ms =
            std::max(50, env_int({"XIAOMAN_EMOTION_DETECT_INTERVAL_MS", "NEWBOT_EMOTION_DETECT_INTERVAL_MS"}, kEmotionDetectIntervalMs));
        auto last_emotion_detect = std::chrono::steady_clock::now() - std::chrono::milliseconds(emotion_detect_interval_ms);
        auto last_emotion_print = std::chrono::steady_clock::now();
        uint64_t last_emotion_sequence = 0;
        bool emotion_detector_tried = false;
        bool emotion_unavailable_printed = false;
#endif

        while (running_)
        {
            if (!g_lcd_camera_mode.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(idle_sleep_ms));
                continue;
            }

            const bool emotion_active = g_emotion_mode.load();
            const auto now = std::chrono::steady_clock::now();

#ifdef SMART_SPEAKER_USE_RKNN_EMOTION
            if (emotion_active)
            {
                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            now - last_emotion_detect)
                                            .count();
                if (elapsed_ms < emotion_detect_interval_ms)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        std::min<int>(frame_wait_ms, emotion_detect_interval_ms - static_cast<int>(elapsed_ms))));
                    continue;
                }

                if (!emotion_detector_tried)
                {
                    emotion_detector_tried = true;
                    emotion_detector = std::make_unique<serial_motor::EmotionDetector>();
                    if (!emotion_detector->available())
                    {
                        emotion_detector.reset();
                        emotion_unavailable_printed = true;
                        std::cerr << "emotion: RKNN emotion detector is not available\n";
                    }
                }
                if (!emotion_detector || !emotion_detector->available())
                {
                    if (!emotion_unavailable_printed)
                    {
                        emotion_unavailable_printed = true;
                        std::cerr << "emotion: RKNN emotion detector is not available\n";
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(idle_sleep_ms));
                    continue;
                }

                const std::shared_ptr<const CameraFrame> latest =
                    wait_for_new_camera_frame(last_emotion_sequence,
                                              kVisionCachedFrameMaxAgeMs,
                                              std::chrono::milliseconds(frame_wait_ms));
                if (!latest)
                {
                    continue;
                }

                emotion_detections = emotion_detector->detect(latest->image);
                last_emotion_detect = std::chrono::steady_clock::now();
                last_emotion_sequence = latest->sequence;
                if (!emotion_detections.empty())
                {
                    const auto print_elapsed_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(last_emotion_detect - last_emotion_print).count();
                    if (print_elapsed_ms >= 1000)
                    {
                        const auto &det = emotion_detections.front();
                        std::cout << "emotion: " << det.label_cn
                                  << " (" << det.label_en << ")"
                                  << " conf=" << det.confidence << "\n";
                        last_emotion_print = last_emotion_detect;
                    }
                }

                auto snapshot = std::make_shared<VisionSnapshot>();
                snapshot->camera_sequence = latest->sequence;
                snapshot->emotion_mode = true;
                snapshot->processed_at = last_emotion_detect;
                snapshot->emotion_detections = emotion_detections;
                publish_vision_snapshot(std::move(snapshot));
                continue;
            }
#endif

#ifdef SMART_SPEAKER_USE_RKNN_PERSON
            if (!emotion_active)
            {
                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            now - last_person_detect)
                                            .count();
                if (elapsed_ms < person_detect_interval_ms)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        std::min<int>(frame_wait_ms, person_detect_interval_ms - static_cast<int>(elapsed_ms))));
                    continue;
                }

                if (!person_detector_tried)
                {
                    person_detector_tried = true;
                    person_detector = std::make_unique<serial_motor::PersonDetector>();
                    if (!person_detector->available())
                    {
                        person_detector.reset();
                        person_unavailable_printed = true;
                        std::cerr << "person: RKNN person detector is not available\n";
                    }
                }
                if (!person_detector || !person_detector->available())
                {
                    if (!person_unavailable_printed)
                    {
                        person_unavailable_printed = true;
                        std::cerr << "person: RKNN person detector is not available\n";
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(idle_sleep_ms));
                    continue;
                }

                const std::shared_ptr<const CameraFrame> latest =
                    wait_for_new_camera_frame(last_person_sequence,
                                              kVisionCachedFrameMaxAgeMs,
                                              std::chrono::milliseconds(frame_wait_ms));
                if (!latest)
                {
                    continue;
                }

                person_detections = person_detector->detect(latest->image);
                last_person_detect = std::chrono::steady_clock::now();
                last_person_sequence = latest->sequence;
                if (!person_detections.empty())
                {
                    const auto print_elapsed_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(last_person_detect - last_person_print).count();
                    if (print_elapsed_ms >= 1000)
                    {
                        std::cout << "person: count=" << person_detections.size()
                                  << " conf=" << person_detections.front().confidence << "\n";
                        last_person_print = last_person_detect;
                    }
                }

                auto snapshot = std::make_shared<VisionSnapshot>();
                snapshot->camera_sequence = latest->sequence;
                snapshot->emotion_mode = false;
                snapshot->processed_at = last_person_detect;
                snapshot->person_detections = person_detections;
                publish_vision_snapshot(std::move(snapshot));
                continue;
            }
#endif

            std::this_thread::sleep_for(std::chrono::milliseconds(idle_sleep_ms));
        }
    }

    std::atomic<bool> running_{false};
    std::thread worker_;
};
#endif
#endif

class DisplayWorker
{
public:
    DisplayWorker(std::string spi_dev,
                  bool enabled,
                  std::string emoji_root,
                  std::string emoji_states,
                  LcdGeometry geometry,
                  bool status_only)
        : spi_dev_(std::move(spi_dev)),
          emoji_root_(std::move(emoji_root)),
          emoji_states_(std::move(emoji_states)),
          geometry_(std::move(geometry)),
          status_only_(status_only),
          enabled_(enabled) {}

    ~DisplayWorker()
    {
        stop();
    }

    void start()
    {
        if (!enabled_ || running_.exchange(true))
        {
            return;
        }
        worker_ = std::thread([this]
                              { run(); });
    }

    void stop()
    {
        running_ = false;
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

private:
    void run()
    {
        set_current_thread_name("xiaoman-lcd");
        try
        {
            LcdDisplay lcd(spi_dev_, geometry_);
            size_t tick = 0;
            std::vector<uint16_t> status_frame;
            draw_status_face(geometry_, UiState::Idle, tick++, &status_frame);
            lcd.display_rgb565(status_frame);
            std::cout << "LCD display started on " << spi_dev_
                      << " driver=" << geometry_.driver
                      << " " << geometry_.width << "x" << geometry_.height
                      << " offset=" << geometry_.x_offset << "," << geometry_.y_offset
                      << " madctl=0x" << std::hex << static_cast<int>(geometry_.madctl) << std::dec
                      << " spi_hz=" << geometry_.spi_hz
                      << " gpio(dc/rst/bl)=" << geometry_.dc_gpio << "/"
                      << geometry_.reset_gpio << "/" << geometry_.backlight_gpio << "\n";
#ifdef SMART_SPEAKER_USE_OPENCV
            std::unique_ptr<JpgEmojiPlayer> emoji_player;
            std::vector<uint8_t> camera_rgb565;
            cv::Mat camera_resize_scratch;
            cv::Mat camera_color_scratch;
            cv::Mat camera_canvas_scratch;
            const int lcd_camera_interval_ms =
                env_int({"XIAOMAN_LCD_CAMERA_INTERVAL_MS", "NEWBOT_LCD_CAMERA_INTERVAL_MS"}, 120);
            const int emoji_interval_ms =
                env_int({"XIAOMAN_LCD_EMOJI_INTERVAL_MS", "NEWBOT_LCD_EMOJI_INTERVAL_MS"}, kJpgEmojiIntervalMs);
            if (!status_only_)
            {
                try
                {
                    std::cout << "LCD emoji: loading JPG frames from " << emoji_root_ << "\n";
                    emoji_player = std::make_unique<JpgEmojiPlayer>(emoji_root_, emoji_states_, geometry_);
                    std::cout << "LCD emoji mode: JPG frame animation\n";
                }
                catch (const std::exception &e)
                {
                    std::cerr << "warning: JPG emoji unavailable: " << e.what()
                              << ", using built-in geometry face.\n";
                }
            }
#endif
            const int geometry_interval_ms =
                env_int({"XIAOMAN_LCD_GEOMETRY_INTERVAL_MS", "NEWBOT_LCD_GEOMETRY_INTERVAL_MS"}, kGeometryFaceIntervalMs);
            const int stats_interval_ms =
                std::max(250, env_int({"XIAOMAN_LCD_STATS_INTERVAL_MS", "NEWBOT_LCD_STATS_INTERVAL_MS"}, 1000));
            CpuTimes previous_cpu_times;
            bool has_previous_cpu_times = false;
#if defined(SMART_SPEAKER_USE_OPENCV) && (defined(SMART_SPEAKER_USE_RKNN_PERSON) || defined(SMART_SPEAKER_USE_RKNN_EMOTION))
            const int vision_overlay_max_age_ms =
                std::max(100, env_int({"XIAOMAN_VISION_OVERLAY_MAX_AGE_MS", "NEWBOT_VISION_OVERLAY_MAX_AGE_MS"}, 1000));
#endif
            while (running_)
            {
                if (status_only_)
                {
                    const SystemStats stats = read_system_stats(&previous_cpu_times, &has_previous_cpu_times);
                    draw_system_stats_panel(geometry_, stats, &status_frame);
                    lcd.display_rgb565(status_frame);
                    std::this_thread::sleep_for(std::chrono::milliseconds(stats_interval_ms));
                    ++tick;
                    continue;
                }

                UiState state = static_cast<UiState>(g_ui_state.load());
                if (g_lcd_camera_mode.load())
                {
                    state = UiState::Camera;
                }
                if (g_lcd_stats_mode.load())
                {
                    state = UiState::Stats;
                }
                if (g_lcd_stats_mode.load())
                {
                    const SystemStats stats = read_system_stats(&previous_cpu_times, &has_previous_cpu_times);
                    draw_system_stats_panel(geometry_, stats, &status_frame);
                    lcd.display_rgb565(status_frame);
                    std::this_thread::sleep_for(std::chrono::milliseconds(stats_interval_ms));
                    ++tick;
                    continue;
                }
#ifdef SMART_SPEAKER_USE_OPENCV
                if (g_lcd_camera_mode.load())
                {
                    const std::shared_ptr<const CameraFrame> latest = get_recent_camera_frame();
                    if (latest && !latest->image.empty())
                    {
                        const cv::Mat &frame = latest->image;
                        cv::Mat overlay_frame;
                        const cv::Mat *display_frame = &frame;
                        auto mutable_overlay = [&]() -> cv::Mat &
                        {
                            if (overlay_frame.empty())
                            {
                                overlay_frame = frame.clone();
                                display_frame = &overlay_frame;
                            }
                            return overlay_frame;
                        };
#if defined(SMART_SPEAKER_USE_RKNN_PERSON) || defined(SMART_SPEAKER_USE_RKNN_EMOTION)
                        const std::shared_ptr<const VisionSnapshot> vision =
                            get_recent_vision_snapshot(vision_overlay_max_age_ms);
                        const bool emotion_active = g_emotion_mode.load();
                        if (vision && vision->emotion_mode == emotion_active)
                        {
#ifdef SMART_SPEAKER_USE_RKNN_PERSON
                            if (!vision->emotion_mode && !vision->person_detections.empty())
                            {
                                serial_motor::draw_person_detections(mutable_overlay(), vision->person_detections);
                            }
#endif
#ifdef SMART_SPEAKER_USE_RKNN_EMOTION
                            if (vision->emotion_mode && !vision->emotion_detections.empty())
                            {
                                serial_motor::draw_emotion_detections(mutable_overlay(), vision->emotion_detections);
                            }
#endif
                        }
#endif
                        bgr_mat_to_rgb565_bytes(*display_frame,
                                                geometry_,
                                                &camera_rgb565,
                                                &camera_resize_scratch,
                                                &camera_color_scratch,
                                                &camera_canvas_scratch);
                        lcd.display_rgb565_bytes(camera_rgb565);
                        std::this_thread::sleep_for(std::chrono::milliseconds(lcd_camera_interval_ms));
                        ++tick;
                        continue;
                    }
                }
#endif
#ifdef SMART_SPEAKER_USE_OPENCV
                if (emoji_player)
                {
                    emoji_player->show_next_frame(lcd);
                    std::this_thread::sleep_for(std::chrono::milliseconds(emoji_interval_ms));
                    ++tick;
                    continue;
                }
#endif
                draw_status_face(geometry_, state, tick++, &status_frame);
                lcd.display_rgb565(status_frame);
                std::this_thread::sleep_for(std::chrono::milliseconds(geometry_interval_ms));
            }
            lcd.clear();
        }
        catch (const std::exception &e)
        {
            std::cerr << "LCD disabled: " << e.what() << "\n";
        }
    }

    std::string spi_dev_;
    std::string emoji_root_;
    std::string emoji_states_;
    LcdGeometry geometry_;
    bool status_only_ = false;
    bool enabled_ = true;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

bool recognize_once(const std::string &mic_device,
                    const std::string &model_dir,
                    const RecordOptions &options,
                    std::string *text_out)
{
    PcmAudio audio;
    if (!record_pcm_alsa(mic_device, options, &audio))
    {
        return false;
    }
    const VoiceGateConfig gate = load_voice_gate_config();
    if (!passes_voice_energy_gate(audio, gate))
    {
        if (gate.debug)
        {
            std::cout << "voice gate rejected before ASR: samples=" << audio.samples.size()
                      << " avg_rms=" << audio.avg_rms
                      << " peak_rms=" << audio.peak_rms << "\n";
        }
        return false;
    }
    g_ui_state = static_cast<int>(UiState::Think);
    if (!asr_samples(model_dir, audio, text_out))
    {
        return false;
    }
    if (is_probable_asr_noise_text(*text_out))
    {
        if (gate.debug)
        {
            std::cout << "voice gate rejected ASR noise text: " << *text_out << "\n";
        }
        text_out->clear();
        return false;
    }
    return true;
}

void handle_emotion_command()
{
#ifdef SMART_SPEAKER_USE_OPENCV
#ifdef SMART_SPEAKER_USE_RKNN_EMOTION
    show_camera_on_lcd();
    send_desktop_control_command("emotion");
    g_emotion_mode = true;
    speak_text("开始情感识别。");
#else
    cv::Mat frame;
    if (!wait_for_camera_frame_for_request(&frame))
    {
        speak_text("我现在还没有拿到摄像头画面，稍后可以再试一次。");
        return;
    }
    show_camera_on_lcd();
    send_desktop_control_command("emotion");
    ask_vision(
        "请识别画面中主要人物的面部表情和情绪状态，用一句自然中文回答。不要做医学诊断，只描述看起来的表情。",
        encode_frame_to_jpeg_base64(frame));
#endif
#else
    speak_text("当前没有编译 OpenCV 摄像头支持。");
#endif
}

void handle_vision_command(const std::string &text)
{
#ifdef SMART_SPEAKER_USE_OPENCV
    cv::Mat frame;
    if (!wait_for_camera_frame_for_request(&frame))
    {
        speak_text("我现在还没有拿到摄像头画面，稍后可以再试一次。");
        return;
    }
    ask_vision(text, encode_frame_to_jpeg_base64(frame));
#else
    speak_text("当前没有编译 OpenCV 摄像头支持。");
#endif
}

bool handle_user_text(const std::string &text)
{
    std::cout << "User: " << text << "\n";
    switch (classify_user_intent(text))
    {
    case UserIntent::Stop:
        stop_music();
        show_expression_on_lcd();
        send_desktop_control_command("expression");
        speak_text("已停止。");
        return true;
    case UserIntent::ExitDialogue:
        stop_music();
        disable_emotion_mode();
        speak_text("好的，我先待命。");
        return false;
    case UserIntent::OpenCamera:
        show_camera_on_lcd();
        send_desktop_control_command("camera");
        speak_text("摄像头画面已打开。");
        return true;
    case UserIntent::CloseCamera:
        show_expression_on_lcd();
        send_desktop_control_command("expression");
        speak_text("已切回表情。");
        return true;
    case UserIntent::SystemStats:
        show_stats_on_lcd();
        speak_text("系统状态已显示。");
        return true;
    case UserIntent::Music:
        play_online_music(text);
        return true;
    case UserIntent::Emotion:
        handle_emotion_command();
        return true;
    case UserIntent::Vision:
        handle_vision_command(text);
        return true;
    case UserIntent::Chat:
        ask_llm(text);
        return true;
    }

    return true;
}

bool dispatch_user_text(const std::string &raw_text)
{
    const std::string text = trim(raw_text);
    if (text.empty())
    {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_command_mutex);
    g_command_active = true;
    try
    {
        const bool keep_dialogue = handle_user_text(text);
        g_command_active = false;
        return keep_dialogue;
    }
    catch (...)
    {
        g_command_active = false;
        throw;
    }
}

bool is_global_voice_shortcut_intent(UserIntent intent)
{
    switch (intent)
    {
    case UserIntent::Stop:
    case UserIntent::OpenCamera:
    case UserIntent::CloseCamera:
    case UserIntent::SystemStats:
    case UserIntent::Emotion:
        return true;
    case UserIntent::ExitDialogue:
    case UserIntent::Music:
    case UserIntent::Vision:
    case UserIntent::Chat:
        return false;
    }
    return false;
}

bool enqueue_keyboard_line_command(const std::string &raw_line,
                                   const char *source,
                                   bool allow_program_exit)
{
    const std::string line = trim(raw_line);
    if (line.empty())
    {
        return true;
    }
    if (is_keyboard_program_exit_request(line))
    {
        if (!allow_program_exit)
        {
            std::cout << source << ": exit ignored\n";
            return true;
        }
        std::cout << source << ": exit requested\n";
        stop_music();
        show_expression_on_lcd();
        g_running = 0;
        return false;
    }
    if (is_keyboard_wake_request(line))
    {
        if (std::string(source ? source : "") == "ui-fifo")
        {
            request_manual_wake(source);
            return true;
        }
        enqueue_keyboard_event({KeyboardEventType::TextWake, ""});
        const std::string first_text = strip_wake_word(line);
        if (!first_text.empty() && first_text != line)
        {
            enqueue_keyboard_event({KeyboardEventType::Command, first_text});
        }
        return true;
    }

    const std::string command_text = keyboard_command_to_text(line);
    enqueue_keyboard_event({KeyboardEventType::Command, command_text});
    return true;
}

void keyboard_control_loop()
{
    set_current_thread_name("xiaoman-key");
    std::cout << "keyboard: type w/wake/星期五 to chat; shortcuts work anytime: c/camera, f/face, p/status, e/emotion, v/vision, s/stop, m 歌名, q/exit。\n";
    std::string line;
    while (g_running && std::getline(std::cin, line))
    {
        if (!enqueue_keyboard_line_command(line, "keyboard", true))
        {
            break;
        }
    }
}

void handle_keyboard_event(const KeyboardEvent &event, KeyboardDialogueState *dialogue)
{
    if (event.type == KeyboardEventType::TextWake)
    {
        start_keyboard_dialogue(dialogue);
        return;
    }

    if (!dialogue->active)
    {
        const UserIntent intent = classify_user_intent(event.text);
        if (intent == UserIntent::Chat)
        {
            std::cout << "keyboard: sleeping, type w/wake/星期五 first\n";
            return;
        }
        if (intent == UserIntent::ExitDialogue)
        {
            std::cout << "keyboard: already sleeping\n";
            return;
        }

        (void)dispatch_user_text(event.text);
        g_ui_state = static_cast<int>(UiState::Idle);
        return;
    }

    const bool keep_dialogue = dispatch_user_text(event.text);
    if (keep_dialogue)
    {
        refresh_keyboard_dialogue_deadline(dialogue);
    }
    else
    {
        close_keyboard_dialogue(dialogue);
        std::cout << "keyboard: dialogue closed\n";
    }
    g_ui_state = static_cast<int>(UiState::Idle);
}

void drain_keyboard_events(KeyboardDialogueState *dialogue)
{
    KeyboardEvent event;
    while (g_running && pop_keyboard_event(&event))
    {
        handle_keyboard_event(event, dialogue);
    }
}

class UiCommandPipeWorker
{
public:
    explicit UiCommandPipeWorker(UiCommandPipeConfig config) : config_(std::move(config)) {}

    ~UiCommandPipeWorker()
    {
        stop();
    }

    void start()
    {
        if (!config_.enabled || running_.exchange(true))
        {
            return;
        }
        worker_ = std::thread([this]
                              { run(); });
    }

    void stop()
    {
        running_ = false;
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

private:
    void run()
    {
        set_current_thread_name("xiaoman-ui-fifo");
        try
        {
            ensure_fifo();
            const int fd = ::open(config_.path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
            {
                throw std::runtime_error("open UI command fifo failed: " + config_.path + ": " + std::strerror(errno));
            }
            std::cout << "ui command fifo=" << config_.path << "\n";

            std::string pending;
            char buffer[256];
            while (running_ && g_running)
            {
                const ssize_t n = ::read(fd, buffer, sizeof(buffer));
                if (n > 0)
                {
                    pending.append(buffer, buffer + n);
                    drain_lines(&pending);
                    if (pending.size() > 4096)
                    {
                        pending.clear();
                    }
                    continue;
                }
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                {
                    std::cerr << "warning: UI command fifo read failed: " << std::strerror(errno) << "\n";
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ::close(fd);
        }
        catch (const std::exception &e)
        {
            std::cerr << "warning: UI command fifo disabled: " << e.what() << "\n";
        }
    }

    void ensure_fifo()
    {
        if (config_.path.empty())
        {
            throw std::runtime_error("empty UI command fifo path");
        }
        struct stat st
        {
        };
        if (::stat(config_.path.c_str(), &st) == 0)
        {
            if (!S_ISFIFO(st.st_mode))
            {
                throw std::runtime_error(config_.path + " exists but is not a fifo");
            }
            allow_fifo_writers();
            return;
        }
        if (errno != ENOENT)
        {
            throw std::runtime_error("stat UI command fifo failed: " + config_.path + ": " + std::strerror(errno));
        }
        if (::mkfifo(config_.path.c_str(), 0666) != 0 && errno != EEXIST)
        {
            throw std::runtime_error("mkfifo failed: " + config_.path + ": " + std::strerror(errno));
        }
        allow_fifo_writers();
    }

    void allow_fifo_writers()
    {
        if (::chmod(config_.path.c_str(), 0666) != 0)
        {
            std::cerr << "warning: chmod UI command fifo failed: " << config_.path
                      << ": " << std::strerror(errno) << "\n";
        }
    }

    static void drain_lines(std::string *pending)
    {
        for (;;)
        {
            const std::string::size_type pos = pending->find('\n');
            if (pos == std::string::npos)
            {
                return;
            }
            std::string line = pending->substr(0, pos);
            pending->erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            enqueue_keyboard_line_command(line, "ui-fifo", false);
        }
    }

    UiCommandPipeConfig config_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

class DesktopProcess
{
public:
    explicit DesktopProcess(DesktopConfig config) : config_(std::move(config)) {}

    ~DesktopProcess()
    {
        stop();
    }

    void start()
    {
        if (!config_.autostart || config_.command.empty() || pid_ > 0)
        {
            return;
        }
        const pid_t pid = ::fork();
        if (pid < 0)
        {
            std::cerr << "warning: desktop autostart failed: " << std::strerror(errno) << "\n";
            return;
        }
        if (pid == 0)
        {
            ::execl("/bin/sh", "sh", "-lc", config_.command.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        pid_ = pid;
        std::cout << "desktop autostart pid=" << pid_ << "\n";
    }

    void stop()
    {
        if (pid_ <= 0)
        {
            return;
        }
        int status = 0;
        const pid_t done = ::waitpid(pid_, &status, WNOHANG);
        if (done == 0)
        {
            ::kill(pid_, SIGTERM);
            for (int i = 0; i < 20; ++i)
            {
                if (::waitpid(pid_, &status, WNOHANG) == pid_)
                {
                    pid_ = -1;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ::kill(pid_, SIGKILL);
            (void)::waitpid(pid_, &status, 0);
        }
        pid_ = -1;
    }

private:
    DesktopConfig config_;
    pid_t pid_ = -1;
};

void wake_button_loop(WakeButtonConfig config)
{
    set_current_thread_name("xiaoman-button");
    try
    {
        GpioInputPin button(config.gpio);
        std::cout << "wake button: gpio" << config.gpio
                  << " active_" << (config.active_low ? "low" : "high")
                  << " debounce=" << config.debounce_ms << "ms\n";

        bool last_active = config.active_low ? !button.high() : button.high();
        auto last_event = std::chrono::steady_clock::now() - std::chrono::milliseconds(config.debounce_ms);
        while (g_running)
        {
            const bool high = button.high();
            const bool active = config.active_low ? !high : high;
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_event).count();
            if (active && !last_active && elapsed_ms >= config.debounce_ms)
            {
                request_manual_wake("wake button");
                last_event = now;
            }
            last_active = active;
            std::this_thread::sleep_for(std::chrono::milliseconds(config.poll_ms));
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "wake button disabled: " << e.what() << "\n";
    }
}

void run_dialogue_loop(const std::string &mic_device,
                       const std::string &model_dir,
                       std::string first_text)
{
    speak_text(next_wake_reply());
    if (first_text.empty())
    {
        std::cout << "wake reply sent\n";
    }
    else
    {
        std::cout << "wake phrase also contained request: " << first_text << "\n";
    }

    bool keep_dialogue = true;
    int empty_turns = 0;
    while (g_running && keep_dialogue)
    {
        std::string text = trim(first_text);
        first_text.clear();
        if (text.empty())
        {
            g_ui_state = static_cast<int>(UiState::Listen);
                    std::cout << "friday: listening for your question...\n";
            if (!recognize_once(mic_device, model_dir, {kDialogueRecordSeconds, kDialogueMaxListenSeconds, true}, &text))
            {
                if (has_keyboard_event_pending())
                {
                    std::cout << "friday: keyboard input received, leaving voice dialogue\n";
                    break;
                }
                ++empty_turns;
                if (empty_turns >= 1)
                {
                    std::cout << "friday: dialogue timeout, back to wake mode\n";
                    break;
                }
                continue;
            }
        }
        empty_turns = 0;
        keep_dialogue = dispatch_user_text(text);
        g_ui_state = static_cast<int>(UiState::Idle);
    }
}

void run_dialogue_session(const std::string &mic_device,
                          const std::string &model_dir,
                          const std::string &source,
                          std::string first_text = "")
{
    bool expected = false;
    if (!g_dialogue_active.compare_exchange_strong(expected, true))
    {
        std::cout << source << ": dialogue already active\n";
        return;
    }
    try
    {
        run_dialogue_loop(mic_device, model_dir, std::move(first_text));
    }
    catch (...)
    {
        g_dialogue_active = false;
        throw;
    }
    g_dialogue_active = false;
}

struct Options
{
    Options();

    std::string mic_device;
    std::string lcd_spi_dev;
    std::string emoji_root;
    std::string emoji_states;
    CameraConfig camera;
    WakeButtonConfig wake_button;
    UiCommandPipeConfig ui_command_pipe;
    DesktopConfig desktop;
    LcdGeometry lcd_geometry;
    bool lcd_enabled;
    bool lcd_status_only;
    bool keyboard_enabled;
    bool voice_enabled;
    bool tts_enabled;
};

Options::Options()
    : mic_device(env_first({"XIAOMAN_MIC_DEVICE", "NEWBOT_CHAT_AUDIO_DEVICE", "AUDIODEV"})),
      lcd_spi_dev(env_first({"XIAOMAN_LCD_SPI_DEV", "NEWBOT_LCD_SPI_DEV"})),
      emoji_root(default_emoji_image_root()),
      emoji_states(env_first({"XIAOMAN_EMOJI_STATES", "NEWBOT_EMOJI_STATES"})),
      ui_command_pipe{env_bool({"XIAOMAN_UI_COMMAND_FIFO_ENABLED", "NEWBOT_UI_COMMAND_FIFO_ENABLED"}, false),
                      env_first({"XIAOMAN_UI_COMMAND_FIFO", "NEWBOT_UI_COMMAND_FIFO"})},
      desktop{env_bool({"XIAOMAN_DESKTOP_AUTOSTART", "NEWBOT_DESKTOP_AUTOSTART"}, false),
              env_first({"XIAOMAN_DESKTOP_COMMAND", "NEWBOT_DESKTOP_COMMAND"})},
      lcd_geometry(load_lcd_geometry()),
      lcd_enabled(env_bool({"XIAOMAN_LCD_ENABLED", "NEWBOT_LCD_ENABLED"}, true)),
      lcd_status_only(env_bool({"XIAOMAN_LCD_STATUS_ONLY", "NEWBOT_LCD_STATUS_ONLY"}, false)),
      keyboard_enabled(env_bool({"XIAOMAN_KEYBOARD_ENABLED", "NEWBOT_KEYBOARD_ENABLED"}, true)),
      voice_enabled(env_bool({"XIAOMAN_VOICE_ENABLED", "NEWBOT_VOICE_ENABLED"}, true)),
      tts_enabled(env_bool({"NEWBOT_TTS_ENABLED", "XIAOMAN_TTS_ENABLED"}, true))
{
    if (mic_device.empty())
    {
        mic_device = kDefaultMicDevice;
    }
    if (lcd_spi_dev.empty())
    {
        lcd_spi_dev = kDefaultLcdSpiDev;
    }
    if (ui_command_pipe.path.empty())
    {
        ui_command_pipe.path = kDefaultUiCommandFifo;
    }
    const std::string camera_dev = env_first({"XIAOMAN_CAMERA_DEV", "NEWBOT_CAMERA_DEV"});
    if (!camera_dev.empty())
    {
        camera.dev = camera_dev;
    }
    camera.width = env_int({"XIAOMAN_CAMERA_WIDTH", "NEWBOT_CAMERA_WIDTH"}, camera.width);
    camera.height = env_int({"XIAOMAN_CAMERA_HEIGHT", "NEWBOT_CAMERA_HEIGHT"}, camera.height);
    camera.fps = env_int({"XIAOMAN_CAMERA_FPS", "NEWBOT_CAMERA_FPS"}, camera.fps);
    camera.mjpeg = env_bool({"XIAOMAN_CAMERA_MJPEG", "NEWBOT_CAMERA_MJPEG"}, camera.mjpeg);
    wake_button.gpio = env_int({"XIAOMAN_WAKE_BUTTON_GPIO", "NEWBOT_WAKE_BUTTON_GPIO"}, wake_button.gpio);
    wake_button.active_low = env_bool({"XIAOMAN_WAKE_BUTTON_ACTIVE_LOW", "NEWBOT_WAKE_BUTTON_ACTIVE_LOW"}, wake_button.active_low);
    wake_button.poll_ms = std::max(5, env_int({"XIAOMAN_WAKE_BUTTON_POLL_MS", "NEWBOT_WAKE_BUTTON_POLL_MS"}, wake_button.poll_ms));
    wake_button.debounce_ms = std::max(20, env_int({"XIAOMAN_WAKE_BUTTON_DEBOUNCE_MS", "NEWBOT_WAKE_BUTTON_DEBOUNCE_MS"}, wake_button.debounce_ms));
}

Options parse_args(int argc, char **argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto need_value = [&](const std::string &name) -> std::string
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(name + " requires a value");
            }
            return argv[++i];
        };
        if (arg == "--config")
        {
            (void)need_value("--config");
        }
        else if (arg.rfind("--config=", 0) == 0)
        {
        }
        else if (arg == "--mic")
        {
            opt.mic_device = need_value("--mic");
        }
        else if (arg == "--camera-dev")
        {
            opt.camera.dev = need_value("--camera-dev");
        }
        else if (arg == "--camera-width")
        {
            opt.camera.width = std::stoi(need_value("--camera-width"));
        }
        else if (arg == "--camera-height")
        {
            opt.camera.height = std::stoi(need_value("--camera-height"));
        }
        else if (arg == "--camera-fps")
        {
            opt.camera.fps = std::stoi(need_value("--camera-fps"));
        }
        else if (arg == "--lcd-spi")
        {
            opt.lcd_spi_dev = need_value("--lcd-spi");
        }
        else if (arg == "--emoji-root")
        {
            opt.emoji_root = need_value("--emoji-root");
        }
        else if (arg == "--emoji-states")
        {
            opt.emoji_states = need_value("--emoji-states");
        }
        else if (arg == "--no-lcd")
        {
            opt.lcd_enabled = false;
        }
        else if (arg == "--lcd")
        {
            opt.lcd_enabled = true;
        }
        else if (arg == "--lcd-camera")
        {
            show_camera_on_lcd();
        }
        else if (arg == "--lcd-status-only")
        {
            opt.lcd_status_only = true;
        }
        else if (arg == "--lcd-legacy")
        {
            opt.lcd_status_only = false;
        }
        else if (arg == "--no-keyboard")
        {
            opt.keyboard_enabled = false;
        }
        else if (arg == "--keyboard")
        {
            opt.keyboard_enabled = true;
        }
        else if (arg == "--wake-button-gpio")
        {
            opt.wake_button.gpio = std::stoi(need_value("--wake-button-gpio"));
        }
        else if (arg.rfind("--wake-button-gpio=", 0) == 0)
        {
            opt.wake_button.gpio = std::stoi(arg.substr(std::string("--wake-button-gpio=").size()));
        }
        else if (arg == "--no-wake-button")
        {
            opt.wake_button.gpio = -1;
        }
        else if (arg == "--ui-command-fifo")
        {
            opt.ui_command_pipe.enabled = true;
            opt.ui_command_pipe.path = need_value("--ui-command-fifo");
        }
        else if (arg.rfind("--ui-command-fifo=", 0) == 0)
        {
            opt.ui_command_pipe.enabled = true;
            opt.ui_command_pipe.path = arg.substr(std::string("--ui-command-fifo=").size());
        }
        else if (arg == "--no-ui-command-fifo")
        {
            opt.ui_command_pipe.enabled = false;
        }
        else if (arg == "--desktop-autostart")
        {
            opt.desktop.autostart = true;
        }
        else if (arg == "--no-desktop-autostart")
        {
            opt.desktop.autostart = false;
        }
        else if (arg == "--desktop-command")
        {
            opt.desktop.command = need_value("--desktop-command");
        }
        else if (arg.rfind("--desktop-command=", 0) == 0)
        {
            opt.desktop.command = arg.substr(std::string("--desktop-command=").size());
        }
        else if (arg == "--voice-off" ||
                 arg == "voice-off" ||
                 arg == "语音off" || arg == "语音关闭")
        {
            opt.voice_enabled = false;
            opt.tts_enabled = false;
        }
        else if (arg == "--voice-on")
        {
            opt.voice_enabled = true;
        }
        else if (arg == "--tts-off" || arg == "--no-tts" ||
                 arg == "--print-only" || arg == "--silent-reply" ||
                 arg == "tts-off" || arg == "静音回复")
        {
            opt.tts_enabled = false;
        }
        else if (arg == "--tts-on")
        {
            opt.tts_enabled = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "Usage: smart_voice_speaker [--config friday_voice_speaker.conf]\n"
                      << "                           [--mic plughw:2,0] [--camera-dev /dev/video0]\n"
                      << "                           [--camera-width 640] [--camera-height 480] [--camera-fps 15]\n"
                      << "                           [--lcd-spi /dev/spidev3.0] [--no-lcd] [--lcd-camera] [--lcd-status-only]\n"
                      << "                           [--emoji-root image] [--emoji-states 正常,微笑,眨眼]\n"
                      << "                           [--no-keyboard] [--keyboard] [--wake-button-gpio N]\n"
                      << "                           [--ui-command-fifo /tmp/friday_voice_speaker_ui.fifo]\n"
                      << "                           [--desktop-autostart --desktop-command CMD]\n"
                      << "                           [--no-wake-button] [--voice-off] [--voice-on]\n"
                      << "                           [--tts-off|--print-only] [--tts-on]\n";
            std::exit(0);
        }
        else
        {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return opt;
}

} // namespace

int main(int argc, char **argv)
{
    ::signal(SIGINT, handle_signal);
    ::signal(SIGTERM, handle_signal);

    try
    {
        const ConfigLoadResult config = load_config_from_args(argc, argv);
        const Options opt = parse_args(argc, argv);
        g_voice_enabled = opt.voice_enabled;
        g_tts_enabled = opt.tts_enabled;
        if (!opt.tts_enabled)
        {
            ::setenv("NEWBOT_TTS_ENABLED", "0", 1);
        }
        else
        {
            ::setenv("NEWBOT_TTS_ENABLED", "1", 1);
        }
        std::string model_dir = "disabled";
        bool asr_model_found = false;
        if (opt.voice_enabled)
        {
            try
            {
                model_dir = find_asr_model_dir();
                asr_model_found = true;
            }
            catch (const std::exception &e)
            {
                model_dir = "unavailable";
                std::cerr << "warning: ASR model lookup failed: " << e.what() << "\n";
            }
        }
        std::cout << kAssistantName << "智能语音音箱启动\n";
        if (config.loaded)
        {
            std::cout << "config=" << config.path
                      << " applied=" << config.applied
                      << " skipped_env=" << config.skipped_existing << "\n";
        }
        std::cout << "wake words: 你好星期五 / 星期五\n";
        std::cout << "mic=" << opt.mic_device << "\n";
        std::cout << "camera=" << opt.camera.dev << " "
                  << opt.camera.width << "x" << opt.camera.height
                  << "@" << opt.camera.fps << "\n";
        std::cout << "lcd spi=" << opt.lcd_spi_dev << "\n";
        std::cout << "ASR model=" << model_dir << "\n";
        std::cout << "emoji root=" << opt.emoji_root << "\n";
        std::cout << "small lcd=" << (opt.lcd_status_only ? "status-only" : "legacy-display") << "\n";
        std::cout << "keyboard=" << (opt.keyboard_enabled ? "enabled" : "disabled") << "\n";
        std::cout << "ui fifo="
                  << (opt.ui_command_pipe.enabled ? opt.ui_command_pipe.path : "disabled")
                  << "\n";
        std::cout << "desktop control fifo=" << desktop_control_fifo_path() << "\n";
        std::cout << "wake button="
                  << (opt.wake_button.gpio >= 0 ? ("gpio" + std::to_string(opt.wake_button.gpio)) : "disabled")
                  << "\n";
        std::cout << "voice input=" << (opt.voice_enabled ? "enabled" : "off") << "\n";
        std::cout << "tts playback=" << (opt.tts_enabled ? "enabled" : "print-only") << "\n";

        DisplayWorker display(opt.lcd_spi_dev, opt.lcd_enabled, opt.emoji_root, opt.emoji_states, opt.lcd_geometry, opt.lcd_status_only);
        display.start();

        UiCommandPipeWorker ui_pipe(opt.ui_command_pipe);
        ui_pipe.start();

        DesktopProcess desktop(opt.desktop);
        desktop.start();

#ifdef SMART_SPEAKER_USE_OPENCV
        BackgroundCamera camera(opt.camera);
        camera.start();
#if defined(SMART_SPEAKER_USE_RKNN_PERSON) || defined(SMART_SPEAKER_USE_RKNN_EMOTION)
        VisionWorker vision;
        vision.start();
#endif
#else
        std::cerr << "warning: OpenCV not found at build time; camera and vision are disabled.\n";
#endif

        bool asr_ready = false;
#ifdef SMART_SPEAKER_USE_SHERPA_ONNX_C_API
        if (opt.voice_enabled && asr_model_found)
        {
            std::lock_guard<std::mutex> lock(g_asr_mutex);
            if (ensure_recognizer_locked(model_dir) != nullptr)
            {
                asr_ready = true;
                std::cout << "ASR recognizer ready\n";
            }
            else
            {
                std::cerr << "warning: ASR recognizer is unavailable; voice wake is disabled until the model/runtime is fixed\n";
            }
        }
        else
        {
            std::cout << "ASR recognizer skipped: "
                      << (opt.voice_enabled ? "model is unavailable" : "voice is off") << "\n";
        }
#else
        if (opt.voice_enabled)
        {
            std::cerr << "warning: sherpa-onnx C API is not enabled; voice wake is disabled\n";
        }
#endif

        if (opt.keyboard_enabled)
        {
            if (::isatty(STDIN_FILENO))
            {
                std::thread(keyboard_control_loop).detach();
            }
            else
            {
                std::cout << "keyboard: stdin is not a terminal, keyboard control disabled\n";
            }
        }
        if (opt.wake_button.gpio >= 0)
        {
            std::thread(wake_button_loop, opt.wake_button).detach();
        }

        KeyboardDialogueState keyboard_dialogue;
        while (g_running)
        {
            drain_keyboard_events(&keyboard_dialogue);
            if (!g_running)
            {
                break;
            }
            if (keyboard_dialogue_expired(keyboard_dialogue))
            {
                close_keyboard_dialogue(&keyboard_dialogue);
                std::cout << "keyboard: dialogue timeout, back to wake mode\n";
            }
            if (g_command_active.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (has_keyboard_event_pending())
            {
                continue;
            }
            if (consume_manual_wake_request())
            {
                if (!opt.voice_enabled || !asr_ready)
                {
                    std::cout << "manual wake ignored: voice ASR is unavailable\n";
                    continue;
                }
                if (keyboard_dialogue.active)
                {
                    close_keyboard_dialogue(&keyboard_dialogue);
                    std::cout << "keyboard: dialogue interrupted by manual wake\n";
                }
                g_ui_state = static_cast<int>(UiState::Wake);
                run_dialogue_session(opt.mic_device, model_dir, "manual wake");
                continue;
            }
            if (!opt.voice_enabled || !asr_ready)
            {
                g_ui_state = static_cast<int>(UiState::Idle);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            g_ui_state = static_cast<int>(UiState::Idle);
            std::cout << "friday: waiting for wake word...\n";
            std::string heard;
            if (!recognize_once(opt.mic_device, model_dir, {kWakeRecordSeconds, kWakeMaxListenSeconds, true}, &heard))
            {
                continue;
            }
            std::cout << "heard: " << heard << "\n";
            if (!has_wake_word(heard))
            {
                const UserIntent intent = classify_user_intent(heard);
                if (is_global_voice_shortcut_intent(intent))
                {
                    if (keyboard_dialogue.active)
                    {
                        close_keyboard_dialogue(&keyboard_dialogue);
                        std::cout << "keyboard: dialogue interrupted by voice shortcut\n";
                    }
                    std::cout << "voice shortcut: " << heard << "\n";
                    (void)dispatch_user_text(heard);
                    g_ui_state = static_cast<int>(UiState::Idle);
                }
                continue;
            }
            if (keyboard_dialogue.active)
            {
                close_keyboard_dialogue(&keyboard_dialogue);
                std::cout << "keyboard: dialogue interrupted by voice wake\n";
            }
            g_ui_state = static_cast<int>(UiState::Wake);
            std::string first_text = strip_wake_word(heard);
            run_dialogue_session(opt.mic_device, model_dir, "voice wake", first_text);
        }

        stop_music();
        destroy_recognizer();
        std::cout << kAssistantName << "已退出\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        g_ui_state = static_cast<int>(UiState::Error);
        std::cerr << "fatal: " << e.what() << "\n";
        stop_music();
        destroy_recognizer();
        return 1;
    }
}
