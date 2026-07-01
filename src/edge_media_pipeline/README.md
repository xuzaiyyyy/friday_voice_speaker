# edge_media_pipeline 项目说明

这是给香橙派 3B / RK3566 使用的 Rockchip 实时视频推流工程，目标是把本地 USB 摄像头和第二路视频源封装成 RTSP / WebRTC 流，并在链路中接入人体识别。

当前核心链路：

```text
V4L2 USB MJPEG Camera
  -> MPP MJPEG Decode
  -> RGA NV12 Copy / Convert
  -> RKNN Person Detection / Draw Box
  -> MPP H.264 Encode
  -> ZLMediaKit RTSP / WebRTC Publish
```

第二路视频源默认使用项目内的 `test.mp4`，也可以通过 `--file` / `--rtsp` 替换：

```text
File / RTSP / Other FFmpeg Source
  -> MPP Decode
  -> RGA
  -> RKNN Person Detection
  -> MPP H.264 Encode
  -> ZLMediaKit Publish
```

## 当前能力

- 支持 USB 摄像头 `/dev/video0`，默认 MJPEG 采集。
- 默认启用摄像头 + `test.mp4` 双路推流，也支持手动切换第二路本地视频/RTSP。
- 支持 ZLMediaKit 发布 RTSP 和 WebRTC。
- 支持 RKNN 人体识别，模型默认使用 `models/yolov6n_85.rknn`。
- 支持画框后继续推流，推理失败不会主动中断视频链路。
- 支持可选音频采集，摄像头主路可推 G711A 音频。
- 支持 LAN / WAN 两套 ZLMediaKit 配置。
- 支持性能日志，持续输出 CPU、温度、FPS、队列深度、推理耗时、编码耗时等指标。
- 多线程解耦，采集、解码、处理、编码输入、编码输出、音频各自独立。

## 目录结构

```text
src/
  main.cpp                 主流程、线程调度、命令行参数、性能监控
  v4l2Camera.cpp           USB 摄像头采集和 DMABUF 池
  rkmppdec.cpp             MPP 解码
  rkrga.cpp                RGA 图像处理和输出池
  rkmppenc.cpp             MPP H.264 编码
  rknn_instance.cpp        RKNN 模型加载和张量属性解析
  postprocess.cpp          YOLO 后处理
  zlm_publisher.cpp        ZLMediaKit 发布封装
  alsa_audio_capture.cpp   ALSA 音频采集
  ffmpeg_file_source.cpp   文件/RTSP 等第二路输入

include/                   项目头文件
include/ZLM/               ZLMediaKit C API 头文件
lib/                       本项目使用的 ZLMediaKit / RKNN 运行库
www/                       WebRTC 测试页面
tools/                     ZLMediaKit 检查、安装、重编脚本
Plan/                      设计记录
config.lan.ini             局域网推流配置
config.wan.ini             公网/穿透推流配置
```

## 依赖

建议在香橙派本机编译，避免 ABI 不匹配。

必需组件：

- CMake >= 3.16
- g++，支持 C++17
- OpenCV
- Rockchip MPP
- Rockchip RGA
- RKNN runtime
- ALSA 开发库
- FFmpeg 开发库：`libavformat`、`libavcodec`、`libavutil`
- OpenSSL
- libsrtp2
- wiringPi
- ZLMediaKit C API：`libmk_api.so`
- ZLToolKit：`libZLToolKit.so` 或 `libZLToolKit.a`

当前 `CMakeLists.txt` 会优先使用：

```text
src/edge_media_pipeline/lib/libmk_api.so
src/edge_media_pipeline/lib/libZLToolKit.so 或 libZLToolKit.a
../../lib/librknnrt.so
/usr/local/lib/libsrtp2.so
```

## 编译

在开发板上进入本项目目录：

```bash
cd ~/cpp/friday_voice_speaker/src/edge_media_pipeline
cmake -S . -B build
cmake --build build -j1
```

编译产物：

```text
build/app
```

如果开发板内存紧张，建议 `-j1`。如果看到 `cc1plus` 被杀掉，一般是内存不够，可以临时加 swap 后继续编译。

## 运行

### 默认双路推流

```bash
./build/app --lan-mode
```

默认会启动：

```text
camera: /dev/video0
file:   /home/orangepi/cpp/friday_voice_speaker/src/edge_media_pipeline/test.mp4
```

不推音频：

```bash
./build/app --lan-mode --no-audio
```

