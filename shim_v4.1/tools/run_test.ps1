# Fase 3 (Windows / PowerShell): copia al telefono y ejecuta la prueba aislada.
# Uso: .\tools\run_test.ps1 -Icd C:\ruta\libvulkan_panfrost.so
param([Parameter(Mandatory = $true)][string]$Icd)
$ErrorActionPreference = 'Continue'
if (-not (Test-Path $Icd)) { throw "No existe $Icd" }
Set-Location (Join-Path $PSScriptRoot '..')
$D = '/data/local/tmp/panvk'

adb shell "mkdir -p $D"
adb push build\libvulkan_panvk_shim.so "$D/"
adb push build\test_panvk_shim "$D/"
adb push $Icd "$D/libvulkan_panfrost.so"
adb shell "chmod 755 $D/test_panvk_shim"

adb logcat -c
$cmd = "cd $D && PANVK_SHIM_STDERR=1 PANVK_ICD_PATH=$D/libvulkan_panfrost.so LD_LIBRARY_PATH=$D ./test_panvk_shim $D/libvulkan_panvk_shim.so"
$out = adb shell $cmd 2>&1 | ForEach-Object { $_.ToString() }
$code = $LASTEXITCODE
$out | Tee-Object -FilePath test_output.log
"exit code: $code" | Tee-Object -FilePath test_output.log -Append
adb logcat -d -s PanVKShim:V '*:E' | Out-File -Encoding utf8 test_logcat.log
Write-Host "`nLogs guardados: test_output.log, test_logcat.log"
