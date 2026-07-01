#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK_ROOT="${ZLM_WORK_ROOT:-${HOME}/src/xiaoman_zlmediakit}"
SRC_DIR="${ZLM_SOURCE_DIR:-${WORK_ROOT}/ZLMediaKit}"
BUILD_DIR="${ZLM_BUILD_DIR:-${SRC_DIR}/build_xiaoman}"
REPO_URL="${ZLM_REPO_URL:-https://github.com/ZLMediaKit/ZLMediaKit.git}"
TOOLKIT_REPO_URL="${ZLM_TOOLKIT_REPO_URL:-https://gitee.com/xia-chu/ZLToolKit.git}"
JSONCPP_REPO_URL="${ZLM_JSONCPP_REPO_URL:-https://gitee.com/mirrors/jsoncpp.git}"
MEDIA_SERVER_REPO_URL="${ZLM_MEDIA_SERVER_REPO_URL:-https://gitee.com/ireader/media-server.git}"
JOBS="${JOBS:-1}"
ENABLE_SCTP="${ZLM_ENABLE_SCTP:-OFF}"
ENABLE_HLS="${ZLM_ENABLE_HLS:-OFF}"
ENABLE_MP4="${ZLM_ENABLE_MP4:-OFF}"
ENABLE_RTPPROXY="${ZLM_ENABLE_RTPPROXY:-ON}"
ENABLE_SRT="${ZLM_ENABLE_SRT:-OFF}"
REPLACE_INCOMPLETE_DEPS="${ZLM_REPLACE_INCOMPLETE_DEPS:-ON}"
DISABLE_AV1="${ZLM_DISABLE_AV1:-ON}"

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing command: $1" >&2
        echo "install deps first, for Ubuntu/Orange Pi OS:" >&2
        echo "  sudo apt update" >&2
        echo "  sudo apt install -y git cmake g++ make pkg-config libssl-dev libsrtp2-dev zlib1g-dev" >&2
        echo "  optional for WebRTC datachannel only: sudo apt install -y libusrsctp-dev" >&2
        exit 1
    fi
}

need_cmd git
need_cmd cmake
need_cmd make

clone_if_missing() {
    local name="$1"
    local dir="$2"
    local marker="$3"
    local repo="$4"

    if [ -f "${dir}/${marker}" ]; then
        return 0
    fi

    echo "[ZLM] missing ${name}, trying to recover it"
    if [ -d "${SRC_DIR}/.git" ]; then
        git -C "${SRC_DIR}" submodule update --init --recursive --depth 1 || \
            git -C "${SRC_DIR}" submodule update --init --recursive
    fi

    if [ -f "${dir}/${marker}" ]; then
        return 0
    fi

    if [ ! -e "${dir}" ] || [ -z "$(find "${dir}" -mindepth 1 -print -quit 2>/dev/null)" ]; then
        mkdir -p "$(dirname "${dir}")"
        echo "[ZLM] cloning ${name} to ${dir}"
        git clone --recursive --depth 1 "${repo}" "${dir}"
    else
        if [ "${REPLACE_INCOMPLETE_DEPS}" = "ON" ]; then
            local backup="${dir}.bad.$(date +%Y%m%d_%H%M%S).$$"
            echo "[ZLM] ${dir} exists but does not contain ${marker}"
            echo "[ZLM] moving incomplete ${name} to ${backup}"
            mv "${dir}" "${backup}"
            mkdir -p "$(dirname "${dir}")"
            echo "[ZLM] cloning ${name} to ${dir}"
            git clone --recursive --depth 1 "${repo}" "${dir}"
        else
            echo "[ZLM] ${dir} exists but does not contain ${marker}" >&2
            echo "[ZLM] move that directory away, or use a fresh recursive clone:" >&2
            echo "  git clone --recursive --depth 1 ${REPO_URL}" >&2
            echo "[ZLM] or rerun with ZLM_REPLACE_INCOMPLETE_DEPS=ON to backup and replace it automatically" >&2
            exit 1
        fi
    fi

    if [ ! -f "${dir}/${marker}" ]; then
        echo "[ZLM] ${name} was cloned, but ${marker} is still missing" >&2
        echo "[ZLM] repository may be incomplete or incompatible: ${repo}" >&2
        exit 1
    fi
}

mkdir -p "${WORK_ROOT}"

if [ ! -d "${SRC_DIR}" ]; then
    echo "[ZLM] cloning source to ${SRC_DIR}"
    git clone --recursive --depth 1 "${REPO_URL}" "${SRC_DIR}"
else
    echo "[ZLM] using existing source: ${SRC_DIR}"
fi

if [ -d "${SRC_DIR}/.git" ]; then
    echo "[ZLM] updating submodules"
    git -C "${SRC_DIR}" submodule update --init --recursive --depth 1 || \
        git -C "${SRC_DIR}" submodule update --init --recursive
else
    echo "[ZLM] source is not a git checkout, skipping submodule update"
fi

clone_if_missing "ZLToolKit" "${SRC_DIR}/3rdpart/ZLToolKit" "CMakeLists.txt" "${TOOLKIT_REPO_URL}"
clone_if_missing "jsoncpp" "${SRC_DIR}/3rdpart/jsoncpp" "src/lib_json/json_reader.cpp" "${JSONCPP_REPO_URL}"
clone_if_missing "media-server" "${SRC_DIR}/3rdpart/media-server" "libmov/source/mov-reader.c" "${MEDIA_SERVER_REPO_URL}"
clone_if_missing "media-server" "${SRC_DIR}/3rdpart/media-server" "libriff/include/riff-acm.h" "${MEDIA_SERVER_REPO_URL}"

