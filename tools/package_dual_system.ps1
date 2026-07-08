param(
    [string]$PackageName = "",
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$workspaceRoot = Split-Path $PSScriptRoot -Parent

$idfPath = "F:\ESP32\v5.5.1\esp-idf"
$toolsPath = "F:\ESP32\tools\v5.5.1"
$python = Join-Path $toolsPath "python_env\idf5.5_py3.14_env\Scripts\python.exe"

$buildDir = Join-Path $workspaceRoot "build"
$distDir = Join-Path $workspaceRoot "dist"
$releaseDir = Join-Path $workspaceRoot "release"
$flashArgsPath = Join-Path $buildDir "flash_args"
$flasherArgsPath = Join-Path $buildDir "flasher_args.json"
$partitionCsvPath = Join-Path $workspaceRoot "partitions.csv"
$sizeCheckScript = Join-Path $workspaceRoot "tools\check_dual_system_sizes.py"

function Set-IdfEnvironment {
    $env:IDF_PATH = $idfPath
    $env:IDF_TOOLS_PATH = $toolsPath
    $env:IDF_PYTHON_ENV_PATH = Join-Path $toolsPath "python_env\idf5.5_py3.14_env"
    $env:ESP_ROM_ELF_DIR = Join-Path $toolsPath "tools\esp-rom-elfs\20241011"
    $env:PYTHON = $python

    $exportLines = & $python (Join-Path $idfPath "tools\idf_tools.py") export --format key-value
    foreach ($line in $exportLines) {
        if ($line -notmatch '^[A-Z0-9_]+=') {
            continue
        }

        $name, $value = $line -split '=', 2
        if ($name -eq "PATH") {
            $value = $value -replace ';%PATH%$', ''
            $env:PATH = "$value;$env:PATH"
        }
        else {
            Set-Item -Path "Env:$name" -Value $value
        }
    }
}

function Invoke-Step {
    param(
        [string]$Title,
        [scriptblock]$Action
    )

    Write-Host ""
    Write-Host "== $Title ==" -ForegroundColor Cyan
    & $Action
}

function Assert-File {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing file: $Path"
    }
}

function New-TextFile {
    param(
        [string]$Path,
        [string[]]$Lines
    )

    Set-Content -LiteralPath $Path -Value $Lines -Encoding ASCII
}

function Resolve-BuildArtifactPath {
    param(
        [string]$BaseDir,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($BaseDir) -or [string]::IsNullOrWhiteSpace($Path)) {
        throw "Invalid build artifact path"
    }

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $BaseDir $Path))
}

function Get-FlasherEntry {
    param(
        [pscustomobject]$FlasherArgs,
        [string]$BaseDir,
        [string]$Name,
        [string]$PackageRelativePath,
        [string]$ReadmeLabel
    )

    $property = $FlasherArgs.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "Missing flasher_args.json entry: $Name"
    }

    $entry = $property.Value
    if ($null -eq $entry -or
        [string]::IsNullOrWhiteSpace([string]$entry.offset) -or
        [string]::IsNullOrWhiteSpace([string]$entry.file)) {
        throw "Invalid flasher_args.json entry: $Name"
    }

    return [PSCustomObject]@{
        Name = $Name
        Offset = ([string]$entry.offset).ToLowerInvariant()
        SourcePath = Resolve-BuildArtifactPath -BaseDir $BaseDir -Path ([string]$entry.file)
        PackageRelativePath = $PackageRelativePath
        ReadmeLabel = $ReadmeLabel
        Encrypted = [string]$entry.encrypted
    }
}