禁用人体识别，只验证纯视频推流：

```bash
./build/app --lan-mode --no-audio --no-infer
```

### 单路摄像头

如果只想跑摄像头一路：

```bash
./build/app --lan-mode --single-link --no-audio
```

### 替换第二路输入

本地视频作为第二路：

```bash
./build/app --lan-mode --file ./test.mp4 --no-audio
```

RTSP 作为第二路：

```bash
./build/app --lan-mode --rtsp rtsp://user:pass@ip/stream --no-audio
```

第二路默认发布为：

```text
rtsp://<设备IP>:8554/live/file
http://<设备IP>:8000/webrtc_play_test.html?app=live&stream=file
```

### 指定模型

```bash
./build/app --model /home/orangepi/cpp/friday_voice_speaker/models/yolov6n_85.rknn
```

### RKNN 输入模式

当前代码默认使用 RKNN zero-copy 输入模式：

```text
--rknn-zero-copy   使用 RGA 写入 RKNN tensor memory，再通过 rknn_set_io_mem 绑定输入
```

如果板端 RKNN runtime、模型或驱动版本不匹配，出现 NPU submit 异常，再临时切到 CPU input 兼容模式：

```bash
./build/app --lan-mode --no-audio --rknn-cpu-input
```

兼容模式会使用 shared context + CPU input buffer，拷贝更多，但更容易用来定位环境问题。

## 命令行参数

```text
--camera <dev>                摄像头设备，默认 /dev/video0
--model <path>                RKNN 人体识别模型路径
--file <path>                 替换默认第二路本地视频输入
--input <url/path>            替换默认第二路输入，等价于通用输入
--rtsp <url>                  替换默认第二路 RTSP 输入
--single-link / --no-second-link
                              禁用第二路，只保留摄像头主路
--lan-mode                    使用 config.lan.ini
--wan-mode                    使用 config.wan.ini
--zlm-profile <lan|wan>       指定配置 profile
--zlm-config <path>           直接指定 ZLMediaKit 配置文件
--audio-device <alsa_dev>     指定 ALSA 音频设备
--audio-sync-offset-ms <ms>   音频同步补偿
--no-audio                    禁用音频
--no-infer / --no-rknn        禁用 RKNN 推理
--rknn-zero-copy              使用 RKNN zero-copy 输入模式
--rknn-cpu-input              使用 CPU input 兼容模式
--no-file-loop                第二路文件播放结束后不循环
```

## 播放地址

程序启动后会打印播放地址。默认端口：

```text
HTTP/WebRTC 页面: http://<设备IP>:8000/webrtc_play_test.html
WebRTC API:       http://<设备IP>:8000/index/api/webrtc?app=live&stream=camera&type=play
RTSP:             rtsp://<设备IP>:8554/live/camera
RTC:              8001
```

浏览器测试：

```text
http://<设备IP>:8000/webrtc_play_test.html?app=live&stream=camera
```

FFplay 测试：

```bash
ffplay rtsp://<设备IP>:8554/live/camera
```

第二路播放：

```text
http://<设备IP>:8000/webrtc_play_test.html?app=live&stream=file
rtsp://<设备IP>:8554/live/file
```

## Qt 桌面接入

Qt 网络摄像头页面会启动：

```text
src/edge_media_pipeline/build/app
```

页面会显示：

- 启动状态
- 当前模式
- 摄像头设备
- 第二路输入
- 模型路径
- 本机 IP
- PC 播放页
- WebRTC API
- 子进程运行日志

如果按钮启动失败，优先检查页面日志中的完整命令行，然后在终端复制同一条命令直接运行。

## 日志说明

典型启动日志：

```text
[ZLM] 启动模式: lan, 配置文件: ./config.lan.ini
[INFO] 双路推流已启用: camera=/dev/video0, file/input=/home/orangepi/cpp/friday_voice_speaker/src/edge_media_pipeline/test.mp4
[RKNN] Model initialized successfully: ...
[RKNN] input mode: zero-copy rknn_set_io_mem
[camera] RTSP 拉流地址: rtsp://<ip>:8554/live/camera
[camera] WebRTC 播放页: http://<ip>:8000/webrtc_play_test.html?app=live&stream=camera
```

性能日志：

```text
[MON][SYS] cpu_total_pct=..., cpu_proc_pct=..., rss_mb=..., cpu_temp_c=...
[MON][camera] in_fps=..., out_fps=..., dec_ms=..., rga_ms=..., infer_ms=..., enc_in_ms=...
```

