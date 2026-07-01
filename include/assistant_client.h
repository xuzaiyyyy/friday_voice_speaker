#pragma once

#include <string>

namespace xiaoman
{

struct OnlineMusicInfo
{
    bool ok = false;
    std::string title;
    std::string artist;
    std::string url;
    std::string error;
};

void speak_text(const std::string &text);
std::string ask_llm(const std::string &question);
std::string ask_vision(const std::string &question, const std::string &image_b64);
OnlineMusicInfo resolve_online_music(const std::string &request_text);

} // namespace xiaoman
