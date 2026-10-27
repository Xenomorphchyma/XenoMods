param(
    [string]$Configuration = 'Release',
    [string]$VcToolsRoot = '',
    [string]$WindowsSdkRoot = '',
    [string]$WindowsSdkVersion = '',
    [string]$ModKitPath = '',
    [string]$WorkspaceRoot = $env:SRHD_WORKSPACE_ROOT,
    # Rangers executables for the offline signature probe; the installed
    # Steam Rangers.exe is used when none are given and it exists.
    [string[]]$ProbeExecutables = @(),
    # In the public repository also rebuild CFG\*.dat and DATA\Arrows.
    [switch]$Resources
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Find-WorkspaceRoot([string]$StartPath) {
    if(-not [string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
        return [IO.Path]::GetFullPath($WorkspaceRoot)
    }
    $cursor = [IO.DirectoryInfo](Get-Item -LiteralPath $StartPath)
    while($null -ne $cursor) {
        if(Test-Path -LiteralPath (Join-Path $cursor.FullName 'Tools\SRHDModKit\srhd.py') -PathType Leaf) {
            return $cursor.FullName
        }
        $cursor = $cursor.Parent
    }
    throw 'SRHD workspace root was not found; pass -WorkspaceRoot or set SRHD_WORKSPACE_ROOT.'
}

function Find-VcToolsRoot {
    if(-not [string]::IsNullOrWhiteSpace($VcToolsRoot)) { return [IO.Path]::GetFullPath($VcToolsRoot) }
    if(-not [string]::IsNullOrWhiteSpace($env:VCToolsInstallDir)) { return [IO.Path]::GetFullPath($env:VCToolsInstallDir) }
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if(-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Visual Studio locator was not found; pass -VcToolsRoot.' }
    $installationPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if([string]::IsNullOrWhiteSpace($installationPath)) { throw 'Visual C++ x86 tools were not found; pass -VcToolsRoot.' }
    $versionFile = Join-Path $installationPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    $version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    return Join-Path $installationPath "VC\Tools\MSVC\$version"
}

function Find-WindowsSdkRoot {
    if(-not [string]::IsNullOrWhiteSpace($WindowsSdkRoot)) { return [IO.Path]::GetFullPath($WindowsSdkRoot) }
    if(-not [string]::IsNullOrWhiteSpace($env:WindowsSdkDir)) { return [IO.Path]::GetFullPath($env:WindowsSdkDir) }
    $kitsRoot = Get-ItemPropertyValue -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10 -ErrorAction Stop
    return [IO.Path]::GetFullPath($kitsRoot)
}

$resolvedWorkspaceRoot = Find-WorkspaceRoot $projectRoot
$vcTools = Find-VcToolsRoot
$windowsSdk = Find-WindowsSdkRoot
if([string]::IsNullOrWhiteSpace($WindowsSdkVersion)) {
    $WindowsSdkVersion = Get-ChildItem -LiteralPath (Join-Path $windowsSdk 'Include') -Directory |
        Where-Object {
            (Test-Path -LiteralPath (Join-Path $_.FullName 'ucrt') -PathType Container) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'um') -PathType Container)
        } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1 -ExpandProperty Name
}
if([string]::IsNullOrWhiteSpace($WindowsSdkVersion)) { throw 'Windows 10 SDK was not found; pass -WindowsSdkRoot and -WindowsSdkVersion.' }
# The workspace keeps the mod in a subfolder; the public repository builds in place.
$modRoot = if (Test-Path -LiteralPath (Join-Path $projectRoot 'XenoHangarPaging\ModuleInfo.txt') -PathType Leaf) {
    Join-Path $projectRoot 'XenoHangarPaging'
} else {
    $projectRoot
}
$sourceRoot = Join-Path $projectRoot 'Sources'
$buildRoot = Join-Path $projectRoot 'Build'
$runtimeSource = Join-Path $sourceRoot 'Runtime\XenoHangarPaging.Runtime.cpp'
$runtimeOutput = Join-Path $modRoot 'XenoHangarPaging.Runtime.dll'
$mainSource = Join-Path $sourceRoot 'Main.txt'
$mainOutput = Join-Path $modRoot 'CFG\Main.dat'
$cacheDataSource = Join-Path $sourceRoot 'CacheData.txt'
$cacheDataOutput = Join-Path $modRoot 'CFG\CacheData.dat'
$arrowSource = Join-Path $sourceRoot 'Assets\Arrows'
$arrowOutput = Join-Path $modRoot 'DATA\Arrows'
$moduleInfoSource = Join-Path $sourceRoot 'ModuleInfo.txt'
$moduleInfoOutput = Join-Path $modRoot 'ModuleInfo.txt'
$nativeManifestSource = Join-Path $sourceRoot 'XenoNativePlugin.ini'
$nativeManifestOutput = Join-Path $modRoot 'XenoNativePlugin.ini'
$modkit = if([string]::IsNullOrWhiteSpace($ModKitPath)) {
    Join-Path $resolvedWorkspaceRoot 'Tools\SRHDModKit\srhd.py'
} else {
    [IO.Path]::GetFullPath($ModKitPath)
}

New-Item -ItemType Directory -Force -Path $buildRoot, (Split-Path -Parent $mainOutput) | Out-Null

$env:INCLUDE = (Join-Path $vcTools 'include') + ';' +
    (Join-Path $windowsSdk "Include\$WindowsSdkVersion\ucrt") + ';' +
    (Join-Path $windowsSdk "Include\$WindowsSdkVersion\shared") + ';' +
    (Join-Path $windowsSdk "Include\$WindowsSdkVersion\um") + ';' +
    (Join-Path $windowsSdk "Include\$WindowsSdkVersion\winrt")
$env:LIB = (Join-Path $vcTools 'lib\x86') + ';' +
    (Join-Path $windowsSdk "Lib\$WindowsSdkVersion\ucrt\x86") + ';' +
    (Join-Path $windowsSdk "Lib\$WindowsSdkVersion\um\x86")
$compiler = Join-Path $vcTools 'bin\Hostx86\x86\cl.exe'
$objectOutput = Join-Path $buildRoot 'XenoHangarPaging.Runtime.obj'
$probeSource = Join-Path $sourceRoot 'Tests\signature_probe.cpp'
$probeOutput = Join-Path $buildRoot 'XenoHangarPagingSignatureProbe.exe'
& $compiler /nologo /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE `
    /Fo:(Join-Path $buildRoot 'signature_probe.obj') $probeSource user32.lib oleaut32.lib /link /OUT:$probeOutput
if ($LASTEXITCODE -ne 0) { throw "Signature probe build failed: $LASTEXITCODE" }
if ($ProbeExecutables.Count -eq 0) {
    $steamRangers = 'D:\Steam\steamapps\common\Space Rangers HD A War Apart\Rangers.exe'
    if (Test-Path -LiteralPath $steamRangers -PathType Leaf) { $ProbeExecutables = @($steamRangers) }
}
if ($ProbeExecutables.Count -gt 0) {
    & $probeOutput @ProbeExecutables
    if ($LASTEXITCODE -ne 0) { throw "Hangar signature probe failed: $LASTEXITCODE" }
}

& $compiler /nologo /LD /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /Fo:$objectOutput $runtimeSource user32.lib oleaut32.lib /link /OUT:$runtimeOutput
if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }

$inPlace = $modRoot -eq $projectRoot
if ($inPlace -and -not $Resources) {
    # In the public repository CFG and ModuleInfo.txt are release artifacts
    # (DAT encoding uses a fresh key each time); rebuild them only on request.
    Get-Item -LiteralPath $runtimeOutput | Select-Object FullName, Length, LastWriteTime
    return
}

New-Item -ItemType Directory -Force -Path $arrowOutput | Out-Null
$arrowPngs = @(Get-ChildItem -LiteralPath $arrowSource -Filter '*.png' -File | ForEach-Object FullName)
if ($arrowPngs.Count -ne 8) { throw "Expected 8 arrow PNGs in $arrowSource, found $($arrowPngs.Count)." }
# Type 0 (32-bit) keeps the padded arrows lossless; stock forms use it too.
python -B $modkit convert png-gi @arrowPngs -o $arrowOutput --mode 0_32 --overwrite
if ($LASTEXITCODE -ne 0) { throw "Arrow GI conversion failed: $LASTEXITCODE" }

python -B $modkit dat encode $mainSource $mainOutput --overwrite
if ($LASTEXITCODE -ne 0) { throw "Main.dat encode failed: $LASTEXITCODE" }
python -B $modkit dat encode $cacheDataSource $cacheDataOutput --overwrite
if ($LASTEXITCODE -ne 0) { throw "CacheData.dat encode failed: $LASTEXITCODE" }

if ($inPlace) {
    Get-Item -LiteralPath $runtimeOutput, $mainOutput, $cacheDataOutput |
        Select-Object FullName, Length, LastWriteTime
    return
}

$moduleInfoText = [IO.File]::ReadAllText($moduleInfoSource, [Text.Encoding]::UTF8)
[IO.File]::WriteAllText(
    $moduleInfoOutput,
    $moduleInfoText,
    [Text.Encoding]::GetEncoding(1251))
Copy-Item -LiteralPath $nativeManifestSource -Destination $nativeManifestOutput -Force

Get-Item -LiteralPath `
    $runtimeOutput, $mainOutput, $cacheDataOutput, $moduleInfoOutput, $nativeManifestOutput |
    Select-Object FullName, Length, LastWriteTime
