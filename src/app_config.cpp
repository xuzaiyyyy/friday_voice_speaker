#include "app_config.h"

#include "app_utils.h"

#include <cstdlib>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace xiaoman
{
namespace
{

constexpr const char *kDefaultConfigName = "friday_voice_speaker.conf";

std::string dirname_of(const std::string &path)
{
    const size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos)
    {
        return ".";
    }
    if (pos == 0)
    {
        return "/";
    }
    return path.substr(0, pos);
}

bool is_absolute_path(const std::string &value)
{
    return !value.empty() &&
           (value[0] == '/' ||
            value.rfind("~/", 0) == 0 ||
            (value.size() > 2 && value[1] == ':'));
}

std::string absolute_path_for_config(const std::string &path)
{
    if (is_absolute_path(path))
    {
        return path;
    }
    const std::vector<std::string> dirs = cwd_and_parents(1);
    if (dirs.empty())
    {
        return path;
    }
    return join_path(dirs.front(), path);
}

bool is_env_name(const std::string &key)
{
    if (key.empty() || !(std::isalpha(static_cast<unsigned char>(key[0])) || key[0] == '_'))
    {
        return false;
    }
    for (char ch : key)
    {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
        {
            return false;
        }
    }
    return true;
}

std::string strip_inline_comment(const std::string &value)
{
    bool single_quote = false;
    bool double_quote = false;
    for (size_t i = 0; i < value.size(); ++i)
    {
        const char ch = value[i];
        if (ch == '\'' && !double_quote)
        {
            single_quote = !single_quote;
        }
        else if (ch == '"' && !single_quote)
        {
            double_quote = !double_quote;
        }
        else if (ch == '#' && !single_quote && !double_quote &&
                 (i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1]))))
        {
            return trim(value.substr(0, i));
        }
    }
    return trim(value);
}