function Read-FlashArgsPlan {
    param(
        [string]$Path,
        [string]$BaseDir
    )

    $lines = Get-Content -LiteralPath $Path | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    if ($lines.Count -lt 2) {
        throw "Invalid flash_args file: $Path"
    }

    $writeFlashArgs = @(([string]$lines[0]).Trim() -split '\s+')
    $entries = @()

    for ($index = 1; $index -lt $lines.Count; $index++) {
        $line = ([string]$lines[$index]).Trim()
        $parts = $line -split '\s+', 2
        if ($parts.Count -ne 2) {
            throw "Invalid flash_args entry: $line"
        }

        $entries += [PSCustomObject]@{
            Offset = ([string]$parts[0]).ToLowerInvariant()
            BuildRelativePath = [string]$parts[1]
            SourcePath = Resolve-BuildArtifactPath -BaseDir $BaseDir -Path ([string]$parts[1])
        }
    }

    return [PSCustomObject]@{
        WriteFlashArgs = $writeFlashArgs
        Entries = $entries
    }
}

function Get-PackageEntryMetadata {
    param([string]$BuildRelativePath)

    $normalized = $BuildRelativePath.Replace('/', '\').Trim().ToLowerInvariant()
    switch ($normalized) {
        "bootloader\bootloader.bin" {
            return [PSCustomObject]@{
                Name = "bootloader"
                PackageRelativePath = "bootloader\bootloader.bin"
                ReadmeLabel = "bootloader"
            }
        }
        "moriburnner.bin" {
            return [PSCustomObject]@{
                Name = "app"
                PackageRelativePath = "moriburnner.bin"
                ReadmeLabel = "factory app"
            }
        }
        "partition_table\partition-table.bin" {
            return [PSCustomObject]@{
                Name = "partition-table"
                PackageRelativePath = "partition_table\partition-table.bin"
                ReadmeLabel = "partition table"
            }
        }
        "ota_data_initial.bin" {
            return [PSCustomObject]@{
                Name = "otadata"
                PackageRelativePath = "ota_data_initial.bin"
                ReadmeLabel = "ota data"
            }
        }
        "assets.bin" {
            return [PSCustomObject]@{
                Name = "assets"
                PackageRelativePath = "assets.bin"
                ReadmeLabel = "assets"
            }
        }
        "..\..\retro-go-master\launcher\build\launcher.bin" {
            return [PSCustomObject]@{
                Name = "launcher"
                PackageRelativePath = "launcher.bin"
                ReadmeLabel = "retro-go launcher"
            }
        }
        "..\..\retro-go-master\retro-core\build\retro-core.bin" {
            return [PSCustomObject]@{
                Name = "retro-core"
                PackageRelativePath = "retro-core.bin"
                ReadmeLabel = "retro-core"
            }
        }
        "..\..\retro-go-master\gwenesis\build\gwenesis.bin" {
            return [PSCustomObject]@{
                Name = "gwenesis"
                PackageRelativePath = "gwenesis.bin"
                ReadmeLabel = "gwenesis"
            }
        }
        "..\..\retro-go-master\prboom-go\build\prboom-go.bin" {
            return [PSCustomObject]@{
                Name = "prboom-go"
                PackageRelativePath = "prboom-go.bin"
                ReadmeLabel = "prboom-go"
            }
        }
        "..\..\retro-go-master\fmsx\build\fmsx.bin" {
            return [PSCustomObject]@{
                Name = "fmsx"
                PackageRelativePath = "fmsx.bin"
                ReadmeLabel = "fmsx"
            }
        }
        default {
            throw "Unsupported flash_args entry: $BuildRelativePath"
        }
    }
}

function New-FlashCommandText {
    param(
        [pscustomobject]$FlasherArgs,
        [string]$PortToken,
        [string[]]$WriteFlashArgs,
        [object[]]$Entries
    )

    $chip = [string]$FlasherArgs.extra_esptool_args.chip
    if ([string]::IsNullOrWhiteSpace($chip)) {
        $chip = "esp32s3"
    }

    $parts = @(
        "py -m esptool",
        "--chip", $chip,
        "--port", $PortToken,
        "--baud", "460800"
    )

    $before = [string]$FlasherArgs.extra_esptool_args.before
    if (-not [string]::IsNullOrWhiteSpace($before)) {
        $parts += @("--before", $before)
    }

    $after = [string]$FlasherArgs.extra_esptool_args.after
    if (-not [string]::IsNullOrWhiteSpace($after)) {
        $parts += @("--after", $after)
    }

    $stubProperty = $FlasherArgs.extra_esptool_args.PSObject.Properties["stub"]
    if ($null -ne $stubProperty -and -not [bool]$stubProperty.Value) {
        $parts += "--no-stub"
    }

    $parts += "write_flash"
    $parts += @($WriteFlashArgs | ForEach-Object { [string]$_ })

    foreach ($entry in $Entries) {
        $parts += @($entry.Offset, $entry.PackageRelativePath)
    }

    return ($parts -join ' ')
}

function New-MergedFlashCommandText {
    param(
        [pscustomobject]$FlasherArgs,
        [string]$PortToken,
        [string]$MergedImageName
    )

    $chip = [string]$FlasherArgs.extra_esptool_args.chip
    if ([string]::IsNullOrWhiteSpace($chip)) {
        $chip = "esp32s3"
    }

    $parts = @(
        "py -m esptool",
        "--chip", $chip,
        "--port", $PortToken,
        "--baud", "460800"
    )

    $before = [string]$FlasherArgs.extra_esptool_args.before
    if (-not [string]::IsNullOrWhiteSpace($before)) {
        $parts += @("--before", $before)
    }

    $after = [string]$FlasherArgs.extra_esptool_args.after
    if (-not [string]::IsNullOrWhiteSpace($after)) {
        $parts += @("--after", $after)
    }

    $stubProperty = $FlasherArgs.extra_esptool_args.PSObject.Properties["stub"]
    if ($null -ne $stubProperty -and -not [bool]$stubProperty.Value) {
        $parts += "--no-stub"
    }

    $parts += @(
        "write_flash",
        "--flash_mode", [string]$FlasherArgs.flash_settings.flash_mode,
        "--flash_freq", [string]$FlasherArgs.flash_settings.flash_freq,
        "--flash_size", [string]$FlasherArgs.flash_settings.flash_size,
        "0x0", $MergedImageName
    )

    return ($parts -join ' ')
}

function New-PackageFlasherArgsObject {
    param(
        [pscustomobject]$BuildFlasherArgs,
        [string[]]$WriteFlashArgs,
        [object[]]$Entries
    )

    $flashFiles = [ordered]@{}
    foreach ($entry in $Entries) {
        $flashFiles[$entry.Offset] = ($entry.PackageRelativePath -replace '\\', '/')
    }

    $jsonObject = [ordered]@{
        write_flash_args = @($WriteFlashArgs | ForEach-Object { [string]$_ })
        flash_settings = [ordered]@{
            flash_mode = [string]$BuildFlasherArgs.flash_settings.flash_mode
            flash_size = [string]$BuildFlasherArgs.flash_settings.flash_size
            flash_freq = [string]$BuildFlasherArgs.flash_settings.flash_freq
        }
        flash_files = $flashFiles
    }

    foreach ($entry in $Entries) {
        $jsonObject[$entry.Name] = [ordered]@{
            offset = $entry.Offset
            file = ($entry.PackageRelativePath -replace '\\', '/')
            encrypted = $entry.Encrypted
        }
    }

    $jsonObject["extra_esptool_args"] = [ordered]@{
        after = [string]$BuildFlasherArgs.extra_esptool_args.after
        before = [string]$BuildFlasherArgs.extra_esptool_args.before
        stub = [bool]$BuildFlasherArgs.extra_esptool_args.stub
        chip = [string]$BuildFlasherArgs.extra_esptool_args.chip
    }

    return $jsonObject
}

Set-IdfEnvironment

if (-not $SkipBuild) {
    Invoke-Step "Build latest dual-system binaries" {
        & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "flash_dual_system.ps1") -BuildOnly
    }
}

