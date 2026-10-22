# Fase 2 (Windows / PowerShell): compila shim + test para Android arm64.
# Requiere ANDROID_NDK_HOME y el paquete cmake instalado con sdkmanager.
$ErrorActionPreference = 'Stop'
if (-not $env:ANDROID_NDK_HOME) { throw 'Define $env:ANDROID_NDK_HOME con la ruta del NDK' }
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { 'C:\Android' }

$cmakeDir = Get-ChildItem (Join-Path $sdk 'cmake') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $cmakeDir) { throw "No hay cmake en $sdk\cmake (instala con sdkmanager 'cmake;<version>')" }
$cmake = Join-Path $cmakeDir.FullName 'bin\cmake.exe'
$ninja = Join-Path $cmakeDir.FullName 'bin\ninja.exe'
$api = if ($env:API) { $env:API } else { '28' }

Set-Location (Join-Path $PSScriptRoot '..')

& $cmake -S . -B build -G Ninja -DPANVK_SHIM_DIAG=1 "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$env:ANDROID_NDK_HOME\build\cmake\android.toolchain.cmake" `
    -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$api" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'Fallo cmake (configure)' }
& $cmake --build build
if ($LASTEXITCODE -ne 0) { throw 'Fallo cmake (build)' }

Write-Host "`n== Verificacion de binarios =="
$bin = "$env:ANDROID_NDK_HOME\toolchains\llvm\prebuilt\windows-x86_64\bin"
Write-Host '--- simbolos exportados por el shim (esperado: vkGetInstanceProcAddr y vkGetDeviceProcAddr) ---'
& "$bin\llvm-nm.exe" -D --defined-only build\libvulkan_panvk_shim.so
Write-Host '--- dependencias (NEEDED) ---'
& "$bin\llvm-readelf.exe" -d build\libvulkan_panvk_shim.so | Select-String NEEDED
& "$bin\llvm-readelf.exe" -h build\libvulkan_panvk_shim.so | Select-String 'Class|Machine'
