#!/usr/bin/env bash
# Fase 2: compila shim + test para Android arm64 con el NDK.
set -euo pipefail
: "${ANDROID_NDK_HOME:?Define ANDROID_NDK_HOME con la ruta del NDK}"
API="${API:-28}"
cd "$(dirname "$0")/.."

cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$API" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

echo; echo "== Verificacion de binarios =="
RE="$(ls "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-readelf | head -1)"
NM="$(ls "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-nm | head -1)"
file build/libvulkan_panvk_shim.so build/test_panvk_shim || true
echo "--- simbolos exportados por el shim (esperado: vkGetInstanceProcAddr y vkGetDeviceProcAddr) ---"
"$NM" -D --defined-only build/libvulkan_panvk_shim.so
echo "--- dependencias (NEEDED) ---"
"$RE" -d build/libvulkan_panvk_shim.so | grep NEEDED