std::string unquote(std::string value)
{
    value = strip_inline_comment(value);
    if (value.size() >= 2)
    {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
        {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

std::string map_config_key(const std::string &key)
{
    static const std::unordered_map<std::string, std::string> aliases{
        {"mic_device", "XIAOMAN_MIC_DEVICE"},
        {"camera_dev", "XIAOMAN_CAMERA_DEV"},
        {"camera_width", "XIAOMAN_CAMERA_WIDTH"},
        {"camera_height", "XIAOMAN_CAMERA_HEIGHT"},
        {"camera_fps", "XIAOMAN_CAMERA_FPS"},
        {"camera_capture_interval_ms", "XIAOMAN_CAMERA_CAPTURE_INTERVAL_MS"},
        {"camera_always_on", "XIAOMAN_CAMERA_ALWAYS_ON"},
        {"camera_idle_sleep_ms", "XIAOMAN_CAMERA_IDLE_SLEEP_MS"},
        {"camera_warmup_ms", "XIAOMAN_CAMERA_WARMUP_MS"},
        {"camera_preview_file", "XIAOMAN_CAMERA_PREVIEW_FILE"},
        {"camera_preview_interval_ms", "XIAOMAN_CAMERA_PREVIEW_INTERVAL_MS"},
        {"camera_preview_jpeg_quality", "XIAOMAN_CAMERA_PREVIEW_JPEG_QUALITY"},
        {"lcd_spi_dev", "XIAOMAN_LCD_SPI_DEV"},
        {"lcd_enabled", "XIAOMAN_LCD_ENABLED"},
        {"lcd_driver", "XIAOMAN_LCD_DRIVER"},
        {"lcd_width", "XIAOMAN_LCD_WIDTH"},
        {"lcd_height", "XIAOMAN_LCD_HEIGHT"},
        {"lcd_x_offset", "XIAOMAN_LCD_X_OFFSET"},
        {"lcd_y_offset", "XIAOMAN_LCD_Y_OFFSET"},
        {"lcd_madctl", "XIAOMAN_LCD_MADCTL"},
        {"lcd_spi_hz", "XIAOMAN_LCD_SPI_HZ"},
        {"lcd_dc_gpio", "XIAOMAN_LCD_DC_GPIO"},
        {"lcd_reset_gpio", "XIAOMAN_LCD_RESET_GPIO"},
        {"lcd_backlight_gpio", "XIAOMAN_LCD_BACKLIGHT_GPIO"},
        {"lcd_backlight_active_high", "XIAOMAN_LCD_BACKLIGHT_ACTIVE_HIGH"},
        {"lcd_status_only", "XIAOMAN_LCD_STATUS_ONLY"},
        {"lcd_camera_interval_ms", "XIAOMAN_LCD_CAMERA_INTERVAL_MS"},
        {"lcd_emoji_interval_ms", "XIAOMAN_LCD_EMOJI_INTERVAL_MS"},
        {"lcd_geometry_interval_ms", "XIAOMAN_LCD_GEOMETRY_INTERVAL_MS"},
        {"lcd_stats_interval_ms", "XIAOMAN_LCD_STATS_INTERVAL_MS"},
        {"ui_command_fifo_enabled", "XIAOMAN_UI_COMMAND_FIFO_ENABLED"},
        {"ui_command_fifo", "XIAOMAN_UI_COMMAND_FIFO"},
        {"desktop_control_fifo", "XIAOMAN_DESKTOP_CONTROL_FIFO"},
        {"desktop_autostart", "XIAOMAN_DESKTOP_AUTOSTART"},
        {"desktop_command", "XIAOMAN_DESKTOP_COMMAND"},
        {"keyboard_enabled", "XIAOMAN_KEYBOARD_ENABLED"},
        {"keyboard_dialogue_timeout_seconds", "XIAOMAN_KEYBOARD_DIALOGUE_TIMEOUT_SECONDS"},
        {"wake_button_gpio", "XIAOMAN_WAKE_BUTTON_GPIO"},
        {"wake_button_active_low", "XIAOMAN_WAKE_BUTTON_ACTIVE_LOW"},
        {"wake_button_poll_ms", "XIAOMAN_WAKE_BUTTON_POLL_MS"},
        {"wake_button_debounce_ms", "XIAOMAN_WAKE_BUTTON_DEBOUNCE_MS"},
        {"voice_enabled", "XIAOMAN_VOICE_ENABLED"},
        {"emoji_root", "XIAOMAN_EMOJI_ROOT"},
        {"emoji_states", "XIAOMAN_EMOJI_STATES"},
        {"asr_model_dir", "XIAOMAN_ASR_MODEL_DIR"},
        {"asr_threads", "XIAOMAN_ASR_THREADS"},
        {"voice_start_rms", "XIAOMAN_VOICE_START_RMS"},
        {"voice_stop_rms", "XIAOMAN_VOICE_STOP_RMS"},
        {"voice_min_avg_rms", "XIAOMAN_VOICE_MIN_AVG_RMS"},
        {"voice_min_peak_rms", "XIAOMAN_VOICE_MIN_PEAK_RMS"},
        {"voice_noise_ratio", "XIAOMAN_VOICE_NOISE_RATIO"},
        {"voice_confirm_ms", "XIAOMAN_VOICE_CONFIRM_MS"},
        {"voice_min_ms", "XIAOMAN_VOICE_MIN_MS"},
        {"voice_silence_stop_ms", "XIAOMAN_VOICE_SILENCE_STOP_MS"},
        {"voice_pre_roll_ms", "XIAOMAN_VOICE_PRE_ROLL_MS"},
        {"voice_debug", "XIAOMAN_VOICE_DEBUG"},
        {"llm_helper", "XIAOMAN_LLM_HELPER"},
        {"local_llm_url", "NEWBOT_LOCAL_LLM_URL"},
        {"local_llm_model", "NEWBOT_LOCAL_LLM_MODEL"},
        {"local_llm_model_path", "NEWBOT_LOCAL_LLM_MODEL_PATH"},
        {"chat_backend", "NEWBOT_CHAT_BACKEND"},
        {"cloud_llm_max_tokens", "NEWBOT_CLOUD_LLM_MAX_TOKENS"},
        {"cloud_llm_temperature", "NEWBOT_CLOUD_LLM_TEMPERATURE"},
        {"allow_cloud_llm_fallback", "NEWBOT_ALLOW_CLOUD_LLM_FALLBACK"},
        {"xunfei_apipassword", "XUNFEI_APIPASSWORD"},
        {"chat_memory_enabled", "NEWBOT_CHAT_MEMORY_ENABLED"},
        {"chat_memory_path", "NEWBOT_CHAT_MEMORY_PATH"},
        {"chat_memory_turns", "NEWBOT_CHAT_MEMORY_TURNS"},
        {"chat_memory_max_chars", "NEWBOT_CHAT_MEMORY_MAX_CHARS"},
        {"llama_server", "NEWBOT_LLAMA_SERVER"},
        {"person_model", "XIAOMAN_YOLO_PERSON_MODEL"},
        {"person_detect_interval_ms", "XIAOMAN_PERSON_DETECT_INTERVAL_MS"},
        {"emotion_model", "XIAOMAN_EMOTION_MODEL"},
        {"emotion_detect_interval_ms", "XIAOMAN_EMOTION_DETECT_INTERVAL_MS"},
        {"vision_idle_sleep_ms", "XIAOMAN_VISION_IDLE_SLEEP_MS"},
        {"vision_frame_wait_ms", "XIAOMAN_VISION_FRAME_WAIT_MS"},
        {"vision_overlay_max_age_ms", "XIAOMAN_VISION_OVERLAY_MAX_AGE_MS"},
        {"vision_persona_enabled", "NEWBOT_VISION_PERSONA_ENABLED"},
        {"vision_refine_enabled", "NEWBOT_VISION_REFINE_ENABLED"},
        {"vision_refine_mode", "NEWBOT_VISION_REFINE_MODE"},
        {"vision_refine_max_tokens", "NEWBOT_VISION_REFINE_MAX_TOKENS"},
        {"vision_speech_max_chars", "NEWBOT_VISION_SPEECH_MAX_CHARS"},
        {"vision_debug", "NEWBOT_VISION_DEBUG"},
        {"tts_enabled", "NEWBOT_TTS_ENABLED"},
        {"tts_voice", "NEWBOT_TTS_VOICE"},
        {"tts_rate", "NEWBOT_TTS_RATE"},
        {"tts_volume", "NEWBOT_TTS_VOLUME"},
        {"tts_pitch", "NEWBOT_TTS_PITCH"},
        {"tts_player", "NEWBOT_TTS_PLAYER"},
        {"tts_alsa_device", "NEWBOT_TTS_ALSA_DEVICE"},
        {"tts_backend", "NEWBOT_TTS_BACKEND"},
        {"tts_command", "NEWBOT_TTS_COMMAND"},
        {"tts_command_output_suffix", "NEWBOT_TTS_COMMAND_OUTPUT_SUFFIX"},
        {"tts_clip_enabled", "NEWBOT_TTS_CLIP_ENABLED"},
        {"tts_clip_root", "NEWBOT_TTS_CLIP_ROOT"},
        {"tts_cache_enabled", "NEWBOT_TTS_CACHE_ENABLED"},
        {"tts_cache_dir", "NEWBOT_TTS_CACHE_DIR"},
        {"tts_cache_namespace", "NEWBOT_TTS_CACHE_NAMESPACE"},
    };
    const auto it = aliases.find(key);
    return it == aliases.end() ? key : it->second;
}

bool should_resolve_relative_path(const std::string &key, const std::string &value)
{
    if (value.empty() || is_absolute_path(value))
    {
        return false;
    }
    if (key == "NEWBOT_LLAMA_SERVER" && value.find('/') == std::string::npos)
    {
        return false;
    }
    return key == "XIAOMAN_ASR_MODEL_DIR" ||
           key == "XIAOMAN_EMOJI_ROOT" ||
           key == "XIAOMAN_LLM_HELPER" ||
           key == "NEWBOT_LOCAL_LLM_MODEL_PATH" ||
           key == "NEWBOT_CHAT_MEMORY_PATH" ||
           key == "NEWBOT_LLAMA_SERVER" ||
           key == "XIAOMAN_YOLO_PERSON_MODEL" ||
           key == "NEWBOT_YOLO_PERSON_MODEL" ||
           key == "XIAOMAN_EMOTION_MODEL" ||
           key == "NEWBOT_EMOTION_MODEL" ||
           key == "NEWBOT_TTS_CLIP_ROOT" ||
           key == "NEWBOT_TTS_CACHE_DIR";
}

std::string resolve_config_value(const std::string &key,
                                 const std::string &value,
                                 const std::string &config_dir)
{
    if (value.rfind("~/", 0) == 0)
    {
        const char *home = std::getenv("HOME");
        if (home != nullptr && home[0] != '\0')
        {
            return join_path(home, value.substr(2));
        }
    }
    if (!should_resolve_relative_path(key, value))
    {
        return value;
    }
    return join_path(config_dir, value);
}

void set_process_env(const std::string &key, const std::string &value)
{
#ifdef _WIN32
    _putenv_s(key.c_str(), value.c_str());
#else
    ::setenv(key.c_str(), value.c_str(), 0);
#endif
}

std::string explicit_config_path_from_args(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc)
        {
            return argv[i + 1];
        }
        constexpr const char *prefix = "--config=";
        if (arg.rfind(prefix, 0) == 0)
        {
            return arg.substr(std::strlen(prefix));
        }
    }
    return env_first({"XIAOMAN_CONFIG_FILE", "NEWBOT_CONFIG_FILE"});
}

std::string find_default_config_path()
{
    std::vector<std::string> candidates{
        kDefaultConfigName,
        join_path("..", kDefaultConfigName),
        join_path("config", kDefaultConfigName),
        join_path(join_path("..", "config"), kDefaultConfigName),
    };
    for (const std::string &dir : cwd_and_parents(6))
    {
        candidates.push_back(join_path(dir, kDefaultConfigName));
        candidates.push_back(join_path(join_path(dir, "config"), kDefaultConfigName));
        candidates.push_back(join_path(join_path(dir, "friday_voice_speaker"), kDefaultConfigName));
    }
    candidates.push_back("/home/orangepi/cpp/friday_voice_speaker/friday_voice_speaker.conf");

    for (const std::string &candidate : candidates)
    {
        if (path_exists(candidate))
        {
            return candidate;
        }
    }
    return "";
}

} // namespace

