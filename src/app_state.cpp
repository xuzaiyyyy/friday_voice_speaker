#include "app_state.h"

namespace xiaoman
{

volatile sig_atomic_t g_running = 1;
std::atomic<int> g_ui_state{static_cast<int>(UiState::Idle)};
std::atomic<bool> g_lcd_camera_mode{false};
std::atomic<bool> g_lcd_stats_mode{false};
std::atomic<bool> g_emotion_mode{false};
std::atomic<bool> g_voice_enabled{true};
std::atomic<bool> g_tts_enabled{true};

void handle_signal(int)
{
    g_running = 0;
}

void show_camera_on_lcd()
{
    g_lcd_camera_mode = true;
    g_lcd_stats_mode = false;
}

void show_stats_on_lcd()
{
    g_emotion_mode = false;
    g_lcd_camera_mode = false;
    g_lcd_stats_mode = true;
}

void show_expression_on_lcd()
{
    g_emotion_mode = false;
    g_lcd_camera_mode = false;
    g_lcd_stats_mode = false;
}

void disable_emotion_mode()
{
    g_emotion_mode = false;
}

} // namespace xiaoman
