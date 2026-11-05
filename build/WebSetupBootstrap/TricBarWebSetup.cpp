#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <urlmon.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <appmodel.h>
#include <dwmapi.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>
#include <atomic>
#include <iterator>
#include <cwctype>
#include <cwchar>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

namespace
{
    constexpr wchar_t kInstallerWindowClass[] = L"TricBarWebSetupWnd";
    constexpr int IDI_ICON = 1;

    HWND g_hwndMain = nullptr;
    HWND g_hwndTitle = nullptr;
    HWND g_hwndSubtitle = nullptr;
    HWND g_hwndStage = nullptr;
    HWND g_hwndProgress = nullptr;
    HWND g_hwndDetail = nullptr;
    HWND g_hwndActionBtn = nullptr;
    HWND g_hwndCancelBtn = nullptr;
    HWND g_hwndPathEdit = nullptr;
    HWND g_hwndBrowseBtn = nullptr;

    HFONT g_hFontTitle = nullptr;
    HFONT g_hFontNormal = nullptr;
    HFONT g_hFontSmall = nullptr;
    HBRUSH g_hbrBackground = nullptr;
    HBRUSH g_hbrCard = nullptr;
    HBRUSH g_hbrField = nullptr;

    std::wstring g_installDirectory;

    std::atomic<bool> g_cancelled{ false };
    std::atomic<bool> g_completed{ false };
    std::atomic<bool> g_failed{ false };
    std::atomic<bool> g_installStarted{ false };
    std::wstring g_failureMessage;
    DWORD g_lastExitCode = 0;     // último código de saída de um processo filho
    DWORD g_lastLaunchError = 0;  // GetLastError se o processo nem iniciou (ex.: 740 = precisa de admin)

    class DownloadProgressCallback : public IBindStatusCallback
    {
    public:
        DownloadProgressCallback(HWND progressWnd, HWND detailWnd, const std::wstring& stepName)
            : m_progressWnd(progressWnd), m_detailWnd(detailWnd), m_stepName(stepName), m_refCount(1) {}

        // IUnknown
        STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override
        {
            if (!ppvObject) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_IBindStatusCallback)
            {
                *ppvObject = static_cast<IBindStatusCallback*>(this);
                AddRef();
                return S_OK;
            }
            *ppvObject = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override { return ++m_refCount; }
        STDMETHODIMP_(ULONG) Release() override
        {
            ULONG count = --m_refCount;
            if (count == 0) delete this;
            return count;
        }

        // IBindStatusCallback
        STDMETHODIMP OnStartBinding(DWORD, IBinding*) override { return S_OK; }
        STDMETHODIMP GetPriority(LONG*) override { return S_OK; }
        STDMETHODIMP OnLowResource(DWORD) override { return S_OK; }
        STDMETHODIMP OnProgress(ULONG ulProgress, ULONG ulProgressMax, ULONG, LPCWSTR) override
        {
            if (g_cancelled.load()) return E_ABORT;
            if (m_progressWnd && ulProgressMax > 0)
            {
                int percent = static_cast<int>((static_cast<double>(ulProgress) / ulProgressMax) * 100.0);
                PostMessageW(m_progressWnd, PBM_SETPOS, percent, 0);

                if (m_detailWnd)
                {
                    wchar_t buf[128];
                    double mbCur = ulProgress / (1024.0 * 1024.0);
                    double mbMax = ulProgressMax / (1024.0 * 1024.0);
                    swprintf_s(buf, L"%s (%d%% - %.1f MB de %.1f MB)", m_stepName.c_str(), percent, mbCur, mbMax);
                    SetWindowTextW(m_detailWnd, buf);
                }
            }
            return S_OK;
        }
        STDMETHODIMP OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
        STDMETHODIMP GetBindInfo(DWORD* grfBINDF, BINDINFO* pbindinfo) override
        {
            if (!grfBINDF || !pbindinfo) return E_POINTER;
            *grfBINDF = BINDF_ASYNCHRONOUS | BINDF_ASYNCSTORAGE | BINDF_PULLDATA;
            return S_OK;
        }
        STDMETHODIMP OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
        STDMETHODIMP OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }

