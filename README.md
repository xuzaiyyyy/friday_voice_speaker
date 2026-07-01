# 星期五智能语音音箱项目介绍与使用说明

> 本文档用于 GitHub 项目展示、部署说明和技术复盘。  
> 项目名称：`friday_voice_speaker`。  
> 本仓库中的云服务凭据和私有地址均使用占位符，运行前请按需配置自己的合法凭据。

## 1. 项目定位

`friday_voice_speaker` 是一个运行在 Orange Pi / Rockchip 平台上的综合型智能语音音箱与多模态交互系统。项目把语音唤醒、本地语音识别、LLM 对话、视觉问答、在线 TTS、本地/在线音乐播放、在线音源搜索、摄像头预览、Qt 触摸大屏、智能猫眼、人脸识别、情感检测，以及端侧视频处理子模块整合到同一个可运行的产品原型中。

更准确地说，它不是单一小功能 Demo，而是一个“智能终端外壳 + 多个 AI/音视频模块 + Qt 产品化界面”的综合项目。

项目通过配置文件、FIFO、命令路由和多线程模块化方式，将语音交互、视觉问答、Qt 大屏、摄像头、人脸识别、情感检测和端侧视频处理能力整合为一个可运行的产品原型。其中 `edge_media_pipeline` 作为端侧视频处理子系统被集成到本仓库中。

## 2. 项目边界说明

- 本项目是综合集成项目，重点体现模块整合、Qt 封装、端侧部署和工程化能力。
- `src/edge_media_pipeline` 是内嵌的端侧 AI 视频处理与流媒体子模块，可作为独立的视频处理子系统理解。
- `desktop/` 是 Qt 桌面与大屏交互界面，负责表情页、桌面页、工具页、摄像头页、音乐页、天气页、智能猫眼等 UI 封装。
- `src/smart_voice_speaker.cpp` 是智能音箱主程序，负责语音、状态机、命令路由、摄像头、LCD、ASR、TTS、音乐和 UI 命令联动。
- 云端大模型、图片理解、Edge TTS、天气服务等外部能力需要 API 或网络环境，仓库中的示例配置使用 `********` 作为占位符。
- 运行项目时，需要使用者自行配置合法 API Key、模型文件路径、音频设备号、摄像头设备号和屏幕参数。

## 3. 核心能力概览

| 模块 | 能力 | 主要代码位置 |
| --- | --- | --- |
| 智能音箱主控 | 唤醒、ASR、命令路由、状态管理、TTS、音乐播放、摄像头联动 | `src/smart_voice_speaker.cpp` |
| 配置系统 | 统一读取硬件、模型、TTS、LLM、视觉、Qt FIFO 等配置 | `friday_voice_speaker.conf`、`src/app_config.cpp` |
| 本地 ASR | 使用 sherpa-onnx 中文模型完成本地语音识别 | `models/sherpa-onnx-*`、`smart_voice_speaker.cpp` |
| LLM/视觉/TTS Helper | 本地 llama.cpp、云端文本模型、图片理解 API、Edge TTS 封装 | `xunfei_llm_chat.py`、`src/assistant_client.cpp` |
| 命令路由 | 语音命令、键盘命令、Qt 按钮命令统一解析 | `src/command_router.cpp` |
| Qt 大屏 | 表情页、桌面页、工具页、摄像头页、音乐页、天气页、聊天页 | `desktop/` |
| UI 解耦 | Qt 通过 FIFO 向后端发送命令，后端也可通过 FIFO 控制 Qt 页面 | `desktop/uicommand.cpp`、`desktop/desktopcontrol.cpp` |
| 人脸识别/猫眼 | 摄像头预览、人脸注册、识别、删除、本地人脸库 | `desktop/door.cpp`、`desktop/facethread.cpp` |
| 本地 RKNN 推理 | 人体检测、情感检测、人脸检测/识别等端侧推理 | `src/emotion_detector.cpp`、`src/person_detector.cpp`、`desktop/facethread.cpp` |
| 音乐播放 | 本地歌单、在线点歌、音源搜索、URL 解析、播放/停止控制 | `desktop/musicpage.cpp`、`src/music_player.cpp`、`xunfei_llm_chat.py` |
| 端侧视频子模块 | V4L2/MPP/RGA/RKNN/ZLMediaKit 视频处理与推流 | `src/edge_media_pipeline` |

