# TricBar v2

O TricBar personaliza a aparência da barra de tarefas do Windows com efeitos Blur, Acrylic, Liquid Glass e cor sólida. O configurador permite escolher o modo visual, ajustar a cor e a opacidade e configurar a inicialização com o Windows

## Requisitos

- Windows x64
- Windows 10 (versão mínima 1809) ou Windows 11.
- Os modos **Light Liquid Glass** e **Dark Liquid Glass** requerem Windows 11 22H2 ou posterior. Em versões anteriores, o app usa Acrylic como alternativa

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
