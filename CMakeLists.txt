cmake_minimum_required(VERSION 3.10)

project(friday_voice_speaker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SHERPA_ONNX_ROOT "" CACHE PATH "Optional sherpa-onnx install root")
option(SMART_SPEAKER_ENABLE_RKNN_EMOTION "Enable local RKNN face emotion detection" ON)
option(SMART_SPEAKER_ENABLE_RKNN_PERSON "Enable local RKNN YOLOv6 person detection" ON)

add_executable(smart_voice_speaker
    src/app_config.cpp
    src/app_state.cpp
    src/app_utils.cpp
    src/assistant_client.cpp
    src/command_router.cpp
    src/music_player.cpp
    src/smart_voice_speaker.cpp
)

if(MSVC)
    target_compile_options(smart_voice_speaker PRIVATE /W4 /utf-8)
    target_compile_definitions(smart_voice_speaker PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(smart_voice_speaker PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(smart_voice_speaker PRIVATE Threads::Threads)
target_include_directories(smart_voice_speaker PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")

find_package(ALSA QUIET)
if(ALSA_FOUND)
    target_include_directories(smart_voice_speaker PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(smart_voice_speaker PRIVATE ${ALSA_LIBRARIES})
    target_compile_definitions(smart_voice_speaker PRIVATE SMART_SPEAKER_USE_ALSA_PCM=1)
    message(STATUS "smart_voice_speaker: ALSA microphone capture enabled")
else()
    message(WARNING "ALSA development library not found: microphone capture disabled. Install libasound2-dev.")
endif()

find_package(OpenCV QUIET CONFIG
    COMPONENTS core imgcodecs imgproc videoio
    HINTS
        /usr/lib/aarch64-linux-gnu/cmake/opencv4
        /usr/lib/arm-linux-gnueabihf/cmake/opencv4
        /usr/lib64/cmake/opencv4
        /usr/lib/cmake/opencv4
)

if(OpenCV_FOUND)
    target_include_directories(smart_voice_speaker PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(smart_voice_speaker PRIVATE ${OpenCV_LIBS})
    target_compile_definitions(smart_voice_speaker PRIVATE SMART_SPEAKER_USE_OPENCV=1)
    set(SMART_SPEAKER_HAS_OPENCV ON)
    message(STATUS "smart_voice_speaker: OpenCV camera + vision frame capture enabled")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(OPENCV4 QUIET IMPORTED_TARGET opencv4)
        if(OPENCV4_FOUND)
            target_link_libraries(smart_voice_speaker PRIVATE PkgConfig::OPENCV4)
            target_compile_definitions(smart_voice_speaker PRIVATE SMART_SPEAKER_USE_OPENCV=1)
            set(SMART_SPEAKER_HAS_OPENCV ON)
            message(STATUS "smart_voice_speaker: OpenCV found via pkg-config")
        endif()
    endif()
    if(NOT OPENCV4_FOUND)
        message(WARNING "OpenCV not found: camera and visual questions will be disabled. Install libopencv-dev.")
    endif()
endif()

if(SMART_SPEAKER_ENABLE_RKNN_EMOTION OR SMART_SPEAKER_ENABLE_RKNN_PERSON)
    find_path(SMART_SPEAKER_RKNN_INCLUDE_DIR
        NAMES rknn_api.h
        HINTS
            "${CMAKE_CURRENT_LIST_DIR}/include"
            /usr/include
            /usr/include/rknn
            /usr/local/include)
    find_library(SMART_SPEAKER_RKNNRT_LIBRARY
        NAMES rknnrt
        HINTS
            "${CMAKE_CURRENT_LIST_DIR}/lib"
            /usr/lib
            /usr/lib/aarch64-linux-gnu
            /usr/local/lib
            /usr/local/lib64)

    if(SMART_SPEAKER_HAS_OPENCV AND SMART_SPEAKER_RKNN_INCLUDE_DIR AND SMART_SPEAKER_RKNNRT_LIBRARY)
        set(SMART_SPEAKER_HAS_RKNN_RUNTIME ON)
        target_include_directories(smart_voice_speaker PRIVATE "${SMART_SPEAKER_RKNN_INCLUDE_DIR}")
        target_link_libraries(smart_voice_speaker PRIVATE "${SMART_SPEAKER_RKNNRT_LIBRARY}")
        set_property(TARGET smart_voice_speaker APPEND PROPERTY BUILD_RPATH "${CMAKE_CURRENT_LIST_DIR}/lib;$ORIGIN/../lib")
        set_property(TARGET smart_voice_speaker APPEND PROPERTY INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN/lib")
    endif()
endif()

if(SMART_SPEAKER_ENABLE_RKNN_PERSON)
    if(SMART_SPEAKER_HAS_RKNN_RUNTIME)
        target_sources(smart_voice_speaker PRIVATE src/person_detector.cpp)
        target_compile_definitions(smart_voice_speaker PRIVATE SMART_SPEAKER_USE_RKNN_PERSON=1)
        message(STATUS "smart_voice_speaker: local RKNN person detection enabled")
    else()
        message(WARNING "RKNN person detection disabled: requires OpenCV, rknn_api.h, and librknnrt.so.")
    endif()
else()
    message(STATUS "smart_voice_speaker: local RKNN person detection disabled by option")
endif()

if(SMART_SPEAKER_ENABLE_RKNN_EMOTION)
    if(SMART_SPEAKER_HAS_RKNN_RUNTIME)
        target_sources(smart_voice_speaker PRIVATE src/emotion_detector.cpp)
        target_compile_definitions(smart_voice_speaker PRIVATE SMART_SPEAKER_USE_RKNN_EMOTION=1)
        message(STATUS "smart_voice_speaker: local RKNN emotion detection enabled")
    else()
        message(WARNING "RKNN emotion detection disabled: requires OpenCV, rknn_api.h, and librknnrt.so.")
    endif()
else()
    message(STATUS "smart_voice_speaker: local RKNN emotion detection disabled by option")
endif()

set(SMART_SPEAKER_SHERPA_SEARCH_HINTS)
list(APPEND SMART_SPEAKER_SHERPA_SEARCH_HINTS "${CMAKE_CURRENT_LIST_DIR}")
list(APPEND SMART_SPEAKER_SHERPA_SEARCH_HINTS "${CMAKE_CURRENT_LIST_DIR}/../voice_test")
list(APPEND SMART_SPEAKER_SHERPA_SEARCH_HINTS "${CMAKE_CURRENT_LIST_DIR}/../newbot-master/tools/voice_test")
list(APPEND SMART_SPEAKER_SHERPA_SEARCH_HINTS "${CMAKE_CURRENT_LIST_DIR}/../tools/voice_test")
if(SHERPA_ONNX_ROOT)
    list(APPEND SMART_SPEAKER_SHERPA_SEARCH_HINTS "${SHERPA_ONNX_ROOT}")
endif()

find_path(SMART_SPEAKER_SHERPA_INCLUDE_DIR
    NAMES sherpa-onnx/c-api/c-api.h
    HINTS ${SMART_SPEAKER_SHERPA_SEARCH_HINTS}
    PATH_SUFFIXES include)
find_library(SMART_SPEAKER_SHERPA_C_API_LIBRARY
    NAMES sherpa-onnx-c-api
    HINTS ${SMART_SPEAKER_SHERPA_SEARCH_HINTS}
    PATH_SUFFIXES lib lib64)
find_library(SMART_SPEAKER_ONNXRUNTIME_LIBRARY
    NAMES onnxruntime
    HINTS ${SMART_SPEAKER_SHERPA_SEARCH_HINTS}
    PATH_SUFFIXES lib lib64)

if(SMART_SPEAKER_SHERPA_INCLUDE_DIR AND SMART_SPEAKER_SHERPA_C_API_LIBRARY)
    target_include_directories(smart_voice_speaker PRIVATE "${SMART_SPEAKER_SHERPA_INCLUDE_DIR}")
    target_link_libraries(smart_voice_speaker PRIVATE "${SMART_SPEAKER_SHERPA_C_API_LIBRARY}")
    if(SMART_SPEAKER_ONNXRUNTIME_LIBRARY)
        target_link_libraries(smart_voice_speaker PRIVATE "${SMART_SPEAKER_ONNXRUNTIME_LIBRARY}")
    endif()
    target_compile_definitions(smart_voice_speaker PRIVATE SMART_SPEAKER_USE_SHERPA_ONNX_C_API=1)

    set(_smart_speaker_rpath
        "${CMAKE_CURRENT_LIST_DIR}/lib"
        "${CMAKE_CURRENT_LIST_DIR}/../voice_test/lib"
        "${CMAKE_CURRENT_LIST_DIR}/../newbot-master/tools/voice_test/lib"
        "$ORIGIN/../lib"
        "$ORIGIN/../../voice_test/lib")
    if(SHERPA_ONNX_ROOT)
        list(APPEND _smart_speaker_rpath
            "${SHERPA_ONNX_ROOT}/lib"
            "${SHERPA_ONNX_ROOT}/lib64")
    endif()
    set_target_properties(smart_voice_speaker PROPERTIES
        BUILD_RPATH "${_smart_speaker_rpath}"
        INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN/../../voice_test/lib")
    message(STATUS "smart_voice_speaker: sherpa-onnx C API enabled")
else()
    message(WARNING "sherpa-onnx C API not found: local wake word and ASR disabled. Pass -DSHERPA_ONNX_ROOT=/path.")
endif()