## 4. 系统架构

```text
用户语音 / 键盘 / Qt 触摸按钮
        |
        v
smart_voice_speaker 主程序
        |
        +-- ALSA 麦克风采集
        +-- sherpa-onnx 本地 ASR
        +-- command_router 命令解析
        +-- 摄像头按需采集 / 预览
        +-- SPI 小屏状态显示
        +-- 音乐播放 / 在线音源搜索
        |
        +------------------------------+
        |                              |
        v                              v
xunfei_llm_chat.py helper         Qt desktop 大屏
        |                              |
        +-- 本地 llama.cpp              +-- 表情页
        +-- 云端文本对话                +-- 桌面工具页
        +-- 图片理解 API                +-- 摄像头/音乐/天气
        +-- Edge TTS                    +-- 智能猫眼
        |                              |
        +-------------- FIFO ----------+
                       |
                       v
            统一命令入口与状态联动
```

端侧视频处理子系统可以单独理解为：

```text
摄像头 / 文件 / RTSP 输入
        |
        v
V4L2 / FFmpeg
        |
        v
MPP 硬解码 / RGA 图像处理 / RKNN 推理
        |
        v
MPP H.264 硬编码
        |
        v
ZLMediaKit RTSP / WebRTC 推流
```

## 5. 关键技术点

### 5.1 统一配置系统

项目使用 `friday_voice_speaker.conf` 统一管理运行参数，包括：

- 麦克风设备：`XIAOMAN_MIC_DEVICE`
- 摄像头设备：`XIAOMAN_CAMERA_DEV`
- LCD/SPI 屏幕参数
- Qt 大屏启动命令
- FIFO 路径
- ASR/LLM/RKNN 模型路径
- TTS 后端和音色
- 云端视觉/文本模型配置
- 性能调优参数，例如摄像头空闲策略、VAD 阈值、检测间隔

配置优先级为：

```text
命令行参数 > 环境变量 > friday_voice_speaker.conf > 程序默认值
```

这个设计让项目可以在不同板子、不同声卡、不同屏幕和不同模型路径下快速迁移。

### 5.2 语音交互状态机

项目支持语音唤醒和连续对话。典型流程：

```text
空闲监听
  -> 检测唤醒词“你好星期五 / 星期五”
  -> 播放应答语
  -> 录制用户语音
  -> 本地 ASR 转文字
  -> 命令路由判断是否为设备命令
  -> 普通问题交给 LLM
  -> TTS 合成并播放
  -> 回到等待下一轮输入
```

这样做的好处是把“设备控制命令”和“开放式聊天”分开：打开摄像头、停止播放、切换表情、开始情感识别等命令可以直接执行；普通问题才进入 LLM。

### 5.3 Qt 与后端 FIFO 解耦

Qt 界面不直接调用 AI 模型，也不直接维护语音状态机，而是通过 FIFO 把按钮命令交给后端。

默认管道：

```text
/tmp/friday_voice_speaker_ui.fifo
/tmp/friday_voice_speaker_desktop.fifo
```

这种设计的优点：

- Qt 只负责界面和交互，不和 AI/音频/摄像头逻辑强耦合。
- 后端统一处理语音、键盘、触摸按钮三种输入。
- 调试方便，终端输入和 Qt 按钮可以走同一套命令路由。
- 大屏崩溃或未启动时，后端主程序仍然可以独立运行。

### 5.4 端云协同

