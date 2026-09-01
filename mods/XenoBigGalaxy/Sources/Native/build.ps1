param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$RangersExe = $env:SRHD_RANGERS_EXE,
    [string]$Rangers1Exe = $env:SRHD_RANGERS1_EXE
)

$ErrorActionPreference = "Stop"
$nativeRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $RangersExe -and $env:SRHD_GAME_ROOT) {
    $RangersExe = Join-Path $env:SRHD_GAME_ROOT "Rangers.exe"
}
if (-not $Rangers1Exe -and $env:SRHD_GAME_ROOT) {
    $Rangers1Exe = Join-Path $env:SRHD_GAME_ROOT "Rangers1.exe"
}
$generatorRoot = Join-Path $nativeRoot "GalaxyGenerator"
$nextDayRoot = Join-Path $nativeRoot "NextDay"
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("XenoBigGalaxy-Native-" + [Guid]::NewGuid().ToString("N"))
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe was not found. Install the MSVC x86 build tools."
}
$compilerPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find "VC\Tools\MSVC\**\bin\Hostx64\x86\cl.exe" | Select-Object -First 1
if (-not $compilerPath) {
    throw "MSVC x86 compiler was not found."
}
$compiler = (Get-Item -LiteralPath $compilerPath).FullName
$compilerDirectory = Split-Path $compiler -Parent
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

function Invoke-Compiler {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    & $compiler @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "MSVC failed with exit code $LASTEXITCODE."
    }
}

New-Item -ItemType Directory -Path $temporaryRoot, $OutputDirectory -Force | Out-Null

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

    $testExecutables = @()
    foreach ($candidate in @($RangersExe, $Rangers1Exe)) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            $testExecutables += [IO.Path]::GetFullPath($candidate)
        }
    }

    $generatorBuild = Join-Path $temporaryRoot "GalaxyGenerator"
    New-Item -ItemType Directory -Path $generatorBuild -Force | Out-Null
    $generatorSource = Join-Path $generatorRoot "src"
    $generatorInclude = Join-Path $generatorRoot "include"
    $generatorTests = Join-Path $generatorRoot "tests"
    $generatorCommon = @(
        "/nologo", "/MT", "/O2", "/EHsc", "/std:c++17", "/W4", "/permissive-",
        ("/I" + $generatorInclude), ("/I" + $generatorSource),
        ("/Fo" + $generatorBuild + "\")
    )
    $generatorSources = @(
        (Join-Path $generatorSource "generator_plan.cpp"),
        (Join-Path $generatorSource "hex_sector_layout.cpp"),
        (Join-Path $generatorSource "runtime_base_throttle.cpp"),
        (Join-Path $generatorSource "runtime_scan.cpp"),
        (Join-Path $generatorSource "xeno_galaxy_generator.cpp")
    )
    $generatorTestExe = Join-Path $generatorBuild "XenoGalaxyGeneratorTests.exe"
    Invoke-Compiler ($generatorCommon + $generatorSources + @(
        (Join-Path $generatorTests "native_tests.cpp"),
        ("/Fe:" + $generatorTestExe)
    ))
    & $generatorTestExe @testExecutables
    if ($LASTEXITCODE -ne 0) {
        throw "Native galaxy-generator tests failed."
    }
    $generatorSmokeExe = Join-Path $generatorBuild "XenoGalaxyGeneratorHookSmoke.exe"
    Invoke-Compiler ($generatorCommon + $generatorSources + @(
        (Join-Path $generatorTests "hook_smoke.cpp"),
        ("/Fe:" + $generatorSmokeExe)
    ))
    foreach ($candidate in $testExecutables) {
        & $generatorSmokeExe $candidate
        if ($LASTEXITCODE -ne 0) {
            throw "Native galaxy-generator hook smoke failed for $candidate."
        }
    }
    $generatorDll = Join-Path $generatorBuild "XenoGalaxyGenerator.XenoPlugin.dll"
    Invoke-Compiler ($generatorCommon + @("/LD") + $generatorSources + @(
        ("/Fe:" + $generatorDll),
        "/link", "/Brepro", ("/DEF:" + (Join-Path $generatorSource "xeno_galaxy_generator.def"))
    ))

    $nextDayBuild = Join-Path $temporaryRoot "NextDay"
    New-Item -ItemType Directory -Path $nextDayBuild -Force | Out-Null
    $nextDaySource = Join-Path $nextDayRoot "src"
    $nextDayInclude = Join-Path $nextDayRoot "include"
    $nextDayTests = Join-Path $nextDayRoot "tests"
    $nextDayCommon = @(
        "/nologo", "/MT", "/O2", "/EHsc", "/std:c++17", "/W4", "/permissive-",
        ("/I" + $nextDayInclude), ("/I" + $nextDaySource),
        ("/Fo" + $nextDayBuild + "\")
    )
    $nextDaySources = @(
        (Join-Path $nextDaySource "live_game_adapter.cpp"),
        (Join-Path $nextDaySource "nextday_policy.cpp"),
        (Join-Path $nextDaySource "runtime_scan.cpp"),
        (Join-Path $nextDaySource "xeno_nextday.cpp")
    )
    $nextDayTestExe = Join-Path $nextDayBuild "XenoNextDayTests.exe"
    Invoke-Compiler ($nextDayCommon + $nextDaySources + @(
        (Join-Path $nextDayTests "native_tests.cpp"),
        ("/Fe:" + $nextDayTestExe)
    ))
    & $nextDayTestExe @testExecutables
    if ($LASTEXITCODE -ne 0) {
        throw "Native NextDay tests failed."
    }
    $nextDaySmokeExe = Join-Path $nextDayBuild "XenoNextDayHookSmoke.exe"
    Invoke-Compiler ($nextDayCommon + $nextDaySources + @(
        (Join-Path $nextDayTests "hook_smoke.cpp"),
        ("/Fe:" + $nextDaySmokeExe)
    ))
    foreach ($candidate in $testExecutables) {
        & $nextDaySmokeExe $candidate
        if ($LASTEXITCODE -ne 0) {
            throw "Native NextDay hook smoke failed for $candidate."
        }
    }
    $nextDayDll = Join-Path $nextDayBuild "XenoNextDay.XenoPlugin.dll"
    Invoke-Compiler ($nextDayCommon + @("/LD") + $nextDaySources + @(
        ("/Fe:" + $nextDayDll),
        "/link", "/Brepro", ("/DEF:" + (Join-Path $nextDaySource "xeno_nextday.def"))
    ))

    Copy-Item -LiteralPath $generatorDll -Destination (Join-Path $OutputDirectory "XenoGalaxyGenerator.XenoPlugin.dll") -Force
    Copy-Item -LiteralPath $nextDayDll -Destination (Join-Path $OutputDirectory "XenoNextDay.XenoPlugin.dll") -Force
    Get-Item -LiteralPath `
        (Join-Path $OutputDirectory "XenoGalaxyGenerator.XenoPlugin.dll"), `
        (Join-Path $OutputDirectory "XenoNextDay.XenoPlugin.dll") |
        Select-Object FullName, Length, LastWriteTime
}
finally {
    $env:INCLUDE = $savedInclude
    $env:LIB = $savedLib
    if (Test-Path -LiteralPath $temporaryRoot) {
        [IO.Directory]::Delete($temporaryRoot, $true)
    }
}
