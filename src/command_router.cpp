#include "command_router.h"

#include "app_utils.h"

#include <array>
#include <atomic>
#include <cstring>
#include <initializer_list>

namespace xiaoman
{
namespace
{

constexpr std::array<const char *, 8> kWakeWords{{
    "你好星期五",
    "你好 星期五",
    "星期五",
    "星期 5",
    "星期5",
    "星期开",
    "星期无",
    "星期伍",
}};
constexpr std::array<const char *, 4> kWakeReplies{{
    "你好BOSS，我是星期五。",
    "好的老板。",
    "正在处理。",
    "请稍等。",
}};

std::atomic<size_t> g_wake_reply_index{0};

std::string normalize_text_for_match(const std::string &text)
{
    std::string out = text;
    const std::array<std::string, 22> drops{{
        " ", "\t", "\r", "\n", "，", "。", "！", "？", "、", ",", ".", "!", "?",
        "：", ":", "；", ";", "“", "”", "\"", "'", "　",
    }};
    for (const std::string &drop : drops)
    {
        size_t pos = 0;
        while ((pos = out.find(drop, pos)) != std::string::npos)
        {
            out.erase(pos, drop.size());
        }
    }
    return out;
}

bool contains_any(const std::string &text, std::initializer_list<const char *> keywords)
{
    for (const char *keyword : keywords)
    {
        if (text.find(keyword) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

bool is_vision_question(const std::string &text)
{
    if (contains_any(text, {"没看到", "看不到", "看不见", "没看清", "看不清"}))
    {
        return false;
    }
    return contains_any(text, {
                                  "你看到了什么",
                                  "看到什么",
                                  "看见什么",
                                  "看一下",
                                  "看看",
                                  "前面有什么",
                                  "这是什么",
                                  "识别一下",
                                  "拍照看看",
                              });
}

bool is_camera_open_request(const std::string &text)
{
    return contains_any(text, {"打开摄像头", "显示摄像头", "摄像头画面", "镜子模式", "看摄像头"});
}

bool is_camera_close_request(const std::string &text)
{
    return contains_any(text, {"关闭摄像头", "退出摄像头", "显示表情", "表情模式", "切回表情"});
}

bool is_system_stats_request(const std::string &text)
{
    return contains_any(text, {
                                  "系统状态",
                                  "状态面板",
                                  "显示状态",
                                  "性能面板",
                                  "性能状态",
                                  "cpu占用",
                                  "CPU占用",
                                  "cpu温度",
                                  "CPU温度",
                                  "温度面板",
                                  "查看温度",
                                  "查看cpu",
                                  "查看CPU",
                              });
}

bool is_music_request(const std::string &text)
{
    return contains_any(text, {
                                  "我想听",
                                  "想听",
                                  "播放",
                                  "放一首",
                                  "放首",
                                  "来一首",
                                  "听一首",
                                  "听首",
                                  "换首",
                                  "换一首",
                                  "唱歌",
                                  "唱首歌",
                                  "来点音乐",
                              });
}

bool is_stop_request(const std::string &text)
{
    return contains_any(text, {
                                  "停止",
                                  "停一下",
                                  "停止播放",
                                  "暂停播放",
                                  "停歌",
                                  "别唱了",
                                  "关掉音乐",
                                  "关闭音乐",
                                  "停止情感识别",
                                  "停止表情识别",
                              });
}

bool is_emotion_request(const std::string &text)
{
    return contains_any(text, {
                                  "开始情感识别",
                                  "情感识别",
                                  "情感检测",
                                  "开始表情识别",
                                  "表情识别",
                                  "表情检测",
                                  "情绪识别",
                                  "情绪检测",
                                  "识别情绪",
                                  "识别情感",
                                  "识别表情",
                                  "分析表情",
                                  "看看我的表情",
                              });
}

bool is_exit_dialogue_request(const std::string &text)
{
    return contains_any(text, {"退出对话", "结束对话", "不用了", "休息吧", "再见", "拜拜"});
}

} // namespace

bool has_wake_word(const std::string &text)
{
    const std::string normalized = normalize_text_for_match(text);
    for (const char *word : kWakeWords)
    {
        if (normalized.find(normalize_text_for_match(word)) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

std::string strip_wake_word(const std::string &text)
{
    std::string out = text;
    for (const char *word : kWakeWords)
    {
        size_t pos = std::string::npos;
        while ((pos = out.find(word)) != std::string::npos)
        {
            out.erase(pos, std::strlen(word));
        }
    }
    return trim(out);
}

std::string next_wake_reply()
{
    const size_t index = g_wake_reply_index.fetch_add(1);
    return kWakeReplies[index % kWakeReplies.size()];
}

UserIntent classify_user_intent(const std::string &text)
{
    if (is_stop_request(text))
    {
        return UserIntent::Stop;
    }
    if (is_exit_dialogue_request(text))
    {
        return UserIntent::ExitDialogue;
    }
    if (is_camera_open_request(text))
    {
        return UserIntent::OpenCamera;
    }
    if (is_camera_close_request(text))
    {
        return UserIntent::CloseCamera;
    }
    if (is_system_stats_request(text))
    {
        return UserIntent::SystemStats;
    }
    if (is_music_request(text))
    {
        return UserIntent::Music;
    }
    if (is_emotion_request(text))
    {
        return UserIntent::Emotion;
    }
    if (is_vision_question(text))
    {
        return UserIntent::Vision;
    }
    return UserIntent::Chat;
}

bool is_keyboard_program_exit_request(const std::string &text)
{
    std::string value = to_lower_ascii(trim(text));
    if (value.size() > 1 &&
        value.find_first_not_of(value[0]) == std::string::npos &&
        value[0] == 'q')
    {
        value = "q";
    }
    return value == "q" ||
           value == "quit" ||
           value == "exit" ||
           contains_any(text, {"退出程序", "关闭程序", "结束程序"});
}

bool is_keyboard_wake_request(const std::string &text)
{
    std::string value = to_lower_ascii(trim(text));
    if (value.size() > 1 &&
        value.find_first_not_of(value[0]) == std::string::npos &&
        value[0] == 'w')
    {
        value = "w";
    }
    return value == "w" ||
           value == "wake" ||
           value == "friday" ||
           has_wake_word(text) ||
           contains_any(text, {"唤醒", "叫醒星期五", "你好星期五"});
}

std::string keyboard_command_to_text(const std::string &line)
{
    const std::string text = trim(line);
    std::string lower = to_lower_ascii(text);
    if (lower.size() > 1 &&
        lower.find_first_not_of(lower[0]) == std::string::npos &&
        std::string("scfepvqw").find(lower[0]) != std::string::npos)
    {
        lower = lower.substr(0, 1);
    }
    auto after_prefix = [&](const char *prefix) -> std::string
    {
        const std::string p = prefix;
        if (lower.rfind(p, 0) != 0)
        {
            return "";
        }
        return trim(text.substr(p.size()));
    };

    if (lower == "s" || lower == "stop")
    {
        return "停止";
    }
    if (lower == "c" || lower == "cam" || lower == "camera")
    {
        return "打开摄像头";
    }
    if (lower == "f" || lower == "face" || lower == "emoji")
    {
        return "显示表情";
    }
    if (lower == "p" || lower == "perf" || lower == "status" || lower == "stats")
    {
        return "系统状态";
    }
    if (lower == "e" || lower == "emotion")
    {
        return "开始情感识别";
    }
    if (lower == "v" || lower == "vision")
    {
        return "你看到了什么";
    }

    std::string song = after_prefix("m ");
    if (song.empty())
    {
        song = after_prefix("music ");
    }
    if (song.empty())
    {
        song = after_prefix("play ");
    }
    if (!song.empty())
    {
        return "播放" + song;
    }

    return text;
}

} // namespace xiaoman
