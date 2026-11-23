param(
    [string]$Action = "build"
)

$espRoot = "C:\Users\zz101\esp"
$idfPath = "$espRoot\v5.4.1\esp-idf"
$pythonExe = "$espRoot\python_env\idf5.4_py3.11_env\Scripts\python.exe"

$env:IDF_PATH = $idfPath
$env:IDF_TOOLS_PATH = $espRoot

# Activate ESP-IDF environment using the correct Python 3.11 virtualenv
$exportLines = & $pythonExe "$idfPath\tools\activate.py" --export
$scriptToSource = ($exportLines | Select-Object -Last 1).Trim()

if (Test-Path $scriptToSource) {
    . $scriptToSource
} else {
    Write-Warning "Could not find activation script: $scriptToSource"
}

# Ensure ESP_ROM_ELF_DIR is explicitly defined
if (-not $env:ESP_ROM_ELF_DIR) {
    $env:ESP_ROM_ELF_DIR = "$espRoot\tools\esp-rom-elfs\20241011"
}

Set-Location $PSScriptRoot
Write-Host ">>> Invoking ESP-IDF v5.4.1 ($Action) in $PSScriptRoot" -ForegroundColor Cyan
idf.py $Action
