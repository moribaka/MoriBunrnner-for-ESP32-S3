param(
    [Parameter(Mandatory=$true)][ValidatePattern('^v?\d+\.\d+\.\d+$')][string]$Version,
    [string]$PythonExe = 'F:/ESP32/tools/v5.5.1/python_env/idf5.5_py3.14_env/Scripts/python.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $repo 'build'
$releaseDir = Join-Path $repo 'release'
$tag = 'v' + $Version.TrimStart('v')
$fullName = "ESP32_FULL_$tag.bin"
$batchName = "AG32_BATCH_$tag.bin"
$fullPath = Join-Path $buildDir $fullName
$batchPath = Join-Path $repo 'example/moriburnner_ag32_batch.bin'
$zipPath = Join-Path $releaseDir "MoriBurnner_$tag.zip"
$description = Get-Content (Join-Path $buildDir 'project_description.json') -Raw | ConvertFrom-Json
if ($description.project_version -ne $tag) {
    throw "Build version $($description.project_version) does not match package $tag"
}
if (Test-Path -LiteralPath $zipPath) { throw "Package already exists: $zipPath" }
$config = Get-Content (Join-Path $buildDir 'flasher_args.json') -Raw | ConvertFrom-Json
$mergeArgs = @('-m','esptool','--chip','esp32s3','merge_bin','-o',$fullPath,
    '--flash_mode','dio','--flash_freq','80m','--flash_size','16MB','--fill-flash-size','16MB')
foreach ($segment in $config.flash_files.PSObject.Properties) { $mergeArgs += @($segment.Name,$segment.Value) }
Push-Location $buildDir
try {
    & $PythonExe @mergeArgs
    if ($LASTEXITCODE -ne 0) { throw 'esptool merge failed' }
} finally { Pop-Location }
$full = [IO.File]::ReadAllBytes($fullPath)
if ($full.Length -ne 16777216) { throw 'Full image must be 16 MiB' }
$hash = [Security.Cryptography.SHA256]::Create()
try {
    $ranges = @()
    foreach ($segment in $config.flash_files.PSObject.Properties) {
        $payload = [IO.File]::ReadAllBytes([IO.Path]::GetFullPath((Join-Path $buildDir $segment.Value)))
        $address = [Convert]::ToInt32($segment.Name.Substring(2),16)
        if ($address + $payload.Length -gt $full.Length) { throw 'Segment outside flash' }
        $actual = [BitConverter]::ToString($hash.ComputeHash($full,$address,$payload.Length))
        if ($actual -ne [BitConverter]::ToString($hash.ComputeHash($payload))) { throw "Segment mismatch: $($segment.Value)" }
        $ranges += [PSCustomObject]@{Start=$address;End=$address+$payload.Length}
    }
    $ranges = @($ranges | Sort-Object Start)
    if ($ranges.Count -ne 10) { throw 'Expected all 10 dual-system flash segments' }
    for ($i=1;$i -lt $ranges.Count;$i++) {
        if ($ranges[$i-1].End -gt $ranges[$i].Start) { throw 'Overlapping flash segments' }
    }
    [IO.Directory]::CreateDirectory($releaseDir) | Out-Null
    $zip = [IO.Compression.ZipFile]::Open($zipPath,[IO.Compression.ZipArchiveMode]::Create)
    try {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$fullPath,$fullName) | Out-Null
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$batchPath,$batchName) | Out-Null
    } finally { $zip.Dispose() }
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        if ($zip.Entries.Count -ne 2) { throw 'ZIP must contain exactly two BIN files' }
        foreach ($entry in $zip.Entries) {
            $stream=$entry.Open()
            try { $actual=[BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','') } finally { $stream.Dispose() }
            $source=if($entry.Name -eq $fullName){$fullPath}else{$batchPath}
            if ($actual -ne (Get-FileHash $source -Algorithm SHA256).Hash) { throw 'ZIP readback mismatch' }
            [PSCustomObject]@{Name=$entry.Name;Bytes=$entry.Length;SHA256=$actual} | ConvertTo-Json -Compress
        }
    } finally { $zip.Dispose() }
} finally { $hash.Dispose() }
Get-FileHash $zipPath -Algorithm SHA256 | Format-List
