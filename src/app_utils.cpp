#include "app_utils.h"

#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace xiaoman
{
namespace
{

constexpr const char *kDefaultAsrModelDirName = "sherpa-onnx-paraformer-zh-small-2024-03-09";

bool is_directory_mode(int mode)
{
#ifdef _WIN32
    return (mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(mode);
#endif
}

std::string parent_path(const std::string &path)
{
    const size_t pos = path.find_last_of('/');
    if (pos == std::string::npos || pos == 0)
    {
        return "/";
    }
    return path.substr(0, pos);
}

} // namespace

std::string trim(const std::string &text)
{
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
    {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string to_lower_ascii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return text;
}

bool path_exists(const std::string &path)
{
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

bool dir_exists(const std::string &path)
{
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && is_directory_mode(st.st_mode);
}

std::string join_path(const std::string &a, const std::string &b)
{
    if (a.empty())
    {
        return b;
    }
    if (a.back() == '/')
    {
        return a + b;
    }
    return a + "/" + b;
}

std::vector<std::string> cwd_and_parents(int max_dirs)
{
    std::vector<std::string> dirs;
    if (max_dirs <= 0)
    {
        return dirs;
    }

    char cwd[4096]{};
#ifdef _WIN32
    if (::_getcwd(cwd, sizeof(cwd)) == nullptr)
#else
    if (::getcwd(cwd, sizeof(cwd)) == nullptr)
#endif
    {
        return dirs;
    }

    std::string current = cwd;
    for (int i = 0; i < max_dirs && !current.empty(); ++i)
    {
        dirs.push_back(current);
        const std::string parent = parent_path(current);
        if (parent == current)
        {
            break;
        }
        current = parent;
    }
    return dirs;
}

std::vector<std::string> split_csv(const std::string &text)
{
    std::vector<std::string> items;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t comma = text.find(',', start);
        const size_t end = comma == std::string::npos ? text.size() : comma;
        std::string item = trim(text.substr(start, end - start));
        if (!item.empty())
        {
            items.push_back(item);
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return items;
}

std::string env_first(std::initializer_list<const char *> names)
{
    for (const char *name : names)
    {
        const char *value = std::getenv(name);
        if (value != nullptr && value[0] != '\0')
        {
            return value;
        }
    }
    return "";
}

bool env_bool(std::initializer_list<const char *> names, bool default_value)
{
    const std::string value = to_lower_ascii(trim(env_first(names)));
    if (value.empty())
    {
        return default_value;
    }
    if (value == "1" || value == "true" || value == "yes" || value == "on" || value == "enable" || value == "enabled")
    {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off" || value == "disable" || value == "disabled")
    {
        return false;
    }
    return default_value;
}

int env_int(std::initializer_list<const char *> names, int default_value)
{
    const std::string value = trim(env_first(names));
    if (value.empty())
    {
        return default_value;
    }
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' ||
        parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max())
    {
        return default_value;
    }
    return static_cast<int>(parsed);
}

std::string default_emoji_image_root()
{
    const std::string explicit_root = env_first({"XIAOMAN_EMOJI_ROOT", "NEWBOT_EMOJI_ROOT"});
    if (!explicit_root.empty())
    {
        return explicit_root;
    }

    std::vector<std::string> candidates{
        "image",
        "./image",
        "../image",
        "friday_voice_speaker/image",
        "../friday_voice_speaker/image",
    };
    for (const std::string &dir : cwd_and_parents(6))
    {
        candidates.push_back(join_path(dir, "image"));
        candidates.push_back(join_path(join_path(dir, "friday_voice_speaker"), "image"));
    }
    candidates.push_back("/home/orangepi/cpp/friday_voice_speaker/image");

    for (const std::string &candidate : candidates)
    {
        if (dir_exists(candidate))
        {
            return candidate;
        }
    }
    return "image";
}

std::string find_asr_model_dir()
{
    const std::string explicit_dir = env_first({"XIAOMAN_ASR_MODEL_DIR", "NEWBOT_ASR_MODEL_DIR"});
    if (!explicit_dir.empty())
    {
        if (path_exists(join_path(explicit_dir, "model.int8.onnx")) &&
            path_exists(join_path(explicit_dir, "tokens.txt")))
        {
            return explicit_dir;
        }
        std::cerr << "warning: explicit ASR model dir is invalid, falling back: "
                  << explicit_dir << "\n";
    }

    std::vector<std::string> candidates{
        join_path("models", kDefaultAsrModelDirName),
        join_path(join_path("..", "models"), kDefaultAsrModelDirName),
        join_path(join_path("friday_voice_speaker", "models"), kDefaultAsrModelDirName),
        join_path(join_path("serial_motor_control", "models"), kDefaultAsrModelDirName),
    };
    for (const std::string &dir : cwd_and_parents(6))
    {
        candidates.push_back(join_path(join_path(dir, "models"), kDefaultAsrModelDirName));
        candidates.push_back(join_path(join_path(dir, "friday_voice_speaker/models"), kDefaultAsrModelDirName));
        candidates.push_back(join_path(join_path(dir, "serial_motor_control/models"), kDefaultAsrModelDirName));
        candidates.push_back(join_path(join_path(dir, "tools/serial_motor_control/models"), kDefaultAsrModelDirName));
    }
    candidates.push_back(join_path("/home/orangepi/cpp/friday_voice_speaker/models", kDefaultAsrModelDirName));
    candidates.push_back(join_path("/home/orangepi/cpp/serial_motor_control/models", kDefaultAsrModelDirName));
    candidates.push_back(join_path("/home/orangepi/cpp/tools/serial_motor_control/models", kDefaultAsrModelDirName));

    for (const std::string &candidate : candidates)
    {
        if (path_exists(join_path(candidate, "model.int8.onnx")) &&
            path_exists(join_path(candidate, "tokens.txt")))
        {
            return candidate;
        }
    }
    throw std::runtime_error("cannot find ASR model. Set XIAOMAN_ASR_MODEL_DIR.");
}

std::string find_helper_path()
{
    const std::string explicit_path = env_first({"XIAOMAN_LLM_HELPER", "NEWBOT_LLM_HELPER"});
    if (!explicit_path.empty() && path_exists(explicit_path))
    {
        return explicit_path;
    }

    constexpr const char *helper_name = "xunfei_llm_chat.py";
    std::vector<std::string> candidates{helper_name, join_path("..", helper_name)};
    for (const std::string &dir : cwd_and_parents(6))
    {
        candidates.push_back(join_path(dir, helper_name));
        candidates.push_back(join_path(join_path(dir, "friday_voice_speaker"), helper_name));
        candidates.push_back(join_path(join_path(dir, "serial_motor_control"), helper_name));
    }
    candidates.push_back("/home/orangepi/cpp/friday_voice_speaker/xunfei_llm_chat.py");
    candidates.push_back("/home/orangepi/cpp/serial_motor_control/xunfei_llm_chat.py");

    for (const std::string &candidate : candidates)
    {
        if (path_exists(candidate))
        {
            return candidate;
        }
    }
    throw std::runtime_error("cannot find xunfei_llm_chat.py. Set XIAOMAN_LLM_HELPER.");
}

std::string json_escape(const std::string &input)
{
    std::ostringstream out;
    for (unsigned char ch : input)
    {
        switch (ch)
        {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                out << "\\u00";
                const char *hex = "0123456789abcdef";
                out << hex[(ch >> 4) & 0xf] << hex[ch & 0xf];
            }
            else
            {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

std::string base64_encode(const uint8_t *data, size_t size)
{
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3)
    {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1 < size) ? data[i + 1] : 0;
        const uint32_t b2 = (i + 2 < size) ? data[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kTable[(triple >> 18) & 0x3f]);
        out.push_back(kTable[(triple >> 12) & 0x3f]);
        out.push_back((i + 1 < size) ? kTable[(triple >> 6) & 0x3f] : '=');
        out.push_back((i + 2 < size) ? kTable[triple & 0x3f] : '=');
    }
    return out;
}

} // namespace xiaoman