Assert-File $flashArgsPath
Assert-File $flasherArgsPath
Assert-File $partitionCsvPath
Assert-File $sizeCheckScript

$flasherArgs = Get-Content -Raw -LiteralPath $flasherArgsPath | ConvertFrom-Json
$flashPlan = Read-FlashArgsPlan -Path $flashArgsPath -BaseDir $buildDir
$flashMode = [string]$flasherArgs.flash_settings.flash_mode
$flashFreq = [string]$flasherArgs.flash_settings.flash_freq
$flashSize = [string]$flasherArgs.flash_settings.flash_size

if ([string]::IsNullOrWhiteSpace($flashMode) -or
    [string]::IsNullOrWhiteSpace($flashFreq) -or
    [string]::IsNullOrWhiteSpace($flashSize)) {
    throw "Invalid flasher_args.json flash settings"
}

$flashEntries = @()
foreach ($planEntry in $flashPlan.Entries) {
    $metadata = Get-PackageEntryMetadata -BuildRelativePath $planEntry.BuildRelativePath
    $namedEntry = Get-FlasherEntry `
        -FlasherArgs $flasherArgs `
        -BaseDir $buildDir `
        -Name $metadata.Name `
        -PackageRelativePath $metadata.PackageRelativePath `
        -ReadmeLabel $metadata.ReadmeLabel

    if ($namedEntry.Offset -ne $planEntry.Offset) {
        throw "flash_args and flasher_args.json offset mismatch for $($metadata.Name): $($planEntry.Offset) vs $($namedEntry.Offset)"
    }

    if ($namedEntry.SourcePath.ToLowerInvariant() -ne $planEntry.SourcePath.ToLowerInvariant()) {
        throw "flash_args and flasher_args.json path mismatch for $($metadata.Name)"
    }

    $flashEntries += [PSCustomObject]@{
        Name = $metadata.Name
        Offset = $planEntry.Offset
        SourcePath = $planEntry.SourcePath
        PackageRelativePath = $metadata.PackageRelativePath
        ReadmeLabel = $metadata.ReadmeLabel
        Encrypted = $namedEntry.Encrypted
    }
}

foreach ($entry in $flashEntries) {
    Assert-File $entry.SourcePath
}

Invoke-Step "Validate dual-system partition sizes" {
    & $python $sizeCheckScript `
        --partition-csv $partitionCsvPath `
        --flash-args $flashArgsPath `
        --flash-size $flashSize
}

