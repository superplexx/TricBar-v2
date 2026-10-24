param(
    [string]$OutputDirectory
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

$scriptDirectory = $PSScriptRoot
$root = Get-ProjectRoot -StartPath $scriptDirectory

$dist = Join-Path $root "dist"
$appBuild = if ($OutputDirectory) {
    [System.IO.Path]::GetFullPath($OutputDirectory)
} else {
    Join-Path $dist "app"
}
$launcherBin = Join-Path $root "Launcher\bin"
$launcherObj = Join-Path $root "Launcher\obj"

$appExecutables = @(
    @{ Name = "TricBarConfigurator"; File = "TricBarConfigurator.exe" },
    @{ Name = "TricBar"; File = "TricBar.exe" }
)
$runningAppProcesses = foreach ($app in $appExecutables) {
    $expectedPath = [IO.Path]::GetFullPath((Join-Path $appBuild $app.File))
    Get-Process -Name $app.Name -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Path -and
            [string]::Equals(
                [IO.Path]::GetFullPath($_.Path),
                $expectedPath,
                [StringComparison]::OrdinalIgnoreCase
            )
        }
}

if ($runningAppProcesses) {
    $processNames = ($runningAppProcesses | ForEach-Object { "$($_.ProcessName).exe" } |
        Sort-Object -Unique) -join ", "
    throw "Feche $processNames antes de compilar. O Engine mantém a DLL do hook carregada; reabra o TricBar após o build para carregar a versão atualizada."
}

foreach ($path in @($appBuild, $launcherBin, $launcherObj)) {
    if (Test-Path -LiteralPath $path) {
        try {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop
        }
        catch {
            throw "Não foi possível limpar '$path'. Feche o TricBar e outros programas que possam estar usando os arquivos de build e tente novamente. Erro: $($_.Exception.Message)"
        }
    }
}
New-Item -ItemType Directory -Path $appBuild -Force | Out-Null
New-Item -ItemType Directory -Path $dist -Force | Out-Null

$dotnetCmd = Get-Command dotnet -ErrorAction SilentlyContinue
if (-not $dotnetCmd) {
    throw "dotnet SDK não foi encontrado no PATH. Instale o .NET 8 SDK."
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe não foi encontrado. Instale o Visual Studio 2022 com WinUI 3 e Desktop C++ antes de compilar."
}

$vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsPath) {
    throw "Não foi possível localizar o MSBuild do Visual Studio."
}

$msbuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path -LiteralPath $msbuild)) {
    throw "MSBuild.exe não foi encontrado em: $msbuild"
}

Write-Host "[1/3] Compilando hook nativo..."
& (Join-Path $root "NativeHook\build.bat")
if ($LASTEXITCODE -ne 0) { throw "Falha ao compilar NativeHook\TricBar.dll" }

Write-Host "[2/3] Compilando motor C++..."
& (Join-Path $root "Engine\build.bat")
if ($LASTEXITCODE -ne 0) { throw "Falha ao compilar Engine\TricBar.exe" }

Write-Host "[3/3] Publicando configurador WinUI 3..."
$launcherProject = Join-Path $root "Launcher\TricBarLauncher.csproj"
& $msbuild $launcherProject /restore /t:Publish /p:Configuration=Release /p:RuntimeIdentifier=win-x64 /p:SelfContained=true /p:WindowsAppSDKSelfContained=true "/p:PublishDir=$appBuild\" /v:minimal
if ($LASTEXITCODE -ne 0) {
    throw "Falha ao publicar o TricBarConfigurator.exe. Verifique se o Visual Studio 2022 tem WinUI 3, Windows App SDK e Desktop C++ instalados."
}

$priFile = Get-ChildItem -LiteralPath $launcherBin -Filter "TricBarConfigurator.pri" -File -Recurse |
    Where-Object { $_.FullName -match '[\\/]Release[\\/]' } |
    Select-Object -First 1
if (-not $priFile) {
    throw "O build não gerou TricBarConfigurator.pri em $launcherBin. Verifique os erros anteriores do MSBuild; os arquivos Microsoft.UI*.pri não substituem o PRI do aplicativo."
}

$xamlFiles = @(Get-ChildItem -LiteralPath $priFile.DirectoryName -Filter "*.xbf" -File)
foreach ($requiredXaml in @("App.xbf", "MainWindow.xbf")) {
    if (-not ($xamlFiles | Where-Object { $_.Name -eq $requiredXaml })) {
        throw "O recurso XAML obrigatório $requiredXaml não foi gerado junto com TricBarConfigurator.pri."
    }
}
Copy-Item -LiteralPath $priFile.FullName -Destination $appBuild -Force
Copy-Item -LiteralPath $xamlFiles.FullName -Destination $appBuild -Force

$enginePath = Join-Path $root "Engine\bin\TricBar.exe"
$hookPath = Join-Path $root "NativeHook\bin\TricBar.dll"
if (-not (Test-Path -LiteralPath $enginePath)) { throw "TricBar.exe não foi gerado em $enginePath" }
if (-not (Test-Path -LiteralPath $hookPath)) { throw "TricBar.dll não foi gerado em $hookPath" }

Copy-Item -LiteralPath $enginePath -Destination $appBuild -Force
Copy-Item -LiteralPath $hookPath -Destination $appBuild -Force
Copy-Item -LiteralPath (Join-Path $root "icon.ico") -Destination $appBuild -Force
Copy-Item -LiteralPath (Join-Path $root "tricbartransparent.png") -Destination $appBuild -Force

$mainExe = Join-Path $appBuild "TricBarConfigurator.exe"
if (-not (Test-Path -LiteralPath $mainExe)) {
    throw "Executável do configurador não foi encontrado em $appBuild"
}

Write-Host "Build concluído. Arquivos em: $appBuild"
Write-Host "Executável: $mainExe"
