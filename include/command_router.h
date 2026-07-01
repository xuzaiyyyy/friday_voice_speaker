#pragma once

#include <string>

namespace xiaoman
{

enum class UserIntent
{
    Stop,
    ExitDialogue,
    OpenCamera,
    CloseCamera,
    Music,
    Emotion,
    Vision,
    SystemStats,
    Chat,
};

bool has_wake_word(const std::string &text);
std::string strip_wake_word(const std::string &text);
std::string next_wake_reply();

UserIntent classify_user_intent(const std::string &text);
bool is_keyboard_program_exit_request(const std::string &text);
bool is_keyboard_wake_request(const std::string &text);
std::string keyboard_command_to_text(const std::string &line);

} // namespace xiaoman