$projectInfo = Get-Content -Raw -LiteralPath (Join-Path $buildDir "project_description.json") | ConvertFrom-Json
$projectVersion = $projectInfo.project_version
$shortCommit = (& git -C $workspaceRoot rev-parse --short HEAD).Trim()
& git -C $workspaceRoot diff --quiet
$sourceState = if ($LASTEXITCODE -eq 0) { "clean" } else { "dirty" }
$stamp = Get-Date -Format "yyyyMMdd_HHmm"
$stampReadable = Get-Date -Format "yyyy-MM-dd HH:mm"
$safeProjectVersion = ($projectVersion -replace '[<>:"/\\|?*]', '_')

if ([string]::IsNullOrWhiteSpace($PackageName)) {
    $PackageName = "moriburnner_dualsystem_fullflash_${stamp}_${shortCommit}_${sourceState}"
}

$packageDir = Join-Path $distDir $PackageName
$zipPath = Join-Path $distDir ($PackageName + ".zip")
$mergedName = "moriburnner_dualsystem_fullflash_merged.bin"
$mergedPath = Join-Path $packageDir $mergedName
$releaseMergedName = "moriburnner-dualsystem-full-${safeProjectVersion}-${stamp}.bin"
$releaseMergedPath = Join-Path $releaseDir $releaseMergedName

