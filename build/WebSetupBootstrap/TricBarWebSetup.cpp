#include <windows.h>
#include <urlmon.h>
#include <shlobj.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace
{
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

    bool IsDotNetDesktopRuntimeInstalled()
    {
        std::wstring tempFile = GetTempDirectory() + L"tricbar-dotnet-check.txt";
        std::wstring command = L"dotnet --list-runtimes > " + QuoteForCommandLine(tempFile) + L" 2>nul";
        _wsystem(command.c_str());

        std::wifstream input(tempFile);
        if (!input.is_open())
            return false;

        std::wstring line;
        while (std::getline(input, line))
        {
            if (line.find(L"Microsoft.WindowsDesktop.App") != std::wstring::npos &&
                line.find(L"8.0") != std::wstring::npos)
            {
                return true;
            }
        }

        return false;
    }

    bool DownloadFile(const std::wstring& url, const std::wstring& destination)
    {
        HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), destination.c_str(), 0, nullptr);
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
            return false;

        WaitForSingleObject(processInfo.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return exitCode == 0 || exitCode == 1641 || exitCode == 3010;
    }

    bool InstallDotNetDesktopRuntimeIfNeeded()
    {
        if (IsDotNetDesktopRuntimeInstalled())
            return true;

        std::wstring tempDir = GetTempDirectory();
        std::wstring installerPath = tempDir + L"windowsdesktop-runtime-win-x64.exe";
        std::wstring installerUrl = L"https://aka.ms/dotnet/8.0/windowsdesktop-runtime-win-x64.exe";
        if (!DownloadFile(installerUrl, installerPath))
            return false;

        return RunHiddenProcess(installerPath, L"/install /quiet /norestart");
    }

    bool IsWindowsAppRuntimeInstalled()
    {
        std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"$ErrorActionPreference='Stop'; $package = Get-AppxPackage -AllUsers -Name 'Microsoft.WindowsAppRuntime.1.6' -ErrorAction SilentlyContinue; if ($package) { exit 0 } exit 1\"";
        return RunHiddenProcess(L"powershell.exe", L"-NoProfile -ExecutionPolicy Bypass -Command \"$ErrorActionPreference='Stop'; $package = Get-AppxPackage -AllUsers -Name 'Microsoft.WindowsAppRuntime.1.6' -ErrorAction SilentlyContinue; if ($package) { exit 0 } exit 1\"");
    }

    bool InstallWindowsAppRuntimeIfNeeded()
    {
        if (IsWindowsAppRuntimeInstalled())
            return true;

        std::wstring tempDir = GetTempDirectory();
        std::wstring installerPath = tempDir + L"windowsappruntimeinstall-x64.exe";
        std::wstring installerUrl = L"https://aka.ms/windowsappsdk/1.6/1.6.250205002/windowsappruntimeinstall-x64.exe";
        if (!DownloadFile(installerUrl, installerPath))
            return false;

        return RunHiddenProcess(installerPath, L"--quiet");
    }

    bool ExpandZip(const std::wstring& archivePath, const std::wstring& destination)
    {
        std::wstring command =
            L"-NoProfile -ExecutionPolicy Bypass -Command \"$ErrorActionPreference='Stop'; "
            L"if (-not (Test-Path -LiteralPath '" + destination + L"')) { New-Item -ItemType Directory -Path '" + destination + L"' -Force | Out-Null }; "
            L"Expand-Archive -LiteralPath '" + archivePath + L"' -DestinationPath '" + destination + L"' -Force; \"";

        return RunHiddenProcess(L"powershell.exe", command);
    }

    std::wstring GetInstallDirectory()
    {
        wchar_t programFiles[MAX_PATH] = {};
        DWORD length = GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return L"C:\\Program Files\\TricBar";
        return std::wstring(programFiles) + L"\\TricBar";
    }

    bool CreateShortcut(const std::wstring& shortcutPath, const std::wstring& targetPath)
    {
        std::wstring installDir = GetInstallDirectory();
        std::wstring command =
            L"-NoProfile -ExecutionPolicy Bypass -Command \"$ErrorActionPreference='Stop'; "
            L"$WshShell = New-Object -ComObject WScript.Shell; "
            L"$Shortcut = $WshShell.CreateShortcut('" + shortcutPath + L"'); "
            L"$Shortcut.TargetPath = '" + targetPath + L"'; "
            L"$Shortcut.WorkingDirectory = '" + installDir + L"'; "
            L"$Shortcut.IconLocation = '" + installDir + L"\\icon.ico'; "
            L"$Shortcut.Save(); \"";
        return RunHiddenProcess(L"powershell.exe", command);
    }

    bool EnsureInstallDirectoryExists(const std::wstring& installDirectory)
    {
        if (std::filesystem::exists(installDirectory))
            return true;
        std::error_code ec;
        return std::filesystem::create_directories(installDirectory, ec) || ec.value() == 0;
    }
}

int wmain()
{
    std::wstring tempDir = GetTempDirectory();
    std::wstring archivePath = tempDir + L"TricBar-v2.zip";
    std::wstring installDirectory = GetInstallDirectory();
    std::wstring appPath = installDirectory + L"\\TricBarConfigurator.exe";

    if (!InstallDotNetDesktopRuntimeIfNeeded())
        return 1;

    if (!InstallWindowsAppRuntimeIfNeeded())
        return 2;

    std::wstring archiveUrl = L"https://raw.githubusercontent.com/superplexx/TricBar-v2/webdownloads/TricBar-v2.zip";
    if (!DownloadFile(archiveUrl, archivePath))
        return 3;

    if (!EnsureInstallDirectoryExists(installDirectory))
        return 4;

    if (!ExpandZip(archivePath, installDirectory))
        return 5;

    if (!std::filesystem::exists(appPath))
        return 6;

    wchar_t desktopShortcut[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%USERPROFILE%\\Desktop\\TricBar.lnk", desktopShortcut, MAX_PATH);
    CreateShortcut(desktopShortcut, appPath);

    wchar_t startMenuDirectory[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\TricBar", startMenuDirectory, MAX_PATH);
    std::filesystem::create_directories(startMenuDirectory);
    std::wstring shortcutPath = std::wstring(startMenuDirectory) + L"\\TricBar.lnk";
    CreateShortcut(shortcutPath, appPath);

    SHELLEXECUTEINFOW executeInfo = {};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.lpVerb = L"open";
    executeInfo.lpFile = appPath.c_str();
    executeInfo.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&executeInfo);

    return 0;
}
