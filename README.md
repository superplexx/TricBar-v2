# TricBar v2

[🇺🇸 English (US) Version](#-english-us)

O TricBar personaliza a aparência da barra de tarefas do Windows com efeitos Blur, Acrylic, Liquid Glass e cor sólida. O configurador permite escolher o modo visual, ajustar a cor e a opacidade e configurar a inicialização com o Windows

## Requisitos

- Windows x64
- Windows 10 (versão mínima 1809) ou Windows 11.
- Os modos **Liquid Glass Claro** e **Liquid Glass Escuro** requerem Windows 11 22H2 ou posterior. Em versões anteriores, o app usa Acrylic como alternativa

## Instalação

Baixe os instaladores na seção **Releases** do GitHub:

- **TricBar-v2-LocalSetup.exe** — instalador autocontido; inclui os arquivos do aplicativo
- **TricBar-v2-WebSetup.exe** — bootstrapper autocontido; abre sem exigir .NET previamente instalado e baixa os componentes necessários e o pacote do aplicativo durante a instalação

O WebSetup inclui o runtime .NET usado pelo próprio instalador, portanto pode iniciar em uma máquina sem .NET. Para manter o aplicativo final menor, o instalador ainda baixa e instala o **.NET 8 Desktop Runtime**, o **Windows App Runtime 1.6** e o pacote do TricBar durante o processo. Como o runtime do instalador é incluído no executável, o WebSetup fica maior que um bootstrapper dependente de .NET instalado previamente.

O WebSetup baixa o pacote do aplicativo de [`TricBar-v2.zip` na branch `webdownloads`](https://raw.githubusercontent.com/superplexx/TricBar-v2/webdownloads/TricBar-v2.zip), além dos instaladores oficiais do .NET Desktop Runtime 8 e do Windows App Runtime 1.6.

## Compilar no Windows

### Ferramentas necessárias

- .NET SDK 8 (a versão do repositório está definida em [`global.json`](global.json))
- Visual Studio 2022 com MSBuild
- Workload/componentes para WinUI 3 e Windows App SDK
- Ferramentas de desenvolvimento **Desktop development with C++** para compilar o hook e o motor nativos

Feche qualquer instância do TricBar antes de compilar e execute no PowerShell, a partir da raiz do repositório:

```powershell
.\build\buildsetup.ps1
```

O script compila o aplicativo e os dois instaladores. Os artefatos ficam em `dist\releases\`:

- `TricBar-v2-LocalSetup.exe`
- `TricBar-v2-WebSetup.exe`
- `TricBar-v2.zip`

O ZIP `TricBar-v2.zip` é o pacote sem runtimes externos que o WebSetup baixa. Publique-o na raiz da branch `webdownloads` para manter a URL configurada no instalador

## Estrutura do projeto

- `Launcher\` — configurador WinUI 3
- `Engine\` — processo/motor nativo do TricBar
- `NativeHook\` — hook nativo responsável pelos efeitos na barra de tarefas
- `Installer\` — código e projeto dos instaladores
- `build\` — scripts PowerShell de compilação e empacotamento

## Licença

Consulte o arquivo de licença do repositório


# 🇺🇸 English (US)

TricBar customizes the appearance of the Windows taskbar with Blur, Acrylic, Liquid Glass, and solid color effects. The configurator allows you to choose the visual mode, adjust color and opacity, and set the application to launch automatically with Windows.

## Requirements

- Windows x64
- Windows 10 (version 1809 or later) or Windows 11
- The **Light Liquid Glass** and **Dark Liquid Glass** modes require Windows 11 22H2 or later. On earlier versions, the app uses Acrylic as an alternative

## Installation

Download the installers from the **Releases** section on GitHub:

- **TricBar-v2-LocalSetup.exe** — a self-contained installer; includes all application files.
- **TricBar-v2-WebSetup.exe** — a lightweight bootstrapper; downloads the necessary components and the application package during installation.

The WebSetup provided in this repository requires the **.NET 8 Desktop Runtime** to launch. While it attempts to download this runtime during the installation process, the managed bootstrapper may fail to open—and thus fail to initiate the download—if the runtime is not already installed on your computer. For a clean-machine installation, use LocalSetup until WebSetup is released with a bootstrapper that does not have a pre-existing dependency on .NET

WebSetup downloads the application package from [`TricBar-v2.zip` on the `webdownloads` branch](https://raw.githubusercontent.com/superplexx/TricBar-v2/webdownloads/TricBar-v2.zip), along with the official installers for .NET Desktop Runtime 8 and Windows App Runtime 1.6, provided it is able to launch

## Building on Windows

### Required Tools

- .NET SDK 8 (the version used in the repository is specified in [`global.json`](global.json))
- Visual Studio 2022 with MSBuild
- Workload/components for WinUI 3 and Windows App SDK
- **Desktop development with C++** development tools (required to compile the native hook and engine)

Close any running instances of TricBar before building, then execute the following command in PowerShell from the repository root:

```powershell
.\build\buildsetup.ps1 or .\build\buildapp.ps1
```

The script compiles the application and both installers. The resulting artifacts will be located in `dist\releases\`:

- `TricBar-v2-LocalSetup.exe`
- `TricBar-v2-WebSetup.exe`
- `TricBar-v2.zip`

The `TricBar-v2.zip` file is the application package—excluding external runtimes—that is downloaded by WebSetup. Publish it to the root of the `webdownloads` branch to maintain the URL configured in the installer

## Project Structure

- `Launcher\` — WinUI 3 configurator
- `Engine\` — TricBar native process/engine
- `NativeHook\` — native hook responsible for the effects on the taskbar
- `Installer\` — code and project files for the installers
- `build\` — PowerShell scripts for building and packaging

## License

Please refer to the license file in the repository
