#include "zlm_publisher.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>

namespace
{
constexpr const char kWebRtcApiPath[] = "/index/api/webrtc";
constexpr const char kWebRtcPlayerPagePath[] = "/webrtc_play_test.html";

struct SharedZlmServerState
{
    std::mutex mutex;
    size_t     ref_count     = 0;
    uint16_t   rtsp_port     = 0;
    uint16_t   http_port     = 0;
    uint16_t   rtc_port      = 0;
    bool       env_ready     = false;
    bool       webrtc_ready  = false;
    bool       events_ready  = false;
    mk_events  events        = {};
    std::string http_root_path;
    ZlmPublishConfig bootstrap_cfg = {};
};

SharedZlmServerState g_shared_server;

uint64_t SteadyNowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count());
}

bool FileExists(const char* path)
{
    if (!path || path[0] == '\0')
    {
        return false;
    }
    std::ifstream file(path);
    return file.good();
}

void LogLoadedZlmLibraryPath()
{
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(mk_env_init), &info) && info.dli_fname)
    {
        std::cout << "[ZLM] libmk_api loaded from: " << info.dli_fname << std::endl;
    }
}

const char* FindConfigIniPath()
{
    const char* env_path = std::getenv("RKMEDIA_ZLM_CONFIG");
    if (env_path && env_path[0] != '\0' && FileExists(env_path))
    {
        return env_path;
    }

    static const char* kCandidates[] = {"./config.ini", "../config.ini"};
    for (const char* path : kCandidates)
    {
        if (FileExists(path))
        {
            return path;
        }
    }
    return nullptr;
}

bool IniOptionExists(mk_ini ini, const char* key)
{
    if (!ini || !key)
    {
        return false;
    }
    const char* value = mk_ini_get_option(ini, key);
    return value && value[0] != '\0';
}

void ResetSharedServerStateFields()
{
    g_shared_server.ref_count        = 0;
    g_shared_server.rtsp_port        = 0;
    g_shared_server.http_port        = 0;
    g_shared_server.rtc_port         = 0;
    g_shared_server.env_ready        = false;
    g_shared_server.webrtc_ready     = false;
    g_shared_server.events_ready     = false;
    g_shared_server.events           = {};
    g_shared_server.http_root_path.clear();
    g_shared_server.bootstrap_cfg    = {};
}

const char* kJsonResponseHeader[] = {"Content-Type", "application/json",
                                     "Access-Control-Allow-Origin", "*", nullptr};

