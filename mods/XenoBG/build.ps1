[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = $PSScriptRoot
$workspaceRoot = (Resolve-Path (Join-Path $projectRoot '..\..\..\..')).Path
$modKit = Join-Path $workspaceRoot 'Tools\SRHDModKit\srhd.cmd'
$buildParent = Join-Path $projectRoot 'Build'
$buildRoot = Join-Path $buildParent 'XenoBG'

if (-not (Test-Path -LiteralPath $modKit -PathType Leaf)) {
    throw "SRHD ModKit not found: $modKit"
}

if (Test-Path -LiteralPath $buildRoot) {
    $resolvedBuildParent = (Resolve-Path -LiteralPath $buildParent).Path.TrimEnd('\')
    $resolvedBuildRoot = (Resolve-Path -LiteralPath $buildRoot).Path
    if (-not $resolvedBuildRoot.StartsWith("$resolvedBuildParent\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe build path: $resolvedBuildRoot"
    }
    Remove-Item -LiteralPath $resolvedBuildRoot -Recurse -Force
}

New-Item -ItemType Directory -Force -Path (Join-Path $buildRoot 'CFG') | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot 'ModuleInfo.txt') -Destination $buildRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'INSTALL.TXT') -Destination $buildRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'XenoBG.pkg') -Destination $buildRoot

& $modKit dat encode (Join-Path $projectRoot 'Sources\CFG\CacheData.txt') (Join-Path $buildRoot 'CFG\CacheData.dat') --overwrite
if ($LASTEXITCODE -ne 0) { throw "CacheData.dat encode failed: $LASTEXITCODE" }

& $modKit dat encode (Join-Path $projectRoot 'Sources\CFG\Main.txt') (Join-Path $buildRoot 'CFG\Main.dat') --overwrite
if ($LASTEXITCODE -ne 0) { throw "Main.dat encode failed: $LASTEXITCODE" }

& $modKit dat validate (Join-Path $buildRoot 'CFG\CacheData.dat')
if ($LASTEXITCODE -ne 0) { throw "CacheData.dat validation failed: $LASTEXITCODE" }

& $modKit dat validate (Join-Path $buildRoot 'CFG\Main.dat')
if ($LASTEXITCODE -ne 0) { throw "Main.dat validation failed: $LASTEXITCODE" }

Write-Host "Built: $buildRoot"
