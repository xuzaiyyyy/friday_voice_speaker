#pragma once

#include <atomic>
#include <signal.h>

namespace xiaoman
{

enum class UiState
{
    Idle = 0,
    Wake = 1,
    Listen = 2,
    Think = 3,
    Speak = 4,
    Camera = 5,
    Stats = 6,
    Error = 7,
};

extern volatile sig_atomic_t g_running;
extern std::atomic<int> g_ui_state;
extern std::atomic<bool> g_lcd_camera_mode;
extern std::atomic<bool> g_lcd_stats_mode;
extern std::atomic<bool> g_emotion_mode;
extern std::atomic<bool> g_voice_enabled;
extern std::atomic<bool> g_tts_enabled;

void handle_signal(int);
void show_camera_on_lcd();
void show_stats_on_lcd();
void show_expression_on_lcd();
void disable_emotion_mode();

} // namespace xiaoman
