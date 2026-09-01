param(
    [string]$Version = "0.10.5",
    [ValidateRange(20, 55)]
    [int]$SectorCount = 55,
    [ValidateRange(73, 200)]
    [int]$StarCount = 200,
    [ValidateSet("Compatible", "Profile", "DominatorCatchUp", "DominatorFast")]
    [string]$NextDayMode = "Compatible",
    [switch]$DisableNextDay,
    [string]$WorkspaceRoot = $env:SRHD_WORKSPACE_ROOT,
    [string]$RangersExe = $env:SRHD_RANGERS_EXE,
    [string]$Rangers1Exe = $env:SRHD_RANGERS1_EXE
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $RangersExe -and $env:SRHD_GAME_ROOT) {
    $RangersExe = Join-Path $env:SRHD_GAME_ROOT "Rangers.exe"
}
if (-not $Rangers1Exe -and $env:SRHD_GAME_ROOT) {
    $Rangers1Exe = Join-Path $env:SRHD_GAME_ROOT "Rangers1.exe"
}
if ($WorkspaceRoot) {
    $workspaceRoot = [IO.Path]::GetFullPath($WorkspaceRoot)
}
else {
    $workspaceRoot = $null
    $candidateRoot = $projectRoot
    while ($candidateRoot) {
        if (Test-Path -LiteralPath (Join-Path $candidateRoot "Tools\SRHDModKit\srhd.cmd") -PathType Leaf) {
            $workspaceRoot = $candidateRoot
            break
        }
        $parentRoot = Split-Path -Parent $candidateRoot
        if (-not $parentRoot -or $parentRoot -eq $candidateRoot) { break }
        $candidateRoot = $parentRoot
    }
    if (-not $workspaceRoot) {
        throw "Workspace root was not found. Set -WorkspaceRoot or SRHD_WORKSPACE_ROOT."
    }
}
$sourceRoot = Join-Path $projectRoot "Sources"
$mainTextSource = Join-Path $sourceRoot "CFG\Main.txt"
$cacheDataTextSource = Join-Path $sourceRoot "CFG\CacheData.txt"
$cacheDataPrebuilt = Join-Path $projectRoot "CFG\CacheData.dat"
$blockPiratesCompatSource = Join-Path $projectRoot "Compat\Mod_BlockPirates.adaptive.rson"
$moduleInfoSource = Join-Path $projectRoot "ModuleInfo.source.txt"
$sectorNamesSource = Join-Path $projectRoot "Patches\sector_names_ru.tsv"
$starNamesSource = Join-Path $projectRoot "Patches\star_names_ru.tsv"
$planetNamesSource = Join-Path $projectRoot "Patches\planet_names_extra_ru.tsv"
$nativeBuildScript = Join-Path $sourceRoot "Native\build.ps1"
$generatorConfigSource = Join-Path $sourceRoot "Native\Config\XenoGalaxyGenerator.XenoPlugin.ini"
$nextDayConfigSource = Join-Path $sourceRoot "Native\Config\XenoNextDay.XenoPlugin.ini"
$mapMarkerSource = Join-Path $sourceRoot "Assets\MapMarker"
$galaxySurfaceSource = Join-Path $sourceRoot "Assets\BG\VanillaGalaxySurfaceUpscaled.png"
$galaxySceneSource = Join-Path $sourceRoot "Assets\BG\VanillaGalaxySceneUpscaled.png"
$panelPngSource = Join-Path $sourceRoot "Assets\Panel\GalaxyPanel.png"
$modKit = Join-Path $workspaceRoot "Tools\SRHDModKit\srhd.cmd"
$outputRoot = Join-Path $workspaceRoot "Build\Output\XenoBigGalaxy"
$modOutput = Join-Path $outputRoot "Mods\XenoMods\XenoBigGalaxy"
$releaseRoot = Join-Path $workspaceRoot "Releases\Mods\XenoBigGalaxy"
$releaseZip = Join-Path $releaseRoot ("XenoBigGalaxy_v" + $Version + ".zip")
$releaseManifest = Join-Path $releaseRoot ("XenoBigGalaxy_v" + $Version + ".manifest.json")
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("XenoBigGalaxy-" + [Guid]::NewGuid().ToString("N"))

function Invoke-ModKit {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    & $modKit @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "SRHD ModKit failed with exit code ${LASTEXITCODE}: $($Arguments -join ' ')"
    }
}

function Reset-OutputDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$AllowedRoot
    )
    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullRoot = [IO.Path]::GetFullPath($AllowedRoot)
    $rootPrefix = $fullRoot.TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to reset output outside ${fullRoot}: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) {
        [IO.Directory]::Delete($fullPath, $true)
    }
    [IO.Directory]::CreateDirectory($fullPath) | Out-Null
}

