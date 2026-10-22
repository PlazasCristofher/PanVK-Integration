# Fase 0 (Windows / PowerShell): comprueba que tienes. No instala nada.
Write-Host "== Sistema =="
[System.Environment]::OSVersion.VersionString
$PSVersionTable.PSVersion.ToString()

Write-Host "`n== Herramientas en PATH =="
foreach ($t in 'java','adb','cmake','ninja','git') {
    $c = Get-Command $t -ErrorAction SilentlyContinue
    if ($c) { "{0,-8} OK    {1}" -f $t, $c.Source } else { "{0,-8} FALTA" -f $t }
}
Write-Host "`n== Java (lo necesita sdkmanager) =="
try { & java -version 2>&1 | ForEach-Object { $_.ToString() } } catch { "java no encontrado" }

Write-Host "`n== Variables =="
"ANDROID_HOME      = $env:ANDROID_HOME"
"ANDROID_SDK_ROOT  = $env:ANDROID_SDK_ROOT"
"ANDROID_NDK_HOME  = $env:ANDROID_NDK_HOME"

Write-Host "`n== sdkmanager =="
$sm = "C:\Android\cmdline-tools\latest\bin\sdkmanager.bat"
"Test-Path $sm -> $(Test-Path $sm)"

Write-Host "`n== SDK / NDK / CMake instalados =="
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { "C:\Android" }
foreach ($d in 'platform-tools','platforms','ndk','cmake') {
    $p = Join-Path $sdk $d
    if (Test-Path $p) { "{0}: {1}" -f $d, ((Get-ChildItem $p -Directory | Select-Object -Expand Name) -join ', ') }
    else { "{0}: no existe" -f $d }
}

Write-Host "`n== Telefono (adb) =="
if (Get-Command adb -ErrorAction SilentlyContinue) {
    adb devices
    adb shell getprop ro.product.model
    adb shell getprop ro.build.version.release
    adb shell getprop ro.product.cpu.abi
    adb shell ls -l /dev/mali0
} else { "adb no esta en PATH (se instala con platform-tools)" }
