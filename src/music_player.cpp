#include "music_player.h"

#include "app_state.h"
#include "app_utils.h"
#include "assistant_client.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

namespace xiaoman
{
namespace
{

std::mutex g_music_mutex;
pid_t g_music_pid = -1;

void reap_music_locked()
{
    if (g_music_pid <= 0)
    {
        return;
    }
    int status = 0;
    const pid_t done = ::waitpid(g_music_pid, &status, WNOHANG);
    if (done == g_music_pid)
    {
        g_music_pid = -1;
    }
}

std::string default_music_player()
{
    std::string player = env_first({"XIAOMAN_MUSIC_PLAYER", "NEWBOT_MUSIC_PLAYER"});
    if (player.empty())
    {
        player = "ffplay";
    }
    return player;
}

bool start_music_player(const std::string &url)
{
    stop_music();

    const std::string player = default_music_player();
    const std::string alsa_device =
        env_first({"XIAOMAN_MUSIC_ALSA_DEVICE", "NEWBOT_MUSIC_ALSA_DEVICE", "AUDIODEV"});

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        std::cerr << "music: fork failed: " << std::strerror(errno) << "\n";
        return false;
    }
    if (pid == 0)
    {
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }

        std::vector<std::string> args;
        args.push_back(player);
        if (player.find("ffplay") != std::string::npos)
        {
            if (!alsa_device.empty())
            {
                ::setenv("SDL_AUDIODRIVER", "alsa", 1);
                ::setenv("AUDIODEV", alsa_device.c_str(), 1);
            }
            args.push_back("-nodisp");
            args.push_back("-autoexit");
            args.push_back("-loglevel");
            args.push_back("quiet");
            args.push_back("-i");
            args.push_back(url);
        }
        else if (player.find("mpg123") != std::string::npos)
        {
            args.push_back("-q");
            if (!alsa_device.empty())
            {
                args.push_back("-o");
                args.push_back("alsa");
                args.push_back("-a");
                args.push_back(alsa_device);
            }
            args.push_back(url);
        }
        else
        {
            args.push_back(url);
        }

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (std::string &arg : args)
        {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(player.c_str(), argv.data());
        _exit(127);
    }

    {
        std::lock_guard<std::mutex> lock(g_music_mutex);
        g_music_pid = pid;
    }
    return true;
}

} // namespace

void stop_music()
{
    std::lock_guard<std::mutex> lock(g_music_mutex);
    reap_music_locked();
    if (g_music_pid > 0)
    {
        ::kill(g_music_pid, SIGTERM);
        ::waitpid(g_music_pid, nullptr, 0);
        g_music_pid = -1;
    }
}

void play_online_music(const std::string &request_text)
{
    g_ui_state = static_cast<int>(UiState::Think);
    std::cout << "music: resolving online song...\n";
    const OnlineMusicInfo info = resolve_online_music(request_text);
    if (!info.ok)
    {
        speak_text(info.error.empty() ? "我没有找到能播放的歌曲。" : "这首歌暂时播放不了。");
        return;
    }

    std::string title = info.title;
    if (!info.artist.empty())
    {
        title += " - " + info.artist;
    }
    speak_text(title.empty() ? "开始播放。" : "为你播放，" + title + "。");
    if (!start_music_player(info.url))
    {
        speak_text("播放器启动失败，请检查 ffplay 或 mpg123。");
    }
}

} // namespace xiaoman
