param(
    [Parameter()]
    [ValidatePattern('^[0-9A-Za-z._-]+$')]
    [string]$Version = 'preview',

    [Parameter()]
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$configPath = Join-Path $PSScriptRoot 'release-set.json'
$config = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
$distRoot = Join-Path $repoRoot 'dist'
$workRoot = Join-Path $distRoot ('.staging-' + $Version)
$bundleRoot = Join-Path $workRoot ('XenoMods-' + $Version)
$archivePath = Join-Path $distRoot ('XenoMods-' + $Version + '.zip')
$manifestPath = Join-Path $distRoot ('XenoMods-' + $Version + '.manifest.json')
$shaPath = Join-Path $distRoot ('XenoMods-' + $Version + '.sha256')

function Assert-WithinRepo([string]$Path) {
    $repoFull = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
    $pathFull = [IO.Path]::GetFullPath($Path)
    if (-not $pathFull.StartsWith($repoFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing path outside repository: $pathFull"
    }
}

foreach ($path in @($workRoot, $archivePath, $manifestPath, $shaPath)) {
    Assert-WithinRepo $path
    if (Test-Path -LiteralPath $path) {
        if (-not $Force) {
            throw "Output already exists: $path. Pass -Force to replace generated output."
        }
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}

$publicBuilder = Join-Path $PSScriptRoot 'build-public-folder.ps1'
if (-not (Test-Path -LiteralPath $publicBuilder -PathType Leaf)) {
    throw "Public folder builder is missing: $publicBuilder"
}
& $publicBuilder -OutputRoot $bundleRoot | Out-Null

$publicConfig = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'public-set.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$publicNames = @($publicConfig.mods | ForEach-Object name)
$releaseMods = @($config.mods | Where-Object { $_.includeInArchive })
$releaseNames = @($releaseMods | ForEach-Object name)
if (($publicNames -join "`n") -ne ($releaseNames -join "`n")) {
    throw "public-set.json and release-set.json contain different public mod lists or order"
}

$included = @($releaseMods | ForEach-Object {
    [ordered]@{
        name = $_.name
        version = $_.version
        dependencies = @($_.dependencies)
        conflicts = @($_.conflicts)
    }
})

$files = Get-ChildItem -LiteralPath $bundleRoot -File -Recurse | Sort-Object FullName
$manifest = [ordered]@{
    schema = 1
    bundleVersion = $Version
    category = $config.category
    loader = $config.loader
    mods = $included
    files = @($files | ForEach-Object {
        [ordered]@{
            path = $_.FullName.Substring($bundleRoot.Length + 1).Replace('\', '/')
            size = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Add-Type -AssemblyName System.IO.Compression
$archiveStream = [IO.File]::Open($archivePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $archive = [IO.Compression.ZipArchive]::new(
        $archiveStream,
        [IO.Compression.ZipArchiveMode]::Create,
        $false,
        [Text.Encoding]::UTF8)
    try {
        $fixedTimestamp = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
        foreach ($file in $files) {
            $relativePath = $file.FullName.Substring($bundleRoot.Length + 1).Replace('\', '/')
            $entry = $archive.CreateEntry($relativePath, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $fixedTimestamp
            $inputStream = $file.OpenRead()
            $outputStream = $entry.Open()
            try {
                $inputStream.CopyTo($outputStream)
            }
            finally {
                $outputStream.Dispose()
                $inputStream.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}
finally {
    $archiveStream.Dispose()
}

$archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(
    $shaPath,
    ($archiveHash + '  ' + (Split-Path -Leaf $archivePath) + "`r`n"),
    [Text.Encoding]::ASCII)

if (Test-Path -LiteralPath $workRoot) {
    Assert-WithinRepo $workRoot
    Remove-Item -LiteralPath $workRoot -Recurse -Force
}

[pscustomobject]@{
    archive = $archivePath
    manifest = $manifestPath
    sha256 = $archiveHash
    mods = $included.Count
    files = $files.Count
} | ConvertTo-Json -Depth 4
