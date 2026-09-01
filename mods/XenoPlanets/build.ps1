[CmdletBinding()]
param(
    [Parameter()]
    [string]$ModKitPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = $PSScriptRoot
$modKitCandidates = @(
    $ModKitPath,
    $env:SRHD_MODKIT,
    (Join-Path $projectRoot '..\..\..\..\Tools\SRHDModKit\srhd.cmd'),
    (Join-Path $projectRoot '..\..\..\Tools\SRHDModKit\srhd.cmd')
) | Where-Object { $_ }
$modKit = $modKitCandidates |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
$buildParent = Join-Path $projectRoot 'Build'
$buildRoot = Join-Path $buildParent 'XenoPlanets'
$generatedRoot = Join-Path $buildParent 'Generated'

if (-not $modKit) {
    throw 'SRHD ModKit not found. Pass -ModKitPath or set SRHD_MODKIT.'
}

if (Test-Path -LiteralPath $buildRoot) {
    $resolvedBuildParent = (Resolve-Path -LiteralPath $buildParent).Path.TrimEnd('\')
    $resolvedBuildRoot = (Resolve-Path -LiteralPath $buildRoot).Path
    if (-not $resolvedBuildRoot.StartsWith("$resolvedBuildParent\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe build path: $resolvedBuildRoot"
    }
    Remove-Item -LiteralPath $resolvedBuildRoot -Recurse -Force
}

New-Item -ItemType Directory -Force -Path `
    (Join-Path $buildRoot 'CFG\Rus'), `
    (Join-Path $buildRoot 'CFG\Eng'), `
    (Join-Path $buildRoot 'DATA\Script'), `
    $generatedRoot | Out-Null

Copy-Item -LiteralPath (Join-Path $projectRoot 'Sources\ModuleInfo.txt') -Destination (Join-Path $buildRoot 'ModuleInfo.txt')
$packagePath = Join-Path $projectRoot 'XenoPlanets.pkg'
& $modKit resource verify $packagePath
if ($LASTEXITCODE -ne 0) { throw "XenoPlanets.pkg validation failed: $LASTEXITCODE" }
Copy-Item -LiteralPath $packagePath -Destination $buildRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'INSTALL.TXT') -Destination $buildRoot

& $modKit script build `
    (Join-Path $projectRoot 'Sources\Script\XenoPlanets.rson') `
    --scr (Join-Path $buildRoot 'DATA\Script\XenoPlanets.scr') `
    --lang (Join-Path $generatedRoot 'XenoPlanets.lang.txt') `
    --overwrite
if ($LASTEXITCODE -ne 0) { throw "XenoPlanets.scr build failed: $LASTEXITCODE" }

& $modKit dat encode (Join-Path $projectRoot 'Sources\CFG\CacheData.txt') (Join-Path $buildRoot 'CFG\CacheData.dat') --overwrite
if ($LASTEXITCODE -ne 0) { throw "CacheData.dat encode failed: $LASTEXITCODE" }

& $modKit dat encode (Join-Path $projectRoot 'Sources\CFG\Main.txt') (Join-Path $buildRoot 'CFG\Main.dat') --overwrite
if ($LASTEXITCODE -ne 0) { throw "Main.dat encode failed: $LASTEXITCODE" }

& $modKit dat encode (Join-Path $projectRoot 'Sources\CFG\Rus\Lang.txt') (Join-Path $buildRoot 'CFG\Rus\Lang.dat') --overwrite
if ($LASTEXITCODE -ne 0) { throw "Russian Lang.dat encode failed: $LASTEXITCODE" }

& $modKit dat encode (Join-Path $projectRoot 'Sources\CFG\Eng\Lang.txt') (Join-Path $buildRoot 'CFG\Eng\Lang.dat') --overwrite
if ($LASTEXITCODE -ne 0) { throw "English Lang.dat encode failed: $LASTEXITCODE" }

foreach ($dat in @(
    (Join-Path $buildRoot 'CFG\CacheData.dat'),
    (Join-Path $buildRoot 'CFG\Main.dat'),
    (Join-Path $buildRoot 'CFG\Rus\Lang.dat'),
    (Join-Path $buildRoot 'CFG\Eng\Lang.dat')
)) {
    & $modKit dat validate $dat
    if ($LASTEXITCODE -ne 0) { throw "DAT validation failed: $dat" }
}

Write-Host "Built: $buildRoot"
