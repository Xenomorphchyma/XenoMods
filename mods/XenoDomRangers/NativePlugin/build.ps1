param(
    [string]$RangersExe = $env:SRHD_RANGERS_EXE,
    [string]$Rangers1Exe = $env:SRHD_RANGERS1_EXE
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $RangersExe -and $env:SRHD_GAME_ROOT) {
    $RangersExe = Join-Path $env:SRHD_GAME_ROOT "Rangers.exe"
}
if (-not $Rangers1Exe -and $env:SRHD_GAME_ROOT) {
    $Rangers1Exe = Join-Path $env:SRHD_GAME_ROOT "Rangers1.exe"
}
$sourceRoot = Join-Path $projectRoot "src"
$includeRoot = Join-Path $projectRoot "include"
$testRoot = Join-Path $projectRoot "tests"
$buildRoot = Join-Path $projectRoot ".build"
$workspaceRoot = Split-Path $projectRoot -Parent
$mainNativeRoot = Join-Path $workspaceRoot "Native"
$testNativeRoot = Join-Path (Join-Path (Split-Path $workspaceRoot -Parent) "XenoDomRangersTest") "Native"
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
New-Item -ItemType Directory -Path $mainNativeRoot -Force | Out-Null
New-Item -ItemType Directory -Path $testNativeRoot -Force | Out-Null

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
        ("/Fo" + $buildRoot + "\\")
    )
    & $compiler.FullName @common `
        (Join-Path $sourceRoot "mask_policy.cpp") `
        (Join-Path $sourceRoot "runtime_scan.cpp") `
        (Join-Path $sourceRoot "xdr_config.cpp") `
        (Join-Path $testRoot "native_tests.cpp") `
        /Fe:(Join-Path $buildRoot "XenoDomRangersNativeTests.exe")
    if ($LASTEXITCODE -ne 0) {
        throw "Native test compilation failed."
    }

    $testArguments = @()
    foreach ($candidate in @($RangersExe, $Rangers1Exe)) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            $testArguments += $candidate
        }
    }
    & (Join-Path $buildRoot "XenoDomRangersNativeTests.exe") @testArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Native XenoDomRangers tests failed."
    }

    function Build-Plugin([string]$Name, [bool]$TestVariant) {
        $output = Join-Path $buildRoot ($Name + ".XenoPlugin.dll")
        $arguments = @($common)
        if ($TestVariant) { $arguments += "/DXDR_PLUGIN_TEST=1" }
        $arguments += "/LD"
        $arguments += (Join-Path $sourceRoot "mask_policy.cpp")
        $arguments += (Join-Path $sourceRoot "runtime_scan.cpp")
        $arguments += (Join-Path $sourceRoot "xdr_config.cpp")
        $arguments += (Join-Path $sourceRoot "xeno_dom_rangers.cpp")
        $arguments += ("/Fe:" + $output)
        $arguments += "/link"
        $arguments += ("/DEF:" + (Join-Path $sourceRoot "xeno_dom_rangers.def"))
        & $compiler.FullName @arguments | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw ($Name + " plugin compilation failed.")
        }
        return $output
    }

    $mainPlugin = Build-Plugin "XenoDomRangers" $false
    $testPlugin = Build-Plugin "XenoDomRangersTest" $true
    Copy-Item -LiteralPath $mainPlugin -Destination $mainNativeRoot -Force
    Copy-Item -LiteralPath $testPlugin -Destination $testNativeRoot -Force
    Get-Item -LiteralPath `
        (Join-Path $mainNativeRoot "XenoDomRangers.XenoPlugin.dll"), `
        (Join-Path $testNativeRoot "XenoDomRangersTest.XenoPlugin.dll") |
        Select-Object FullName, Length, LastWriteTime
}
finally {
    $env:INCLUDE = $savedInclude
    $env:LIB = $savedLib
}
