using Microsoft.Win32;
using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Windows.Forms;

namespace TricBarSetup;

internal static class Program
{
    private const string UninstallKeyPath = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\TricBar";
    private const string AppFolderName = "TricBar";
    private const string LauncherFileName = "TricBarConfigurator.exe";
    private const string EngineFileName = "TricBar.exe";
    private const string SetupFileName = "Setup.exe";
    private const string AppBundleResourceName = "TricBarSetup.AppBundle.zip";
#if TRICBAR_WEB_SETUP
    private const string AppBundleUrl = "https://raw.githubusercontent.com/superplexx/TricBar-v2/webdownloads/TricBar-v2.zip";
    private const string DotNetDesktopRuntimeUrl = "https://aka.ms/dotnet/8.0/windowsdesktop-runtime-win-x64.exe";
    private const string WindowsAppRuntimeUrl = "https://aka.ms/windowsappsdk/1.6/1.6.250205002/windowsappruntimeinstall-x64.exe";
#endif
    private static readonly string InstallDirectory = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), AppFolderName);
    private static readonly string ProgramsDirectory = Environment.GetFolderPath(
        Environment.SpecialFolder.Programs);
    private static readonly string StartMenuDirectory = Path.Combine(ProgramsDirectory, AppFolderName);
    private static readonly string DesktopShortcutPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "TricBar.lnk");

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "FindWindowExW")]
    private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string? className, string? windowName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "PostMessageW")]
    private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [STAThread]
    private static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();

        if (args.Length > 0 && args[0].Equals("--install", StringComparison.OrdinalIgnoreCase))
        {
            bool desktopShortcut = ReadOption(args, "--desktop");
            bool startMenuShortcuts = ReadOption(args, "--startmenu");
            return RunElevated(
                () => Install(desktopShortcut, startMenuShortcuts),
                "instalar");
        }

        if (args.Length > 0 && args[0].Equals("--uninstall", StringComparison.OrdinalIgnoreCase))
            return RunElevated(Uninstall, "desinstalar");

        Application.Run(new SetupForm());
        return 0;
    }

    private static bool ReadOption(string[] args, string name)
    {
        for (int index = 1; index + 1 < args.Length; index++)
        {
            if (args[index].Equals(name, StringComparison.OrdinalIgnoreCase))
                return bool.TryParse(args[index + 1], out bool value) && value;
        }
        return false;
    }

    private static int RunElevated(Action operation, string verb, bool prerequisitesReady = false)
    {
        if (!IsAdministrator())
        {
            try
            {
                var startInfo = new ProcessStartInfo
                {
                    FileName = Environment.ProcessPath ?? throw new InvalidOperationException(
                        "Não foi possível localizar o Setup.exe."),
                    UseShellExecute = true,
                    Verb = "runas"
                };
                foreach (string argument in Environment.GetCommandLineArgs().Skip(1))
                    startInfo.ArgumentList.Add(argument);
                Process.Start(startInfo);
                return 0;
            }
            catch (System.ComponentModel.Win32Exception ex) when (ex.NativeErrorCode == 1223)
            {
                MessageBox.Show("A operação foi cancelada porque a permissão de administrador não foi concedida.",
                    "TricBar Setup", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return 1;
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Não foi possível iniciar o Setup como administrador.\n\n{ex.Message}",
                    "TricBar Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return 1;
            }
        }

        using var progress = new ProgressForm(verb);
        try
        {
            progress.Shown += async (_, _) =>
            {
                try
                {
                    await Task.Run(operation);
                    progress.Complete();
                }
                catch (Exception ex)
                {
                    progress.Fail(ex);
                }
            };
            progress.ShowDialog();
            return progress.Succeeded ? 0 : 1;
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Não foi possível {verb} o TricBar.\n\n{ex.Message}",
                "TricBar Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }

    private static bool IsAdministrator()
    {
        using WindowsIdentity identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    private static void Install(bool createDesktopShortcut, bool createStartMenuShortcuts)
    {
        EnsureApplicationClosed();
        Directory.CreateDirectory(InstallDirectory);

#if TRICBAR_WEB_SETUP
        InstallWebBundle();
#else
        CopyApplicationBundle();
#endif
        FinishInstall(createDesktopShortcut, createStartMenuShortcuts);
    }

    private static void CopyApplicationBundle()
    {
        string temporaryDirectory = Path.Combine(Path.GetTempPath(), $"TricBarSetup-{Guid.NewGuid():N}");
        Directory.CreateDirectory(temporaryDirectory);
        try
        {
            using Stream archive = Assembly.GetExecutingAssembly().GetManifestResourceStream(AppBundleResourceName)
                ?? throw new InvalidOperationException("O pacote local do TricBar não foi incluído no Setup.");
            using var temporaryArchive = File.Create(Path.Combine(temporaryDirectory, "TricBar-v2.zip"));
            archive.CopyTo(temporaryArchive);
            temporaryArchive.Close();

            string sourceDirectory = Path.Combine(temporaryDirectory, "AppBundle");
            ZipFile.ExtractToDirectory(Path.Combine(temporaryDirectory, "TricBar-v2.zip"), sourceDirectory);
            CopyApplicationFiles(sourceDirectory);
        }
        finally
        {
            Directory.Delete(temporaryDirectory, recursive: true);
        }
    }

    private static void CopyApplicationFiles(string sourceDirectory)
    {
        ValidateApplicationBundle(sourceDirectory);
        foreach (string sourceFile in Directory.EnumerateFiles(sourceDirectory, "*", SearchOption.AllDirectories))
        {
            string relativePath = Path.GetRelativePath(sourceDirectory, sourceFile);
            string destination = Path.Combine(InstallDirectory, relativePath);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Copy(sourceFile, destination, overwrite: true);
        }
    }

    private static void ValidateApplicationBundle(string sourceDirectory)
    {
        string[] requiredFiles =
        [
            LauncherFileName,
            EngineFileName,
            "TricBar.dll",
            "TricBarConfigurator.pri",
            "App.xbf",
            "MainWindow.xbf"
        ];
        string[] missingFiles = requiredFiles
            .Where(file => !File.Exists(Path.Combine(sourceDirectory, file)))
            .ToArray();
        if (missingFiles.Length > 0)
            throw new InvalidOperationException(
                $"O pacote baixado não contém arquivos necessários do TricBar: {string.Join(", ", missingFiles)}.");
    }

#if TRICBAR_WEB_SETUP
    private static void InstallWebBundle()
    {
        string temporaryDirectory = Path.Combine(Path.GetTempPath(), $"TricBarWebSetup-{Guid.NewGuid():N}");
        Directory.CreateDirectory(temporaryDirectory);
        try
        {
            InstallWebPrerequisites(temporaryDirectory);

            string archivePath = Path.Combine(temporaryDirectory, "TricBar-v2.zip");
            DownloadFile(AppBundleUrl, archivePath);
            string sourceDirectory = Path.Combine(temporaryDirectory, "AppBundle");
            ZipFile.ExtractToDirectory(archivePath, sourceDirectory);
            CopyApplicationFiles(sourceDirectory);
        }
        finally
        {
            Directory.Delete(temporaryDirectory, recursive: true);
        }
    }

    private static void InstallWebPrerequisites(string temporaryDirectory)
    {
        string dotNetInstaller = Path.Combine(temporaryDirectory, "windowsdesktop-runtime.exe");
        DownloadFile(DotNetDesktopRuntimeUrl, dotNetInstaller);
        RunPrerequisiteInstaller(dotNetInstaller, "/install /quiet /norestart", ".NET 8 Desktop Runtime");

        string appRuntimeInstaller = Path.Combine(temporaryDirectory, "windows-app-runtime.exe");
        DownloadFile(WindowsAppRuntimeUrl, appRuntimeInstaller);
        RunPrerequisiteInstaller(appRuntimeInstaller, "--quiet", "Windows App Runtime 1.6");
    }

    private static void DownloadFile(string url, string destination)
    {
        using var client = new HttpClient();
        client.DefaultRequestHeaders.UserAgent.ParseAdd("TricBarSetup/2.0");
        using HttpResponseMessage response = client.GetAsync(url, HttpCompletionOption.ResponseHeadersRead)
            .GetAwaiter().GetResult();
        response.EnsureSuccessStatusCode();
        using Stream source = response.Content.ReadAsStream();
        using var target = File.Create(destination);
        source.CopyTo(target);
        if (target.Length == 0)
            throw new InvalidOperationException($"O download retornou um arquivo vazio: {url}");
    }

    private static void RunPrerequisiteInstaller(string installerPath, string arguments, string componentName)
    {
        using Process process = Process.Start(new ProcessStartInfo
        {
            FileName = installerPath,
            Arguments = arguments,
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden
        }) ?? throw new InvalidOperationException($"Não foi possível iniciar a instalação de {componentName}.");
        process.WaitForExit();
        if (process.ExitCode is not (0 or 1641 or 3010))
            throw new InvalidOperationException(
                $"A instalação de {componentName} falhou (código {process.ExitCode}).");
    }
#endif

    private static void FinishInstall(
        bool createDesktopShortcut,
        bool createStartMenuShortcuts)
    {
        string setupPath = Path.Combine(InstallDirectory, SetupFileName);
        string runningSetup = Path.GetFullPath(Environment.ProcessPath
            ?? throw new InvalidOperationException("Não foi possível localizar o Setup.exe."));
        if (!string.Equals(runningSetup, setupPath, StringComparison.OrdinalIgnoreCase))
            File.Copy(runningSetup, setupPath, overwrite: true);

        string launcherPath = Path.Combine(InstallDirectory, LauncherFileName);
        string uninstallPath = $"\"{setupPath}\" --uninstall";
        if (createStartMenuShortcuts)
        {
            CreateShortcut(Path.Combine(StartMenuDirectory, "TricBar.lnk"),
                launcherPath, string.Empty, "Abrir TricBar");
            CreateShortcut(Path.Combine(StartMenuDirectory, "Desinstalar TricBar.lnk"),
                setupPath, "--uninstall", "Desinstalar TricBar");
        }
        else
        {
            DeleteShortcut(Path.Combine(StartMenuDirectory, "TricBar.lnk"));
            DeleteShortcut(Path.Combine(StartMenuDirectory, "Desinstalar TricBar.lnk"));
        }

        if (createDesktopShortcut)
            CreateShortcut(DesktopShortcutPath, launcherPath, string.Empty, "Abrir TricBar");
        else
            DeleteShortcut(DesktopShortcutPath);

        using RegistryKey uninstallKey = Registry.LocalMachine.CreateSubKey(UninstallKeyPath, writable: true)
            ?? throw new InvalidOperationException("Não foi possível registrar o TricBar no Windows.");
        uninstallKey.SetValue("DisplayName", "TricBar");
        uninstallKey.SetValue("DisplayVersion", "2.0.0");
        uninstallKey.SetValue("Publisher", "Superplex");
        uninstallKey.SetValue("InstallLocation", InstallDirectory);
        uninstallKey.SetValue("UninstallString", uninstallPath);
        uninstallKey.SetValue("DisplayIcon", Path.Combine(InstallDirectory, "icon.ico"));
        uninstallKey.SetValue("NoModify", 1, RegistryValueKind.DWord);
        uninstallKey.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        uninstallKey.SetValue("InstallDate", DateTime.Now.ToString("yyyyMMdd"));
    }

    private static void CreateShortcut(string shortcutPath, string targetPath, string arguments, string description)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(shortcutPath)!);
        Type shellType = Type.GetTypeFromProgID("WScript.Shell")
            ?? throw new InvalidOperationException("O componente de atalhos do Windows não está disponível.");
        object shell = Activator.CreateInstance(shellType)
            ?? throw new InvalidOperationException("Não foi possível iniciar o componente de atalhos do Windows.");
        try
        {
            dynamic shortcut = shellType.InvokeMember("CreateShortcut",
                BindingFlags.InvokeMethod, null, shell, [shortcutPath])!;
            shortcut.TargetPath = targetPath;
            shortcut.Arguments = arguments;
            shortcut.WorkingDirectory = InstallDirectory;
            shortcut.Description = description;
            shortcut.IconLocation = Path.Combine(InstallDirectory, "icon.ico");
            shortcut.Save();
            Marshal.FinalReleaseComObject(shortcut);
        }
        finally
        {
            Marshal.FinalReleaseComObject(shell);
        }
    }

    private static void DeleteShortcut(string shortcutPath)
    {
        if (File.Exists(shortcutPath))
            File.Delete(shortcutPath);
    }

    private static void EnsureApplicationClosed()
    {
        CloseProcessesFromInstall(LauncherFileName);
        IntPtr engineWindow = FindWindowEx(new IntPtr(-3), IntPtr.Zero, "TricBarEngineHidden", null);
        if (engineWindow != IntPtr.Zero)
            PostMessage(engineWindow, 0x0010, IntPtr.Zero, IntPtr.Zero);
        CloseProcessesFromInstall(EngineFileName);
    }

    private static void CloseProcessesFromInstall(string executableName)
    {
        string expectedPath = Path.Combine(InstallDirectory, executableName);
        Process[] processes = Process.GetProcessesByName(Path.GetFileNameWithoutExtension(executableName));
        foreach (Process process in processes)
        {
            using (process)
            {
                string? processPath;
                try
                {
                    processPath = process.MainModule?.FileName;
                }
                catch (System.ComponentModel.Win32Exception)
                {
                    continue;
                }
                if (!string.Equals(processPath, expectedPath, StringComparison.OrdinalIgnoreCase))
                    continue;
                process.CloseMainWindow();
                if (!process.WaitForExit(3000))
                    throw new InvalidOperationException(
                        $"Feche o {executableName} antes de continuar a instalação ou desinstalação.");
            }
        }
    }

    private static void Uninstall()
    {
        if (MessageBox.Show("Deseja remover o TricBar deste computador?",
                "Desinstalar TricBar", MessageBoxButtons.YesNo, MessageBoxIcon.Question,
                MessageBoxDefaultButton.Button2) != DialogResult.Yes)
            throw new OperationCanceledException("A desinstalação foi cancelada.");

        EnsureApplicationClosed();
        DeleteShortcut(DesktopShortcutPath);
        DeleteShortcut(Path.Combine(StartMenuDirectory, "TricBar.lnk"));
        DeleteShortcut(Path.Combine(StartMenuDirectory, "Desinstalar TricBar.lnk"));
        if (Directory.Exists(StartMenuDirectory) && !Directory.EnumerateFileSystemEntries(StartMenuDirectory).Any())
            Directory.Delete(StartMenuDirectory);

        Registry.LocalMachine.DeleteSubKeyTree(UninstallKeyPath, throwOnMissingSubKey: false);

        string scriptPath = Path.Combine(Path.GetTempPath(), $"TricBar-uninstall-{Guid.NewGuid():N}.cmd");
        string script = "@echo off\r\n" +
                        "timeout /t 3 /nobreak >nul\r\n" +
                        $"rmdir /s /q \"{InstallDirectory}\"\r\n" +
                        "del /f /q \"%~f0\"\r\n";
        File.WriteAllText(scriptPath, script, Encoding.ASCII);
        var startInfo = new ProcessStartInfo
        {
            FileName = Environment.GetEnvironmentVariable("ComSpec") ?? "cmd.exe",
            Arguments = $"/d /c \"\"{scriptPath}\"\"",
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden
        };
        if (Process.Start(startInfo) is null)
            throw new InvalidOperationException("Não foi possível iniciar a limpeza da pasta do TricBar.");
    }

    private sealed class SetupForm : Form
    {
        private readonly CheckBox _desktopShortcut;
        private readonly CheckBox _startMenuShortcuts;

        public SetupForm()
        {
            Text = "TricBar Setup";
            Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!);
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            ClientSize = new System.Drawing.Size(500, 310);
            Font = new System.Drawing.Font("Segoe UI", 10);

            var title = new Label
            {
                Text = "Instalar TricBar",
                Font = new System.Drawing.Font("Segoe UI", 18, System.Drawing.FontStyle.Bold),
                AutoSize = true,
                Location = new System.Drawing.Point(24, 20)
            };
            var destination = new Label
            {
                Text = $"Pasta de instalação: {InstallDirectory}",
                AutoSize = true,
                Location = new System.Drawing.Point(26, 68)
            };
#if TRICBAR_WEB_SETUP
            var requirements = new Label
            {
                Text = "Baixa o TricBar e instala .NET 8 e Windows App Runtime 1.6.",
                AutoSize = true,
                Location = new System.Drawing.Point(26, 96)
            };
#endif
            _desktopShortcut = new CheckBox
            {
                Text = "Criar atalho na Área de Trabalho",
                Checked = true,
                AutoSize = true,
                Location = new System.Drawing.Point(28, 137)
            };
            _startMenuShortcuts = new CheckBox
            {
                Text = "Criar atalhos no menu Iniciar (abrir e desinstalar)",
                Checked = true,
                AutoSize = true,
                Location = new System.Drawing.Point(28, 172)
            };
            var install = new Button
            {
                Text = "Instalar",
                DialogResult = DialogResult.None,
                Size = new System.Drawing.Size(112, 38),
                Location = new System.Drawing.Point(366, 250)
            };
            install.Click += Install_Click;
            var cancel = new Button
            {
                Text = "Cancelar",
                DialogResult = DialogResult.Cancel,
                Size = new System.Drawing.Size(112, 38),
                Location = new System.Drawing.Point(244, 250)
            };
#if TRICBAR_WEB_SETUP
            Controls.Add(requirements);
#endif
            Controls.AddRange([title, destination, _desktopShortcut, _startMenuShortcuts, install, cancel]);
            AcceptButton = install;
            CancelButton = cancel;
        }

        private void Install_Click(object? sender, EventArgs e)
        {
            string desktop = _desktopShortcut.Checked.ToString().ToLowerInvariant();
            string startMenu = _startMenuShortcuts.Checked.ToString().ToLowerInvariant();
            var startInfo = new ProcessStartInfo
            {
                FileName = Environment.ProcessPath ?? throw new InvalidOperationException(
                    "Não foi possível localizar o Setup.exe."),
                UseShellExecute = false,
                Arguments = $"--install --desktop {desktop} --startmenu {startMenu}"
            };
            try
            {
                Process.Start(startInfo);
                Close();
            }
            catch (System.ComponentModel.Win32Exception ex) when (ex.NativeErrorCode == 1223)
            {
                MessageBox.Show("A instalação foi cancelada porque a permissão de administrador não foi concedida.",
                    "TricBar Setup", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Não foi possível iniciar a instalação.\n\n{ex.Message}",
                    "TricBar Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }
    }

    private sealed class ProgressForm : Form
    {
        private readonly string _verb;

        public bool Succeeded { get; private set; }

        public ProgressForm(string verb, string? message = null)
        {
            _verb = verb;
            Text = "TricBar Setup";
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            ControlBox = false;
            ClientSize = new System.Drawing.Size(390, 112);
            Font = new System.Drawing.Font("Segoe UI", 10);
            Controls.Add(new Label
            {
                Text = message ?? $"Aguarde enquanto o Setup vai {verb} o TricBar...",
                AutoSize = true,
                Location = new System.Drawing.Point(20, 20)
            });
            Controls.Add(new ProgressBar
            {
                Style = ProgressBarStyle.Marquee,
                MarqueeAnimationSpeed = 25,
                Size = new System.Drawing.Size(350, 20),
                Location = new System.Drawing.Point(20, 58)
            });
        }

        public void Complete(bool showSuccessMessage = true)
        {
            Succeeded = true;
            if (showSuccessMessage)
                MessageBox.Show($"TricBar foi {(_verb == "instalar" ? "instalado" : "desinstalado")} com sucesso.",
                    "TricBar Setup", MessageBoxButtons.OK, MessageBoxIcon.Information);
            Close();
        }

        public void Fail(Exception exception)
        {
            if (exception is OperationCanceledException)
            {
                Close();
                return;
            }
            MessageBox.Show($"Não foi possível {_verb} o TricBar.\n\n{exception.Message}",
                "TricBar Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
            Close();
        }
    }
}
