param(
    [string]$OutputDir,
    [switch]$SkipBuild,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Get-ProjectRoot {
    param([string]$StartPath)
    $current = $StartPath
    while ($current) {
        $hasNativeHook = Test-Path -LiteralPath (Join-Path $current "NativeHook")
        $hasLauncher = Test-Path -LiteralPath (Join-Path $current "Launcher")
        $hasEngine = Test-Path -LiteralPath (Join-Path $current "Engine")
        if ($hasNativeHook -and $hasLauncher -and $hasEngine) {
            return $current
        }
        $parent = Split-Path -Parent $current
        if ($parent -eq $current) { break }
        $current = $parent
    }
    throw "Não foi possível localizar a raiz do TricBar (pastas Engine/NativeHook/Launcher ausentes)."
}

$root = Get-ProjectRoot -StartPath $PSScriptRoot
$setupScript = Join-Path $root "build\build.ps1"
$defaultOutputDir = Join-Path $root "dist\releases"
$targetOutputDir = if ($OutputDir) { [System.IO.Path]::GetFullPath($OutputDir) } else { $defaultOutputDir }

if (-not (Test-Path -LiteralPath $setupScript)) {
    throw "build.ps1 não foi encontrado em $setupScript. O script de setup depende dele."
}

if ($Clean) {
    if (Test-Path -LiteralPath $targetOutputDir) {
        Remove-Item -LiteralPath $targetOutputDir -Recurse -Force
    }
}

New-Item -ItemType Directory -Path $targetOutputDir -Force | Out-Null

if (-not $SkipBuild) {
    Write-Host "[buildsetup] Iniciando empacotamento do instalador..."
    & $setupScript
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao executar o build de setup em $setupScript"
    }
}

$releaseFiles = @(
    @{ Name = "TricBar-v2-LocalSetup.exe"; Label = "Instalador Local" },
    @{ Name = "TricBar-v2-WebSetup.exe"; Label = "Instalador Web" },
    @{ Name = "TricBar-v2.zip"; Label = "App ZIP para a branch webdownloads" }
)

foreach ($releaseFile in $releaseFiles) {
    $sourcePath = Join-Path $root "dist\$($releaseFile.Name)"
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        throw "$($releaseFile.Label) não foi gerado em '$sourcePath'."
    }
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $targetOutputDir $releaseFile.Name) -Force
}

Write-Host "[buildsetup] Setup concluído."
foreach ($releaseFile in $releaseFiles) {
    Write-Host "$($releaseFile.Label): $(Join-Path $targetOutputDir $releaseFile.Name)"
}
