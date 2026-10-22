#!/usr/bin/env bash
# Fase 3: copia al telefono y ejecuta la prueba aislada.
# Uso: tools/run_test.sh /ruta/a/libvulkan_panfrost.so
set -euo pipefail
ICD="${1:?Pasa la ruta del libvulkan_panfrost.so original (sin modificar)}"
cd "$(dirname "$0")/.."
D=/data/local/tmp/panvk

adb shell "mkdir -p $D"
adb push build/libvulkan_panvk_shim.so build/test_panvk_shim "$D/"
adb push "$ICD" "$D/libvulkan_panfrost.so"
adb shell "chmod 755 $D/test_panvk_shim"

adb logcat -c
set +e
adb shell "cd $D && PANVK_SHIM_STDERR=1 PANVK_ICD_PATH=$D/libvulkan_panfrost.so LD_LIBRARY_PATH=$D ./test_panvk_shim $D/libvulkan_panvk_shim.so" \
  2>&1 | tee test_output.log
echo "exit code: ${PIPESTATUS[0]}" | tee -a test_output.log
adb logcat -d -s PanVKShim:V '*:E' > test_logcat.log
echo; echo "Logs guardados: test_output.log, test_logcat.log"