ConfigLoadResult load_config_from_args(int argc, char **argv)
{
    ConfigLoadResult result;
    std::string config_path = explicit_config_path_from_args(argc, argv);
    const bool explicit_path = !config_path.empty();
    if (!explicit_path)
    {
        config_path = find_default_config_path();
    }
    if (config_path.empty())
    {
        return result;
    }
    if (!path_exists(config_path))
    {
        if (explicit_path)
        {
            throw std::runtime_error("config file not found: " + config_path);
        }
        return result;
    }
    config_path = absolute_path_for_config(config_path);

    std::ifstream in(config_path);
    if (!in)
    {
        throw std::runtime_error("cannot open config file: " + config_path);
    }

    const std::string config_dir = dirname_of(config_path);
    result.loaded = true;
    result.path = config_path;

    std::string line;
    size_t line_no = 0;
    while (std::getline(in, line))
    {
        ++line_no;
        std::string text = trim(line);
        if (text.empty() || text[0] == '#')
        {
            continue;
        }
        constexpr const char *export_prefix = "export ";
        if (text.rfind(export_prefix, 0) == 0)
        {
            text = trim(text.substr(std::strlen(export_prefix)));
        }

        const size_t eq = text.find('=');
        if (eq == std::string::npos)
        {
            std::cerr << "config warning: ignore line " << line_no << " without '='\n";
            continue;
        }

        const std::string raw_key = trim(text.substr(0, eq));
        const std::string key = map_config_key(raw_key);
        if (!is_env_name(key))
        {
            std::cerr << "config warning: ignore invalid key at line " << line_no << ": " << raw_key << "\n";
            continue;
        }
        if (std::getenv(key.c_str()) != nullptr)
        {
            ++result.skipped_existing;
            continue;
        }

        const std::string raw_value = unquote(text.substr(eq + 1));
        const std::string value = resolve_config_value(key, raw_value, config_dir);
        set_process_env(key, value);
        ++result.applied;
    }

    return result;
}

} // namespace xiaoman