    private:
        HWND m_progressWnd;
        HWND m_detailWnd;
        std::wstring m_stepName;
        std::atomic<ULONG> m_refCount;
    };

    std::wstring GetTempDirectory()
    {
        wchar_t buffer[MAX_PATH] = {};
        DWORD length = GetTempPathW(MAX_PATH, buffer);
        if (length == 0 || length >= MAX_PATH)
            return L"C:\\Temp\\";
        return std::wstring(buffer);
    }

    std::wstring QuoteForCommandLine(const std::wstring& value)
    {
        std::wstring quoted = L"\"";
        for (wchar_t ch : value)
        {
            if (ch == L'"')
            {
                quoted += L"\\\"";
            }
            else
            {
                quoted.push_back(ch);
            }
        }
        quoted += L"\"";
        return quoted;
    }

    // Dentro de '...' no PowerShell, a aspa simples precisa ser duplicada. Sem isso, caminhos
    // como C:\Users\O'Brien\... quebram o comando (Temp, Desktop e Menu Iniciar ficam no perfil).
    std::wstring EscapePowerShellSingleQuoted(const std::wstring& value)
    {
        std::wstring escaped;
        escaped.reserve(value.size());
        for (wchar_t ch : value)
        {
            escaped.push_back(ch);
            if (ch == L'\'')
                escaped.push_back(L'\'');
        }
        return escaped;
    }

    // Procura a pasta do runtime em Program Files\\dotnet (não depende do PATH).
    bool HasDotNet8DesktopRuntimeFolder()
    {
        wchar_t programFiles[MAX_PATH] = {};
        DWORD length = GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return false;
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::path(programFiles) / L"dotnet" / L"shared" / L"Microsoft.WindowsDesktop.App";
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->path().filename().wstring().rfind(L"8.0.", 0) == 0)
                return true;
        }
        return false;
    }

    bool IsDotNetDesktopRuntimeInstalled()
    {
        if (HasDotNet8DesktopRuntimeFolder())
            return true;
        std::wstring tempFile = GetTempDirectory() + L"tricbar-dotnet-check.txt";
        std::wstring command = L"dotnet --list-runtimes > " + QuoteForCommandLine(tempFile) + L" 2>nul";
        _wsystem(command.c_str());

        std::wifstream input(tempFile);
        if (!input.is_open())
            return false;

        std::wstring line;
        while (std::getline(input, line))
        {
            // Formato: "Microsoft.WindowsDesktop.App 8.0.x [caminho]". Procurar so "8.0" casava
            // com qualquer versao ou caminho que contivesse esses caracteres.
            if (line.find(L"Microsoft.WindowsDesktop.App 8.0.") != std::wstring::npos)
            {
                return true;
            }
        }

        return false;
    }

    void UpdateUIStage(const wchar_t* stage, const wchar_t* detail, int progressPercent, bool marquee = false)
    {
        if (g_hwndStage && stage)
            SetWindowTextW(g_hwndStage, stage);
        if (g_hwndDetail && detail)
            SetWindowTextW(g_hwndDetail, detail);
        if (g_hwndProgress)
        {
            // A barra é própria (TricBarProgress): trata modo indeterminado e posição sozinha.
            const bool isError = stage && wcsncmp(stage, L"Erro", 4) == 0;
            SendMessageW(g_hwndProgress, PBM_SETSTATE, isError ? 2 /*PBST_ERROR*/ : 1 /*PBST_NORMAL*/, 0);
            SendMessageW(g_hwndProgress, PBM_SETMARQUEE, marquee ? TRUE : FALSE, 0);
            if (!marquee)
                SendMessageW(g_hwndProgress, PBM_SETPOS, progressPercent, 0);
        }
    }

    bool DownloadFileWithProgress(const std::wstring& url, const std::wstring& destination, const std::wstring& stepName)
    {
        UpdateUIStage(nullptr, stepName.c_str(), 0, false);
        DownloadProgressCallback* callback = new DownloadProgressCallback(g_hwndProgress, g_hwndDetail, stepName);
        HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), destination.c_str(), 0, callback);
        callback->Release();
        return SUCCEEDED(hr);
    }

    bool RunHiddenProcess(const std::wstring& fileName, const std::wstring& arguments)
    {
        std::wstring commandLine = fileName;
        if (!arguments.empty())
            commandLine += L" " + arguments;

        std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
        buffer.push_back(L'\0');

        STARTUPINFOW startupInfo = {};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION processInfo = {};
        BOOL created = CreateProcessW(
            nullptr,
            buffer.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo);

        if (!created)
        {
            g_lastLaunchError = GetLastError();
            g_lastExitCode = 0;
            return false;
        }
        g_lastLaunchError = 0;

        WaitForSingleObject(processInfo.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
        g_lastExitCode = exitCode;
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return exitCode == 0 || exitCode == 1641 || exitCode == 3010;
    }

    bool InstallDotNetDesktopRuntimeIfNeeded()
    {
        UpdateUIStage(L"Etapa 1 de 4: Verificando .NET 8 Runtime...", L"Verificando instalação existente...", 0, true);
        if (IsDotNetDesktopRuntimeInstalled())
            return true;
        g_lastExitCode = 0;
        g_lastLaunchError = 0;

        std::wstring tempDir = GetTempDirectory();
        std::wstring installerPath = tempDir + L"windowsdesktop-runtime-win-x64.exe";
        std::wstring installerUrl = L"https://aka.ms/dotnet/8.0/windowsdesktop-runtime-win-x64.exe";

        UpdateUIStage(L"Etapa 1 de 4: Baixando .NET 8 Runtime...", L"Iniciando download do runtime...", 0, false);
        if (!DownloadFileWithProgress(installerUrl, installerPath, L"Baixando .NET 8 Desktop Runtime"))
            return false;

        UpdateUIStage(L"Etapa 1 de 4: Instalando .NET 8 Runtime...", L"Instalando silenciosamente...", 100, true);
        // 1638 = já há uma versão 8.0.x igual ou mais nova instalada (ok)
        return RunHiddenProcess(installerPath, L"/install /quiet /norestart") || g_lastExitCode == 1638;
    }

    // Consulta nativa (instantânea, sem PowerShell): há algum pacote instalado nessa família?
    // Se requireX64, exige que ao menos um deles seja x64.
    bool IsPackageFamilyInstalled(const wchar_t* familyName, bool requireX64)
    {
        UINT32 count = 0, bufferLength = 0;
        LONG rc = GetPackagesByPackageFamily(familyName, &count, nullptr, &bufferLength, nullptr);
        if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0)
            return false;
        if (!requireX64)
            return true;

        std::vector<PWSTR> names(count);
        std::vector<wchar_t> buffer(bufferLength);
        rc = GetPackagesByPackageFamily(familyName, &count, names.data(), &bufferLength, buffer.data());
        if (rc != ERROR_SUCCESS)
            return false;
        for (UINT32 i = 0; i < count; ++i)
        {
            if (names[i] && wcsstr(names[i], L"_x64_") != nullptr)
                return true;
        }
        return false;
    }

    // Apps não empacotados precisam do Framework + Main do runtime. Apps da Microsoft Store costumam
    // instalar só o Framework, então checar apenas ele dava falso positivo (e o TricBar não abria).
    bool IsWindowsAppRuntimeInstalled()
    {
        return IsPackageFamilyInstalled(L"Microsoft.WindowsAppRuntime.1.6_8wekyb3d8bbwe", true) &&
               IsPackageFamilyInstalled(L"MicrosoftCorporationII.WinAppRuntime.Main.1.6_8wekyb3d8bbwe", false);
    }

    bool InstallWindowsAppRuntimeIfNeeded()
    {
        UpdateUIStage(L"Etapa 2 de 4: Verificando Windows App Runtime...", L"Verificando pacote do Windows App SDK...", 0, true);
        if (IsWindowsAppRuntimeInstalled())
            return true;
        g_lastExitCode = 0;
        g_lastLaunchError = 0;

        std::wstring tempDir = GetTempDirectory();
        std::wstring installerPath = tempDir + L"windowsappruntimeinstall-x64.exe";
        std::wstring installerUrl = L"https://aka.ms/windowsappsdk/1.6/1.6.250205002/windowsappruntimeinstall-x64.exe";

        UpdateUIStage(L"Etapa 2 de 4: Baixando Windows App Runtime 1.6...", L"Iniciando download...", 0, false);
        if (!DownloadFileWithProgress(installerUrl, installerPath, L"Baixando Windows App Runtime 1.6"))
            return false;

        UpdateUIStage(L"Etapa 2 de 4: Instalando Windows App Runtime 1.6...", L"Instalando silenciosamente...", 100, true);
        return RunHiddenProcess(installerPath, L"--quiet");
    }

    std::wstring GetExtractLogPath()
    {
        wchar_t buffer[MAX_PATH] = {};
        DWORD length = GetTempPathW(MAX_PATH, buffer);
        std::wstring dir = (length == 0 || length >= MAX_PATH) ? std::wstring(L"C:\\Temp\\") : std::wstring(buffer);
        return dir + L"tricbar-extract.log";
    }

    // Lê a primeira linha do log gravado pelo PowerShell (UTF-8, com ou sem BOM).
    std::wstring ReadExtractLogMessage()
    {
        std::ifstream in(GetExtractLogPath(), std::ios::binary);
        if (!in.is_open())
            return L"";
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
            bytes.erase(0, 3);
        size_t eol = bytes.find_first_of("\r\n");
        if (eol != std::string::npos)
            bytes.resize(eol);
        if (bytes.empty())
            return L"";
        int len = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring wide(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), static_cast<int>(bytes.size()), wide.data(), len);
        return wide;
    }

    bool ExpandZip(const std::wstring& archivePathRaw, const std::wstring& destinationRaw)
    {
        const std::wstring archivePath = EscapePowerShellSingleQuoted(archivePathRaw);
        const std::wstring destination = EscapePowerShellSingleQuoted(destinationRaw);
        const std::wstring logPath = GetExtractLogPath();
        DeleteFileW(logPath.c_str());
        std::wstring command =
            L"-NoProfile -ExecutionPolicy Bypass -Command \"$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; "
            L"try { "
            L"if (-not (Test-Path -LiteralPath '" + destination + L"')) { New-Item -ItemType Directory -Path '" + destination + L"' -Force | Out-Null }; "
            L"Expand-Archive -LiteralPath '" + archivePath + L"' -DestinationPath '" + destination + L"' -Force "
            L"} catch { $_.Exception.Message | Out-File -LiteralPath '" + EscapePowerShellSingleQuoted(logPath) + L"' -Encoding utf8; exit 1 }\"";

        return RunHiddenProcess(L"powershell.exe", command);
    }

    std::wstring GetInstallDirectory()
    {
        if (!g_installDirectory.empty())
            return g_installDirectory;
        wchar_t programFiles[MAX_PATH] = {};
        DWORD length = GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return L"C:\\Program Files\\TricBar";
        return std::wstring(programFiles) + L"\\TricBar";
    }

    // Garante que o destino aponte para uma pasta "TricBar" (como no instalador local)
    std::wstring EnsureTricBarLeaf(std::wstring path)
    {
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
            path.pop_back();
        if (path.empty())
            return path;
        std::filesystem::path leaf = std::filesystem::path(path).filename();
        if (_wcsicmp(leaf.c_str(), L"TricBar") != 0)
            path += L"\\TricBar";
        return path;
    }

    std::wstring GetEditPath()
    {
        if (!g_hwndPathEdit)
            return GetInstallDirectory();
        int length = GetWindowTextLengthW(g_hwndPathEdit);
        std::wstring text(length, L'\0');
        if (length > 0)
            GetWindowTextW(g_hwndPathEdit, text.data(), length + 1);
        text.resize(length);
        return EnsureTricBarLeaf(text);
    }

    // Diálogo nativo de seleção de pasta (IFileDialog / FOS_PICKFOLDERS)
    bool PickInstallFolder(HWND owner, std::wstring& outPath)
    {
        IFileDialog* dialog = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog));
        if (FAILED(hr) || !dialog)
            return false;

        if (!outPath.empty())
        {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(outPath.c_str(), nullptr, IID_PPV_ARGS(&folder))))
            {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }

        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS);
        dialog->SetTitle(L"Escolha onde instalar o TricBar");

        hr = dialog->Show(owner);
        if (hr == S_OK)
        {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result)) && result)
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                {
                    outPath = path;
                    CoTaskMemFree(path);
                }
                result->Release();
            }
        }
        dialog->Release();
        return hr == S_OK;
    }

    bool CreateShortcut(const std::wstring& shortcutPathRaw, const std::wstring& targetPathRaw,
        const std::wstring& argumentsRaw = L"", const std::wstring& descriptionRaw = L"")
    {
        const std::wstring shortcutPath = EscapePowerShellSingleQuoted(shortcutPathRaw);
        const std::wstring targetPath = EscapePowerShellSingleQuoted(targetPathRaw);
        std::wstring installDir = EscapePowerShellSingleQuoted(GetInstallDirectory());
        std::wstring command =
            L"-NoProfile -ExecutionPolicy Bypass -Command \"$ErrorActionPreference='Stop'; "
            L"$WshShell = New-Object -ComObject WScript.Shell; "
            L"$Shortcut = $WshShell.CreateShortcut('" + shortcutPath + L"'); "
            L"$Shortcut.TargetPath = '" + targetPath + L"'; "
            L"$Shortcut.Arguments = '" + EscapePowerShellSingleQuoted(argumentsRaw) + L"'; "
            L"$Shortcut.Description = '" + EscapePowerShellSingleQuoted(descriptionRaw) + L"'; "
            L"$Shortcut.WorkingDirectory = '" + installDir + L"'; "
            L"$Shortcut.IconLocation = '" + installDir + L"\\icon.ico'; "
            L"$Shortcut.Save();";
        return RunHiddenProcess(L"powershell.exe", command);
    }

    bool EnsureInstallDirectoryExists(const std::wstring& installDirectory)
    {
        if (std::filesystem::exists(installDirectory))
            return true;
        std::error_code ec;
        return std::filesystem::create_directories(installDirectory, ec) || ec.value() == 0;
    }

    // ---- Registro em "Aplicativos instalados" (Windows 11) e desinstalação ----
    constexpr wchar_t kUninstallKeyPath[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TricBar";
    constexpr wchar_t kAppVersion[] = L"1.0.0";
    constexpr wchar_t kSetupFileName[] = L"Setup.exe";

    void SetRegString(HKEY key, const wchar_t* name, const std::wstring& value)
    {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }

    void SetRegDword(HKEY key, const wchar_t* name, DWORD value)
    {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    }

    std::wstring GetDesktopDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, path)))
            return path;
        return L"";
    }

    std::wstring GetStartMenuTricBarDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        ExpandEnvironmentStringsW(L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\TricBar", path, MAX_PATH);
        return path;
    }

    // Copia este próprio executável para a pasta de instalação como Setup.exe (é ele que desinstala).
    bool CopySetupToInstallDirectory(const std::wstring& installDir, const std::wstring& setupPath)
    {
        wchar_t self[MAX_PATH] = {};
        DWORD len = GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return false;
        std::error_code ec;
        if (std::filesystem::equivalent(self, setupPath, ec))
            return true;
        return CopyFileW(self, setupPath.c_str(), FALSE) != 0;
    }

    bool RegisterUninstallEntry(const std::wstring& installDir, const std::wstring& setupPath)
    {
        HKEY key = nullptr;
        LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKeyPath, 0, nullptr, 0,
            KEY_WRITE, nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS) // sem admin: registra só para o usuário atual
            status = RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKeyPath, 0, nullptr, 0,
                KEY_WRITE, nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS)
            return false;

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t date[16] = {};
        swprintf_s(date, L"%04d%02d%02d", st.wYear, st.wMonth, st.wDay);

        unsigned long long totalBytes = 0;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(installDir, ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code fileEc;
            if (it->is_regular_file(fileEc))
            {
                auto size = it->file_size(fileEc);
                if (!fileEc)
                    totalBytes += size;
            }
        }

        SetRegString(key, L"DisplayName", L"TricBar");
        SetRegString(key, L"DisplayVersion", kAppVersion);
        SetRegString(key, L"Publisher", L"Superplex");
        SetRegString(key, L"InstallLocation", installDir);
        SetRegString(key, L"UninstallString", L"\"" + setupPath + L"\" --uninstall");
        SetRegString(key, L"DisplayIcon", installDir + L"\\icon.ico");
        SetRegString(key, L"InstallDate", date);
        SetRegDword(key, L"NoModify", 1);
        SetRegDword(key, L"NoRepair", 1);
        if (totalBytes > 0)
            SetRegDword(key, L"EstimatedSize", static_cast<DWORD>(totalBytes / 1024));
        RegCloseKey(key);
        return true;
    }

    std::wstring ReadInstallLocationFromRegistry()
    {
        for (HKEY hive : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
        {
            wchar_t buffer[MAX_PATH * 2] = {};
            DWORD size = sizeof(buffer);
            if (RegGetValueW(hive, kUninstallKeyPath, L"InstallLocation", RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS &&
                buffer[0] != L'\0')
                return buffer;
        }
        return L"";
    }

    // Modo --uninstall (sem janela principal): remove atalhos, registro e a pasta de instalação.
    int RunUninstall()
    {
        if (MessageBoxW(nullptr, L"Deseja realmente remover o TricBar deste computador?", L"Desinstalar TricBar",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return 1;

        std::wstring installDir = ReadInstallLocationFromRegistry();
        if (installDir.empty())
        {
            wchar_t self[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            installDir = std::filesystem::path(self).parent_path().wstring();
        }

        // Sai da pasta de instalação para ela poder ser apagada.
        std::wstring tempDir = GetTempDirectory();
        SetCurrentDirectoryW(tempDir.c_str());

        _wsystem(L"taskkill /IM TricBarConfigurator.exe /F /T >nul 2>&1");
        _wsystem(L"taskkill /IM TricBar.exe /F /T >nul 2>&1");

        std::wstring desktop = GetDesktopDirectory();
        if (!desktop.empty())
            DeleteFileW((desktop + L"\\TricBar.lnk").c_str());
        std::wstring startMenu = GetStartMenuTricBarDirectory();
        DeleteFileW((startMenu + L"\\TricBar.lnk").c_str());
        DeleteFileW((startMenu + L"\\Desinstalar TricBar.lnk").c_str());
        RemoveDirectoryW(startMenu.c_str()); // só remove se estiver vazia

        HKEY hklm = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_ALL_ACCESS, &hklm) == ERROR_SUCCESS)
        {
            RegDeleteTreeW(hklm, L"TricBar");
            RegCloseKey(hklm);
        }
        HKEY hkcu = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_ALL_ACCESS, &hkcu) == ERROR_SUCCESS)
        {
            RegDeleteTreeW(hkcu, L"TricBar");
            RegCloseKey(hkcu);
        }

        // Este .exe pode estar dentro da pasta: um script apaga a pasta depois que ele fechar.
        std::wstring scriptPath = tempDir + L"TricBar-uninstall-" + std::to_wstring(GetTickCount64()) + L".cmd";
        std::wstring script =
            L"@echo off\r\nchcp 65001 >nul\r\nset /a tries=0\r\n:retry\r\n"
            L"timeout /t 2 /nobreak >nul 2>nul\r\n"
            L"rmdir /s /q \"" + installDir + L"\" >nul 2>nul\r\n"
            L"if not exist \"" + installDir + L"\" goto done\r\n"
            L"set /a tries+=1\r\nif %tries% lss 60 goto retry\r\n:done\r\n"
            L"del /f /q \"%~f0\"\r\n";
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, script.c_str(), static_cast<int>(script.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(utf8Len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, script.c_str(), static_cast<int>(script.size()), utf8.data(), utf8Len, nullptr, nullptr);
        {
            std::ofstream out(scriptPath, std::ios::binary);
            out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        }

        std::wstring cmdLine = L"cmd.exe /d /c \"\"" + scriptPath + L"\"\"";
        std::vector<wchar_t> buffer(cmdLine.begin(), cmdLine.end());
        buffer.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, tempDir.c_str(), &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }

        MessageBoxW(nullptr, L"O TricBar foi removido deste computador.", L"Desinstalar TricBar", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    std::wstring DescribeLastFailure()
    {
        if (g_lastLaunchError != 0)
            return L" (não foi possível iniciar o instalador, erro " + std::to_wstring(g_lastLaunchError) + L")";
        if (g_lastExitCode != 0)
            return L" (código " + std::to_wstring(g_lastExitCode) + L")";
        return L"";
    }

    DWORD WINAPI InstallWorkerThread(LPVOID)
    {
        std::wstring tempDir = GetTempDirectory();
        std::wstring archivePath = tempDir + L"TricBar-v2.zip";
        std::wstring installDirectory = GetInstallDirectory();
        std::wstring appPath = installDirectory + L"\\TricBarConfigurator.exe";

        if (!InstallDotNetDesktopRuntimeIfNeeded())
        {
            g_failed = true;
            g_failureMessage = L"Não foi possível instalar o .NET 8 Desktop Runtime." + DescribeLastFailure();
            UpdateUIStage(L"Erro na instalação", g_failureMessage.c_str(), 0, false);
            EnableWindow(g_hwndCancelBtn, TRUE);
            SetWindowTextW(g_hwndCancelBtn, L"Fechar");
            return 1;
        }

        if (!InstallWindowsAppRuntimeIfNeeded())
        {
            g_failed = true;
            g_failureMessage = L"Não foi possível instalar o Windows App Runtime 1.6." + DescribeLastFailure();
            UpdateUIStage(L"Erro na instalação", g_failureMessage.c_str(), 0, false);
            EnableWindow(g_hwndCancelBtn, TRUE);
            SetWindowTextW(g_hwndCancelBtn, L"Fechar");
            return 2;
        }

        UpdateUIStage(L"Etapa 3 de 4: Baixando TricBar...", L"Iniciando download do pacote do aplicativo...", 0, false);
        std::wstring archiveUrl = L"https://raw.githubusercontent.com/superplexx/TricBar-v2/webdownloads/TricBar-v2.zip";
        if (!DownloadFileWithProgress(archiveUrl, archivePath, L"Baixando pacote TricBar"))
        {
            g_failed = true;
            g_failureMessage = L"Não foi possível baixar o pacote do TricBar. Verifique sua conexão com a internet e tente novamente.";
            UpdateUIStage(L"Erro na instalação", g_failureMessage.c_str(), 0, false);
            EnableWindow(g_hwndCancelBtn, TRUE);
            SetWindowTextW(g_hwndCancelBtn, L"Fechar");
            return 3;
        }

        // Encerra instâncias em execução do TricBar para que a extração não falhe
        // ao sobrescrever arquivos em uso.
        _wsystem(L"taskkill /IM TricBarConfigurator.exe /F /T >nul 2>&1");
        _wsystem(L"taskkill /IM TricBar.exe /F /T >nul 2>&1");

        UpdateUIStage(L"Etapa 4 de 4: Extraindo arquivos...", L"Criando pasta de instalação...", 0, true);
        if (!EnsureInstallDirectoryExists(installDirectory))
        {
            g_failed = true;
            g_failureMessage = L"Não foi possível criar a pasta de instalação. Verifique a permissão de escrita na pasta escolhida ou execute como administrador.";
            UpdateUIStage(L"Erro na instalação", g_failureMessage.c_str(), 0, false);
            EnableWindow(g_hwndCancelBtn, TRUE);
            SetWindowTextW(g_hwndCancelBtn, L"Fechar");
            return 4;
        }

        UpdateUIStage(L"Etapa 4 de 4: Extraindo arquivos...", L"Descompactando arquivos do aplicativo...", 50, true);
        if (!ExpandZip(archivePath, installDirectory))
        {
            g_failed = true;
            g_failureMessage = L"Falha ao descompactar os arquivos do aplicativo.";
            std::wstring reason = ReadExtractLogMessage();
            if (!reason.empty())
            {
                if (reason.size() > 100)
                    reason.resize(100);
                g_failureMessage += L" " + reason;
            }
            UpdateUIStage(L"Erro na instalação", g_failureMessage.c_str(), 0, false);
            EnableWindow(g_hwndCancelBtn, TRUE);
            SetWindowTextW(g_hwndCancelBtn, L"Fechar");
            return 5;
        }

        if (!std::filesystem::exists(appPath))
        {
            g_failed = true;
            g_failureMessage = L"O executável TricBarConfigurator.exe não foi encontrado após a instalação.";
            UpdateUIStage(L"Erro na instalação", g_failureMessage.c_str(), 0, false);
            EnableWindow(g_hwndCancelBtn, TRUE);
            SetWindowTextW(g_hwndCancelBtn, L"Fechar");
            return 6;
        }

        UpdateUIStage(L"Etapa 4 de 4: Finalizando...", L"Criando atalhos...", 90, true);
        // A Area de Trabalho pode estar redirecionada (ex.: OneDrive), entao nao se monta
        // %USERPROFILE%\Desktop na mao.
        wchar_t desktopDirectory[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, desktopDirectory)) &&
            desktopDirectory[0] != L'\0')
        {
            CreateShortcut(std::wstring(desktopDirectory) + L"\\TricBar.lnk", appPath);
        }

        wchar_t startMenuDirectory[MAX_PATH] = {};
        ExpandEnvironmentStringsW(L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\TricBar", startMenuDirectory, MAX_PATH);
        std::filesystem::create_directories(startMenuDirectory);
        std::wstring shortcutPath = std::wstring(startMenuDirectory) + L"\\TricBar.lnk";
        CreateShortcut(shortcutPath, appPath);

        // Registra em "Aplicativos instalados" (Configurações > Aplicativos) com desinstalador
        const std::wstring setupPath = installDirectory + L"\\" + kSetupFileName;
        if (CopySetupToInstallDirectory(installDirectory, setupPath))
        {
            CreateShortcut(std::wstring(startMenuDirectory) + L"\\Desinstalar TricBar.lnk", setupPath,
                L"--uninstall", L"Desinstalar TricBar");
            RegisterUninstallEntry(installDirectory, setupPath);
        }

        UpdateUIStage(L"Instalação concluída com sucesso!", L"TricBar foi instalado e está pronto para uso.", 100, false);
        g_completed = true;

        if (g_hwndActionBtn)
        {
            SetWindowTextW(g_hwndActionBtn, L"Abrir");
            ShowWindow(g_hwndActionBtn, SW_SHOW);
            EnableWindow(g_hwndActionBtn, TRUE);
            InvalidateRect(g_hwndActionBtn, nullptr, FALSE);
        }
        if (g_hwndCancelBtn)
        {
            SetWindowTextW(g_hwndCancelBtn, L"Sair");
        }

        return 0;
    }

    // Pinta um botão owner-draw (Instalar amarelo; Procurar/Cancelar escuros)
    /* substituído pelos botões customizados da classe TricBarThemeButton */

    // Botões 100% customizados (sem thema/sistema): hover/press pelo nosso tema
    struct ThemeButtonState
    {
        bool hovered = false;
        bool pressed = false;
    };

    void PaintThemeButton(HWND hWnd)
    {
        int id = GetDlgCtrlID(hWnd);
        auto state = reinterpret_cast<ThemeButtonState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        bool isAction = (id == 1001);
        bool hovered = state && state->hovered;
        bool pressed = state && state->pressed;

        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        // Fundo da janela nos cantos (em vez de região recortada serrilhada)
        HBRUSH parentBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rc, parentBrush);
        DeleteObject(parentBrush);

        HBRUSH bgBrush = nullptr;
        HPEN borderPen = nullptr;
        COLORREF textColor = RGB(255, 255, 255);

        if (isAction)
        {
            COLORREF bg = pressed ? RGB(214, 170, 8) : hovered ? RGB(255, 222, 60) : RGB(255, 204, 0);
            bgBrush = CreateSolidBrush(bg);
            borderPen = CreatePen(PS_SOLID, 1, bg);
            textColor = RGB(30, 24, 2);
        }
        else
        {
            COLORREF bg = pressed ? RGB(18, 18, 18) : hovered ? RGB(38, 38, 38) : RGB(26, 26, 26);
            bgBrush = CreateSolidBrush(bg);
            borderPen = CreatePen(PS_SOLID, 1, pressed ? RGB(42, 42, 42) : RGB(52, 52, 52));
            textColor = RGB(238, 238, 242);
        }

        HGDIOBJ oldBrush = SelectObject(hdc, bgBrush);
        HGDIOBJ oldPen = SelectObject(hdc, borderPen);
        RoundRect(hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, 30, 30);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(bgBrush);
        DeleteObject(borderPen);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, textColor);
        SelectObject(hdc, g_hFontNormal);
        wchar_t text[64];
        GetWindowTextW(hWnd, text, 64);
        DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hWnd, &ps);
    }

    LRESULT CALLBACK ThemeButtonProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto state = reinterpret_cast<ThemeButtonState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_CREATE:
            {
                auto cs = reinterpret_cast<LPCREATESTRUCTW>(lParam);
                SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new ThemeButtonState()));
                return DefWindowProcW(hWnd, msg, wParam, lParam);
            }
        case WM_ERASEBKGND:
            return 1;
        case WM_PRINTCLIENT:
            return 0;
        case WM_PAINT:
            PaintThemeButton(hWnd);
            return 0;
        case WM_MOUSEMOVE:
            {
                bool wasHovered = state && state->hovered;
                if (state && !state->hovered)
                {
                    state->hovered = true;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
                if (!wasHovered)
                {
                    TRACKMOUSEEVENT tme{};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hWnd;
                    TrackMouseEvent(&tme);
                }
                return 0;
            }
        case WM_MOUSELEAVE:
            if (state && state->hovered)
            {
                state->hovered = false;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (state && !state->pressed)
            {
                state->pressed = true;
                SetCapture(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            {
                bool wasPressed = state && state->pressed;
                if (state && state->pressed)
                {
                    state->pressed = false;
                    ReleaseCapture();
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
                if (wasPressed)
                {
                    RECT rc;
                    GetClientRect(hWnd, &rc);
                    POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    if (pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom)
                        SendMessageW(GetParent(hWnd), WM_COMMAND,
                            MAKEWPARAM(GetDlgCtrlID(hWnd), 0), reinterpret_cast<LPARAM>(hWnd));
                }
                return 0;
            }
        case WM_CAPTURECHANGED:
            if (state && state->pressed)
            {
                state->pressed = false;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        case WM_DESTROY:
            if (state)
                delete state;
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    bool RegisterThemeButtonClass(HINSTANCE hInstance)
    {
        WNDCLASSEXW wcBtn{};
        wcBtn.cbSize = sizeof(wcBtn);
        wcBtn.lpfnWndProc = ThemeButtonProc;
        wcBtn.hInstance = hInstance;
        wcBtn.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wcBtn.hbrBackground = nullptr;
        wcBtn.lpszClassName = L"TricBarThemeButton";
        return RegisterClassExW(&wcBtn) != 0;
    }

    // ---- Barra de progresso própria: cápsula suave, sem artefatos do controle nativo ----
    struct ProgressState
    {
        int target = 0;
        double shown = 0.0;
        bool marquee = false;
        bool error = false;
        double phase = 0.0;
    };

    LRESULT CALLBACK ThemeProgressProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* st = reinterpret_cast<ProgressState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_NCCREATE:
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new ProgressState()));
            return TRUE;
        case WM_CREATE:
            SetTimer(hWnd, 1, 16, nullptr);
            return 0;
        case PBM_SETRANGE:
        case PBM_SETRANGE32:
            return 0;
        case PBM_SETPOS:
            if (st)
            {
                int old = st->target;
                st->target = static_cast<int>(wParam);
                if (st->target < 0) st->target = 0;
                if (st->target > 100) st->target = 100;
                if (st->target < st->shown) st->shown = st->target; // novo passo: reinicia sem "voltar" animado
                return old;
            }
            return 0;
        case PBM_SETSTATE:
            if (st)
            {
                st->error = (wParam == 2);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 1;
        case PBM_SETMARQUEE:
            if (st)
            {
                bool on = (wParam != 0);
                if (on && !st->marquee) st->phase = 0.0;
                st->marquee = on;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 1;
        case WM_TIMER:
            if (st)
            {
                bool dirty = false;
                if (st->marquee)
                {
                    st->phase += 16.0 / 1500.0;
                    if (st->phase >= 1.0) st->phase -= 1.0;
                    dirty = true;
                }
                else if (st->shown != st->target)
                {
                    double diff = st->target - st->shown;
                    st->shown += (diff > -0.1 && diff < 0.1) ? diff : diff * 0.18;
                    dirty = true;
                }
                if (dirty && IsWindowVisible(hWnd))
                    InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hWnd, &ps);
                RECT rc;
                GetClientRect(hWnd, &rc);
                const int W = rc.right, H = rc.bottom;
                const int S = 4; // supersampling para bordas suaves
                const int BW = W * S, BH = H * S;

                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, BW, BH);
                HGDIOBJ oldBmp = SelectObject(mem, bmp);

                // Fundo = cor do cartão
                HBRUSH cardBrush = CreateSolidBrush(RGB(14, 14, 14));
                RECT full{ 0, 0, BW, BH };
                FillRect(mem, &full, cardBrush);
                DeleteObject(cardBrush);

                // Trilho em cápsula com borda sutil
                HBRUSH trackBrush = CreateSolidBrush(RGB(22, 22, 22));
                HPEN trackPen = CreatePen(PS_INSIDEFRAME, S, RGB(46, 46, 46));
                HGDIOBJ oB = SelectObject(mem, trackBrush);
                HGDIOBJ oP = SelectObject(mem, trackPen);
                RoundRect(mem, 0, 0, BW, BH, BH, BH);
                SelectObject(mem, oB);
                SelectObject(mem, oP);
                DeleteObject(trackBrush);
                DeleteObject(trackPen);

                // Preenchimento
                const int inset = 3 * S;
                const int x0 = inset, y0 = inset, x1 = BW - inset, y1 = BH - inset;
                const int ih = y1 - y0;
                const int iw = x1 - x0;
                HBRUSH fillBrush = CreateSolidBrush((st && st->error) ? RGB(196, 43, 28) : RGB(0, 120, 212));
                HGDIOBJ oB2 = SelectObject(mem, fillBrush);
                HGDIOBJ oP2 = SelectObject(mem, GetStockObject(NULL_PEN));

                if (st && st->error)
                {
                    RoundRect(mem, x0, y0, x1 + 1, y1 + 1, ih, ih); // erro: cápsula vermelha cheia
                }
                else if (st && st->marquee)
                {
                    HRGN clip = CreateRoundRectRgn(x0, y0, x1 + 1, y1 + 1, ih, ih);
                    SelectClipRgn(mem, clip);
                    const double segW = iw * 0.30;
                    double t = st->phase;
                    double e = t * t * (3.0 - 2.0 * t); // suaviza início/fim
                    double left = x0 - segW + (iw + segW) * e;
                    RoundRect(mem, static_cast<int>(left), y0, static_cast<int>(left + segW) + 1, y1 + 1, ih, ih);
                    SelectClipRgn(mem, nullptr);
                    DeleteObject(clip);
                }
                else if (st && st->shown > 0.05)
                {
                    int fw = static_cast<int>(iw * (st->shown / 100.0));
                    if (fw < ih) fw = ih; // nunca menor que a cápsula
                    RoundRect(mem, x0, y0, x0 + fw + 1, y1 + 1, ih, ih);
                }
                SelectObject(mem, oB2);
                SelectObject(mem, oP2);
                DeleteObject(fillBrush);

                SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, nullptr);
                StretchBlt(hdc, 0, 0, W, H, mem, 0, 0, BW, BH, SRCCOPY);

                SelectObject(mem, oldBmp);
                DeleteObject(bmp);
                DeleteDC(mem);
                EndPaint(hWnd, &ps);
                return 0;
            }
        case WM_NCDESTROY:
            KillTimer(hWnd, 1);
            delete st;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    bool RegisterThemeProgressClass(HINSTANCE hInstance)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ThemeProgressProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"TricBarProgress";
        return RegisterClassExW(&wc) != 0;
    }

    LRESULT CALLBACK InstallerWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        switch (uMsg)
        {
        case WM_CREATE:
            {
                BOOL dark = TRUE;
                DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
                int cornerPreference = DWMWCP_ROUND;
                DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));

                g_hFontTitle = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
                if (!g_hFontTitle)
                    g_hFontTitle = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

                g_hFontNormal = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
                if (!g_hFontNormal)
                    g_hFontNormal = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

                g_hFontSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
                if (!g_hFontSmall)
                    g_hFontSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

                g_hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
                g_hbrCard = CreateSolidBrush(RGB(14, 14, 14));
                g_hbrField = CreateSolidBrush(RGB(22, 22, 22));

                g_hwndTitle = CreateWindowExW(0, L"STATIC", L"TricBar Web Setup",
                    WS_CHILD | WS_VISIBLE | SS_LEFT, 28, 20, 460, 28, hwnd, nullptr, nullptr, nullptr);
                SendMessageW(g_hwndTitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontTitle), TRUE);

                g_hwndSubtitle = CreateWindowExW(0, L"STATIC", L"Instalando componentes e configurando sua barra de tarefas...",
                    WS_CHILD | WS_VISIBLE | SS_LEFT, 28, 52, 460, 20, hwnd, nullptr, nullptr, nullptr);
                SendMessageW(g_hwndSubtitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

                g_hwndStage = CreateWindowExW(0, L"STATIC", L"Iniciando instalação...",
                    WS_CHILD | WS_VISIBLE | SS_LEFT, 40, 104, 440, 20, hwnd, nullptr, nullptr, nullptr);
                SendMessageW(g_hwndStage, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

                g_hwndProgress = CreateWindowExW(0, L"TricBarProgress", nullptr,
                    WS_CHILD | WS_VISIBLE, 40, 132, 444, 18, hwnd, nullptr, nullptr, nullptr);
                SendMessageW(g_hwndProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

                g_hwndDetail = CreateWindowExW(0, L"STATIC", L"Preparando ambiente de instalação...",
                    WS_CHILD | WS_VISIBLE | SS_LEFT, 40, 158, 440, 32, hwnd, nullptr, nullptr, nullptr);
                SendMessageW(g_hwndDetail, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontSmall), TRUE);

                g_hwndActionBtn = CreateWindowExW(0, L"TricBarThemeButton", L"Iniciar TricBar",
                    WS_CHILD, 226, 216, 134, 36, hwnd, reinterpret_cast<HMENU>(1001), nullptr, nullptr);
                SendMessageW(g_hwndActionBtn, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

                g_hwndCancelBtn = CreateWindowExW(0, L"TricBarThemeButton", L"Cancelar",
                    WS_CHILD | WS_VISIBLE, 368, 216, 124, 36, hwnd, reinterpret_cast<HMENU>(1002), nullptr, nullptr);
                SendMessageW(g_hwndCancelBtn, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

                // Campo de pasta personalizada + botão "Procurar..." (visíveis na tela de confirmação)
                g_hwndPathEdit = CreateWindowExW(0, L"EDIT", GetInstallDirectory().c_str(),
                    WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 46, 132, 330, 24, hwnd,
                    reinterpret_cast<HMENU>(1004), nullptr, nullptr);
                SendMessageW(g_hwndPathEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

                g_hwndBrowseBtn = CreateWindowExW(0, L"TricBarThemeButton", L"Procurar...",
                    WS_CHILD | WS_VISIBLE, 396, 126, 84, 32, hwnd,
                    reinterpret_cast<HMENU>(1003), nullptr, nullptr);
                SendMessageW(g_hwndBrowseBtn, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

                // Tela de confirmação: nada é instalado até o usuário clicar em "Instalar".
                // A pasta pode ser personalizada no campo acima do botão Instalar.
                UpdateUIStage(L"Pronto para instalar o TricBar",
                    L"Componentes de runtime (.NET 8 / Windows App SDK) são baixados somente se faltarem.",
                    0, false);
                ShowWindow(g_hwndProgress, SW_HIDE);
                if (g_hwndDetail)
                    SetWindowPos(g_hwndDetail, nullptr, 40, 160, 440, 32, SWP_NOZORDER | SWP_NOACTIVATE);
                if (g_hwndActionBtn)
                {
                    SetWindowTextW(g_hwndActionBtn, L"Instalar");
                    ShowWindow(g_hwndActionBtn, SW_SHOW);
                    EnableWindow(g_hwndActionBtn, TRUE);
                }
            }
            return 0;

        case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                // Windows 11 rounded card for progress stage
                HBRUSH cardBrush = CreateSolidBrush(RGB(14, 14, 14));
                HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(46, 46, 46));
                HGDIOBJ oldBrush = SelectObject(hdc, cardBrush);
                HGDIOBJ oldPen = SelectObject(hdc, cardPen);
                RoundRect(hdc, 24, 88, 498, 196, 16, 16);
                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(cardBrush);
                DeleteObject(cardPen);

                if (g_hwndPathEdit && IsWindowVisible(g_hwndPathEdit))
                {
                    // Campo arredondado atrás do edit de pasta personalizada
                    HBRUSH fieldBrush = CreateSolidBrush(RGB(22, 22, 22));
                    HPEN fieldPen = CreatePen(PS_SOLID, 1, RGB(46, 46, 46));
                    HGDIOBJ oldFieldBrush = SelectObject(hdc, fieldBrush);
                    HGDIOBJ oldFieldPen = SelectObject(hdc, fieldPen);
                    RoundRect(hdc, 36, 126, 394, 158, 10, 10);
                    SelectObject(hdc, oldFieldBrush);
                    SelectObject(hdc, oldFieldPen);
                    DeleteObject(fieldBrush);
                    DeleteObject(fieldPen);
                }

                EndPaint(hwnd, &ps);
                return 0;
            }

        case WM_CTLCOLOREDIT:
            {
                // Edit de pasta em modo escuro
                HDC hdc = reinterpret_cast<HDC>(wParam);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(238, 238, 242));
                SetBkColor(hdc, RGB(22, 22, 22));
                return reinterpret_cast<INT_PTR>(g_hbrField);
            }

        case WM_DRAWITEM:
            {
                // Não há mais botões owner-draw do sistema: os botões são da
                // classe customizada TricBarThemeButton, que se pinta sozinha.
                return DefWindowProcW(hwnd, uMsg, wParam, lParam);
            }

        case WM_CTLCOLORSTATIC:
            {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                SetBkMode(hdc, TRANSPARENT);
                HWND cWnd = reinterpret_cast<HWND>(lParam);
                if (cWnd == g_hwndTitle)
                    SetTextColor(hdc, RGB(238, 238, 242));
                else if (cWnd == g_hwndSubtitle)
                    SetTextColor(hdc, RGB(168, 170, 178));
                else if (cWnd == g_hwndStage)
                    SetTextColor(hdc, RGB(238, 238, 242));
                else
                    SetTextColor(hdc, RGB(168, 170, 178));

                if (cWnd == g_hwndStage || cWnd == g_hwndDetail)
                    return reinterpret_cast<INT_PTR>(g_hbrCard);
                return reinterpret_cast<INT_PTR>(g_hbrBackground);
            }

        case WM_COMMAND:
            if (LOWORD(wParam) == 1001) // Instalar (confirmação) / Abrir (concluído)
            {
                if (!g_installStarted.exchange(true))
                {
                    // Usuário confirmou: usa a pasta escolhida e inicia a instalação
                    g_installDirectory = GetEditPath();
                    ShowWindow(g_hwndPathEdit, SW_HIDE);
                    ShowWindow(g_hwndBrowseBtn, SW_HIDE);
                    ShowWindow(g_hwndProgress, SW_SHOW);
                    InvalidateRect(g_hwndMain, nullptr, TRUE); // apaga o "fantasma" do campo de pasta
                    if (g_hwndDetail)
                        SetWindowPos(g_hwndDetail, nullptr, 40, 158, 440, 32, SWP_NOZORDER | SWP_NOACTIVATE);
                    if (g_hwndActionBtn)
                        ShowWindow(g_hwndActionBtn, SW_HIDE);
                    UpdateUIStage(L"Preparando instalação...", L"Verificando ambiente...", 0, true);
                    HANDLE thread = CreateThread(nullptr, 0, InstallWorkerThread, nullptr, 0, nullptr);
                    if (thread) CloseHandle(thread);
                    return 0;
                }

                std::wstring appPath = GetInstallDirectory() + L"\\TricBarConfigurator.exe";
                SHELLEXECUTEINFOW executeInfo = {};
                executeInfo.cbSize = sizeof(executeInfo);
                executeInfo.lpVerb = L"open";
                executeInfo.lpFile = appPath.c_str();
                executeInfo.nShow = SW_SHOWNORMAL;
                ShellExecuteExW(&executeInfo);
                DestroyWindow(hwnd);
                return 0;
            }
            else if (LOWORD(wParam) == 1002) // Cancelar / Sair
            {
                g_cancelled = true;
                DestroyWindow(hwnd);
                return 0;
            }
            else if (LOWORD(wParam) == 1003) // Procurar... (pasta personalizada)
            {
                std::wstring selected = GetEditPath();
                if (PickInstallFolder(hwnd, selected))
                {
                    std::wstring normalized = EnsureTricBarLeaf(selected);
                    if (!normalized.empty())
                        SetWindowTextW(g_hwndPathEdit, normalized.c_str());
                }
                return 0;
            }
            break;

        case WM_CLOSE:
            g_cancelled = true;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_hFontTitle) DeleteObject(g_hFontTitle);
            if (g_hFontNormal) DeleteObject(g_hFontNormal);
            if (g_hFontSmall) DeleteObject(g_hFontSmall);
            if (g_hbrBackground) DeleteObject(g_hbrBackground);
            if (g_hbrCard) DeleteObject(g_hbrCard);
            if (g_hbrField) DeleteObject(g_hbrField);
            PostQuitMessage(g_completed ? 0 : 1);
            return 0;
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    {
        std::wstring cmdLine = GetCommandLineW();
        for (auto& ch : cmdLine) ch = static_cast<wchar_t>(towlower(ch));
        if (cmdLine.find(L"--uninstall") != std::wstring::npos)
            return RunUninstall();
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        return 1;

    if (!RegisterThemeButtonClass(hInstance) || !RegisterThemeProgressClass(hInstance))
        return 1;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = InstallerWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
    wc.lpszClassName = kInstallerWindowClass;

    if (!RegisterClassExW(&wc))
        return 1;

    const int width = 530;
    const int height = 300;
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int posX = (screenW - width) / 2;
    const int posY = (screenH - height) / 2;

    g_hwndMain = CreateWindowExW(
        WS_EX_APPWINDOW,
        kInstallerWindowClass,
        L"Instalador TricBar",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, width, height,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwndMain)
        return 1;

    ShowWindow(g_hwndMain, SW_SHOWNORMAL);
    UpdateWindow(g_hwndMain);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

int wmain()
{
    return wWinMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), SW_SHOWNORMAL);
}