if (Test-Path -LiteralPath $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

New-Item -ItemType Directory -Path $packageDir | Out-Null
New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null

Invoke-Step "Copy firmware files into package" {
    foreach ($entry in $flashEntries) {
        $destination = Join-Path $packageDir $entry.PackageRelativePath
        $destinationDir = Split-Path -Parent $destination

        if (-not [string]::IsNullOrWhiteSpace($destinationDir)) {
            New-Item -ItemType Directory -Path $destinationDir -Force | Out-Null
        }

        Copy-Item -LiteralPath $entry.SourcePath -Destination $destination
    }

    $packageFlashArgsLines = @(
        ($flashPlan.WriteFlashArgs -join ' ')
    )
    $packageFlashArgsLines += ($flashEntries | ForEach-Object {
        "{0} {1}" -f $_.Offset, ($_.PackageRelativePath -replace '\\', '/')
    })
    New-TextFile -Path (Join-Path $packageDir "flash_args") -Lines $packageFlashArgsLines

    $packageFlasherArgs = New-PackageFlasherArgsObject -BuildFlasherArgs $flasherArgs -WriteFlashArgs $flashPlan.WriteFlashArgs -Entries $flashEntries
    $packageFlasherArgs | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $packageDir "flasher_args.json") -Encoding ASCII
}

Invoke-Step "Merge full flash image" {
    $mergeArgs = @(
        "-m", "esptool",
        "--chip", "esp32s3",
        "merge_bin",
        "--flash_mode", $flashMode,
        "--flash_freq", $flashFreq,
        "--flash_size", $flashSize,
        "--fill-flash-size", $flashSize,
        "--output", $mergedPath
    )

    foreach ($entry in $flashEntries) {
        $mergeArgs += $entry.Offset
        $mergeArgs += (Join-Path $packageDir $entry.PackageRelativePath)
    }

    & $python @mergeArgs

    Copy-Item -LiteralPath $mergedPath -Destination (Join-Path $packageDir "FULL.bin")
    Copy-Item -LiteralPath $mergedPath -Destination (Join-Path $packageDir "fullflash-single.bin")
    Copy-Item -LiteralPath $mergedPath -Destination $releaseMergedPath
}

$multiFlashCmd = New-FlashCommandText -FlasherArgs $flasherArgs -PortToken "%PORT%" -WriteFlashArgs $flashPlan.WriteFlashArgs -Entries $flashEntries
$mergedFlashCmd = New-MergedFlashCommandText -FlasherArgs $flasherArgs -PortToken "%PORT%" -MergedImageName $mergedName

New-TextFile -Path (Join-Path $packageDir "flash_full.cmd") -Lines @(
    "@echo off",
    "set PORT=%1",
    "if ""%PORT%""=="""" set PORT=COM6",
    "py -m esptool version >nul 2>nul",
    "if errorlevel 1 goto idfpy",
    $multiFlashCmd,
    "goto done",
    ":idfpy",
    "set IDFPY=%USERPROFILE%\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe",
    "if not exist ""%IDFPY%"" (",
    "  echo esptool not found. Please run this from ESP-IDF shell or install esptool for py.",
    "  pause",
    "  exit /b 1",
    ")",
    ('"%IDFPY%" ' + ($multiFlashCmd -replace '^py ', '')),
    ":done",
    "pause"
)

New-TextFile -Path (Join-Path $packageDir "flash_merged.cmd") -Lines @(
    "@echo off",
    "set PORT=%1",
    "if ""%PORT%""=="""" set PORT=COM6",
    "py -m esptool version >nul 2>nul",
    "if errorlevel 1 goto idfpy",
    $mergedFlashCmd,
    "goto done",
    ":idfpy",
    "set IDFPY=%USERPROFILE%\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe",
    "if not exist ""%IDFPY%"" (",
    "  echo esptool not found. Please run this from ESP-IDF shell or install esptool for py.",
    "  pause",
    "  exit /b 1",
    ")",
    ('"%IDFPY%" ' + ($mergedFlashCmd -replace '^py ', '')),
    ":done",
    "pause"
)

