$ErrorActionPreference = "Stop"

function Get-ProjectRoot {
    param([string]$StartPath)
    $current = $StartPath
    while ($current) {
        if ((Test-Path -LiteralPath (Join-Path $current "NativeHook")) -and
            (Test-Path -LiteralPath (Join-Path $current "Launcher")) -and
            (Test-Path -LiteralPath (Join-Path $current "Engine"))) {
            return $current
        }
        $parent = Split-Path -Parent $current
        if ($parent -eq $current) { break }
        $current = $parent
    }
    throw "Não foi possível localizar a raiz do TricBar."
}

$root = Get-ProjectRoot -StartPath $PSScriptRoot
$dist = Join-Path $root "dist"
$localBundle = Join-Path $dist "TricBar-v2-LocalBundle.zip"
$setupProject = Join-Path $root "Installer\TricBarSetup.csproj"
$publishRoot = Join-Path $root "Installer\obj\SetupPublish"

Write-Host "[1/5] Compilando o app local completo..."
& (Join-Path $root "build\buildapp.ps1")
if ($LASTEXITCODE -ne 0) { throw "Falha ao compilar a versão local do TricBar." }

Write-Host "[2/5] Criando o pacote autocontido para o instalador Local..."
$localApp = Join-Path $dist "app"
if (-not (Test-Path -LiteralPath (Join-Path $localApp "TricBarConfigurator.exe"))) {
    throw "O app autocontido não foi gerado em $localApp."
}
Get-ChildItem -LiteralPath $localApp -Filter "*.pdb" -File -Recurse | Remove-Item -Force
if (Test-Path -LiteralPath $localBundle) { Remove-Item -LiteralPath $localBundle -Force }
Compress-Archive -Path (Join-Path $localApp "*") -DestinationPath $localBundle -CompressionLevel Optimal

Write-Host "[3/5] Criando ZIP sem runtimes externos para a branch webdownloads..."
& (Join-Path $root "build\buildappzip.ps1")
if ($LASTEXITCODE -ne 0) { throw "Falha ao criar dist\TricBar-v2.zip." }

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe não foi encontrado. Instale o Visual Studio com WinUI 3."
}
$vsPath = & $vswhere -latest -prerelease -products * `
    -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsPath) { throw "Não encontrei o MSBuild do Visual Studio." }
$msbuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild.exe não foi encontrado." }

New-Item -ItemType Directory -Path $dist -Force | Out-Null
New-Item -ItemType Directory -Path $publishRoot -Force | Out-Null
$localPublish = Join-Path $publishRoot "Local"
$webPublish = Join-Path $publishRoot "Web"
foreach ($path in @($localPublish, $webPublish)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    New-Item -ItemType Directory -Path $path -Force | Out-Null
}

Write-Host "[4/5] Publicando TricBar-v2-LocalSetup.exe..."
& $msbuild $setupProject /restore /t:Publish /p:Configuration=Release `
    /p:RuntimeIdentifier=win-x64 /p:SelfContained=true /p:PublishSingleFile=true `
    /p:TricBarWebSetup=false "/p:AppBundleArchivePath=$localBundle" `
    "/p:PublishDir=$localPublish\" /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Falha ao publicar o instalador Local." }
$localSetup = Join-Path $localPublish "TricBarSetup.exe"
if (-not (Test-Path -LiteralPath $localSetup)) { throw "O instalador Local não foi gerado." }
Copy-Item -LiteralPath $localSetup -Destination (Join-Path $dist "TricBar-v2-LocalSetup.exe") -Force

Write-Host "[5/5] Compilando o bootstrapper nativo do WebSetup..."
& (Join-Path $root "build\WebSetupBootstrap\build.bat")
if ($LASTEXITCODE -ne 0) { throw "Falha ao compilar o bootstrapper nativo do instalador Web." }
$webSetup = Join-Path $dist "TricBar-v2-WebSetup.exe"
if (-not (Test-Path -LiteralPath $webSetup)) { throw "O instalador Web nativo não foi gerado." }

Write-Host "Instalador Local: $(Join-Path $dist 'TricBar-v2-LocalSetup.exe')"
Write-Host "Instalador Web:   $(Join-Path $dist 'TricBar-v2-WebSetup.exe')"
