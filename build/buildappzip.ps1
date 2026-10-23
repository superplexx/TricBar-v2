param(
    [string]$OutputPath
)

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
$appDirectory = Join-Path $dist "web-app"
$launcherProject = Join-Path $root "Launcher\TricBarLauncher.csproj"
$launcherBin = Join-Path $root "Launcher\bin\Release"
$outputZip = if ($OutputPath) {
    [System.IO.Path]::GetFullPath($OutputPath)
} else {
    Join-Path $dist "TricBar-v2.zip"
}

New-Item -ItemType Directory -Path $dist -Force | Out-Null
if (Test-Path -LiteralPath $appDirectory) {
    Remove-Item -LiteralPath $appDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $appDirectory -Force | Out-Null

& (Join-Path $root "NativeHook\build.bat")
if ($LASTEXITCODE -ne 0) { throw "Falha ao compilar NativeHook\TricBar.dll" }

& (Join-Path $root "Engine\build.bat")
if ($LASTEXITCODE -ne 0) { throw "Falha ao compilar Engine\TricBar.exe" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe não foi encontrado. Instale o Visual Studio com WinUI 3."
}
$vsPath = & $vswhere -latest -prerelease -products * `
    -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsPath) { throw "Não encontrei o MSBuild do Visual Studio." }
$msbuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild.exe não foi encontrado." }

& $msbuild $launcherProject /restore /t:Publish /p:Configuration=Release `
    /p:RuntimeIdentifier=win-x64 /p:SelfContained=false `
    /p:WindowsAppSDKSelfContained=false "/p:PublishDir=$appDirectory\" /v:minimal
if ($LASTEXITCODE -ne 0) {
    throw "Falha ao publicar a versão sem runtimes do TricBar."
}

$priFile = Get-ChildItem -LiteralPath $launcherBin -Filter "TricBarConfigurator.pri" -File -Recurse |
    Where-Object { $_.FullName -match '[\\/]Release[\\/]' } |
    Select-Object -First 1
if (-not $priFile) { throw "O build não gerou TricBarConfigurator.pri." }
$xamlFiles = @(Get-ChildItem -LiteralPath $priFile.DirectoryName -Filter "*.xbf" -File)
foreach ($requiredXaml in @("App.xbf", "MainWindow.xbf")) {
    if (-not ($xamlFiles | Where-Object { $_.Name -eq $requiredXaml })) {
        throw "O recurso XAML obrigatório $requiredXaml não foi gerado."
    }
}

Copy-Item -LiteralPath $priFile.FullName -Destination $appDirectory -Force
Copy-Item -LiteralPath $xamlFiles.FullName -Destination $appDirectory -Force
Copy-Item -LiteralPath (Join-Path $root "Engine\bin\TricBar.exe") -Destination $appDirectory -Force
Copy-Item -LiteralPath (Join-Path $root "NativeHook\bin\TricBar.dll") -Destination $appDirectory -Force
Copy-Item -LiteralPath (Join-Path $root "icon.ico") -Destination $appDirectory -Force
Copy-Item -LiteralPath (Join-Path $root "tricbartransparent.png") -Destination $appDirectory -Force
Get-ChildItem -LiteralPath $appDirectory -Filter "*.pdb" -File -Recurse | Remove-Item -Force

foreach ($requiredFile in @("TricBarConfigurator.exe", "TricBar.exe", "TricBar.dll", "TricBarConfigurator.pri")) {
    if (-not (Test-Path -LiteralPath (Join-Path $appDirectory $requiredFile))) {
        throw "Arquivo obrigatório ausente no pacote: $requiredFile"
    }
}

$outputDirectory = Split-Path -Parent $outputZip
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
if (Test-Path -LiteralPath $outputZip) {
    Remove-Item -LiteralPath $outputZip -Force
}
Compress-Archive -Path (Join-Path $appDirectory "*") -DestinationPath $outputZip -CompressionLevel Optimal

Write-Host "ZIP do app sem runtimes externos: $outputZip"
