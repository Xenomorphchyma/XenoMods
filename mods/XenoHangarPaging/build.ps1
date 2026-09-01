param(
    [string]$Configuration = 'Release',
    [string]$VcToolsRoot = '',
    [string]$WindowsSdkRoot = '',
    [string]$WindowsSdkVersion = '',
    [string]$ModKitPath = '',
    [string]$WorkspaceRoot = $env:SRHD_WORKSPACE_ROOT
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
$modRoot = Join-Path $projectRoot 'XenoHangarPaging'
$sourceRoot = Join-Path $projectRoot 'Sources'
$buildRoot = Join-Path $projectRoot 'Build'
$runtimeSource = Join-Path $sourceRoot 'Runtime\XenoHangarPaging.Runtime.cpp'
$runtimeOutput = Join-Path $modRoot 'XenoHangarPaging.Runtime.dll'
$mainSource = Join-Path $sourceRoot 'Main.txt'
$mainOutput = Join-Path $modRoot 'CFG\Main.dat'
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
& $compiler /nologo /LD /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /Fo:$objectOutput $runtimeSource user32.lib oleaut32.lib /link /OUT:$runtimeOutput
if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }

python -B $modkit dat encode $mainSource $mainOutput --overwrite
if ($LASTEXITCODE -ne 0) { throw "Main.dat encode failed: $LASTEXITCODE" }

$moduleInfoText = [IO.File]::ReadAllText($moduleInfoSource, [Text.Encoding]::UTF8)
[IO.File]::WriteAllText(
    $moduleInfoOutput,
    $moduleInfoText,
    [Text.Encoding]::GetEncoding(1251))
Copy-Item -LiteralPath $nativeManifestSource -Destination $nativeManifestOutput -Force

Get-Item -LiteralPath `
    $runtimeOutput, $mainOutput, $moduleInfoOutput, $nativeManifestOutput |
    Select-Object FullName, Length, LastWriteTime