function Read-TabRows {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$FieldCount
    )
    $rows = @()
    foreach ($line in [IO.File]::ReadAllLines($Path, [Text.Encoding]::UTF8)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $fields = $line.Split("`t", $FieldCount)
        if ($fields.Count -ne $FieldCount) {
            throw "Invalid TSV row in ${Path}: $line"
        }
        $rows += [pscustomobject]@{ Fields = $fields }
    }
    return $rows
}

$requiredFiles = @(
    $modKit,
    $mainTextSource,
    $cacheDataTextSource,
    $blockPiratesCompatSource,
    $moduleInfoSource,
    $readmeSource,
    $sectorNamesSource,
    $starNamesSource,
    $planetNamesSource,
    $nativeBuildScript,
    $generatorConfigSource,
    $nextDayConfigSource,
    $galaxySurfaceSource,
    $galaxySceneSource,
    $panelPngSource
)
foreach ($requiredFile in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required source file was not found: $requiredFile"
    }
}
if (-not (Test-Path -LiteralPath $mapMarkerSource -PathType Container)) {
    throw "MapMarker source directory was not found: $mapMarkerSource"
}

$sourceLeakPatterns = @(
    "GalaxyHelper",
    "Bm.FormGalaxy.Battle.", "Swords2", "Swords3", "Swords4", "Swords5",
    "2PanelBig", "Name=BlackHole"
)
$sourceLeakScanTargets = @(
    $sourceRoot,
    (Join-Path $projectRoot "Compat"),
    (Join-Path $projectRoot "Patches"),
    $moduleInfoSource,
    $readmeSource
)
foreach ($pattern in $sourceLeakPatterns) {
    $matches = & rg -n --fixed-strings $pattern @sourceLeakScanTargets 2>$null
    if ($LASTEXITCODE -eq 0) {
        throw "Forbidden inherited implementation remains (${pattern}):`n$($matches -join "`n")"
    }
    if ($LASTEXITCODE -ne 1) {
        throw "rg failed while checking source pattern: $pattern"
    }
}

# Attribution to AModGalaxyLite, Lorki and EnaYzeR is intentionally present in
# ModuleInfo. Keep checking implementation sources for inherited identifiers.
$implementationLeakScanTargets = @(
    $sourceRoot,
    (Join-Path $projectRoot "Compat"),
    (Join-Path $projectRoot "Patches")
)
foreach ($pattern in @("AModGalaxyLite", "Lorki", "EnaYzeR")) {
    $matches = & rg -n --fixed-strings $pattern @implementationLeakScanTargets 2>$null
    if ($LASTEXITCODE -eq 0) {
        throw "Forbidden inherited implementation remains (${pattern}):`n$($matches -join "`n")"
    }
    if ($LASTEXITCODE -ne 1) {
        throw "rg failed while checking inherited implementation pattern: $pattern"
    }
}

New-Item -ItemType Directory -Path $temporaryRoot, $releaseRoot -Force | Out-Null
Reset-OutputDirectory -Path $outputRoot -AllowedRoot (Join-Path $workspaceRoot "Build")
New-Item -ItemType Directory -Path `
    (Join-Path $modOutput "CFG\Rus"), `
    (Join-Path $modOutput "DATA\BG"), `
    (Join-Path $modOutput "DATA\MapMarker"), `
    (Join-Path $modOutput "DATA\Panel"), `
    (Join-Path $modOutput "DATA\Script"), `
    (Join-Path $modOutput "Native") -Force | Out-Null

