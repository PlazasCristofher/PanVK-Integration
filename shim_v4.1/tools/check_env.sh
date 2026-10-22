#!/usr/bin/env bash
# Fase 0: comprueba que tienes en el ordenador. No instala nada.
echo "== Sistema =="; uname -a
echo; echo "== Herramientas =="
for t in adb cmake ninja make clang python3 git unzip; do
  if command -v "$t" >/dev/null 2>&1; then printf "%-8s OK   %s\n" "$t" "$(command -v $t)"
  else printf "%-8s FALTA\n" "$t"; fi
done
echo; echo "== NDK =="
echo "ANDROID_NDK_HOME=${ANDROID_NDK_HOME:-<sin definir>}"
echo "ANDROID_HOME=${ANDROID_HOME:-<sin definir>}  ANDROID_SDK_ROOT=${ANDROID_SDK_ROOT:-<sin definir>}"
for d in "$ANDROID_NDK_HOME" "$ANDROID_HOME/ndk" "$ANDROID_SDK_ROOT/ndk" "$HOME/Android/Sdk/ndk" "$HOME/Library/Android/sdk/ndk"; do
  [ -n "$d" ] && [ -d "$d" ] && { echo "encontrado: $d"; ls "$d"; }
done
if [ -n "$ANDROID_NDK_HOME" ] && [ -d "$ANDROID_NDK_HOME" ]; then
  cat "$ANDROID_NDK_HOME/source.properties" 2>/dev/null | head -3
  ls "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android28-clang 2>&1
  ls "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/sysroot/usr/include/vulkan/vulkan.h 2>&1
fi
echo; echo "== Telefono (adb) =="
adb devices 2>&1
adb shell getprop ro.product.model 2>&1
adb shell getprop ro.build.version.release 2>&1
adb shell getprop ro.product.cpu.abi 2>&1
adb shell ls -l /dev/mali0 2>&1