New-TextFile -Path (Join-Path $packageDir "flash_command.txt") -Lines @(
    $multiFlashCmd,
    $mergedFlashCmd
)

New-TextFile -Path (Join-Path $packageDir "README.txt") -Lines @(
    "MoriBurnner ESP32-S3 dual-system full flash package",
    "Build date:",
    "- $stampReadable",
    "Build source:",
    "- app version: $projectVersion",
    "- commit: $shortCommit",
    "- source state: $sourceState working tree",
    "Included images:",
    ("- {0}: {1} @ {2}" -f $flashEntries[0].ReadmeLabel, (Split-Path -Leaf $flashEntries[0].PackageRelativePath), $flashEntries[0].Offset),
    ("- {0}: {1} @ {2}" -f $flashEntries[1].ReadmeLabel, (Split-Path -Leaf $flashEntries[1].PackageRelativePath), $flashEntries[1].Offset),
    ("- {0}: {1} @ {2}" -f $flashEntries[2].ReadmeLabel, (Split-Path -Leaf $flashEntries[2].PackageRelativePath), $flashEntries[2].Offset),
    ("- {0}: {1} @ {2}" -f $flashEntries[3].ReadmeLabel, (Split-Path -Leaf $flashEntries[3].PackageRelativePath), $flashEntries[3].Offset),
    ("- {0}: {1} @ {2}" -f $flashEntries[4].ReadmeLabel, (Split-Path -Leaf $flashEntries[4].PackageRelativePath), $flashEntries[4].Offset),
    ("- {0}: {1} @ {2}" -f $flashEntries[5].ReadmeLabel, (Split-Path -Leaf $flashEntries[5].PackageRelativePath), $flashEntries[5].Offset),
    ("- {0}: {1} @ {2}" -f $flashEntries[6].ReadmeLabel, (Split-Path -Leaf $flashEntries[6].PackageRelativePath), $flashEntries[6].Offset),
    ("- {0}: {1} @ {2}" -f $flashEntries[7].ReadmeLabel, (Split-Path -Leaf $flashEntries[7].PackageRelativePath), $flashEntries[7].Offset),
    ("- {0}: {1} @ {2}" -f $flashEntries[8].ReadmeLabel, (Split-Path -Leaf $flashEntries[8].PackageRelativePath), $flashEntries[8].Offset),
    ("- {0}: {1} @ {2}" -f $flashEntries[9].ReadmeLabel, (Split-Path -Leaf $flashEntries[9].PackageRelativePath), $flashEntries[9].Offset),
    "Flash options:",
    "- multi-file full flash: flash_full.cmd COM6",
    "- one-file merged full flash: flash_merged.cmd COM6",
    "Notes:",
    "- this is the complete dual-system package",
    "- package flash order and addresses follow build/flash_args generated by the current build",
    "- package flasher_args.json is rewritten to package-local paths so it stays self-contained",
    "- flashing FULL.bin or moriburnner_dualsystem_fullflash_merged.bin writes the whole 16MB image",
    "- replace COM6 with your actual port"
)

Invoke-Step "Generate SHA256SUMS" {
    $hashFiles = @()
    $hashFiles += ($flashEntries | ForEach-Object { $_.PackageRelativePath })
    $hashFiles += @(
        $mergedName,
        "FULL.bin",
        "fullflash-single.bin"
    )

    $hashLines = foreach ($relativePath in $hashFiles) {
        $absolutePath = Join-Path $packageDir $relativePath
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $absolutePath).Hash.ToLowerInvariant()
        "$hash *$relativePath"
    }

    New-TextFile -Path (Join-Path $packageDir "SHA256SUMS.txt") -Lines $hashLines
}

Invoke-Step "Create zip package" {
    Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $zipPath -Force
}

Write-Host ""
Write-Host "Package ready:" -ForegroundColor Green
Write-Host "  $packageDir"
Write-Host "  $zipPath"
Write-Host "  $releaseMergedPath"