try {
    $unicodeWithBom = New-Object Text.UnicodeEncoding($false, $true)
    $utf8NoBom = New-Object Text.UTF8Encoding($false)

    $moduleText = [IO.File]::ReadAllText($moduleInfoSource, [Text.Encoding]::UTF8)
    $moduleText = $moduleText.Replace(
        "Большая галактика: 55 секторов",
        "Большая галактика: ${SectorCount} секторов")
    $moduleText = $moduleText.Replace(
        "до 55 полноценных секторов",
        "до ${SectorCount} полноценных секторов")
    $moduleText = $moduleText.Replace(
        "Large galaxy: 55 sectors",
        "Large galaxy: ${SectorCount} sectors")
    $moduleText = $moduleText.Replace(
        "to 55 fully generated sectors",
        "to ${SectorCount} fully generated sectors")
    [IO.File]::WriteAllText((Join-Path $modOutput "ModuleInfo.txt"), $moduleText, $unicodeWithBom)

    $sectorRows = Read-TabRows -Path $sectorNamesSource -FieldCount 2
    $expectedSectorNumber = 21
    foreach ($row in $sectorRows) {
        [int]$sectorNumber = 0
        if (-not [int]::TryParse($row.Fields[0], [ref]$sectorNumber) -or $sectorNumber -ne $expectedSectorNumber) {
            throw "Sector names must be contiguous from 21; expected $expectedSectorNumber."
        }
        if ($row.Fields[1] -match '\s') {
            throw "Sector name must be one word: $($row.Fields[1])"
        }
        $expectedSectorNumber++
    }
    if (($sectorRows[-1].Fields[0] -as [int]) -lt $SectorCount) {
        throw "No sector names are available through sector $SectorCount."
    }

    $starRowsRaw = Read-TabRows -Path $starNamesSource -FieldCount 2
    $starRows = New-Object System.Collections.Generic.List[object]
    $terronName = $null
    foreach ($row in $starRowsRaw) {
        if ($row.Fields[0] -eq "Terron") {
            if ($null -ne $terronName) { throw "Duplicate Terron star row." }
            $terronName = $row.Fields[1]
            continue
        }
        [int]$starIndex = -1
        if (-not [int]::TryParse($row.Fields[0], [ref]$starIndex) -or $starIndex -ne $starRows.Count) {
            throw "Star indices must be contiguous from zero; expected $($starRows.Count)."
        }
        $serializedKey = if ($starIndex -lt 72) {
            $starIndex.ToString("D2")
        }
        else {
            "72" + ($starIndex - 72).ToString("D3")
        }
        $starRows.Add([pscustomobject]@{
            Index = $starIndex
            Key = $serializedKey
            Value = $row.Fields[1]
        })
    }
    if ($starRows.Count -ne 200 -or [string]::IsNullOrWhiteSpace($terronName)) {
        throw "The star-name source must contain exactly 200 indexed rows and Terron."
    }

    $planetRows = New-Object System.Collections.Generic.List[object]
    foreach ($row in (Read-TabRows -Path $planetNamesSource -FieldCount 3)) {
        if (-not $row.Fields[0].StartsWith("PlanetName/", [StringComparison]::Ordinal)) {
            throw "Invalid planet-name path: $($row.Fields[0])"
        }
        $planetRows.Add([pscustomobject]@{
            Race = $row.Fields[0].Substring("PlanetName/".Length)
            Key = $row.Fields[1]
            Value = $row.Fields[2]
        })
    }

    $lang = New-Object Text.StringBuilder
    [void]$lang.AppendLine("Constellations ^{")
    [void]$lang.AppendLine("    GalaxyCountStars=$StarCount")
    [void]$lang.AppendLine("    GalaxySizeX=280")
    [void]$lang.AppendLine("    GalaxySizeY=200")
    [void]$lang.AppendLine("    Name ^{")
    foreach ($row in $sectorRows) {
        [int]$number = $row.Fields[0]
        if ($number -le $SectorCount) {
            [void]$lang.AppendLine("        $number=$($row.Fields[1])")
        }
    }
    [void]$lang.AppendLine("    }")
    [void]$lang.AppendLine("    Generator ^{")
    [void]$lang.AppendLine("        SchemaVersion=1")
    [void]$lang.AppendLine("        SectorCount=$SectorCount")
    [void]$lang.AppendLine("        FirstCustomSector=20")
    [void]$lang.AppendLine("        PreserveVanillaStars=1")
    [void]$lang.AppendLine("        DistributionMode=Weighted")
    [void]$lang.AppendLine("        GeometryMode=Vanilla")
    [void]$lang.AppendLine("        RetryLimit=1")
    [void]$lang.AppendLine("        ContourFallback=1")
    # Kept as an explicit compatibility marker. Native code never enables the
    # unsafe prehistory base-scaling experiment.
    [void]$lang.AppendLine("        ScaleInitialBases=0")
    [void]$lang.AppendLine("        SectorDefaults ^{")
    [void]$lang.AppendLine("            Enabled=1")
    [void]$lang.AppendLine("            Weight=1")
    [void]$lang.AppendLine("            MinStars=0")
    [void]$lang.AppendLine("            MaxStars=0")
    [void]$lang.AppendLine("        }")
    [void]$lang.AppendLine("    }")
    [void]$lang.AppendLine("}")
    [void]$lang.AppendLine("Star ^{")
    foreach ($row in $starRows | Select-Object -First $StarCount) {
        [void]$lang.AppendLine("    $($row.Key)=$($row.Value)")
    }
    [void]$lang.AppendLine("    Terron=$terronName")
    [void]$lang.AppendLine("}")
    [void]$lang.AppendLine("PlanetName ^{")
    foreach ($raceGroup in ($planetRows | Group-Object Race)) {
        [void]$lang.AppendLine("    $($raceGroup.Name) ^{")
        foreach ($row in $raceGroup.Group) {
            [void]$lang.AppendLine("        $($row.Key)=$($row.Value)")
        }
        [void]$lang.AppendLine("    }")
    }
    [void]$lang.AppendLine("}")

    $langText = Join-Path $temporaryRoot "Lang.txt"
    $langDat = Join-Path $modOutput "CFG\Rus\Lang.dat"
    [IO.File]::WriteAllText($langText, $lang.ToString(), $unicodeWithBom)
    Invoke-ModKit @("dat", "encode", $langText, $langDat, "--overwrite")

    $verifiedLangText = Join-Path $temporaryRoot "Lang.verified.txt"
    Invoke-ModKit @("dat", "decode", $langDat, $verifiedLangText, "--overwrite")
    $verifiedText = [IO.File]::ReadAllText($verifiedLangText, [Text.Encoding]::Unicode)
    if ($verifiedText -match "GalaxyHelper" -or $verifiedText -notmatch "(?m)^\s*SectorCount=$SectorCount\s*$") {
        throw "Generated Lang.dat failed the independent-generator verification."
    }
    $rootStarMatch = [regex]::Match($verifiedText, '(?ms)^Star \^\{\r?\n.*?^\}\r?\n?')
    if (-not $rootStarMatch.Success) {
        throw "Generated Lang.dat does not contain a root Star block."
    }
    $actualStarEntries = @([regex]::Matches(
        $rootStarMatch.Value,
        '(?m)^\s+(?<key>[^=\r\n]+)=(?<value>[^\r\n]*)\r?$'))
    $expectedStarRows = @($starRows | Select-Object -First $StarCount)
    if ($actualStarEntries.Count -ne ($StarCount + 1)) {
        throw "Generated Star block has $($actualStarEntries.Count) entries instead of $($StarCount + 1)."
    }
    for ($index = 0; $index -lt $StarCount; $index++) {
        if ($actualStarEntries[$index].Groups['key'].Value.Trim() -cne $expectedStarRows[$index].Key -or
            $actualStarEntries[$index].Groups['value'].Value -cne $expectedStarRows[$index].Value) {
            throw "Generated star order differs at index $index."
        }
    }

    Invoke-ModKit @("dat", "encode", $mainTextSource, (Join-Path $modOutput "CFG\Main.dat"), "--overwrite")
    # BlockParEditor 2.1 produces CacheData files that round-trip in the editor
    # but are loaded as empty by the SRHD runtime. Preserve the game-verified
    # legacy-encoded CacheData payload and let the release audit compare it
    # against the editable source tree.
    Copy-Item -LiteralPath $cacheDataPrebuilt -Destination (Join-Path $modOutput "CFG\CacheData.dat") -Force
    Invoke-ModKit @("dat", "validate", (Join-Path $modOutput "CFG\CacheData.dat"))

    $blockPiratesRson = Join-Path $temporaryRoot "Mod_BlockPirates.adaptive.rson"
    Copy-Item -LiteralPath $blockPiratesCompatSource -Destination $blockPiratesRson -Force
    Invoke-ModKit @("script", "lint-runtime", $blockPiratesRson, "--strict")
    $blockPiratesScr = Join-Path $temporaryRoot "Mod_BlockPirates.scr"
    Invoke-ModKit @(
        "script", "build", $blockPiratesRson,
        "--scr", $blockPiratesScr,
        "--lang", (Join-Path $temporaryRoot "Mod_BlockPirates.lang.txt"),
        "--overwrite"
    )
    Copy-Item -LiteralPath $blockPiratesScr -Destination (Join-Path $modOutput "DATA\Script\Mod_BlockPirates.scr") -Force

    $convertedBackground = Join-Path $temporaryRoot "ConvertedBackground"
    New-Item -ItemType Directory -Path $convertedBackground -Force | Out-Null
    Invoke-ModKit @(
        "convert", "png-gi", $galaxySurfaceSource,
        "--output", $convertedBackground,
        "--mode", "0_32",
        "--overwrite"
    )
    $convertedGi = Join-Path $convertedBackground "VanillaGalaxySurfaceUpscaled.gi"
    if (-not (Test-Path -LiteralPath $convertedGi -PathType Leaf)) {
        throw "The converted galaxy surface was not produced: $convertedGi"
    }
    Copy-Item -LiteralPath $convertedGi -Destination (Join-Path $modOutput "DATA\BG\OpenedBG.gi") -Force
    Copy-Item -LiteralPath $galaxySceneSource -Destination (Join-Path $modOutput "DATA\BG\ClosedBG.png") -Force
    Get-ChildItem -LiteralPath $mapMarkerSource -File | Copy-Item -Destination (Join-Path $modOutput "DATA\MapMarker") -Force

    $convertedPanel = Join-Path $temporaryRoot "ConvertedPanel"
    New-Item -ItemType Directory -Path $convertedPanel -Force | Out-Null
    Invoke-ModKit @(
        "convert", "png-gi", $panelPngSource,
        "--output", $convertedPanel,
        "--mode", "2",
        "--overwrite"
    )
    $convertedPanelGi = Join-Path $convertedPanel "GalaxyPanel.gi"
    if (-not (Test-Path -LiteralPath $convertedPanelGi -PathType Leaf)) {
        throw "The converted galaxy panel was not produced: $convertedPanelGi"
    }
    Invoke-ModKit @("resource", "verify", $convertedPanelGi)
    Copy-Item -LiteralPath $convertedPanelGi -Destination (Join-Path $modOutput "DATA\Panel\GalaxyPanel.gi") -Force

    & $nativeBuildScript -OutputDirectory (Join-Path $modOutput "Native") -RangersExe $RangersExe -Rangers1Exe $Rangers1Exe
    if ($LASTEXITCODE -ne 0) {
        throw "Native plugin build failed with exit code $LASTEXITCODE."
    }
    Copy-Item -LiteralPath $generatorConfigSource -Destination (Join-Path $modOutput "Native\XenoGalaxyGenerator.XenoPlugin.ini") -Force
    $nextDayConfig = [IO.File]::ReadAllText($nextDayConfigSource, [Text.Encoding]::UTF8)
    $enabledValue = if ($DisableNextDay) { "0" } else { "1" }
    $nextDayConfig = ([regex]'(?m)^Enabled=\d+$').Replace($nextDayConfig, "Enabled=$enabledValue", 1)
    $nextDayConfig = ([regex]'(?m)^Mode=\w+$').Replace($nextDayConfig, "Mode=$NextDayMode", 1)
    [IO.File]::WriteAllText(
        (Join-Path $modOutput "Native\XenoNextDay.XenoPlugin.ini"),
        $nextDayConfig,
        $utf8NoBom)

    foreach ($datPath in @(
        (Join-Path $modOutput "CFG\CacheData.dat"),
        (Join-Path $modOutput "CFG\Main.dat"),
        $langDat
    )) {
        Invoke-ModKit @("dat", "validate", $datPath)
    }
    Invoke-ModKit @("script", "audit-mod", $modOutput)
    Invoke-ModKit @("script", "lint-runtime", $modOutput, "--strict")
    Invoke-ModKit @("audit", $modOutput, "--profile", "dev")
    Invoke-ModKit @(
        "release", "check", $modOutput,
        "--prefix", "XenoMods/XenoBigGalaxy",
        "--warnings-as-errors"
    )

    $forbiddenReleaseFiles = Get-ChildItem -LiteralPath $modOutput -Recurse -File |
        Where-Object {
            $_.Extension -in @('.cpp', '.h', '.rson', '.txt') -and
                $_.Name -ne 'ModuleInfo.txt'
        }
    if ($forbiddenReleaseFiles) {
        throw "Source files leaked into the built mod: $($forbiddenReleaseFiles.FullName -join ', ')"
    }
    $forbiddenReleaseNames = Get-ChildItem -LiteralPath $modOutput -Recurse -File |
        Where-Object { $_.FullName -match 'Swords[2-5]|GalaxyHelper|XenoBigGalaxy\.scr|empty\.gai' }
    if ($forbiddenReleaseNames) {
        throw "Removed inherited files leaked into the build: $($forbiddenReleaseNames.FullName -join ', ')"
    }

    Invoke-ModKit @(
        "release", "build", $modOutput, $releaseZip,
        "--prefix", "XenoMods/XenoBigGalaxy",
        "--warnings-as-errors",
        "--strip-sources",
        "--overwrite"
    )
    Invoke-ModKit @("manifest", $modOutput, "--output", $releaseManifest)

    Get-Item -LiteralPath $modOutput, $releaseZip, $releaseManifest |
        Select-Object FullName, Length, LastWriteTime
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        [IO.Directory]::Delete($temporaryRoot, $true)
    }
}
