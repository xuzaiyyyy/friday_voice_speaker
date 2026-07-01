#!/usr/bin/env bash
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 /path/to/zlmediakit/build-or-install-dir" >&2
    exit 2
fi

SRC_ROOT="$1"
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST_DIR="${PROJECT_ROOT}/lib"
HEADER_DEST_DIR="${PROJECT_ROOT}/include/ZLM"

if [ ! -d "${SRC_ROOT}" ]; then
    echo "source directory not found: ${SRC_ROOT}" >&2
    exit 1
fi

mk_api="$(find "${SRC_ROOT}" -name libmk_api.so -type f -print -quit)"
toolkit_so="$(find "${SRC_ROOT}" -name libZLToolKit.so -type f -print -quit)"
toolkit_a="$(find "${SRC_ROOT}" -name libZLToolKit.a -type f -print -quit)"

if [ -z "${mk_api}" ]; then
    echo "could not find libmk_api.so under ${SRC_ROOT}" >&2
    exit 1
fi

if [ -z "${toolkit_so}" ] && [ -z "${toolkit_a}" ]; then
    echo "could not find libZLToolKit.so or libZLToolKit.a under ${SRC_ROOT}" >&2
    exit 1
fi

mkdir -p "${DEST_DIR}"
install -m 755 "${mk_api}" "${DEST_DIR}/libmk_api.so"
if [ -n "${toolkit_so}" ]; then
    install -m 755 "${toolkit_so}" "${DEST_DIR}/libZLToolKit.so"
fi
if [ -n "${toolkit_a}" ]; then
    install -m 644 "${toolkit_a}" "${DEST_DIR}/libZLToolKit.a"
fi

echo "installed:"
echo "  ${DEST_DIR}/libmk_api.so"
if [ -n "${toolkit_so}" ]; then
    echo "  ${DEST_DIR}/libZLToolKit.so"
fi
if [ -n "${toolkit_a}" ]; then
    echo "  ${DEST_DIR}/libZLToolKit.a"
fi

header_root=""
for candidate in \
    "${SRC_ROOT}/api/include" \
    "${SRC_ROOT}/../api/include" \
    "${SRC_ROOT}/../../api/include" \
    "${SRC_ROOT}/../../../api/include"; do
    if [ -d "${candidate}" ] && ls "${candidate}"/mk_*.h >/dev/null 2>&1; then
        header_root="${candidate}"
        break
    fi
done

generated_export=""
for candidate in \
    "${SRC_ROOT}/api/mk_export.h" \
    "${SRC_ROOT}/../api/mk_export.h" \
    "${SRC_ROOT}/../../api/mk_export.h" \
    "${SRC_ROOT}/../../../api/mk_export.h"; do
    if [ -f "${candidate}" ]; then
        generated_export="${candidate}"
        break
    fi
done

if [ -z "${header_root}" ]; then
    header_root="$(find "${SRC_ROOT}" -path "*/api/include" -type d -print -quit)"
fi

if [ -z "${generated_export}" ]; then
    generated_export="$(find "${SRC_ROOT}" -name mk_export.h -type f -print -quit)"
fi

if [ -n "${header_root}" ]; then
    mkdir -p "${HEADER_DEST_DIR}"
    install -m 644 "${header_root}"/mk_*.h "${HEADER_DEST_DIR}/"
    if [ -n "${generated_export}" ]; then
        install -m 644 "${generated_export}" "${HEADER_DEST_DIR}/mk_export.h"
    fi
    echo "  ${HEADER_DEST_DIR}/mk_*.h"
else
    echo "warning: ZLMediaKit C API headers were not found under ${SRC_ROOT}; keeping existing headers"
fi

echo
bash "${PROJECT_ROOT}/tools/check_zlm_abi.sh" "${PROJECT_ROOT}"