std::string JsonEscape(const char* input)
{
    if (!input)
    {
        return "";
    }

    std::string escaped;
    escaped.reserve(std::strlen(input) + 32);
    for (const unsigned char ch : std::string(input))
    {
        switch (ch)
        {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (ch < 0x20)
                {
                    escaped += '?';
                }
                else
                {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

std::string TrimAscii(std::string value)
{
    if (value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF)
    {
        value.erase(0, 3);
    }

    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
    {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string LoadIniOptionFromFile(const char* ini_path, const char* section, const char* key)
{
    if (!ini_path || !section || !key)
    {
        return "";
    }

    std::ifstream file(ini_path);
    if (!file.is_open())
    {
        return "";
    }

    std::string line;
    std::string current_section;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        std::string trimmed = TrimAscii(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
        {
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']')
        {
            current_section = TrimAscii(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }

        if (current_section != section)
        {
            continue;
        }

        const size_t equal_pos = trimmed.find('=');
        if (equal_pos == std::string::npos)
        {
            continue;
        }

        const std::string parsed_key   = TrimAscii(trimmed.substr(0, equal_pos));
        const std::string parsed_value = TrimAscii(trimmed.substr(equal_pos + 1));
        if (parsed_key == key)
        {
            return parsed_value;
        }
    }

    return "";
}

std::string DeriveHttpRootPath(const char* ini_path, mk_ini global_ini)
{
    const std::string http_root_from_file = LoadIniOptionFromFile(ini_path, "http", "rootPath");
    if (!http_root_from_file.empty())
    {
        return http_root_from_file;
    }

    const char* http_root = global_ini ? mk_ini_get_option(global_ini, "http.rootPath") : nullptr;
    if (http_root && http_root[0] != '\0')
    {
        return http_root;
    }

    if (ini_path && ini_path[0] != '\0')
    {
        try
        {
            const std::filesystem::path ini_fs_path(ini_path);
            return (ini_fs_path.parent_path() / "www").lexically_normal().string();
        }
        catch (...)
        {
        }
    }

    return "./www";
}
void SendJsonResponse(const mk_http_response_invoker invoker, const std::string& body)
{
    if (!invoker)
    {
        return;
    }
    mk_http_response_invoker_do_string(invoker, 200, kJsonResponseHeader, body.c_str());
}

void SendWebRtcError(const mk_http_response_invoker invoker, const char* err_msg)
{
    std::string body = "{\"code\":-1,\"msg\":\"";
    body += JsonEscape(err_msg ? err_msg : "invalid request");
    body += "\"}";
    SendJsonResponse(invoker, body);
}

void API_CALL OnWebRtcAnswerSdp(void* user_data, const char* answer, const char* err)
{
    mk_http_response_invoker invoker = static_cast<mk_http_response_invoker>(user_data);
    if (!invoker)
    {
        return;
    }

    if (answer)
    {
        std::string body = "{\"code\":0,\"type\":\"answer\",\"sdp\":\"";
        body += JsonEscape(answer);
        body += "\"}";
        SendJsonResponse(invoker, body);
    }
    else
    {
        SendWebRtcError(invoker, err ? err : "failed to generate webrtc answer");
    }

    mk_http_response_invoker_clone_release(invoker);
}

void API_CALL OnMkHttpRequest(const mk_parser parser, const mk_http_response_invoker invoker,
                              int* consumed, const mk_sock_info /*sender*/)
{
    if (!consumed)
    {
        return;
    }
    *consumed = 0;
    if (!parser || !invoker)
    {
        return;
    }

    const char* url = mk_parser_get_url(parser);
    if (!url || std::strcmp(url, kWebRtcApiPath) != 0)
    {
        return;
    }
    *consumed = 1;

    {
        std::lock_guard<std::mutex> lock(g_shared_server.mutex);
        if (!g_shared_server.webrtc_ready)
        {
            SendWebRtcError(
                invoker,
                "webrtc is unavailable: rtc server not started or ENABLE_WEBRTC disabled");
            return;
        }
    }

    mk_http_response_invoker invoker_clone = mk_http_response_invoker_clone(invoker);
    if (!invoker_clone)
    {
        SendWebRtcError(invoker, "failed to clone http invoker");
        return;
    }

    const char* type = mk_parser_get_url_param(parser, "type");
    if (!type || std::strcmp(type, "play") != 0)
    {
        SendWebRtcError(invoker_clone, "only type=play is supported");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    const char* app = mk_parser_get_url_param(parser, "app");
    if (!app || app[0] == '\0')
    {
        SendWebRtcError(invoker_clone, "missing app query parameter");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    const char* stream = mk_parser_get_url_param(parser, "stream");
    if (!stream || stream[0] == '\0')
    {
        SendWebRtcError(invoker_clone, "missing stream query parameter");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    const char* offer = mk_parser_get_content(parser, nullptr);
    if (!offer || offer[0] == '\0')
    {
        SendWebRtcError(invoker_clone, "missing webrtc offer in http body");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    std::string vhost;
    {
        std::lock_guard<std::mutex> lock(g_shared_server.mutex);
        vhost = g_shared_server.bootstrap_cfg.vhost;
    }
    if (vhost.empty())
    {
        vhost = "__defaultVhost__";
    }
    std::string rtc_url = "rtc://";
    rtc_url += vhost;
    rtc_url += "/";
    rtc_url += app;
    rtc_url += "/";
    rtc_url += stream;
    std::cout << "[ZLM] WebRTC play request vhost=" << vhost
              << ", rtc_url=" << rtc_url << std::endl;

    const char* params = mk_parser_get_url_params(parser);
    if (params && params[0] != '\0')
    {
        rtc_url += "?";
        rtc_url += params;
    }

    mk_webrtc_get_answer_sdp(invoker_clone, OnWebRtcAnswerSdp, type, offer, rtc_url.c_str());
}

void API_CALL OnMkHttpAccess(const mk_parser /*parser*/, const char* /*path*/, int /*is_dir*/,
                             const mk_http_access_path_invoker invoker,
                             const mk_sock_info /*sender*/)
{
    if (!invoker)
    {
        return;
    }

    std::string http_root;
    {
        std::lock_guard<std::mutex> lock(g_shared_server.mutex);
        http_root = g_shared_server.http_root_path;
    }

    if (http_root.empty())
    {
        mk_http_access_path_invoker_do(invoker, nullptr, nullptr, 0);
        return;
    }

    mk_http_access_path_invoker_do(invoker, nullptr, http_root.c_str(), 0);
}
}  // namespace

ZlmPublisher::ZlmPublisher() {}

ZlmPublisher::~ZlmPublisher() { Close(); }

int ZlmPublisher::AcquireSharedServer(const ZlmPublishConfig& cfg)
{
    std::lock_guard<std::mutex> lock(g_shared_server.mutex);

    if (g_shared_server.ref_count == 0)
    {
        const char* ini_path = FindConfigIniPath();
        LogLoadedZlmLibraryPath();
        mk_config   config   = {};
        config.ini           = ini_path;
        config.ini_is_path   = ini_path ? 1 : 0;
        config.log_level     = 0;
        config.log_mask      = LOG_CONSOLE;
        config.log_file_path = nullptr;
        config.log_file_days = 0;
        config.ssl           = nullptr;
        config.ssl_is_path   = 1;
        config.ssl_pwd       = nullptr;
        config.thread_num    = 0;
        mk_env_init(&config);

        mk_ini global_ini = mk_ini_default();
        if (ini_path)
        {
            std::cout << "[ZLM] 已加载配置文件: " << ini_path << std::endl;
            if (global_ini)
            {
                const char* rtc_extern_ip = mk_ini_get_option(global_ini, "rtc.externIP");
                const char* rtc_port      = mk_ini_get_option(global_ini, "rtc.port");
                const char* rtc_tcp_port  = mk_ini_get_option(global_ini, "rtc.tcpPort");
                std::cout << "[ZLM] rtc.externIP="
                          << (rtc_extern_ip && rtc_extern_ip[0] ? rtc_extern_ip : "<empty>")
                          << ", rtc.port=" << (rtc_port && rtc_port[0] ? rtc_port : "<empty>")
                          << ", rtc.tcpPort="
                          << (rtc_tcp_port && rtc_tcp_port[0] ? rtc_tcp_port : "<empty>")
                          << std::endl;
            }
        }
        else
        {
            std::cout << "[ZLM] 未发现 config.ini，将使用代码内默认端口配置" << std::endl;
        }

        g_shared_server.http_root_path = DeriveHttpRootPath(ini_path, global_ini);
        if (global_ini && !g_shared_server.http_root_path.empty())
        {
            mk_ini_set_option(global_ini, "http.rootPath", g_shared_server.http_root_path.c_str());
        }
        std::cout << "[ZLM] http.rootPath=" << g_shared_server.http_root_path << std::endl;

        g_shared_server.env_ready = true;
        g_shared_server.http_port = mk_http_server_start(cfg.http_port, 0);
        if (g_shared_server.http_port == 0)
        {
            mk_stop_all_server();
            ResetSharedServerStateFields();
            return -1;
        }

        g_shared_server.rtsp_port = mk_rtsp_server_start(cfg.rtsp_port, 0);
        if (g_shared_server.rtsp_port == 0)
        {
            mk_stop_all_server();
            ResetSharedServerStateFields();
            return -1;
        }

        uint16_t rtc_target_port = cfg.rtc_port;
        if (rtc_target_port == g_shared_server.http_port)
        {
            rtc_target_port = (rtc_target_port < 65535u)
                                  ? static_cast<uint16_t>(rtc_target_port + 1u)
                                  : static_cast<uint16_t>(8001u);
        }

        if (global_ini)
        {
            if (!IniOptionExists(global_ini, "rtc.port"))
            {
                mk_ini_set_option_int(global_ini, "rtc.port", rtc_target_port);
            }
            if (!IniOptionExists(global_ini, "rtc.tcpPort"))
            {
                mk_ini_set_option_int(global_ini, "rtc.tcpPort", rtc_target_port);
            }
        }

        g_shared_server.rtc_port    = mk_rtc_server_start(rtc_target_port);
        g_shared_server.webrtc_ready = (g_shared_server.rtc_port != 0);
        if (global_ini && g_shared_server.webrtc_ready)
        {
            if (!IniOptionExists(global_ini, "rtc.port"))
            {
                mk_ini_set_option_int(global_ini, "rtc.port", g_shared_server.rtc_port);
            }
            if (!IniOptionExists(global_ini, "rtc.tcpPort"))
            {
                mk_ini_set_option_int(global_ini, "rtc.tcpPort", g_shared_server.rtc_port);
            }
        }

        g_shared_server.events                    = {};
        g_shared_server.events.on_mk_http_request = OnMkHttpRequest;
        g_shared_server.events.on_mk_http_access  = OnMkHttpAccess;
        mk_events_listen(&g_shared_server.events);
        g_shared_server.events_ready  = true;
        g_shared_server.bootstrap_cfg = cfg;
    }
    else if (cfg.http_port != g_shared_server.bootstrap_cfg.http_port ||
             cfg.rtsp_port != g_shared_server.bootstrap_cfg.rtsp_port ||
             cfg.rtc_port != g_shared_server.bootstrap_cfg.rtc_port)
    {
        std::cerr << "[ZLM] shared server already started with ports http="
                  << g_shared_server.http_port << ", rtsp=" << g_shared_server.rtsp_port
                  << ", rtc=" << g_shared_server.rtc_port
                  << ". New publisher will reuse existing ports." << std::endl;
    }

    ++g_shared_server.ref_count;
    RtspPort_ = g_shared_server.rtsp_port;
    HttpPort_ = g_shared_server.http_port;
    RtcPort_  = g_shared_server.rtc_port;
    SharedServerHeld_ = true;
    return 0;
}

void ZlmPublisher::ReleaseSharedServer()
{
    std::lock_guard<std::mutex> lock(g_shared_server.mutex);
    if (!SharedServerHeld_ || g_shared_server.ref_count == 0)
    {
        SharedServerHeld_ = false;
        return;
    }

    --g_shared_server.ref_count;
    SharedServerHeld_ = false;
    if (g_shared_server.ref_count > 0)
    {
        return;
    }

    mk_events_listen(nullptr);
    mk_stop_all_server();
    ResetSharedServerStateFields();
}

int ZlmPublisher::Init(const ZlmPublishConfig& cfg)
{
    if (Initialized_)
    {
        return 0;
    }

    PublishConfig_ = cfg;
    if (AcquireSharedServer(cfg) != 0)
    {
        return -1;
    }

    mk_ini media_option = mk_ini_create();
    if (!media_option)
    {
        ReleaseSharedServer();
        return -1;
    }

    mk_ini_set_option_int(media_option, "enable_rtsp", 1);
    mk_ini_set_option_int(media_option, "enable_rtmp", 0);
    mk_ini_set_option_int(media_option, "enable_hls", 0);
    mk_ini_set_option_int(media_option, "enable_hls_fmp4", 0);
    mk_ini_set_option_int(media_option, "enable_ts", 0);
    mk_ini_set_option_int(media_option, "enable_fmp4", 0);
    mk_ini_set_option_int(media_option, "enable_mp4", 0);
    mk_ini_set_option_int(media_option, "enable_audio", PublishConfig_.enable_audio ? 1 : 0);

    Media_ = mk_media_create2(PublishConfig_.vhost.c_str(), PublishConfig_.app.c_str(),
                              PublishConfig_.stream.c_str(), 0.0f, media_option);
    mk_ini_release(media_option);

    if (!Media_)
    {
        ReleaseSharedServer();
        return -1;
    }

    codec_args track_args = {};
    mk_track   track      = mk_track_create(MKCodecH264, &track_args);
    if (!track)
    {
        Close();
        return -1;
    }

    mk_media_init_track(Media_, track);
    if (PublishConfig_.enable_audio)
    {
        const int audio_init_ret = mk_media_init_audio(
            Media_, MKCodecG711A, static_cast<int>(PublishConfig_.audio_sample_rate),
            static_cast<int>(PublishConfig_.audio_channels),
            static_cast<int>(PublishConfig_.audio_sample_bit));
        if (!audio_init_ret)
        {
            mk_track_unref(track);
            Close();
            return -1;
        }
    }
    mk_media_init_complete(Media_);
    mk_track_unref(track);

    Initialized_ = true;
    return 0;
}

void ZlmPublisher::SetExpectedFps(uint32_t fps)
{
    if (fps == 0)
    {
        fps = 30;
    }
    FrameIntervalUs_ = 1000000 / static_cast<uint64_t>(fps);
    if (FrameIntervalUs_ == 0)
    {
        FrameIntervalUs_ = 1;
    }
}

int ZlmPublisher::InputPacketChunk(const EncPacketView& pkt)
{
    if (!Initialized_ || !Media_ || !pkt.data || pkt.len == 0)
    {
        return -1;
    }
    if (pkt.len > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        return -1;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(pkt.data);
    if (!IsAnnexBStartCode(bytes, pkt.len))
    {
        return -1;
    }

        uint64_t dts_ms = 0;
    uint64_t pts_ms = 0;
    if (!NormalizeTimestamp(pkt, &dts_ms, &pts_ms))
    {
        return -1;
    }

    std::lock_guard<std::mutex> media_lock(MediaInputMutex_);
    mk_frame frame = mk_frame_create(MKCodecH264, dts_ms, pts_ms,
                                     reinterpret_cast<const char*>(bytes), pkt.len, nullptr,
                                     nullptr);
    if (!frame)
    {
        return -1;
    }

    const int ret = mk_media_input_frame(Media_, frame);
    mk_frame_unref(frame);
    if (!ret)
    {
        return -1;
    }

    ++OutputFrameCount_;
    return 0;
}

int ZlmPublisher::InputAudioFrame(const void* data, size_t len, uint64_t dts_ms)
{
    if (!Initialized_ || !Media_ || !data || len == 0)
    {
        return -1;
    }
    if (len > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        return -1;
    }

    std::lock_guard<std::mutex> media_lock(MediaInputMutex_);
    const int ret = mk_media_input_audio(Media_, data, static_cast<int>(len), dts_ms);
    return ret ? 0 : -1;
}
std::string ZlmPublisher::GetRtspUrl() const
{
    if (RtspPort_ == 0)
    {
        return "";
    }

    std::string url = "rtsp://<device-ip>:";
    url += std::to_string(RtspPort_);
    url += "/";
    url += PublishConfig_.app;
    url += "/";
    url += PublishConfig_.stream;
    return url;
}

std::string ZlmPublisher::GetWebRtcApiUrl() const
{
    if (HttpPort_ == 0)
    {
        return "";
    }

    std::string url = "http://<device-ip>:";
    url += std::to_string(HttpPort_);
    url += kWebRtcApiPath;
    url += "?app=";
    url += PublishConfig_.app;
    url += "&stream=";
    url += PublishConfig_.stream;
    url += "&type=play";
    return url;
}

std::string ZlmPublisher::GetWebRtcPlayerPageUrl() const
{
    if (HttpPort_ == 0)
    {
        return "";
    }

    std::string url = "http://<device-ip>:";
    url += std::to_string(HttpPort_);
    url += kWebRtcPlayerPagePath;
    url += "?app=";
    url += PublishConfig_.app;
    url += "&stream=";
    url += PublishConfig_.stream;
    return url;
}

uint64_t ZlmPublisher::GetEstimatedVideoDtsMs() const
{
    const uint64_t base_dts_ms = VideoTimelineDtsMs_.load(std::memory_order_relaxed);
    const uint64_t base_tick_ms = VideoTimelineTickMs_.load(std::memory_order_relaxed);
    if (!VideoTimelineReady_.load(std::memory_order_relaxed) || base_tick_ms == 0)
    {
        return base_dts_ms;
    }

    const uint64_t now_ms = SteadyNowMs();
    const uint64_t elapsed_ms = now_ms > base_tick_ms ? (now_ms - base_tick_ms) : 0;
    const uint64_t frame_interval_ms = std::max<uint64_t>((FrameIntervalUs_ + 999) / 1000, 1);
    const uint64_t advance_ms = std::min<uint64_t>(elapsed_ms, frame_interval_ms);
    return base_dts_ms + advance_ms;
}

void ZlmPublisher::Close()
{
    if (Media_)
    {
        mk_media_release(Media_);
        Media_ = nullptr;
    }

    ReleaseSharedServer();

    LastDtsUs_              = 0;
    LastPtsUs_              = 0;
    LastDtsMs_              = 0;
    LastPtsMs_              = 0;
    VideoTimelineDtsMs_.store(0, std::memory_order_relaxed);
    VideoTimelineTickMs_.store(0, std::memory_order_relaxed);
    OutputFrameCount_       = 0;
    TimestampFallbackCount_ = 0;
    VideoTimelineReady_.store(false, std::memory_order_relaxed);
    RtspPort_               = 0;
    HttpPort_               = 0;
    RtcPort_                = 0;
    HasLastTimestamp_       = false;
    Initialized_            = false;
}

bool ZlmPublisher::NormalizeTimestamp(const EncPacketView& pkt, uint64_t* out_dts_ms,
                                      uint64_t* out_pts_ms)
{
    if (!out_dts_ms || !out_pts_ms)
    {
        return false;
    }

    bool    used_fallback = false;
    int64_t dts_us        = pkt.dts_us;
    int64_t pts_us        = pkt.pts_us;

    if (dts_us < 0 && pts_us >= 0)
    {
        dts_us        = pts_us;
        used_fallback = true;
    }
    if (pts_us < 0 && dts_us >= 0)
    {
        pts_us        = dts_us;
        used_fallback = true;
    }

    if (dts_us < 0 || pts_us < 0)
    {
        const uint64_t step_us = (FrameIntervalUs_ > 0 ? FrameIntervalUs_ : 1);
        if (!HasLastTimestamp_)
        {
            dts_us = 0;
            pts_us = 0;
        }
        else
        {
            dts_us = static_cast<int64_t>(LastDtsUs_ + step_us);
            pts_us = dts_us;
        }
        used_fallback = true;
    }

    if (HasLastTimestamp_ && static_cast<uint64_t>(dts_us) <= LastDtsUs_)
    {
        dts_us        = static_cast<int64_t>(LastDtsUs_ + 1);
        used_fallback = true;
    }
    if (pts_us < dts_us)
    {
        pts_us        = dts_us;
        used_fallback = true;
    }

    uint64_t dts_ms = static_cast<uint64_t>(dts_us / 1000);
    uint64_t pts_ms = static_cast<uint64_t>(pts_us / 1000);
    if (HasLastTimestamp_ && dts_ms <= LastDtsMs_)
    {
        dts_ms        = LastDtsMs_ + 1;
        used_fallback = true;
    }
    if (pts_ms < dts_ms)
    {
        pts_ms        = dts_ms;
        used_fallback = true;
    }

    LastDtsUs_        = static_cast<uint64_t>(dts_us);
    LastPtsUs_        = static_cast<uint64_t>(pts_us);
    LastDtsMs_        = dts_ms;
    LastPtsMs_        = pts_ms;
    VideoTimelineDtsMs_.store(LastDtsMs_, std::memory_order_relaxed);
    VideoTimelineTickMs_.store(SteadyNowMs(), std::memory_order_relaxed);
    VideoTimelineReady_.store(true, std::memory_order_relaxed);
    HasLastTimestamp_ = true;
    if (used_fallback)
    {
        ++TimestampFallbackCount_;
    }

    *out_dts_ms = LastDtsMs_;
    *out_pts_ms = LastPtsMs_;
    return true;
}

bool ZlmPublisher::IsAnnexBStartCode(const uint8_t* data, size_t len) const
{
    if (!data || len < 3)
    {
        return false;
    }

    if (len >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01)
    {
        return true;
    }

    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01)
    {
        return true;
    }

    return false;
}

bool ZlmPublisher::FindAnnexBPrefix(const uint8_t* data, size_t len, size_t pos,
                                    size_t* prefix_len) const
{
    if (!data || !prefix_len || pos >= len)
    {
        return false;
    }

    if (pos + 4 <= len && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x00 &&
        data[pos + 3] == 0x01)
    {
        *prefix_len = 4;
        return true;
    }
    if (pos + 3 <= len && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01)
    {
        *prefix_len = 3;
        return true;
    }
    return false;
}

bool ZlmPublisher::SplitAnnexBNalus(const uint8_t* data, size_t len,
                                    std::vector<NaluRange>* out_nalus) const
{
    if (!data || !out_nalus || len < 3)
    {
        return false;
    }

    out_nalus->clear();
    std::vector<size_t> starts;
    starts.reserve(16);

    for (size_t pos = 0; pos + 3 <= len;)
    {
        size_t prefix_len = 0;
        if (FindAnnexBPrefix(data, len, pos, &prefix_len))
        {
            starts.push_back(pos);
            pos += prefix_len;
        }
        else
        {
            ++pos;
        }
    }

    if (starts.empty() || starts[0] != 0)
    {
        return false;
    }

    for (size_t i = 0; i < starts.size(); ++i)
    {
        const size_t start = starts[i];
        const size_t end   = (i + 1 < starts.size()) ? starts[i + 1] : len;
        if (end <= start)
        {
            continue;
        }

        NaluRange nalu;
        nalu.data = data + start;
        nalu.len  = end - start;
        out_nalus->push_back(nalu);
    }

    return !out_nalus->empty();
}
























