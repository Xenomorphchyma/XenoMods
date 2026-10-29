param([string]$OutputDirectory = (Join-Path $PSScriptRoot "..\..\Native"))
$ErrorActionPreference = "Stop"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw "Install MSVC x86 Build Tools (vswhere.exe missing)." }
$cl = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "VC\Tools\MSVC\**\bin\Hostx64\x86\cl.exe" | Select-Object -First 1
if (-not $cl) { throw "MSVC x86 compiler not found." }
$cl = (Get-Item -LiteralPath $cl).FullName
$vc = [IO.Path]::GetFullPath((Join-Path (Split-Path $cl -Parent) "..\..\.."))
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdk = Get-ChildItem -LiteralPath (Join-Path $kits "Include") -Directory | Where-Object { Test-Path (Join-Path $_.FullName "um\Windows.h") } | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
if (-not $sdk) { throw "Windows SDK not found." }
$oldInclude, $oldLib = $env:INCLUDE, $env:LIB
$temp = Join-Path ([IO.Path]::GetTempPath()) ("srhd-native-" + [Guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Path $temp,$OutputDirectory -Force | Out-Null
    $env:INCLUDE = @((Join-Path $vc "include"),(Join-Path $kits "Include\$sdk\ucrt"),(Join-Path $kits "Include\$sdk\shared"),(Join-Path $kits "Include\$sdk\um")) -join ";"
    $env:LIB = @((Join-Path $vc "lib\x86"),(Join-Path $kits "Lib\$sdk\ucrt\x86"),(Join-Path $kits "Lib\$sdk\um\x86")) -join ";"
    $dll = Join-Path $temp "XenoPBEngineFix.XenoPlugin.dll"
    & $cl /nologo /MT /O2 /EHsc /std:c++17 /W4 /permissive- /LD (Join-Path $PSScriptRoot "XenoPBEngineFix\XenoPBEngineFix.cpp") "/I$(Join-Path $PSScriptRoot "XenoPBEngineFix")" ("/Fo" + $temp + "\") "/Fe:$dll" /link /Brepro "/DEF:$(Join-Path $PSScriptRoot "XenoPBEngineFix\XenoPBEngineFix.def")"
    if ($LASTEXITCODE -ne 0) { throw "MSVC failed with exit code $LASTEXITCODE." }
    $tests = Join-Path $temp "native_tests.exe"
    & $cl /nologo /MT /O2 /EHsc /std:c++17 /W4 /permissive- (Join-Path $PSScriptRoot "tests\native_tests.cpp") "/I$(Join-Path $PSScriptRoot "XenoPBEngineFix")" ("/Fo" + $temp + "\") "/Fe:$tests" /link /Brepro
    if ($LASTEXITCODE -ne 0) { throw "MSVC failed on native tests with exit code $LASTEXITCODE." }
    & $tests
    if ($LASTEXITCODE -ne 0) { throw "Native tests failed." }
    Copy-Item -LiteralPath $dll -Destination (Join-Path $OutputDirectory "XenoPBEngineFix.XenoPlugin.dll") -Force
} finally {
    $env:INCLUDE, $env:LIB = $oldInclude, $oldLib
    if (Test-Path -LiteralPath $temp) { [IO.Directory]::Delete($temp, $true) }
}
