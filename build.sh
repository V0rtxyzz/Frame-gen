#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELEASE_DIR="${ROOT_DIR}/release"
mkdir -p "${RELEASE_DIR}" "${ROOT_DIR}/config"

echo "==> [1/4] Building & running host verification suite for all 13 modules..."
if [[ -d "/tmp/ref/preloader-android/include" && -d "/tmp/tools/json/include" && -d "/tmp/tools/fmt/include" ]]; then
    g++ -std=c++20 -O2 \
        -I"${ROOT_DIR}/include" \
        -I"/tmp/ref/preloader-android/include" \
        -I"/tmp/tools/json/include" \
        -I"/tmp/tools/fmt/include" \
        -DFMT_HEADER_ONLY \
        "${ROOT_DIR}/tests/test_hyper90.cpp" \
        "${ROOT_DIR}/src/Config.cpp" \
        "${ROOT_DIR}/src/Telemetry.cpp" \
        "${ROOT_DIR}/src/EngineOptionsTuner.cpp" \
        "${ROOT_DIR}/src/SystemOptimizers.cpp" \
        "${ROOT_DIR}/src/NativeHooks.cpp" \
        "${ROOT_DIR}/src/FrameBlend.cpp" \
        "${ROOT_DIR}/src/Modules.cpp" \
        -o "/tmp/test_hyper90"
    /tmp/test_hyper90
    /tmp/test_hyper90 --generate-config "${ROOT_DIR}/config"
fi

echo "==> [2/4] Compiling Android arm64-v8a shared library (libhyper90_fps_booster.so)..."
NDK_DIR="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -n "${NDK_DIR}" && -f "${NDK_DIR}/build/cmake/android.toolchain.cmake" ]]; then
    BUILD_DIR="/tmp/hyper90-cmake-android"
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -GNinja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="${NDK_DIR}/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-26 \
        -DANDROID_STL=c++_static
    cmake --build "${BUILD_DIR}" --target hyper90_fps_booster -j"$(nproc)"
    cp "${BUILD_DIR}/libhyper90_fps_booster.so" "${RELEASE_DIR}/libhyper90_fps_booster.so"
elif [[ -f "/tmp/tools/android-libc.txt" ]]; then
    export ZIG_LIBC="/tmp/tools/android-libc.txt"
    python3 -m ziglang c++ -target aarch64-linux-android \
        -D__ANDROID_API__=30 \
        -std=c++20 -Os -flto -shared -fPIC \
        -fvisibility=hidden -fvisibility-inlines-hidden \
        -ffunction-sections -fdata-sections \
        -isystem /tmp/tools/pwn-ndk-sysroot/include \
        -isystem /tmp/tools/pwn-ndk-sysroot/include/aarch64-linux-android \
        -I"${ROOT_DIR}/include" \
        -I"/tmp/ref/preloader-android/include" \
        -I"/tmp/tools/fmt/include" \
        -I"/tmp/tools/json/include" \
        -DFMT_HEADER_ONLY \
        "${ROOT_DIR}/src/Config.cpp" \
        "${ROOT_DIR}/src/Telemetry.cpp" \
        "${ROOT_DIR}/src/EngineOptionsTuner.cpp" \
        "${ROOT_DIR}/src/SystemOptimizers.cpp" \
        "${ROOT_DIR}/src/NativeHooks.cpp" \
        "${ROOT_DIR}/src/FrameBlend.cpp" \
        "${ROOT_DIR}/src/Modules.cpp" \
        "${ROOT_DIR}/src/Hyper90Mod.cpp" \
        -L/tmp/tools/pwn-ndk-sysroot/lib \
        -Wl,--no-as-needed \
        -lpreloader -llog -landroid -lEGL -lGLESv2 -lGLESv3 -lm -ldl -lbionic_c \
        -Wl,--version-script=/tmp/tools/exports.map \
        -Wl,--gc-sections \
        -Wl,-s \
        -Wl,--allow-shlib-undefined \
        -Wl,-z,max-page-size=16384 \
        -Wl,-soname,libhyper90_fps_booster.so \
        -o "${RELEASE_DIR}/libhyper90_fps_booster.so"
else
    echo "Error: Neither ANDROID_NDK_HOME nor /tmp/tools/android-libc.txt found." >&2
    exit 1
fi

echo "==> [3/4] Packaging Hyper90FPSBooster.levipack..."
STAGE_DIR="/tmp/hyper90-levipack-stage"
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}/config"

cp "${ROOT_DIR}/manifest.json" "${STAGE_DIR}/manifest.json"
cp "${RELEASE_DIR}/libhyper90_fps_booster.so" "${STAGE_DIR}/libhyper90_fps_booster.so"
cp "${ROOT_DIR}/config/config.json" "${STAGE_DIR}/config/config.json"
cp "${ROOT_DIR}/config/config.schema.json" "${STAGE_DIR}/config/config.schema.json"

python3 - <<'PY'
import os, zipfile

stage = "/tmp/hyper90-levipack-stage"
outputs = [
    os.path.abspath("Hyper90FPSBooster.levipack"),
    os.path.abspath("release/Hyper90FPSBooster.levipack"),
]

for out_path in outputs:
    if os.path.exists(out_path):
        os.remove(out_path)
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for root, _, files in os.walk(stage):
            for f in sorted(files):
                full_path = os.path.join(root, f)
                rel_path = os.path.relpath(full_path, stage).replace(os.sep, "/")
                zf.write(full_path, rel_path)
    print(f"Created {out_path} ({os.path.getsize(out_path)} bytes)")
PY

echo "==> [4/4] Verifying .levipack package & ELF64 AArch64 binary..."
readelf -h "${RELEASE_DIR}/libhyper90_fps_booster.so" | grep -E "Class:|Machine:|Type:"
nm -D --defined-only "${RELEASE_DIR}/libhyper90_fps_booster.so"
echo "Build complete!"
