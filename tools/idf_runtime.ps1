# 使用当前已激活的 ESP-IDF 环境，避免发布本机安装路径。
function Get-MoriIdfRuntime {
    if ([string]::IsNullOrWhiteSpace($env:IDF_PATH)) {
        throw '请先激活 ESP-IDF 环境，或设置 IDF_PATH。'
    }
    $runtimeIdfPath = [IO.Path]::GetFullPath($env:IDF_PATH)
    if (-not (Test-Path -LiteralPath (Join-Path $runtimeIdfPath 'tools\idf.py'))) {
        throw 'IDF_PATH 未指向有效的 ESP-IDF 安装目录。'
    }
    $runtimeToolsPath = $env:IDF_TOOLS_PATH
    if ([string]::IsNullOrWhiteSpace($runtimeToolsPath)) {
        $runtimeToolsPath = Join-Path $env:USERPROFILE '.espressif'
    }
    if (-not [string]::IsNullOrWhiteSpace($env:IDF_PYTHON_ENV_PATH)) {
        $runtimePython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
        if (-not (Test-Path -LiteralPath $runtimePython)) {
            throw 'IDF_PYTHON_ENV_PATH 中没有可用的 Python。'
        }
    } else {
        $runtimePython = (Get-Command python -ErrorAction Stop).Source
    }
    return [PSCustomObject]@{
        IdfPath = $runtimeIdfPath
        ToolsPath = [IO.Path]::GetFullPath($runtimeToolsPath)
        Python = $runtimePython
    }
}
