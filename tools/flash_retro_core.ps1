param(
    [string]$Port = "COM26",
    [switch]$NoBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$workspaceRoot = Split-Path $PSScriptRoot -Parent
$retroRoot = Join-Path (Split-Path $workspaceRoot -Parent) "retro-go-master"
$idfPath = "F:\ESP32\v5.5.1\esp-idf"
$toolsPath = "F:\ESP32\tools\v5.5.1"
$python = Join-Path $toolsPath "python_env\idf5.5_py3.14_env\Scripts\python.exe"

$env:IDF_PATH = $idfPath
$env:IDF_TOOLS_PATH = $toolsPath
$env:IDF_PYTHON_ENV_PATH = Join-Path $toolsPath "python_env\idf5.5_py3.14_env"

Push-Location $retroRoot
try {
    if (-not $NoBuild) {
        & $python "rg_tool.py" --target moriburnner build retro-core
        if ($LASTEXITCODE -ne 0) {
            throw "retro-core build failed"
        }
    }

    # rg_tool reads the partition table from the device and writes by label.
    # This targets retro-core at 0xAE0000 in the MoriBurnner dual-system layout.
    & $python "rg_tool.py" --target moriburnner --port $Port flash retro-core
    if ($LASTEXITCODE -ne 0) {
        throw "retro-core partition flash failed"
    }
}
finally {
    Pop-Location
}