本项目预留并实现了本地部署大语言模型的接口，`start_local_llm.sh` 可启动 `llama.cpp + GGUF` 本地服务，`xunfei_llm_chat.py` 和 C++ 主程序可以通过 OpenAI-compatible HTTP 接口调用本地模型。

实际测试中，Orange Pi 3B / RK3566 这类平台主要依赖 CPU 跑本地 GGUF 文本模型，回答延迟较高，不适合完整语音交互的即时体验。因此项目默认配置更偏向云端文本 API，用本地模型作为可选后端或离线备用能力。这是基于响应速度、资源占用和用户体验做出的工程取舍，而不是没有本地大模型接入能力。

能力拆分如下：

- 本地：唤醒、ASR、设备控制、状态管理、部分 RKNN 推理、Qt UI。
- 本地 LLM：通过 `llama.cpp + GGUF` 提供本地文本模型服务，可用于离线或低频交互场景。
- 云端/在线：普通高频对话、图片理解、Edge TTS、在线音乐等可以使用网络服务，以获得更快响应和更自然的交互体验。

可以通过 `NEWBOT_CHAT_BACKEND` 切换聊天后端：

```text
NEWBOT_CHAT_BACKEND=cloud   # 默认推荐，响应更快
NEWBOT_CHAT_BACKEND=local   # 使用本地 llama.cpp 服务
NEWBOT_CHAT_BACKEND=auto    # 优先本地，失败后使用云端兜底
```

### 5.5 本地 RKNN 推理

项目中包含多类本地 NPU/RKNN 推理能力：

- 人体检测：`src/person_detector.cpp`
- 情感检测：`src/emotion_detector.cpp`
- 智能猫眼人脸检测与识别：`desktop/facethread.cpp`
- 端侧视频处理子系统中的 YOLO/RKNN 推理：`src/edge_media_pipeline`

项目不仅调用云 API，也包含端侧模型加载、图像预处理、推理结果解析和 Qt 显示联动。

### 5.6 内嵌端侧视频处理子模块

`src/edge_media_pipeline` 是本项目中的端侧音视频子模块，负责端侧 AI 视频处理和流媒体推送能力。它的技术栈包括：

- V4L2 摄像头采集
- FFmpeg 文件/RTSP 输入
- Rockchip MPP 硬解码/硬编码
- RGA 图像缩放、格式转换和硬件加速处理
- RKNN YOLO 推理
- ZLMediaKit RTSP/WebRTC 推流
- 多线程流水线和有界队列
- LAN/WAN WebRTC 配置

### 5.7 音乐页与在线音源搜索

Qt 音乐页现在不只是固定本地歌单，而是加入了在线搜索入口。用户可以在音乐页输入歌名或歌手，界面通过 `QProcess` 调用：

```bash
python3 xunfei_llm_chat.py --music-url
```

helper 会从用户输入中提取歌曲查询词，按配置的 provider 搜索可播放音源，并以键值形式返回给 Qt：

```text
MUSIC_TITLE=歌曲名
MUSIC_ARTIST=歌手
MUSIC_URL=https://...
```

Qt 收到结果后会把在线歌曲加入 `QMediaPlaylist`，并使用 `QMediaPlayer` 播放 URL。这样本地歌单、语音点歌和触摸屏搜索可以复用同一个 helper 能力。

相关配置：

```text
NEWBOT_MUSIC_PROVIDER=netease   # 默认使用网易云搜索
NEWBOT_MUSIC_PROVIDER=qq        # 使用自建/授权 QQ 音乐解析服务
NEWBOT_MUSIC_PROVIDER=auto      # 优先 QQ，失败后回退网易云
NEWBOT_QQ_MUSIC_API_URL=...     # 自建 QQ 音乐解析接口
NEWBOT_QQ_MUSIC_API_TOKEN=...   # 可选鉴权 Token
```

## 6. 目录结构

