param(
    [string]$Configuration = 'Release',
    [string]$WorkspaceRoot = 'D:\SRHD_Modding',
    [string]$OutputRoot = (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..\Native')
)

$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $sourceRoot '..\..\..\..')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$cl = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Tools\MSVC\**\bin\Hostx64\x86\cl.exe' | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($cl)) { throw 'Visual C++ x86 compiler was not found.' }
$cl = [IO.Path]::GetFullPath($cl)
$vcTools = Split-Path (Split-Path (Split-Path (Split-Path $cl -Parent) -Parent) -Parent) -Parent
$sdkRoot = Get-ItemPropertyValue -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10
$sdkVersion = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'ucrt') } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty Name
$env:INCLUDE = (Join-Path $vcTools 'include') + ';' + (Join-Path $sdkRoot "Include\$sdkVersion\ucrt") + ';' + (Join-Path $sdkRoot "Include\$sdkVersion\shared") + ';' + (Join-Path $sdkRoot "Include\$sdkVersion\um")
$env:LIB = (Join-Path $vcTools 'lib\x86') + ';' + (Join-Path $sdkRoot "Lib\$sdkVersion\ucrt\x86") + ';' + (Join-Path $sdkRoot "Lib\$sdkVersion\um\x86")
$buildRoot = Join-Path $sourceRoot 'Build'
$outputRoot = [IO.Path]::GetFullPath($OutputRoot)
$modRoot = [IO.Path]::GetFullPath((Join-Path $sourceRoot '..\..'))
$cfgSource = Join-Path $sourceRoot '..\CFG'
$cfgOutput = Join-Path $modRoot 'CFG'
$modkit = Join-Path $WorkspaceRoot 'Tools\SRHDModKit\srhd.py'
New-Item -ItemType Directory -Force -Path $buildRoot, $outputRoot | Out-Null
New-Item -ItemType Directory -Force -Path $cfgOutput | Out-Null
$object = Join-Path $buildRoot 'XenoSystemZoom.obj'
$dll = Join-Path $outputRoot 'XenoSystemZoom.XenoPlugin.dll'
$def = Join-Path $sourceRoot 'XenoSystemZoom\xeno_system_zoom.def'
& $cl /nologo /LD /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /Fo:$object (Join-Path $sourceRoot 'XenoSystemZoom\xeno_system_zoom.cpp') user32.lib /link "/DEF:$def" /OUT:$dll
if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }
Copy-Item -LiteralPath (Join-Path $sourceRoot 'XenoSystemZoom\xeno_plugin_api.h') -Destination (Join-Path $buildRoot 'xeno_plugin_api.h') -Force
foreach ($name in @('Main', 'CacheData')) {
    $source = Join-Path $cfgSource ($name + '.txt')
    $destination = Join-Path $cfgOutput ($name + '.dat')
    & python -B $modkit dat encode $source $destination --overwrite
    if ($LASTEXITCODE -ne 0) { throw "DAT build failed for ${name}: $LASTEXITCODE" }
}
Get-Item $dll