常用字段含义：

- `cpu_total_pct`：系统总 CPU 使用率
- `cpu_proc_pct`：本进程 CPU 使用率
- `rss_mb`：进程实际常驻内存
- `cpu_temp_c`：CPU 温度
- `in_fps`：输入帧率
- `out_fps`：推流输出帧率
- `dec_ms`：解码耗时
- `rga_ms`：RGA 耗时
- `infer_ms`：人体识别耗时
- `enc_in_ms`：送入编码器耗时
- `q_depth`：三个内部队列深度
- `q_drop`：队列丢帧计数
- `fail(dec/infer/enc)`：解码、推理、编码失败计数
- `rt_opt(reuse/enc_drop)`：推理缓存复用和编码忙丢帧计数
- `ts_fix_total`：时间戳修正次数

## ZLMediaKit 库

如果 `libmk_api.so` 或 `libZLToolKit` 与系统 ABI 不匹配，会出现 OpenSSL、GLIBC、GLIBCXX 相关错误。先检查：

```bash
bash tools/check_zlm_abi.sh
```

安装已经编译好的库：

```bash
bash tools/install_zlm_libs.sh /path/to/ZLMediaKit/release/linux/Debug
```

如果需要在开发板上重新编译 ZLMediaKit，可先参考官方编译教程：https://docs.zlmediakit.com/zh/。

```bash
ZLM_SOURCE_DIR=$PWD/ZLMediaKit JOBS=1 bash tools/rebuild_zlm_on_device.sh
```

注意：

- GitHub 网页下载 ZIP 不包含完整 submodule，推荐 `git clone --recursive`。
- 本项目只需要视频 WebRTC/RTSP，不依赖 WebRTC datachannel。
- `libmk_api.so`、`libZLToolKit` 和 `include/ZLM` 头文件最好来自同一次源码编译。

## 常见问题

### CMake 跑到了主项目

如果配置时出现 `smart_voice_speaker`、`src/app_config.cpp` 等主语音音箱项目内容，说明当前目录不对，或 CMakeLists 被覆盖。

正确检查：

```bash
cd ~/cpp/friday_voice_speaker/src/edge_media_pipeline
head -20 CMakeLists.txt
```

应看到：

```text
project(v4l2_rknn_zlm_app LANGUAGES CXX)
```

### 浏览器显示 camera failed

先看开发板日志。浏览器里的 `Failed to fetch` 往往只是后端推流进程异常退出后的结果。

排查顺序：

1. 直接运行 Qt 页面打印出的完整命令。
2. 用 `--no-infer` 验证纯视频链路。
3. 确认 `models/yolov6n_85.rknn` 存在。
4. 确认 `librknnrt.so` 使用的是当前项目 `../../lib` 或板端可用版本。
5. 确认 `libmk_api.so` 和 ZLM 头文件来自同一次编译。

### RKNN 推理异常

如果只想验证推流：

```bash
./build/app --lan-mode --no-audio --no-infer
```

人体识别默认走 zero-copy。如果遇到 NPU submit 或模型/runtime 兼容问题，可以先切到 CPU input 模式确认链路：

```bash
./build/app --lan-mode --no-audio --rknn-cpu-input
```

CPU input 模式会增加拷贝，只建议作为排障兜底。

### 第二路不启动

确认输入地址可由 FFmpeg 打开：

```bash
ffprobe <file-or-rtsp-url>
```

本地文件默认循环播放；如果不希望循环，加：

```bash
--no-file-loop
```

如果只想关闭第二路，加：

```bash
--single-link
```

## 当前默认值

```text
camera device: /dev/video0
model:         /home/orangepi/cpp/friday_voice_speaker/models/yolov6n_85.rknn
second input:  /home/orangepi/cpp/friday_voice_speaker/src/edge_media_pipeline/test.mp4
rknn input:    zero-copy rknn_set_io_mem
profile:       lan
audio:         enabled, unless --no-audio
audio format:  8000 Hz / mono / 16-bit PCM -> G711A
queue depth:   6
camera stream: live/camera
file stream:   live/file
RTSP port:     8554
HTTP port:     8000
RTC port:      8001
```

## 退出

按一次 `Ctrl+C` 会触发优雅退出；如果线程或驱动阻塞，第二次 `Ctrl+C` 会强制退出。