```text
friday_voice_speaker/
├── README.md                         # 原有运行说明
├── PROJECT_OVERVIEW.md               # 当前项目介绍和发布注意事项
├── friday_voice_speaker.conf         # 统一运行配置模板，敏感项使用占位符
├── xunfei_llm_chat.py                 # LLM / 视觉 / TTS helper
├── start_local_llm.sh                 # llama.cpp 本地服务启动脚本
├── CMakeLists.txt                     # C++ 主程序构建配置
├── src/
│   ├── smart_voice_speaker.cpp        # 智能音箱主程序
│   ├── app_config.cpp                 # 配置加载
│   ├── app_state.cpp                  # 全局状态
│   ├── app_utils.cpp                  # 路径和工具函数
│   ├── assistant_client.cpp           # 与 Python helper 通信
│   ├── command_router.cpp             # 命令解析和路由
│   ├── music_player.cpp               # 音乐播放与点歌控制
│   ├── person_detector.cpp            # 人体检测
│   ├── emotion_detector.cpp           # 情感检测
│   └── edge_media_pipeline/             # 内嵌端侧视频处理与流媒体子模块
├── include/                           # 主程序头文件
├── desktop/                           # Qt 大屏、桌面和智能猫眼界面
│   ├── expressionwindow.cpp           # 表情主页
│   ├── mainwindow.cpp                 # 桌面主页
│   ├── uicommand.cpp                  # Qt -> 后端 FIFO 命令
│   ├── desktopcontrol.cpp             # 后端 -> Qt 控制
│   ├── door.cpp                       # 智能猫眼页面
│   ├── facethread.cpp                 # 人脸检测/识别线程
│   ├── camerapage.cpp                 # 摄像头页面
│   ├── musicpage.cpp                  # 音乐页面与在线音源搜索
│   ├── weatherpage.cpp                # 天气页面
│   └── desktop.pro                    # Qt 工程文件
├── image/                             # 表情资源
├── models/                            # ASR/LLM/RKNN 模型
├── lib/                               # 运行库
├── prompts/                           # 人格提示词
└── tools/                             # 辅助工具
```

## 7. 编译与运行

### 7.1 主程序依赖

编译前需要先准备板端运行库，尤其是端侧视频子模块依赖的 ZLMediaKit 和 RKNN runtime。这些库和系统 ABI 绑定较紧，建议在目标开发板或相同系统环境中编译，避免 OpenSSL、GLIBC、GLIBCXX 版本不匹配。

