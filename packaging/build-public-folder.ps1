param(
    [Parameter(Mandatory)]
    [string]$OutputRoot,

    [Parameter()]
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$config = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'public-set.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$outputFull = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\')
$outputLeaf = Split-Path -Leaf $outputFull

if ($outputLeaf -notmatch '^XenoMods-[0-9A-Za-z._-]+$') {
    throw "Output folder name must start with XenoMods-: $outputFull"
}
if ([IO.Path]::GetPathRoot($outputFull).TrimEnd('\') -eq $outputFull) {
    throw "Refusing to use a drive root: $outputFull"
}
if ($outputFull.Equals([IO.Path]::GetFullPath($repoRoot).TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to overwrite the repository root: $outputFull"
}
if (Test-Path -LiteralPath $outputFull) {
    if (-not $Force) {
        throw "Output already exists: $outputFull. Pass -Force to replace it."
    }
    Remove-Item -LiteralPath $outputFull -Recurse -Force
}

function Assert-RegularTree([string]$Path) {
    $items = @((Get-Item -LiteralPath $Path -Force))
    if ($items[0].PSIsContainer) {
        $items += Get-ChildItem -LiteralPath $Path -Recurse -Force
    }
    foreach ($item in $items) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse points are not allowed in the public bundle: $($item.FullName)"
        }
    }
}

function Copy-ExactEntry([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Missing public runtime entry: $Source"
    }
    Assert-RegularTree $Source
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
}

New-Item -ItemType Directory -Path $outputFull -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'README.md') -Destination (Join-Path $outputFull 'README.md') -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $outputFull 'LICENSE') -Force

$loaderRoot = Join-Path $repoRoot $config.loaderPackage
foreach ($entry in $config.loaderEntries) {
    Copy-ExactEntry (Join-Path $loaderRoot $entry.from) (Join-Path $outputFull $entry.to)
}

$categoryRoot = Join-Path $outputFull ('Mods\' + $config.category)
foreach ($mod in $config.mods) {
    $modSource = Join-Path (Join-Path $repoRoot 'mods') $mod.name
    $modTarget = Join-Path $categoryRoot $mod.name
    foreach ($entry in $mod.runtimeEntries) {
        Copy-ExactEntry (Join-Path $modSource $entry) (Join-Path $modTarget $entry)
    }
    foreach ($entry in @($mod.runtimeExcludes)) {
        if ([string]::IsNullOrWhiteSpace($entry)) { continue }
        $excludedTarget = Join-Path $modTarget $entry
        if (-not (Test-Path -LiteralPath $excludedTarget -PathType Leaf)) {
            throw "Missing configured runtime exclusion: $excludedTarget"
        }
        Remove-Item -LiteralPath $excludedTarget -Force
    }
}

$forbidden = Get-ChildItem -LiteralPath $outputFull -File -Recurse -Force | Where-Object {
    $relative = $_.FullName.Substring($outputFull.Length + 1).Replace('\', '/')
    ($relative -match '(?i)(^|/)(README[^/]*|INSTALL[^/]*)$' -and $relative -ne 'README.md') -or
    ($relative -match '(?i)(^|/)[^/]+\.md$' -and $relative -ne 'README.md') -or
    $relative -match '(?i)(^|/)SOURCE(/|$)' -or
    $relative -match '(?i)(^|/)(Build|\.build|\.srhd-build|\.srhd-cache|TestResults|tests)(/|$)' -or
    $relative -match '(?i)(^|/)build[^/]*\.(ps1|py)$'
}
if ($forbidden) {
    throw "Developer clutter entered the public folder: $($forbidden.FullName -join ', ')"
}

$loaderSource = Get-ChildItem -LiteralPath $outputFull -File -Recurse -Force | Where-Object {
    $_.Extension -in @('.cpp', '.h', '.def') -and
    $_.FullName -notmatch '(?i)\\Mods\\XenoMods\\'
}
if ($loaderSource) {
    throw "NativeLoader source entered the public folder: $($loaderSource.FullName -join ', ')"
}

$files = Get-ChildItem -LiteralPath $outputFull -File -Recurse -Force
[pscustomobject]@{
    output = $outputFull
    mods = @($config.mods).Count
    files = $files.Count
    sizeMiB = [math]::Round((($files | Measure-Object Length -Sum).Sum / 1MB), 2)
    sourceFiles = @($files | Where-Object FullName -Match '(?i)\\SOURCE\\').Count
    nativeLoaderSourceFiles = @($loaderSource).Count
    forbiddenFiles = @($forbidden).Count
} | ConvertTo-Json