MEDIA_SERVER_INCLUDE_FLAGS=""
for include_dir in \
    "${SRC_DIR}/3rdpart/media-server/libflv/include" \
    "${SRC_DIR}/3rdpart/media-server/libmov/include" \
    "${SRC_DIR}/3rdpart/media-server/libmpeg/include" \
    "${SRC_DIR}/3rdpart/media-server/libriff/include" \
    "${SRC_DIR}/3rdpart/media-server/librtp/include"; do
    if [ -d "${include_dir}" ]; then
        MEDIA_SERVER_INCLUDE_FLAGS="${MEDIA_SERVER_INCLUDE_FLAGS} -I${include_dir}"
    fi
done

if [ -d "${BUILD_DIR}" ]; then
    echo "[ZLM] reusing build directory for incremental build: ${BUILD_DIR}"
fi

if [ "${DISABLE_AV1}" = "ON" ] && [ -d "${SRC_DIR}/ext-codec" ]; then
    echo "[ZLM] disabling unused AV1 ext-codec"
    for av1_file in "${SRC_DIR}"/ext-codec/AV1*; do
        if [ ! -e "${av1_file}" ]; then
            continue
        fi
        case "${av1_file}" in
            *.disabled)
                ;;
            *)
                mv "${av1_file}" "${av1_file}.disabled"
                ;;
        esac
    done
    if [ -d "${BUILD_DIR}/ext-codec" ]; then
        echo "[ZLM] removing stale ext-codec build directory"
        rm -rf "${BUILD_DIR}/ext-codec"
    fi
    if [ -d "${BUILD_DIR}/CMakeFiles" ]; then
        for av1_dep in "${BUILD_DIR}"/CMakeFiles/*/AV1*; do
            if [ -e "${av1_dep}" ]; then
                rm -rf "${av1_dep}"
            fi
        done
    fi
    if [ -f "${BUILD_DIR}/CMakeCache.txt" ]; then
        for av1_rule in "${BUILD_DIR}"/CMakeFiles/Makefile2 "${BUILD_DIR}"/Makefile; do
            if [ -f "${av1_rule}" ] && grep -q "AV1Rtp\\|AV1.cpp" "${av1_rule}" 2>/dev/null; then
                echo "[ZLM] AV1 stale rules may remain in ${av1_rule}; cmake will regenerate them"
            fi
        done
    fi
fi

echo "[ZLM] configuring"
echo "[ZLM] build jobs: ${JOBS}"
cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG${MEDIA_SERVER_INCLUDE_FLAGS}" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG${MEDIA_SERVER_INCLUDE_FLAGS}" \
    -DENABLE_API=ON \
    -DENABLE_OPENSSL=ON \
    -DENABLE_WEBRTC=ON \
    -DENABLE_SCTP="${ENABLE_SCTP}" \
    -DENABLE_HLS="${ENABLE_HLS}" \
    -DENABLE_MP4="${ENABLE_MP4}" \
    -DENABLE_RTPPROXY="${ENABLE_RTPPROXY}" \
    -DENABLE_SRT="${ENABLE_SRT}" \
    -DENABLE_TESTS=OFF \
    -DENABLE_PLAYER=OFF

echo "[ZLM] building C API"
if ! cmake --build "${BUILD_DIR}" --target mk_api -j "${JOBS}"; then
    echo "[ZLM] target mk_api was not available, building default target"
    cmake --build "${BUILD_DIR}" -j "${JOBS}"
fi

LIB_ROOT=""
for candidate in \
    "${SRC_DIR}/release/linux/Release" \
    "${BUILD_DIR}/release/linux/Release" \
    "${BUILD_DIR}/api" \
    "${BUILD_DIR}"; do
    if [ -f "${candidate}/libmk_api.so" ] && [ -f "${candidate}/libZLToolKit.so" ]; then
        LIB_ROOT="${candidate}"
        break
    fi
done

if [ -z "${LIB_ROOT}" ]; then
    mk_api="$(find "${SRC_DIR}" -name libmk_api.so -type f -print -quit)"
    toolkit="$(find "${SRC_DIR}" -name libZLToolKit.so -type f -print -quit)"
    if [ -n "${mk_api}" ] && [ -n "${toolkit}" ]; then
        LIB_ROOT="$(dirname "${mk_api}")"
    fi
fi

if [ -z "${LIB_ROOT}" ]; then
    echo "[ZLM] build finished, but libmk_api.so/libZLToolKit.so were not found" >&2
    exit 1
fi

echo "[ZLM] installing from ${LIB_ROOT}"
bash "${PROJECT_ROOT}/tools/install_zlm_libs.sh" "${SRC_DIR}"

echo
echo "[ZLM] done. Now rebuild edge_media_pipeline:"
echo "  rm -rf \"${PROJECT_ROOT}/build\""
echo "  cmake -S \"${PROJECT_ROOT}\" -B \"${PROJECT_ROOT}/build\""
echo "  cmake --build \"${PROJECT_ROOT}/build\" -j${JOBS}"
