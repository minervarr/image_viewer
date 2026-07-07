<#
.SYNOPSIS
  Builds libaom.a and libavif.a from the libs/aom and libs/libavif submodules
  and installs them into libs/prebuilt/<ABI>/, matching what app/src/main/cpp/CMakeLists.txt
  expects as IMPORTED static libraries.

.DESCRIPTION
  These libraries are not built as part of the normal Gradle/CMake app build -
  they're built once per ABI with the NDK toolchain and checked... actually not
  checked into git (see .gitignore) since they're reproducible from source.
  Run this script once after cloning (with submodules) and after any ABI/NDK change.

.PARAMETER Abis
  One or more Android ABIs to build for. Defaults to arm64-v8a (the only ABI
  this project has historically shipped).

.PARAMETER NdkVersion
  NDK version under $env:ANDROID_HOME/ndk/. Defaults to the latest installed.

.EXAMPLE
  ./scripts/build_native_libs.ps1
  ./scripts/build_native_libs.ps1 -Abis arm64-v8a,armeabi-v7a,x86_64
#>
param(
    [string[]]$Abis = @("arm64-v8a"),
    [string]$NdkVersion,
    [string]$AndroidPlatform = "android-26"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path "$PSScriptRoot/.."
$AomSrc = Join-Path $RepoRoot "libs/aom"
$AvifSrc = Join-Path $RepoRoot "libs/libavif"
$PrebuiltDir = Join-Path $RepoRoot "libs/prebuilt"

if (-not (Test-Path (Join-Path $AomSrc "CMakeLists.txt"))) {
    throw "libs/aom is empty. Run 'git submodule update --init --recursive' first."
}
if (-not (Test-Path (Join-Path $AvifSrc "CMakeLists.txt"))) {
    throw "libs/libavif is empty. Run 'git submodule update --init --recursive' first."
}

if (-not $env:ANDROID_HOME) {
    throw "ANDROID_HOME is not set."
}

if (-not $NdkVersion) {
    $NdkVersion = Get-ChildItem "$env:ANDROID_HOME/ndk" | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
}
$NdkDir = Join-Path $env:ANDROID_HOME "ndk/$NdkVersion"
$ToolchainFile = Join-Path $NdkDir "build/cmake/android.toolchain.cmake"
if (-not (Test-Path $ToolchainFile)) {
    throw "NDK toolchain file not found at $ToolchainFile"
}
$ClangAsm = Join-Path $NdkDir "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
if (-not (Test-Path $ClangAsm)) {
    throw "NDK clang (used as ASM compiler) not found at $ClangAsm"
}

Write-Host "Using NDK $NdkVersion at $NdkDir"

# libaom's build needs perl; Windows has no perl on PATH by default but Git for
# Windows ships one.
if (-not (Get-Command perl -ErrorAction SilentlyContinue)) {
    $GitPerlDir = "C:\Program Files\Git\usr\bin"
    if (Test-Path (Join-Path $GitPerlDir "perl.exe")) {
        $env:PATH = "$GitPerlDir;$env:PATH"
    } else {
        throw "perl not found on PATH and not found at $GitPerlDir. Install Perl or Git for Windows."
    }
}

foreach ($Abi in $Abis) {
    Write-Host "`n=== Building libaom for $Abi ==="
    $AomBuild = Join-Path $RepoRoot "build_aom_probe_$Abi"
    if (Test-Path $AomBuild) { Remove-Item -Recurse -Force $AomBuild }
    New-Item -ItemType Directory -Path $AomBuild | Out-Null

    cmake -S $AomSrc -B $AomBuild -GNinja `
        "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile" `
        "-DANDROID_ABI=$Abi" `
        "-DANDROID_PLATFORM=$AndroidPlatform" `
        "-DCMAKE_BUILD_TYPE=Release" `
        "-DCMAKE_ASM_COMPILER=$ClangAsm" `
        "-DCONFIG_AV1_ENCODER=1" `
        "-DCONFIG_AV1_DECODER=0" `
        "-DENABLE_TESTS=0" `
        "-DENABLE_EXAMPLES=0" `
        "-DENABLE_DOCS=0"
    if ($LASTEXITCODE -ne 0) { throw "aom cmake configure failed for $Abi" }

    cmake --build $AomBuild --target aom --parallel
    if ($LASTEXITCODE -ne 0) { throw "aom build failed for $Abi" }

    $LibAom = Join-Path $AomBuild "libaom.a"

    Write-Host "`n=== Building libavif for $Abi ==="
    $AvifBuild = Join-Path $RepoRoot "build_avif_probe_$Abi"
    if (Test-Path $AvifBuild) { Remove-Item -Recurse -Force $AvifBuild }
    New-Item -ItemType Directory -Path $AvifBuild | Out-Null

    cmake -S $AvifSrc -B $AvifBuild -GNinja `
        "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile" `
        "-DANDROID_ABI=$Abi" `
        "-DANDROID_PLATFORM=$AndroidPlatform" `
        "-DCMAKE_BUILD_TYPE=Release" `
        "-DAVIF_CODEC_AOM=SYSTEM" `
        "-DAVIF_CODEC_AOM_ENCODE=ON" `
        "-DAVIF_CODEC_AOM_DECODE=OFF" `
        "-DAOM_INCLUDE_DIR=$AomSrc" `
        "-DAOM_LIBRARY=$LibAom" `
        "-DAVIF_LIBYUV=OFF" `
        "-DAVIF_LIBSHARPYUV=OFF" `
        "-DAVIF_JPEG=OFF" `
        "-DAVIF_ZLIBPNG=OFF" `
        "-DAVIF_LIBXML2=OFF" `
        "-DAVIF_BUILD_APPS=OFF" `
        "-DAVIF_BUILD_EXAMPLES=OFF" `
        "-DAVIF_BUILD_TESTS=OFF" `
        "-DAVIF_BUILD_GDK_PIXBUF=OFF" `
        "-DAVIF_BUILD_MAN_PAGES=OFF"
    if ($LASTEXITCODE -ne 0) { throw "libavif cmake configure failed for $Abi" }

    cmake --build $AvifBuild --target avif_internal --parallel
    if ($LASTEXITCODE -ne 0) { throw "libavif build failed for $Abi" }

    $LibAvif = Join-Path $AvifBuild "libavif_internal.a"

    $OutDir = Join-Path $PrebuiltDir $Abi
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    Copy-Item $LibAom (Join-Path $OutDir "libaom.a") -Force
    Copy-Item $LibAvif (Join-Path $OutDir "libavif.a") -Force
    Write-Host "Installed $Abi libs to $OutDir"

    Remove-Item -Recurse -Force $AomBuild, $AvifBuild
}

Write-Host "`nDone. Prebuilt libs are in libs/prebuilt/."
