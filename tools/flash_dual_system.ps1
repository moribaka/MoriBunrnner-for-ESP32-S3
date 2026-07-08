param(
    [string]$Port = "COM6",
    [switch]$BuildOnly,
    [switch]$FlashOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$workspaceRoot = Split-Path $PSScriptRoot -Parent
$retroRoot = Join-Path (Split-Path $workspaceRoot -Parent) "retro-go-master"
$sizeCheckScript = Join-Path $workspaceRoot "tools\check_dual_system_sizes.py"
$partitionCsvPath = Join-Path $workspaceRoot "partitions.csv"

$idfPath = "F:\ESP32\v5.5.1\esp-idf"
$toolsPath = "F:\ESP32\tools\v5.5.1"
$python = Join-Path $toolsPath "python_env\idf5.5_py3.14_env\Scripts\python.exe"

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

function Invoke-DualSystemSizeValidation {
    $flashArgsPath = Join-Path $workspaceRoot "build\flash_args"

    Assert-File $sizeCheckScript
    Assert-File $partitionCsvPath
    Assert-File $flashArgsPath

    Invoke-Step "Validate dual-system partition sizes" {
        & $python $sizeCheckScript `
            --partition-csv $partitionCsvPath `
            --flash-args $flashArgsPath `
            --flash-size "16MB"
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

function New-EsptoolWriteFlashArguments {
    param(
        [pscustomobject]$FlasherArgs,
        [pscustomobject]$FlashPlan,
        [string]$Port
    )

    $chip = [string]$FlasherArgs.extra_esptool_args.chip
    if ([string]::IsNullOrWhiteSpace($chip)) {
        $chip = "esp32s3"
    }

    $args = @(
        "--chip", $chip,
        "-p", $Port,
        "-b", "460800"
    )

    $before = [string]$FlasherArgs.extra_esptool_args.before
    if (-not [string]::IsNullOrWhiteSpace($before)) {
        $args += @("--before", $before)
    }

    $after = [string]$FlasherArgs.extra_esptool_args.after
    if (-not [string]::IsNullOrWhiteSpace($after)) {
        $args += @("--after", $after)
    }

    $stubProperty = $FlasherArgs.extra_esptool_args.PSObject.Properties["stub"]
    if ($null -ne $stubProperty -and -not [bool]$stubProperty.Value) {
        $args += "--no-stub"
    }

    $args += "write_flash"
    $args += @($FlashPlan.WriteFlashArgs | ForEach-Object { [string]$_ })

    foreach ($entry in $FlashPlan.Entries) {
        $args += @($entry.Offset, $entry.SourcePath)
    }

    return $args
}

Set-IdfEnvironment

if (-not $FlashOnly) {
    Invoke-Step "Build moriburnner" {
        Push-Location $workspaceRoot
        try {
            & $python (Join-Path $idfPath "tools\idf.py") build
        }
        finally {
            Pop-Location
        }
    }

    Invoke-Step "Build retro-go apps" {
        Push-Location $retroRoot
        try {
            & $python "rg_tool.py" --target moriburnner build launcher retro-core gwenesis prboom-go fmsx
        }
        finally {
            Pop-Location
        }
    }

    Invoke-DualSystemSizeValidation
}

if (-not $BuildOnly) {
    $moriburnnerBuild = Join-Path $workspaceRoot "build"
    $flashArgsPath = Join-Path $moriburnnerBuild "flash_args"
    $flasherArgsPath = Join-Path $moriburnnerBuild "flasher_args.json"

    Assert-File $flashArgsPath
    Assert-File $flasherArgsPath

    $flashPlan = Read-FlashArgsPlan -Path $flashArgsPath -BaseDir $moriburnnerBuild
    $flasherArgs = Get-Content -Raw -LiteralPath $flasherArgsPath | ConvertFrom-Json

    foreach ($entry in $flashPlan.Entries) {
        Assert-File $entry.SourcePath
    }

    $esptoolArgs = New-EsptoolWriteFlashArguments -FlasherArgs $flasherArgs -FlashPlan $flashPlan -Port $Port

    Invoke-Step "Flash dual system to $Port" {
        & $python -m esptool @esptoolArgs
    }

    Write-Host ""
    Write-Host "Dual-system flash complete. You can now boot moriburnner and open Retro-Go from the desktop." -ForegroundColor Green
}
