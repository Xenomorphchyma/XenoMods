param()

$ErrorActionPreference = "Stop"
$pluginRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceRoot = Join-Path $pluginRoot "src"
$includeRoot = Join-Path $pluginRoot "include"
$testRoot = Join-Path $pluginRoot "tests"
$buildRoot = Join-Path $pluginRoot ".build"
$mainRoot = Split-Path $pluginRoot -Parent
$workspaceRoot = Split-Path $mainRoot -Parent
$testModRoot = Join-Path $workspaceRoot "XenoCoalitionSupplyLinesEarthTest"
$mainNativeRoot = Join-Path $mainRoot "Native"
$testNativeRoot = Join-Path $testModRoot "Native"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw "vswhere.exe was not found." }
$compilerPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find "VC\Tools\MSVC\**\bin\Hostx64\x86\cl.exe" | Select-Object -First 1
if (-not $compilerPath) { throw "MSVC x86 compiler was not found." }
$compiler = Get-Item -LiteralPath $compilerPath
$compilerDirectory = Split-Path $compiler.FullName -Parent
$vcToolsRoot = [IO.Path]::GetFullPath((Join-Path $compilerDirectory "..\..\.."))
$windowsKitsRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVersion = Get-ChildItem -LiteralPath (Join-Path $windowsKitsRoot "Include") -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "um\Windows.h") } |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
if (-not $sdkVersion) { throw "Windows SDK was not found." }

$savedInclude = $env:INCLUDE
$savedLib = $env:LIB
New-Item -ItemType Directory -Force -Path $buildRoot,$mainNativeRoot,$testNativeRoot | Out-Null
try {
    $env:INCLUDE = @(
        (Join-Path $vcToolsRoot "include"),
        (Join-Path $windowsKitsRoot "Include\$sdkVersion\ucrt"),
        (Join-Path $windowsKitsRoot "Include\$sdkVersion\shared"),
        (Join-Path $windowsKitsRoot "Include\$sdkVersion\um"),
        (Join-Path $windowsKitsRoot "Include\$sdkVersion\winrt")
    ) -join ";"
    $env:LIB = @(
        (Join-Path $vcToolsRoot "lib\x86"),
        (Join-Path $windowsKitsRoot "Lib\$sdkVersion\ucrt\x86"),
        (Join-Path $windowsKitsRoot "Lib\$sdkVersion\um\x86")
    ) -join ";"

    $common = @("/nologo","/MT","/O2","/EHsc","/std:c++17","/W4",("/I" + $includeRoot),("/Fo" + $buildRoot + "\"))
    & $compiler.FullName @common `
        (Join-Path $testRoot "config_tests.cpp") `
        (Join-Path $sourceRoot "xeno_coalition_supply_lines_config.cpp") `
        /Fe:(Join-Path $buildRoot "CSLConfigTests.exe")
    if ($LASTEXITCODE -ne 0) { throw "CSL config test compilation failed." }
    & (Join-Path $buildRoot "CSLConfigTests.exe")
    if ($LASTEXITCODE -ne 0) { throw "CSL config tests failed." }

    function Build-Plugin([string]$Name, [bool]$TestVariant) {
        $output = Join-Path $buildRoot ($Name + ".XenoPlugin.dll")
        $arguments = @($common)
        if ($TestVariant) { $arguments += "/DCSL_PLUGIN_TEST=1" }
        $arguments += "/LD"
        $arguments += (Join-Path $sourceRoot "xeno_coalition_supply_lines_config.cpp")
        $arguments += ("/Fe:" + $output)
        $arguments += "/link"
        $arguments += ("/DEF:" + (Join-Path $sourceRoot "xeno_coalition_supply_lines_config.def"))
        & $compiler.FullName @arguments | Out-Host
        if ($LASTEXITCODE -ne 0) { throw ($Name + " plugin compilation failed.") }
        return $output
    }

    $mainPlugin = Build-Plugin "XenoCoalitionSupplyLines" $false
    $testPlugin = Build-Plugin "XenoCoalitionSupplyLinesEarthTest" $true
    Copy-Item -LiteralPath $mainPlugin -Destination $mainNativeRoot -Force
    Copy-Item -LiteralPath $testPlugin -Destination $testNativeRoot -Force
}
finally {
    $env:INCLUDE = $savedInclude
    $env:LIB = $savedLib
}