- ZLMediaKit：先按官方文档编译，生成 `libmk_api.so`、`libZLToolKit.so` 或 `libZLToolKit.a`，并保持 `include/ZLM` 头文件与库文件来自同一次编译。官方教程：[ZLMediaKit 编译说明](https://docs.zlmediakit.com/zh/)。
- RKNN runtime（如 `lib/librknnrt.so`）：准备与板卡系统和 RKNN Toolkit 版本匹配的 `librknnrt.so`，默认放在 `lib/librknnrt.so`。
- 端侧视频子模块默认从 `src/edge_media_pipeline/lib/` 查找 ZLMediaKit 库，并从 `src/edge_media_pipeline/include/ZLM/` 查找头文件。
- 公开仓库不附带预编译 ZLMediaKit .so / .a 文件；请按官方教程自行编译后放入 src/edge_media_pipeline/lib/，避免 ABI 不匹配和本机编译路径泄露。

常见依赖：

```bash
sudo apt install cmake g++ libasound2-dev libopencv-dev ffmpeg
sudo python3 -m pip install edge-tts websocket-client==0.53.0
```

还需要：

- sherpa-onnx C API 头文件和库
- OpenCV
- ALSA
- RKNN runtime（如 `lib/librknnrt.so`）
- Qt 运行环境
- 可用摄像头和麦克风
- 可选：`llama.cpp` 本地 LLM 服务

### 7.2 编译主程序

```bash
cd ~/cpp/friday_voice_speaker
mkdir -p build
cd build
cmake ..
make -j4
```

运行：

```bash
sudo -E ./smart_voice_speaker
```

如果只是调试文本和界面，可以关闭语音播放：

```bash
sudo -E ./smart_voice_speaker --voice-off
```

### 7.3 启动本地 LLM

```bash
cd ~/cpp/friday_voice_speaker
chmod +x start_local_llm.sh
./start_local_llm.sh
```

测试：

```bash
echo "你是谁" | python3 -u xunfei_llm_chat.py --local-llm-test
```

本地 LLM 接口主要用于离线备用或功能验证。若板端 CPU 推理速度无法满足实时语音交互，可以在配置文件中使用云端后端：

```text
NEWBOT_CHAT_BACKEND=cloud
```

需要强制走本地模型时改为：

```text
NEWBOT_CHAT_BACKEND=local
```

### 7.4 编译 Qt 大屏

```bash
cd desktop
qmake desktop.pro
make -j4
```

MIPI / framebuffer 模式：

```bash
QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0 \
QT_QPA_FB_FORCE_FULLSCREEN=1 \
XIAOMAN_DESKTOP_FULLSCREEN=1 \
XIAOMAN_UI_COMMAND_FIFO=/tmp/friday_voice_speaker_ui.fifo \
./desktop
```

桌面调试模式：

```bash
QT_QPA_PLATFORM=xcb \
QT_XCB_GL_INTEGRATION=none \
LIBGL_ALWAYS_SOFTWARE=1 \
XIAOMAN_DESKTOP_FULLSCREEN=0 \
./desktop
```

### 7.5 编译端侧推流程序

`src/edge_media_pipeline` 是独立的端侧视频处理和推流子工程，需要在子目录单独编译。编译前请先确认 `libmk_api.so`、`libZLToolKit.so` / `libZLToolKit.a`、`librknnrt.so` 以及 `include/ZLM` 头文件已经按前文说明准备好。

```bash
cd ~/cpp/friday_voice_speaker/src/edge_media_pipeline
cmake -S . -B build
cmake --build build -j1
```

默认双路推流运行：

```bash
./build/app --lan-mode --no-audio
```

如果只想先验证纯视频推流链路，可以临时关闭 RKNN 推理：

```bash
./build/app --lan-mode --no-audio --no-infer
```

常用访问地址：

```text
rtsp://<设备IP>:8554/live/camera
http://<设备IP>:8000/webrtc_play_test.html?app=live&stream=camera
```

第二路输入默认使用 `src/edge_media_pipeline/test.mp4`，也可以通过 `--file <path>` 或 `--rtsp <url>` 指定本地视频或 RTSP 输入。更多参数见 `src/edge_media_pipeline/README.md`。

## 8. 常用交互命令

语音唤醒词：

```text
你好星期五
星期五
```

典型语音命令：

```text
你看到了什么
打开摄像头
关闭摄像头
显示表情
开始情感识别
停止播放
播放周杰伦的稻香
清空记忆
退出对话
```

键盘快捷键：

```text
w / wake      文本唤醒
v / vision    视觉问答
p / status    状态面板
c / camera    打开摄像头
f / face      回到表情页
e / emotion   情感识别
s / stop      停止播放或停止检测
m 歌名        点歌
q / exit      退出程序
```

## 9. 安全与配置

本仓库不会提供可直接使用的第三方服务凭据。所有涉及云服务、数据库、公网地址或本机私有路径的配置都应该按自己的运行环境重新填写。

### 9.1 凭据配置

以下配置需要使用者自行申请或填写：

```bash
export XUNFEI_APIPASSWORD="your_api_password"
export XUNFEI_VISION_APPID="your_appid"
export XUNFEI_VISION_APIKEY="your_apikey"
export XUNFEI_VISION_APISECRET="your_apisecret"
```

天气、数据库、WebRTC 公网地址等配置也应使用自己的环境值。示例配置文件中的 `********` 只是占位符，不代表真实服务地址或密钥。

### 9.2 私有配置文件

推荐把真实配置放到不提交 Git 的私有配置文件中，例如：

```bash
cp friday_voice_speaker.conf friday_voice_speaker.local.conf
./smart_voice_speaker --config ../friday_voice_speaker.local.conf
```

也可以使用环境变量注入凭据。不要把真实 API Key、数据库密码、管理员密码或公网服务器地址提交到公开仓库。

### 9.3 建议的 .gitignore

项目中会生成构建产物、缓存、日志和 Qt 本机配置文件，建议忽略：

```gitignore
build/
build-*/
cmake-build-*/
__pycache__/
cache/
*.log
*.tmp
*.user
*.pro.user*
*.o
*.a
*.so
*.dll
*.exe
```

如果仓库不计划直接分发模型和运行库，也可以按需忽略：

```gitignore
models/
lib/
*.rknn
*.gguf
*.onnx
```

## 10. 仓库内容说明

本项目包含智能音箱主程序、Qt 大屏界面、AI helper 脚本、配置模板、提示词、模型目录和端侧视频处理子模块。为了便于理解，仓库按功能拆分为：

- `src/`：智能音箱主程序、命令路由、音乐播放控制、本地检测模块，以及内嵌的 `edge_media_pipeline` 视频子系统。
- `desktop/`：Qt 大屏界面、表情页、桌面页、智能猫眼、摄像头页、音乐页、在线音源搜索、天气页等。
- `prompts/`：星期五人格提示词。
- `models/`：本地 ASR、LLM、RKNN 模型的默认放置目录。
- `lib/`：板端运行库目录。
- `friday_voice_speaker.conf`：运行配置模板。

模型文件、运行库和样例视频可能体积较大，也可能受到第三方授权限制。当前 GitHub 发布副本已默认忽略 GGUF/ONNX 权重和本地音乐素材；如需完整运行本地 LLM/ASR/音乐页，请按配置路径自行放置模型和音频文件。

内嵌的 `src/edge_media_pipeline` 是端侧视频处理与流媒体子模块，提供 V4L2、MPP、RGA、RKNN、ZLMediaKit 相关能力。

## 11. 已知限制

- 项目依赖具体硬件环境，例如 Orange Pi、摄像头、麦克风、SPI 小屏、MIPI 屏等，换板子需要改配置。
- 云端视觉、云端文本模型、Edge TTS、在线音乐和音源搜索都需要网络。QQ 音源搜索需要自行配置合法或自建的解析服务。
- 示例配置中的 Key 和私有地址为占位符，因此直接运行云端相关功能前需要重新配置真实凭据。
- 本地 LLM 接口已经接入，但在 RK3566/RK3588 等板端如果主要依赖 CPU 推理，速度会明显受模型大小、量化方式和线程数影响；实时语音交互场景可使用云端 API 提升响应速度。
- Qt 大屏在不同 framebuffer、桌面环境和显卡驱动下可能需要调整 `QT_QPA_PLATFORM` 等环境变量。
- 内嵌的 `edge_media_pipeline` 是完整视频子模块，体积和依赖更重；如果只是展示智能音箱主流程，可以在 README 中说明该子模块可选。

## 12. 项目简介

English summary:

```text
An Orange Pi / Rockchip based multimodal smart speaker integrating voice interaction, Qt touchscreen UI, local ASR, LLM helper, vision QA, RKNN inference, smart door face recognition, and an embedded edge video streaming subsystem.
```

中文简介：

```text
基于 Orange Pi / Rockchip 平台的多模态智能语音音箱综合项目，集成语音唤醒、本地 ASR、LLM 对话、视觉问答、Qt 大屏、智能猫眼、人脸识别、RKNN 推理和端侧视频处理子模块。
```

