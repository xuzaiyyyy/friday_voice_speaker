cmake_minimum_required(VERSION 3.16)

project(v4l2_rknn_zlm_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# 优先使用自己编译安装到 /usr/local/lib 的 libsrtp2
set(SRTP2_LOCAL_LIB_DIR /usr/local/lib)
set(SRTP2_LOCAL_INCLUDE_DIR /usr/local/include/srtp2)

find_library(SRTP2_LIB
    NAMES srtp2
    PATHS ${SRTP2_LOCAL_LIB_DIR}
    NO_DEFAULT_PATH
)
if (NOT SRTP2_LIB)
    message(FATAL_ERROR "Failed to find self-built libsrtp2 in ${SRTP2_LOCAL_LIB_DIR}; please install libsrtp2.so.1 first")
endif()
message(STATUS "Using libsrtp2: ${SRTP2_LIB}")

find_path(RGA_INCLUDE_DIR NAMES im2d.h PATHS /usr/include/rga NO_DEFAULT_PATH)
if (NOT RGA_INCLUDE_DIR)
    message(FATAL_ERROR "Failed to find RGA headers (im2d.h) in /usr/include/rga")
endif()

find_library(RGA_LIB NAMES rga REQUIRED)
find_library(ROCKCHIP_MPP_LIB NAMES rockchip_mpp REQUIRED)
find_library(ASOUND_LIB NAMES asound REQUIRED)

find_path(FFMPEG_INCLUDE_DIR NAMES libavformat/avformat.h)
if (NOT FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "Failed to find FFmpeg headers")
endif()

find_library(AVFORMAT_LIB NAMES avformat REQUIRED)
find_library(AVCODEC_LIB NAMES avcodec REQUIRED)
find_library(AVUTIL_LIB NAMES avutil REQUIRED)

set(ZLM_LOCAL_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/ZLM)
set(ZLM_LOCAL_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)
set(XIAOMAN_RUNTIME_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)

if (NOT EXISTS ${ZLM_LOCAL_INCLUDE_DIR}/mk_mediakit.h)
    message(FATAL_ERROR "Failed to find ZLM C API headers in ${ZLM_LOCAL_INCLUDE_DIR}")
endif()

find_library(ZLM_MK_API_LIB NAMES mk_api PATHS ${ZLM_LOCAL_LIB_DIR} NO_DEFAULT_PATH)
if (NOT ZLM_MK_API_LIB)
    message(FATAL_ERROR "Failed to find libmk_api.so in ${ZLM_LOCAL_LIB_DIR}")
endif()

find_library(ZLM_TOOLKIT_LIB NAMES ZLToolKit PATHS ${ZLM_LOCAL_LIB_DIR} NO_DEFAULT_PATH)
if (NOT ZLM_TOOLKIT_LIB)
    message(FATAL_ERROR "Failed to find libZLToolKit.so or libZLToolKit.a in ${ZLM_LOCAL_LIB_DIR}")
endif()
message(STATUS "Using ZLMediaKit C API: ${ZLM_MK_API_LIB}")
message(STATUS "Using ZLToolKit: ${ZLM_TOOLKIT_LIB}")

find_library(RKNNRT_LIB NAMES rknnrt PATHS ${XIAOMAN_RUNTIME_LIB_DIR} NO_DEFAULT_PATH)
if (NOT RKNNRT_LIB)
    find_library(RKNNRT_LIB NAMES rknnrt REQUIRED)
endif()
message(STATUS "Using RKNN runtime: ${RKNNRT_LIB}")

add_executable(app
    src/main.cpp
    src/alsa_audio_capture.cpp
    src/ffmpeg_file_source.cpp
    src/rkmppdec.cpp
    src/rkmppenc.cpp
    src/zlm_publisher.cpp
    src/rkrga.cpp
    src/v4l2Camera.cpp
    src/postprocess.cpp
    src/rknn_instance.cpp
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ZLM_LOCAL_INCLUDE_DIR}
    /usr/include/rockchip
    ${RGA_INCLUDE_DIR}
    ${FFMPEG_INCLUDE_DIR}
    ${OpenCV_INCLUDE_DIRS}
    ${SRTP2_LOCAL_INCLUDE_DIR}
)

target_link_libraries(app PRIVATE
    Threads::Threads
    ${RGA_LIB}
    ${ROCKCHIP_MPP_LIB}
    ${ASOUND_LIB}
    ${AVFORMAT_LIB}
    ${AVCODEC_LIB}
    ${AVUTIL_LIB}
    ${OpenCV_LIBS}
    ${RKNNRT_LIB}
    wiringPi
    ${ZLM_MK_API_LIB}
    ${ZLM_TOOLKIT_LIB}
    ${SRTP2_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${CMAKE_DL_LIBS}
    Threads::Threads
)

set_target_properties(app PROPERTIES
    BUILD_RPATH "${SRTP2_LOCAL_LIB_DIR};${CMAKE_CURRENT_SOURCE_DIR}/lib;${XIAOMAN_RUNTIME_LIB_DIR}"
    INSTALL_RPATH "${SRTP2_LOCAL_LIB_DIR};${CMAKE_CURRENT_SOURCE_DIR}/lib;${XIAOMAN_RUNTIME_LIB_DIR}"
    BUILD_WITH_INSTALL_RPATH FALSE
)

# 使用传统 RPATH，尽量保证 libmk_api.so 的间接依赖也优先从 /usr/local/lib 解析
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(app PRIVATE "-Wl,--disable-new-dtags")
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(app PRIVATE -Wall -Wextra -Wpedantic)
endif()