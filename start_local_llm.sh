#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CONFIG_FILE="${XIAOMAN_CONFIG_FILE:-${NEWBOT_CONFIG_FILE:-}}"
CONFIG_EXPLICIT=0
if [ -n "${CONFIG_FILE}" ]; then
    CONFIG_EXPLICIT=1
else
    for candidate in \
        "${ROOT_DIR}/friday_voice_speaker.conf" \
        "${ROOT_DIR}/config/friday_voice_speaker.conf"; do
        if [ -f "${candidate}" ]; then
            CONFIG_FILE="${candidate}"
            break
        fi
    done
fi

if [ -n "${CONFIG_FILE}" ]; then
    if [ ! -f "${CONFIG_FILE}" ]; then
        if [ "${CONFIG_EXPLICIT}" -eq 1 ]; then
            echo "config file not found: ${CONFIG_FILE}" >&2
            exit 1
        fi
    else
        set -a
        # shellcheck source=/dev/null
        . "${CONFIG_FILE}"
        set +a
    fi
fi

resolve_config_path() {
    local value="$1"
    local base_dir
    if [ -z "${value}" ]; then
        return 0
    fi
    case "${value}" in
        /*) printf '%s\n' "${value}" ;;
        ~/*) printf '%s/%s\n' "${HOME}" "${value#~/}" ;;
        *)
            base_dir="$(cd "$(dirname "${CONFIG_FILE:-${ROOT_DIR}/friday_voice_speaker.conf}")" && pwd)"
            printf '%s/%s\n' "${base_dir}" "${value}"
            ;;
    esac
}

if [ -n "${NEWBOT_LOCAL_LLM_MODEL_PATH:-}" ]; then
    NEWBOT_LOCAL_LLM_MODEL_PATH="$(resolve_config_path "${NEWBOT_LOCAL_LLM_MODEL_PATH}")"
fi
if [ -n "${NEWBOT_LLAMA_SERVER:-}" ] && [[ "${NEWBOT_LLAMA_SERVER}" == */* ]]; then
    NEWBOT_LLAMA_SERVER="$(resolve_config_path "${NEWBOT_LLAMA_SERVER}")"
fi

MODEL_PATH="${NEWBOT_LOCAL_LLM_MODEL_PATH:-${ROOT_DIR}/models/Qwen_Qwen3-0.6B-Q4_0.gguf}"
HOST="${NEWBOT_LOCAL_LLM_HOST:-127.0.0.1}"
PORT="${NEWBOT_LOCAL_LLM_PORT:-8080}"
CTX_SIZE="${NEWBOT_LOCAL_LLM_CONTEXT:-1024}"
THREADS="${NEWBOT_LOCAL_LLM_THREADS:-4}"
LLAMA_SERVER="${NEWBOT_LLAMA_SERVER:-llama-server}"

if ! command -v "${LLAMA_SERVER}" >/dev/null 2>&1; then
    for candidate in \
        "${HOME}/llama.cpp/build/bin/llama-server" \
        "${HOME}/llama.cpp_install/bin/llama-server" \
        "${HOME}/cpp/llama.cpp/build/bin/llama-server" \
        "${HOME}/cpp/llama.cpp_install/bin/llama-server" \
        "/home/orangepi/llama.cpp/build/bin/llama-server" \
        "/home/orangepi/llama.cpp_install/bin/llama-server"; do
        if [ -x "${candidate}" ]; then
            LLAMA_SERVER="${candidate}"
            break
        fi
    done
fi

if [ ! -x "${LLAMA_SERVER}" ] && ! command -v "${LLAMA_SERVER}" >/dev/null 2>&1; then
    echo "cannot find llama-server; set NEWBOT_LLAMA_SERVER=/path/to/llama-server" >&2
    exit 1
fi

if [ ! -f "${MODEL_PATH}" ]; then
    echo "model not found: ${MODEL_PATH}" >&2
    echo "set NEWBOT_LOCAL_LLM_MODEL_PATH=/path/to/Qwen_Qwen3-0.6B-Q4_0.gguf" >&2
    exit 1
fi

exec "${LLAMA_SERVER}" \
    -m "${MODEL_PATH}" \
    --host "${HOST}" \
    --port "${PORT}" \
    -c "${CTX_SIZE}" \
    -t "${THREADS}" \
    -ngl 0
