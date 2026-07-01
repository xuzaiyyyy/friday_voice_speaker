#!/usr/bin/env bash
set -u

ROOT_DIR="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
LIB_DIR="${ROOT_DIR}/lib"

echo "Project: ${ROOT_DIR}"
echo "ZLM lib dir: ${LIB_DIR}"
echo

for lib in libmk_api.so libZLToolKit.so libZLToolKit.a; do
    path="${LIB_DIR}/${lib}"
    if [ ! -f "${path}" ]; then
        echo "[missing] ${path}"
        continue
    fi

    echo "== ${lib} =="
    if [[ "${lib}" == *.so ]]; then
        echo "-- required dynamic libs --"
        ldd "${path}" 2>/dev/null || true
    else
        echo "-- static archive --"
        file "${path}" 2>/dev/null || true
    fi
    echo "-- required symbol versions --"
    strings "${path}" | grep -E 'GLIBC_|GLIBCXX_|OPENSSL_' | sort -Vu || true
    echo
done

cat <<'EOF'
Hint:
  If the output contains GLIBC_2.34, GLIBCXX_3.4.30, or OPENSSL_3.0.0
  on an older Orange Pi OS image, these ZLMediaKit libraries were built on
  a newer system and must be rebuilt on the Orange Pi or in a matching sysroot.
EOF
