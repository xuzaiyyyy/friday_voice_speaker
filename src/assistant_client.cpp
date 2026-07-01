#include "assistant_client.h"

#include "app_state.h"
#include "app_utils.h"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace xiaoman
{
namespace
{

constexpr const char *kAssistantName = "星期五";

void write_all_fd(int fd, const std::string &payload)
{
    const char *data = payload.data();
    size_t left = payload.size();
    while (left > 0)
    {
        const ssize_t n = ::write(fd, data, left);
        if (n > 0)
        {
            data += n;
            left -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        throw std::runtime_error("write helper stdin failed: " + std::string(std::strerror(errno)));
    }
}

std::string run_helper(const std::string &payload,
                       const std::vector<std::string> &args,
                       bool echo_output,
                       const std::string &echo_prefix)
{
    const std::string helper = find_helper_path();
    int stdin_pipe[2]{-1, -1};
    int stdout_pipe[2]{-1, -1};
    if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0)
    {
        throw std::runtime_error("pipe failed: " + std::string(std::strerror(errno)));
    }

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        throw std::runtime_error("fork failed: " + std::string(std::strerror(errno)));
    }
    if (pid == 0)
    {
        ::dup2(stdin_pipe[0], STDIN_FILENO);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);

        std::vector<char *> argv;
        argv.push_back(const_cast<char *>("python3"));
        argv.push_back(const_cast<char *>("-u"));
        argv.push_back(const_cast<char *>(helper.c_str()));
        for (const std::string &arg : args)
        {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp("python3", argv.data());
        std::fprintf(stderr, "exec python3 failed: %s\n", std::strerror(errno));
        _exit(127);
    }

    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);
    write_all_fd(stdin_pipe[1], payload);
    ::close(stdin_pipe[1]);

    std::array<char, 256> buffer{};
    std::string output;
    bool echo_started = false;
    while (true)
    {
        const ssize_t n = ::read(stdout_pipe[0], buffer.data(), buffer.size());
        if (n == 0)
        {
            break;
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        const std::string chunk(buffer.data(), buffer.data() + n);
        output += chunk;
        if (echo_output)
        {
            if (!echo_started)
            {
                std::cout << echo_prefix;
                echo_started = true;
            }
            std::cout << chunk << std::flush;
        }
    }
    ::close(stdout_pipe[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (status != 0)
    {
        std::string message = "python helper failed: " + helper;
        if (!output.empty())
        {
            message += ": " + trim(output);
        }
        throw std::runtime_error(message);
    }
    if (echo_output && echo_started && !output.empty() && output.back() != '\n')
    {
        std::cout << "\n";
    }
    return trim(output);
}

std::string value_from_key_lines(const std::string &text, const std::string &key)
{
    std::istringstream in(text);
    std::string line;
    const std::string prefix = key + "=";
    while (std::getline(in, line))
    {
        if (line.rfind(prefix, 0) == 0)
        {
            return trim(line.substr(prefix.size()));
        }
    }
    return "";
}

} // namespace

void speak_text(const std::string &text)
{
    const std::string spoken = trim(text);
    if (spoken.empty())
    {
        return;
    }
    if (!g_tts_enabled.load())
    {
        std::cout << kAssistantName << ": " << spoken << "\n";
        return;
    }
    try
    {
        g_ui_state = static_cast<int>(UiState::Speak);
        run_helper(spoken, {"--tts-test"}, false, "");
    }
    catch (const std::exception &e)
    {
        std::cerr << "TTS warning: " << e.what() << "\n";
    }
}

std::string ask_llm(const std::string &question)
{
    g_ui_state = static_cast<int>(UiState::Think);
    return run_helper(question, {}, true, "AI: ");
}

std::string ask_vision(const std::string &question, const std::string &image_b64)
{
    const std::string payload =
        "{\"text\":\"" + json_escape(question) + "\",\"image_b64\":\"" + image_b64 + "\"}";
    g_ui_state = static_cast<int>(UiState::Think);
    return run_helper(payload, {}, true, "AI: ");
}

OnlineMusicInfo resolve_online_music(const std::string &request_text)
{
    const std::string output = run_helper(request_text, {"--music-url"}, false, "");
    OnlineMusicInfo info;
    info.title = value_from_key_lines(output, "MUSIC_TITLE");
    info.artist = value_from_key_lines(output, "MUSIC_ARTIST");
    info.url = value_from_key_lines(output, "MUSIC_URL");
    info.error = value_from_key_lines(output, "MUSIC_ERROR");
    info.ok = !info.url.empty();
    return info;
}

} // namespace xiaoman
