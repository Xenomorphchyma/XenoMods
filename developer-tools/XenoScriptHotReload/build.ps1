param(
    [string]$GameRoot = $env:SRHD_GAME_ROOT
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$nativeRoot = Join-Path $projectRoot "NativePlugin"
$sourceRoot = Join-Path $nativeRoot "src"
$includeRoot = Join-Path $nativeRoot "include"
$testRoot = Join-Path $nativeRoot "tests"
$buildRoot = Join-Path $nativeRoot ".build"
$modRoot = Join-Path $projectRoot "XenoScriptHotReload"
$modNativeRoot = Join-Path $modRoot "Native"
$releaseRoot = Join-Path $projectRoot "Build"
$zipPath = Join-Path $releaseRoot "XenoScriptHotReload-0.1.0.zip"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe was not found."
}
$compilerPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find "VC\Tools\MSVC\**\bin\Hostx64\x86\cl.exe" | Select-Object -First 1
if (-not $compilerPath) {
    throw "MSVC x86 compiler was not found."
}

$compiler = Get-Item -LiteralPath $compilerPath
$compilerDirectory = Split-Path $compiler.FullName -Parent
$vcToolsRoot = [IO.Path]::GetFullPath((Join-Path $compilerDirectory "..\..\.."))
$vcInclude = Join-Path $vcToolsRoot "include"
$vcLib = Join-Path $vcToolsRoot "lib\x86"
$windowsKitsRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVersion = Get-ChildItem -LiteralPath (Join-Path $windowsKitsRoot "Include") -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "um\Windows.h") } |
    Sort-Object Name -Descending |
    Select-Object -First 1 -ExpandProperty Name
if (-not $sdkVersion) {
    throw "Windows SDK was not found."
}
$sdkInclude = Join-Path $windowsKitsRoot ("Include\" + $sdkVersion)
$sdkLib = Join-Path $windowsKitsRoot ("Lib\" + $sdkVersion)
$savedInclude = $env:INCLUDE
$savedLib = $env:LIB

New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
New-Item -ItemType Directory -Path $modNativeRoot -Force | Out-Null
New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null

try {
    $env:INCLUDE = @(
        $vcInclude,
        (Join-Path $sdkInclude "ucrt"),
        (Join-Path $sdkInclude "shared"),
        (Join-Path $sdkInclude "um"),
        (Join-Path $sdkInclude "winrt")
    ) -join ";"
    $env:LIB = @(
        $vcLib,
        (Join-Path $sdkLib "ucrt\x86"),
        (Join-Path $sdkLib "um\x86")
    ) -join ";"

    $common = @(
        "/nologo", "/MT", "/O2", "/EHsc", "/std:c++17", "/W4",
        ("/I" + $includeRoot), ("/I" + $sourceRoot),
        ("/Fo" + $buildRoot + "\")
    )

    $testExe = Join-Path $buildRoot "XenoScriptHotReloadTests.exe"
    & $compiler.FullName @common `
        (Join-Path $sourceRoot "runtime_scan.cpp") `
        (Join-Path $sourceRoot "xeno_script_hot_reload.cpp") `
        (Join-Path $testRoot "native_tests.cpp") `
        /Fe:$testExe /link user32.lib
    if ($LASTEXITCODE -ne 0) { throw "Native test compilation failed." }

    $testArguments = @()
    if ($GameRoot -and (Test-Path -LiteralPath $GameRoot -PathType Container)) {
        $testArguments = Get-ChildItem -LiteralPath $GameRoot -Filter "Rangers*.exe" -File |
            Select-Object -ExpandProperty FullName
    }
    & $testExe @testArguments
    if ($LASTEXITCODE -ne 0) { throw "Native tests failed." }

    $pluginOutput = Join-Path $buildRoot "XenoScriptHotReload.XenoPlugin.dll"
    & $compiler.FullName @common /LD `
        (Join-Path $sourceRoot "runtime_scan.cpp") `
        (Join-Path $sourceRoot "xeno_script_hot_reload.cpp") `
        /Fe:$pluginOutput /link user32.lib `
        ("/DEF:" + (Join-Path $sourceRoot "xeno_script_hot_reload.def"))
    if ($LASTEXITCODE -ne 0) { throw "Plugin compilation failed." }

    Copy-Item -LiteralPath $pluginOutput -Destination $modNativeRoot -Force
    Copy-Item -LiteralPath `
        (Join-Path $nativeRoot "config\XenoScriptHotReload.XenoPlugin.ini") `
        -Destination $modNativeRoot -Force
    $moduleInfoText = Get-Content -LiteralPath `
        (Join-Path $projectRoot "ModuleInfo.source.txt") -Raw -Encoding UTF8
    [IO.File]::WriteAllText(
        (Join-Path $modRoot "ModuleInfo.txt"),
        $moduleInfoText,
        [Text.Encoding]::GetEncoding(1251))

    if (Test-Path -LiteralPath $zipPath -PathType Leaf) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -LiteralPath $modRoot -DestinationPath $zipPath -CompressionLevel Optimal

    Get-Item -LiteralPath $pluginOutput, $zipPath |
        Select-Object FullName, Length, LastWriteTime
}
finally {
    $env:INCLUDE = $savedInclude
    $env:LIB = $savedLib
}
