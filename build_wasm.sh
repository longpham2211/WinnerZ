#!/usr/bin/env bash
# build_wasm.sh
# Build WinnerZ WASM module using Emscripten in WSL.
# Run this script from the repo root inside WSL.

set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${REPO_ROOT}/build-wasm"

# ── Auto-activate emsdk if available ──────────────────────────────────────────
EMSDK_ENV="${HOME}/emsdk/emsdk_env.sh"
if [ -f "${EMSDK_ENV}" ]; then
    echo "Activating emsdk..."
    source "${EMSDK_ENV}"
else
    echo " emsdk not found at ${EMSDK_ENV}"
    echo "   Install it first:"
    echo "     git clone https://github.com/emscripten-core/emsdk.git ~/emsdk"
    echo "     cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest"
    echo ""
    echo "   Then re-run this script."
    exit 1
fi

# ── Verify emcmake is available ───────────────────────────────────────────────
if ! command -v emcmake &> /dev/null; then
    echo " emcmake not found. Make sure emsdk is activated."
    exit 1
fi
echo "emcmake: $(emcmake --version 2>&1 | head -1)"

# ── Fix missing xz-utils in WSL ───────────────────────────────────────────────
if ! command -v xz &> /dev/null; then
    echo "Installing xz-utils (required for extracting .tar.xz)..."
    sudo apt-get update && sudo apt-get install -y xz-utils
fi

# ── Configure ─────────────────────────────────────────────────────────────────
echo ""
echo "Configuring WASM build..."
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Tự động dọn dẹp cache cũ nếu có để tránh lỗi "does not match the source"
rm -rf CMakeCache.txt CMakeFiles/

emcmake cmake "${REPO_ROOT}/wrapper_wasm" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_CXX_FLAGS="-O3" \
    -DCMAKE_EXE_LINKER_FLAGS="-O3 --no-entry -s STACK_SIZE=5242880 -s ALLOW_MEMORY_GROWTH=1 -s DISABLE_EXCEPTION_CATCHING=0 -s EXPORT_ES6=1 -s MODULARIZE=1"

# ── Build ─────────────────────────────────────────────────────────────────────
echo ""
echo "Building winnerz_wasm target..."
JOBS=$(nproc 2>/dev/null || echo 4)
emmake make winnerz_wasm -j${JOBS}

# ── Check output ──────────────────────────────────────────────────────────────
JS_OUT="${REPO_ROOT}/winnerz/js/winnerz_wasm.js"
WASM_OUT="${REPO_ROOT}/winnerz/js/winnerz_wasm.wasm"

echo ""
if [ -f "${JS_OUT}" ] && [ -f "${WASM_OUT}" ]; then
    JS_SIZE=$(du -sh "${JS_OUT}" | cut -f1)
    WASM_SIZE=$(du -sh "${WASM_OUT}" | cut -f1)
    echo "Build successful!"
    echo " winnerz_wasm.js        : ${JS_SIZE}"
    echo " winnerz_wasm.wasm      : ${WASM_SIZE}"
    echo ""
    echo "Output: ${REPO_ROOT}/winnerz/js/"
    echo ""
else
    echo "Build FAILED – output files not found!"
    echo "   Expected:"
    echo "     ${JS_OUT}"
    echo "     ${WASM_OUT}"
    exit 1
fi