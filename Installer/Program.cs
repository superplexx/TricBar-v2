using Microsoft.Win32;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
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
    private const string AppVersion = "1.0.0";

#if TRICBAR_WEB_SETUP
    private const string AppBundleUrl = "https://raw.githubusercontent.com/superplexx/TricBar-v2/webdownloads/TricBar-v2.zip";
    private const string DotNetDesktopRuntimeUrl = "https://aka.ms/dotnet/8.0/windowsdesktop-runtime-win-x64.exe";
    private const string WindowsAppRuntimeUrl = "https://aka.ms/windowsappsdk/1.6/1.6.250205002/windowsappruntimeinstall-x64.exe";
#endif

    public static string DefaultInstallDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), AppFolderName);

    private static readonly string ProgramsDirectory = Environment.GetFolderPath(
        Environment.SpecialFolder.Programs);
    private static readonly string StartMenuDirectory = Path.Combine(ProgramsDirectory, AppFolderName);
    private static readonly string DesktopShortcutPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "TricBar.lnk");

    private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    private const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    private const int DWMWCP_ROUND = 2;

    // Paleta: Preto puro (fundo), Amarelo (destaque) e Azul (detalhes)
    internal static class Theme
    {
        public static readonly Color Background = Color.FromArgb(0, 0, 0);         // preto puro
        public static readonly Color BackgroundAlt = Color.FromArgb(0, 0, 0);      // rodapé
        public static readonly Color Surface = Color.FromArgb(14, 14, 14);         // painéis / cards
        public static readonly Color Field = Color.FromArgb(22, 22, 22);           // campos
        public static readonly Color Border = Color.FromArgb(46, 46, 46);          // bordas neutras (sem roxo)
        public static readonly Color BorderBlue = Color.FromArgb(48, 48, 48);      // borda neutra mais visível
        public static readonly Color Blue = Color.FromArgb(0, 120, 212);           // azul detalhes (barra)
        public static readonly Color BlueAccent = Color.FromArgb(34, 92, 178);     // azul (hover de detalhes)
        public static readonly Color Yellow = Color.FromArgb(255, 204, 0);         // amarelo destaque
        public static readonly Color YellowHover = Color.FromArgb(255, 226, 80);
        public static readonly Color YellowPressed = Color.FromArgb(214, 170, 8);
        public static readonly Color Text = Color.FromArgb(238, 238, 242);
        public static readonly Color TextSecondary = Color.FromArgb(168, 170, 178);
        public static readonly Color TextOnYellow = Color.FromArgb(30, 24, 2);
    }

    // Callback de progresso: (porcentagem de 0 a 100, texto opcional da etapa)
    private sealed record InstallProgress(int Percent, string? Step);

    // Tipografia: Segoe UI Variable com fallback gracioso
    private static readonly string[] PreferredTextFamilies =
        ["Segoe UI Variable Text", "Segoe UI Variable", "Segoe UI"];
    private static readonly string[] PreferredDisplayFamilies =
        ["Segoe UI Variable Display", "Segoe UI Variable Text", "Segoe UI"];
    private static string[]? _cachedFamilies;

    private static string[] GetInstalledFamilies()
    {
        if (_cachedFamilies != null)
            return _cachedFamilies;
        try
        {
            var collection = new InstalledFontCollection();
            _cachedFamilies = collection.Families.Select(f => f.Name).ToArray();
        }
        catch
        {
            _cachedFamilies = [];
        }
        return _cachedFamilies;
    }

    private static Font CreateUIFont(float size, FontStyle style = FontStyle.Regular)
    {
        string[] installed = GetInstalledFamilies();
        string family = PreferredTextFamilies.FirstOrDefault(name =>
            installed.Any(f => string.Equals(f, name, StringComparison.OrdinalIgnoreCase)))
            ?? SystemFonts.MessageBoxFont?.Name ?? "Segoe UI";
        return new Font(family, size, style);
    }

    private static Font CreateDisplayFont(float size, FontStyle style = FontStyle.Regular)
    {
        string[] installed = GetInstalledFamilies();
        string family = PreferredDisplayFamilies.FirstOrDefault(name =>
            installed.Any(f => string.Equals(f, name, StringComparison.OrdinalIgnoreCase)))
            ?? SystemFonts.MessageBoxFont?.Name ?? "Segoe UI";
        return new Font(family, size, style);
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "FindWindowExW")]
    private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string? className, string? windowName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "PostMessageW")]
    private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);

    [STAThread]
    private static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();

        if (args.Length > 0 && args[0].Equals("--install", StringComparison.OrdinalIgnoreCase))
        {
            bool desktopShortcut = ReadOption(args, "--desktop", defaultValue: true);
            bool startMenuShortcuts = ReadOption(args, "--startmenu", defaultValue: true);
            bool launchAfter = ReadOption(args, "--launch", defaultValue: true);
            string installDir = ReadStringOption(args, "--dir", DefaultInstallDirectory);

            return RunElevated(
                progress => Install(installDir, desktopShortcut, startMenuShortcuts, launchAfter, progress),
                "instalar");
        }

        if (args.Length > 0 && args[0].Equals("--uninstall", StringComparison.OrdinalIgnoreCase))
        {
            return RunElevated(progress => Uninstall(progress), "desinstalar");
        }
        Application.Run(new ModernSetupForm());
        return 0;
    }

    private static bool ReadOption(string[] args, string name, bool defaultValue = false)
    {
        for (int index = 1; index + 1 < args.Length; index++)
        {
            if (args[index].Equals(name, StringComparison.OrdinalIgnoreCase))
                return bool.TryParse(args[index + 1], out bool value) ? value : defaultValue;
        }
        return defaultValue;
    }

    private static string ReadStringOption(string[] args, string name, string defaultValue)
    {
        for (int index = 1; index + 1 < args.Length; index++)
        {
            if (args[index].Equals(name, StringComparison.OrdinalIgnoreCase))
                return args[index + 1];
        }
        return defaultValue;
    }

    private static int RunElevated(Action<IProgress<InstallProgress>> operation, string verb)
    {
        if (!IsAdministrator())
        {
            try
            {
                var startInfo = new ProcessStartInfo
                {
                    FileName = Environment.ProcessPath ?? throw new InvalidOperationException(
                        "Não foi possível localizar o executável do Setup."),
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

        using var progress = new ModernProgressForm(verb);
        try
        {
            var reporter = new Progress<InstallProgress>(report => progress.Report(report.Percent, report.Step));
            progress.Shown += async (_, _) =>
            {
                try
                {
                    await Task.Run(() => operation(reporter));
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

    public static bool IsAdministrator()
    {
        using WindowsIdentity identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    public static void ValidateSystemRequirements()
    {
        if (!Environment.Is64BitOperatingSystem)
            throw new PlatformNotSupportedException("O TricBar requer uma versão de 64 bits do Windows.");

        if (Environment.OSVersion.Version.Major < 10 || Environment.OSVersion.Version.Build < 17763)
            throw new PlatformNotSupportedException("O TricBar requer o Windows 10 (versão 1809 / Build 17763) ou superior.");
    }

    private static void Install(string targetDir, bool createDesktopShortcut, bool createStartMenuShortcuts, bool launchAfter,
        IProgress<InstallProgress>? progress = null)
    {
        progress?.Report(new InstallProgress(0, "Preparando a instalação..."));
        ValidateSystemRequirements();
        EnsureApplicationClosed(targetDir);
        Directory.CreateDirectory(targetDir);

#if TRICBAR_WEB_SETUP
        InstallWebBundle(targetDir, progress);
#else
        CopyApplicationBundle(targetDir, progress);
#endif
        progress?.Report(new InstallProgress(94, "Criando atalhos e registrando o TricBar..."));
        FinishInstall(targetDir, createDesktopShortcut, createStartMenuShortcuts);
        progress?.Report(new InstallProgress(100, "Concluído!"));

        if (launchAfter)
        {
            string launcher = Path.Combine(targetDir, LauncherFileName);
            if (File.Exists(launcher))
            {
                Process.Start(new ProcessStartInfo
                {
                    FileName = launcher,
                    UseShellExecute = true,
                    WorkingDirectory = targetDir
                });
            }
        }
    }

    private static void CopyApplicationBundle(string targetDir, IProgress<InstallProgress>? progress = null)
    {
        string temporaryDirectory = Path.Combine(Path.GetTempPath(), $"TricBarSetup-{Guid.NewGuid():N}");
        Directory.CreateDirectory(temporaryDirectory);
        try
        {
            progress?.Report(new InstallProgress(5, "Preparando o pacote do TricBar..."));
            using Stream archive = Assembly.GetExecutingAssembly().GetManifestResourceStream(AppBundleResourceName)
                ?? throw new InvalidOperationException("O pacote local do TricBar não foi incluído no Setup.");
            string tempZip = Path.Combine(temporaryDirectory, "TricBar-v2.zip");
            using (var temporaryArchive = File.Create(tempZip))
            {
                archive.CopyTo(temporaryArchive);
            }

            string sourceDirectory = Path.Combine(temporaryDirectory, "AppBundle");
            progress?.Report(new InstallProgress(12, "Extraindo o pacote do TricBar..."));
            ZipFile.ExtractToDirectory(tempZip, sourceDirectory);
            progress?.Report(new InstallProgress(20, "Copiando arquivos do TricBar..."));
            CopyApplicationFiles(sourceDirectory, targetDir, progress, 20, 92);
        }
        finally
        {
            try { Directory.Delete(temporaryDirectory, recursive: true); } catch { }
        }
    }

    private static void CopyApplicationFiles(string sourceDirectory, string targetDir,
        IProgress<InstallProgress>? progress = null, int startPercent = 0, int endPercent = 100)
    {
        ValidateApplicationBundle(sourceDirectory);
        string[] sourceFiles = Directory.EnumerateFiles(sourceDirectory, "*", SearchOption.AllDirectories).ToArray();
        int total = Math.Max(sourceFiles.Length, 1);
        for (int index = 0; index < sourceFiles.Length; index++)
        {
            string sourceFile = sourceFiles[index];
            string relativePath = Path.GetRelativePath(sourceDirectory, sourceFile);
            string destination = Path.Combine(targetDir, relativePath);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Copy(sourceFile, destination, overwrite: true);
            if (progress != null && index % 20 == 0)
            {
                int percent = startPercent + (endPercent - startPercent) * (index + 1) / total;
                progress.Report(new InstallProgress(percent,
                    $"Copiando arquivos do TricBar ({index + 1} de {total})..."));
            }
        }
        if (progress != null)
            progress.Report(new InstallProgress(endPercent, $"Copiando arquivos do TricBar ({total} de {total})..."));
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
                $"O pacote de instalação está incompleto ou corrompido. Arquivos ausentes: {string.Join(", ", missingFiles)}.");
    }

#if TRICBAR_WEB_SETUP
    private static void InstallWebBundle(string targetDir, IProgress<InstallProgress>? progress = null)
    {
        string temporaryDirectory = Path.Combine(Path.GetTempPath(), $"TricBarWebSetup-{Guid.NewGuid():N}");
        Directory.CreateDirectory(temporaryDirectory);
        try
        {
            InstallWebPrerequisites(temporaryDirectory, progress);

            string archivePath = Path.Combine(temporaryDirectory, "TricBar-v2.zip");
            progress?.Report(new InstallProgress(45, "Baixando o TricBar..."));
            DownloadFile(AppBundleUrl, archivePath);
            progress?.Report(new InstallProgress(60, "Extraindo o pacote do TricBar..."));
            string sourceDirectory = Path.Combine(temporaryDirectory, "AppBundle");
            ZipFile.ExtractToDirectory(archivePath, sourceDirectory);
            progress?.Report(new InstallProgress(65, "Copiando arquivos do TricBar..."));
            CopyApplicationFiles(sourceDirectory, targetDir, progress, 65, 92);
        }
        finally
        {
            try { Directory.Delete(temporaryDirectory, recursive: true); } catch { }
        }
    }

    private static void InstallWebPrerequisites(string temporaryDirectory, IProgress<InstallProgress>? progress = null)
    {
        string dotNetInstaller = Path.Combine(temporaryDirectory, "windowsdesktop-runtime.exe");
        progress?.Report(new InstallProgress(3, "Baixando o .NET 8 Desktop Runtime..."));
        DownloadFile(DotNetDesktopRuntimeUrl, dotNetInstaller);
        progress?.Report(new InstallProgress(12, "Instalando o .NET 8 Desktop Runtime..."));
        RunPrerequisiteInstaller(dotNetInstaller, "/install /quiet /norestart", ".NET 8 Desktop Runtime");

        string appRuntimeInstaller = Path.Combine(temporaryDirectory, "windows-app-runtime.exe");
        progress?.Report(new InstallProgress(20, "Baixando o Windows App Runtime 1.6..."));
        DownloadFile(WindowsAppRuntimeUrl, appRuntimeInstaller);
        progress?.Report(new InstallProgress(28, "Instalando o Windows App Runtime 1.6..."));
        RunPrerequisiteInstaller(appRuntimeInstaller, "--quiet", "Windows App Runtime 1.6");
    }

    private static void DownloadFile(string url, string destination)
    {
        using var client = new HttpClient();
        client.DefaultRequestHeaders.UserAgent.ParseAdd($"TricBarSetup/{AppVersion}");
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
        string targetDir,
        bool createDesktopShortcut,
        bool createStartMenuShortcuts)
    {
        string setupPath = Path.Combine(targetDir, SetupFileName);
        string? currentExe = Environment.ProcessPath;
        if (!string.IsNullOrEmpty(currentExe))
        {
            string runningSetup = Path.GetFullPath(currentExe);
            if (!string.Equals(runningSetup, setupPath, StringComparison.OrdinalIgnoreCase))
                File.Copy(runningSetup, setupPath, overwrite: true);
        }

        string launcherPath = Path.Combine(targetDir, LauncherFileName);
        string uninstallPath = $"\"{setupPath}\" --uninstall";
        if (createStartMenuShortcuts)
        {
            CreateShortcut(Path.Combine(StartMenuDirectory, "TricBar.lnk"),
                launcherPath, string.Empty, "Abrir TricBar", targetDir);
            CreateShortcut(Path.Combine(StartMenuDirectory, "Desinstalar TricBar.lnk"),
                setupPath, "--uninstall", "Desinstalar TricBar", targetDir);
        }
        else
        {
            DeleteShortcut(Path.Combine(StartMenuDirectory, "TricBar.lnk"));
            DeleteShortcut(Path.Combine(StartMenuDirectory, "Desinstalar TricBar.lnk"));
        }

        if (createDesktopShortcut)
            CreateShortcut(DesktopShortcutPath, launcherPath, string.Empty, "Abrir TricBar", targetDir);
        else
            DeleteShortcut(DesktopShortcutPath);

        using RegistryKey uninstallKey = Registry.LocalMachine.CreateSubKey(UninstallKeyPath, writable: true)
            ?? throw new InvalidOperationException("Não foi possível registrar o TricBar no Windows.");
        uninstallKey.SetValue("DisplayName", "TricBar");
        uninstallKey.SetValue("DisplayVersion", AppVersion);
        uninstallKey.SetValue("Publisher", "Superplex");
        uninstallKey.SetValue("InstallLocation", targetDir);
        uninstallKey.SetValue("UninstallString", uninstallPath);
        uninstallKey.SetValue("DisplayIcon", Path.Combine(targetDir, "icon.ico"));
        uninstallKey.SetValue("NoModify", 1, RegistryValueKind.DWord);
        uninstallKey.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        uninstallKey.SetValue("InstallDate", DateTime.Now.ToString("yyyyMMdd"));
    }

    private static void CreateShortcut(string shortcutPath, string targetPath, string arguments, string description, string workingDir)
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
            shortcut.WorkingDirectory = workingDir;
            shortcut.Description = description;
            shortcut.IconLocation = Path.Combine(workingDir, "icon.ico");
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

    private static void EnsureApplicationClosed(string? installDir = null)
    {
        string dir = installDir ?? DefaultInstallDirectory;
        CloseProcessesFromInstall(LauncherFileName, dir);
        IntPtr engineWindow = FindWindowEx(new IntPtr(-3), IntPtr.Zero, "TricBarEngineHidden", null);
        if (engineWindow != IntPtr.Zero)
            PostMessage(engineWindow, 0x0010, IntPtr.Zero, IntPtr.Zero);
        CloseProcessesFromInstall(EngineFileName, dir);
    }

    private static void CloseProcessesFromInstall(string executableName, string installDir)
    {
        string expectedPath = Path.Combine(installDir, executableName);
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
                catch
                {
                    continue;
                }
                if (!string.Equals(processPath, expectedPath, StringComparison.OrdinalIgnoreCase))
                    continue;

                process.CloseMainWindow();
                if (!process.WaitForExit(3000))
                {
                    try { process.Kill(); } catch { }
                }
            }
        }
    }

    private static void Uninstall(IProgress<InstallProgress>? progress = null)
    {
        if (MessageBox.Show("Deseja realmente remover o TricBar deste computador?",
                "Desinstalar TricBar", MessageBoxButtons.YesNo, MessageBoxIcon.Question,
                MessageBoxDefaultButton.Button2) != DialogResult.Yes)
            throw new OperationCanceledException("A desinstalação foi cancelada.");

        string installDir = DefaultInstallDirectory;
        using (RegistryKey? key = Registry.LocalMachine.OpenSubKey(UninstallKeyPath))
        {
            if (key?.GetValue("InstallLocation") is string customDir && Directory.Exists(customDir))
                installDir = customDir;
        }

        progress?.Report(new InstallProgress(10, "Encerrando o TricBar..."));
        EnsureApplicationClosed(installDir);
        progress?.Report(new InstallProgress(40, "Removendo atalhos..."));
        DeleteShortcut(DesktopShortcutPath);
        DeleteShortcut(Path.Combine(StartMenuDirectory, "TricBar.lnk"));
        DeleteShortcut(Path.Combine(StartMenuDirectory, "Desinstalar TricBar.lnk"));
        if (Directory.Exists(StartMenuDirectory) && !Directory.EnumerateFileSystemEntries(StartMenuDirectory).Any())
            Directory.Delete(StartMenuDirectory);

        progress?.Report(new InstallProgress(70, "Removendo o registro do Windows..."));
        Registry.LocalMachine.DeleteSubKeyTree(UninstallKeyPath, throwOnMissingSubKey: false);

        progress?.Report(new InstallProgress(90, "Agendando a limpeza de arquivos..."));
        string scriptPath = Path.Combine(Path.GetTempPath(), $"TricBar-uninstall-{Guid.NewGuid():N}.cmd");
        string script = "@echo off\r\n" +
                        "set /a tries=0\r\n" +
                        ":retry\r\n" +
                        "timeout /t 2 /nobreak >nul 2>nul\r\n" +
                        $"rmdir /s /q \"{installDir}\" >nul 2>nul\r\n" +
                        $"if not exist \"{installDir}\" goto done\r\n" +
                        "set /a tries+=1\r\n" +
                        "if %tries% lss 60 goto retry\r\n" +
                        ":done\r\n" +
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
        progress?.Report(new InstallProgress(100, "Concluído!"));
    }

    private static void ApplyWindows11Styling(IntPtr handle)
    {
        int darkMode = 1;
        DwmSetWindowAttribute(handle, DWMWA_USE_IMMERSIVE_DARK_MODE, ref darkMode, sizeof(int));
        int cornerPreference = DWMWCP_ROUND;
        DwmSetWindowAttribute(handle, DWMWA_WINDOW_CORNER_PREFERENCE, ref cornerPreference, sizeof(int));
    }

    // Windows 11 Rounded Button
    public sealed class Win11Button : Button
    {
        public int CornerRadius { get; set; } = 14;
        public Color NormalBackColor { get; set; } = Color.FromArgb(30, 30, 30);
        public Color HoverBackColor { get; set; } = Color.FromArgb(40, 40, 40);
        public Color PressedBackColor { get; set; } = Color.FromArgb(24, 24, 24);
        public Color NormalBorderColor { get; set; } = Theme.Border;

        private bool _isHovered;
        private bool _isPressed;

        public Win11Button()
        {
            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderSize = 0;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
            Cursor = Cursors.Hand;
        }

        protected override void OnMouseEnter(EventArgs e) { _isHovered = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { _isHovered = false; _isPressed = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { _isPressed = true; Invalidate(); base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { _isPressed = false; Invalidate(); base.OnMouseUp(e); }

        protected override void OnSizeChanged(EventArgs e)
        {
            base.OnSizeChanged(e);
            UpdateRegion();
        }

        private void UpdateRegion()
        {
            if (Width <= 0 || Height <= 0) return;
            using var path = CreateRoundedRectPath(new Rectangle(0, 0, Width, Height), CornerRadius);
            Region = new Region(path);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            e.Graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;

            Color parentColor = Parent != null ? Parent.BackColor : BackColor;
            using (var bgBrush = new SolidBrush(parentColor))
                e.Graphics.FillRectangle(bgBrush, ClientRectangle);

            var rect = new Rectangle(0, 0, Width - 1, Height - 1);
            using var path = CreateRoundedRectPath(rect, CornerRadius);

            Color fill = _isPressed ? PressedBackColor : _isHovered ? HoverBackColor : NormalBackColor;
            using (var brush = new SolidBrush(fill))
                e.Graphics.FillPath(brush, path);

            if (NormalBorderColor != Color.Transparent)
            {
                using var pen = new Pen(NormalBorderColor, 1f);
                e.Graphics.DrawPath(pen, path);
            }

            TextRenderer.DrawText(e.Graphics, Text, Font, rect, ForeColor,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
        }

        public static GraphicsPath CreateRoundedRectPath(Rectangle r, int radius)
        {
            var path = new GraphicsPath();
            int d = radius * 2;
            if (d > r.Width) d = r.Width;
            if (d > r.Height) d = r.Height;
            path.AddArc(r.X, r.Y, d, d, 180, 90);
            path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            path.CloseFigure();
            return path;
        }
    }

    // Windows 11 Rounded CheckBox
    public sealed class Win11CheckBox : CheckBox
    {
        private bool _isHovered;

        public Win11CheckBox()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
            Cursor = Cursors.Hand;
            BackColor = Color.Transparent;
        }

        protected override void OnMouseEnter(EventArgs e) { _isHovered = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { _isHovered = false; Invalidate(); base.OnMouseLeave(e); }

        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            e.Graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;

            Color parentColor = Parent != null ? Parent.BackColor : Theme.Surface;
            using (var bgBrush = new SolidBrush(parentColor))
                e.Graphics.FillRectangle(bgBrush, ClientRectangle);

            int boxSize = 18;
            int boxY = (Height - boxSize) / 2;
            var boxRect = new Rectangle(0, boxY, boxSize, boxSize);

            using (var path = Win11Button.CreateRoundedRectPath(boxRect, 5))
            {
                if (Checked)
                {
                    Color fill = _isHovered ? Theme.YellowHover : Theme.Yellow;
                    using var fillBrush = new SolidBrush(fill);
                    e.Graphics.FillPath(fillBrush, path);

                    using var checkPen = new Pen(Theme.TextOnYellow, 2.2f);
                    checkPen.StartCap = LineCap.Round;
                    checkPen.EndCap = LineCap.Round;
                    e.Graphics.DrawLines(checkPen, new[]
                    {
                        new Point(boxRect.X + 4, boxRect.Y + 9),
                        new Point(boxRect.X + 8, boxRect.Y + 13),
                        new Point(boxRect.X + 14, boxRect.Y + 5)
                    });
                }
                else
                {
                    Color fill = _isHovered ? Color.FromArgb(32, 32, 36) : Theme.Field;
                    using var fillBrush = new SolidBrush(fill);
                    e.Graphics.FillPath(fillBrush, path);

                    Color border = _isHovered ? Theme.BlueAccent : Color.FromArgb(62, 66, 78);
                    using var borderPen = new Pen(border, 1.2f);
                    e.Graphics.DrawPath(borderPen, path);
                }
            }

            var textRect = new Rectangle(boxSize + 10, 0, Width - boxSize - 10, Height);
            TextRenderer.DrawText(e.Graphics, Text, Font, textRect, ForeColor,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
        }
    }

    // Windows 11 Rounded Card / Container Panel
    public sealed class Win11CardPanel : Panel
    {
        public int CornerRadius { get; set; } = 12;
        public Color BorderColor { get; set; } = Theme.Border;

        public Win11CardPanel()
        {
            BackColor = Theme.Surface;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            e.Graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;

            Color parentColor = Parent != null ? Parent.BackColor : Theme.Background;
            using (var bgBrush = new SolidBrush(parentColor))
                e.Graphics.FillRectangle(bgBrush, ClientRectangle);

            var rect = new Rectangle(0, 0, Width - 1, Height - 1);
            using var path = Win11Button.CreateRoundedRectPath(rect, CornerRadius);
            using var brush = new SolidBrush(BackColor);
            e.Graphics.FillPath(brush, path);

            if (BorderColor != Color.Transparent)
            {
                using var pen = new Pen(BorderColor, 1f);
                e.Graphics.DrawPath(pen, path);
            }
        }
    }

    // Barra de progresso arredondada com preenchimento azul e porcentagem
    public sealed class RoundedProgressBar : Control
    {
        private int _minimum, _maximum = 100, _value;

        public Color TrackColor { get; set; } = Theme.Field;
        public Color FillColor { get; set; } = Theme.Blue;
        public int CornerRadius { get; set; } = 6;

        public int Minimum { get => _minimum; set => _minimum = value; }
        public int Maximum { get => _maximum; set => _maximum = Math.Max(value, _minimum); }
        public int Value
        {
            get => _value;
            set
            {
                _value = Math.Clamp(value, _minimum, _maximum);
                Invalidate();
            }
        }

        public RoundedProgressBar()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            DoubleBuffered = true;
            BackColor = Theme.Surface;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            e.Graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;

            Color parentColor = Parent != null ? Parent.BackColor : Theme.Surface;
            using (var bgBrush = new SolidBrush(parentColor))
                e.Graphics.FillRectangle(bgBrush, ClientRectangle);

            var rect = new Rectangle(0, 0, Width - 1, Height - 1);
            using var trackPath = Win11Button.CreateRoundedRectPath(rect, CornerRadius);
            using (var trackBrush = new SolidBrush(TrackColor))
                e.Graphics.FillPath(trackBrush, trackPath);

            double range = Math.Max(_maximum - _minimum, 1);
            double fraction = Math.Clamp((_value - _minimum) / range, 0.0, 1.0);
            int fillWidth = (int)Math.Round((Width - 2) * fraction);
            if (fillWidth > 0)
            {
                var fillRect = new Rectangle(1, 1, fillWidth, Height - 3);
                using var fillPath = Win11Button.CreateRoundedRectPath(fillRect, CornerRadius);
                using var fillBrush = new SolidBrush(FillColor);
                e.Graphics.FillPath(fillBrush, fillPath);
            }
        }
    }

    private sealed class ModernSetupForm : Form
    {
        private readonly TextBox _pathTextBox;
        private readonly Win11CheckBox _desktopShortcut;
        private readonly Win11CheckBox _startMenuShortcuts;
        private readonly Win11CheckBox _launchAfterInstall;
        private readonly Win11Button _installBtn;
        private readonly Win11Button _cancelBtn;
        private readonly Win11Button _browseBtn;

        public ModernSetupForm()
        {
            Text = "Instalação do TricBar";
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            MinimizeBox = true;
            ClientSize = new Size(540, 440);
            BackColor = Theme.Background;
            ForeColor = Theme.Text;
            Font = CreateUIFont(9.75f);

            try
            {
                if (Environment.ProcessPath is string exePath)
                    Icon = Icon.ExtractAssociatedIcon(exePath);
            }
            catch { }

            // Header banner
            var headerPanel = new Panel
            {
                Dock = DockStyle.Top,
                Height = 84,
                BackColor = Theme.Surface
            };

            var titleLabel = new Label
            {
                Text = "TricBar",
                Font = CreateDisplayFont(19f, FontStyle.Bold),
                ForeColor = Theme.Text,
                AutoSize = true,
                Location = new Point(28, 12)
            };

            var versionBadge = new Label
            {
                Text = $"v{AppVersion} Setup",
                Font = CreateUIFont(9f),
                ForeColor = Theme.TextSecondary,
                AutoSize = true
            };

            var subtitleLabel = new Label
            {
                Text = "Personalização avançada e elegante para sua barra de tarefas",
                Font = CreateUIFont(9.75f),
                ForeColor = Theme.TextSecondary,
                AutoSize = true,
                Location = new Point(28, 48)
            };

            headerPanel.Controls.AddRange([titleLabel, subtitleLabel]);

            // Destination directory card (Rounded Windows 11 style)
            var pathCard = new Win11CardPanel
            {
                Location = new Point(24, 104),
                Size = new Size(492, 78),
                CornerRadius = 12,
                BackColor = Theme.Surface,
                BorderColor = Theme.Border
            };

            var pathLabel = new Label
            {
                Text = "Pasta de Instalação:",
                ForeColor = Color.FromArgb(224, 224, 230),
                Font = CreateUIFont(9.5f, FontStyle.Bold),
                Location = new Point(14, 12),
                AutoSize = true,
                BackColor = Color.Transparent
            };

            // Rounded container for the textbox
            var textContainer = new Win11CardPanel
            {
                Location = new Point(14, 36),
                Size = new Size(356, 32),
                CornerRadius = 10,
                BackColor = Theme.Field,
                BorderColor = Theme.BorderBlue
            };

            _pathTextBox = new TextBox
            {
                Text = DefaultInstallDirectory,
                Location = new Point(12, 6),
                Size = new Size(332, 20),
                BackColor = Theme.Field,
                ForeColor = Theme.Text,
                BorderStyle = BorderStyle.None,
                ReadOnly = true,
                Font = CreateUIFont(9.75f)
            };
            textContainer.Controls.Add(_pathTextBox);

            _browseBtn = new Win11Button
            {
                Text = "Procurar...",
                Location = new Point(380, 36),
                Size = new Size(98, 32),
                CornerRadius = 10,
                NormalBackColor = Color.FromArgb(34, 34, 34),
                HoverBackColor = Color.FromArgb(44, 44, 44),
                PressedBackColor = Color.FromArgb(26, 26, 26),
                NormalBorderColor = Theme.Border,
                ForeColor = Theme.Text
            };
            _browseBtn.Click += BrowseBtn_Click;

            pathCard.Controls.AddRange([pathLabel, textContainer, _browseBtn]);

            // Options card (Rounded Windows 11 style)
            var optionsCard = new Win11CardPanel
            {
                Location = new Point(24, 200),
                Size = new Size(492, 152),
                CornerRadius = 12,
                BackColor = Theme.Surface,
                BorderColor = Theme.Border
            };

            var optionsLabel = new Label
            {
                Text = "Opções:",
                ForeColor = Color.FromArgb(224, 224, 230),
                Font = CreateUIFont(9.5f, FontStyle.Bold),
                Location = new Point(14, 12),
                AutoSize = true,
                BackColor = Color.Transparent
            };

            _desktopShortcut = new Win11CheckBox
            {
                Text = "Criar atalho na Área de Trabalho",
                Checked = true,
                Location = new Point(18, 42),
                Size = new Size(450, 24),
                ForeColor = Theme.Text,
                BackColor = Color.Transparent
            };

            _startMenuShortcuts = new Win11CheckBox
            {
                Text = "Criar atalhos no Menu Iniciar (abrir e desinstalar)",
                Checked = true,
                Location = new Point(18, 74),
                Size = new Size(450, 24),
                ForeColor = Theme.Text,
                BackColor = Color.Transparent
            };

            _launchAfterInstall = new Win11CheckBox
            {
                Text = "Iniciar o TricBar imediatamente após a instalação",
                Checked = true,
                Location = new Point(18, 106),
                Size = new Size(450, 24),
                ForeColor = Theme.Text,
                BackColor = Color.Transparent
            };

            optionsCard.Controls.AddRange([optionsLabel, _desktopShortcut, _startMenuShortcuts, _launchAfterInstall]);

            // Bottom action panel with rounded buttons
            var bottomPanel = new Panel
            {
                Dock = DockStyle.Bottom,
                Height = 68,
                BackColor = Theme.BackgroundAlt
            };

            // Centraliza os cards verticalmente na área útil (entre cabeçalho e rodapé)
            int cardGap = 18;
            int usefulHeight = ClientSize.Height - headerPanel.Height - bottomPanel.Height;
            int contentHeight = pathCard.Height + cardGap + optionsCard.Height;
            int cardsTop = headerPanel.Height + Math.Max(8, (usefulHeight - contentHeight) / 2);
            int centerX = (ClientSize.Width - pathCard.Width) / 2;
            pathCard.Location = new Point(centerX, cardsTop);
            optionsCard.Location = new Point(centerX, cardsTop + pathCard.Height + cardGap);

            // Badge de versão no canto inferior esquerdo do rodapé
            versionBadge.Location = new Point(24, (bottomPanel.Height - versionBadge.Height) / 2);

            _cancelBtn = new Win11Button
            {
                Text = "Cancelar",
                DialogResult = DialogResult.Cancel,
                Size = new Size(116, 38),
                Location = new Point(264, 15),
                CornerRadius = 16,
                NormalBackColor = Color.FromArgb(28, 28, 28),
                HoverBackColor = Color.FromArgb(40, 40, 40),
                PressedBackColor = Color.FromArgb(22, 22, 22),
                NormalBorderColor = Theme.Border,
                ForeColor = Theme.Text
            };

            _installBtn = new Win11Button
            {
                Text = "Instalar",
                Size = new Size(124, 38),
                Location = new Point(392, 15),
                CornerRadius = 16,
                NormalBackColor = Theme.Yellow,
                HoverBackColor = Theme.YellowHover,
                PressedBackColor = Theme.YellowPressed,
                NormalBorderColor = Color.Transparent,
                ForeColor = Theme.TextOnYellow,
                Font = CreateUIFont(9.75f, FontStyle.Bold)
            };
            _installBtn.Click += Install_Click;

            bottomPanel.Controls.AddRange([versionBadge, _cancelBtn, _installBtn]);

            Controls.AddRange([
                headerPanel,
                pathCard,
                optionsCard,
                bottomPanel
            ]);

            AcceptButton = _installBtn;
            CancelButton = _cancelBtn;
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            ApplyWindows11Styling(Handle);
        }

        private void BrowseBtn_Click(object? sender, EventArgs e)
        {
            using var dialog = new FolderBrowserDialog
            {
                Description = "Selecione a pasta de instalação do TricBar",
                SelectedPath = _pathTextBox.Text,
                UseDescriptionForTitle = true
            };
            if (dialog.ShowDialog(this) == DialogResult.OK && !string.IsNullOrWhiteSpace(dialog.SelectedPath))
            {
                _pathTextBox.Text = Path.Combine(dialog.SelectedPath, AppFolderName);
            }
        }

        private void Install_Click(object? sender, EventArgs e)
        {
            string installPath = _pathTextBox.Text.Trim();
            if (string.IsNullOrEmpty(installPath))
            {
                MessageBox.Show("Por favor, informe uma pasta de instalação válida.", "TricBar Setup",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            string desktop = _desktopShortcut.Checked.ToString().ToLowerInvariant();
            string startMenu = _startMenuShortcuts.Checked.ToString().ToLowerInvariant();
            string launch = _launchAfterInstall.Checked.ToString().ToLowerInvariant();

            var startInfo = new ProcessStartInfo
            {
                FileName = Environment.ProcessPath ?? throw new InvalidOperationException("Não foi possível localizar o executável de instalação."),
                UseShellExecute = false,
                Arguments = $"--install --desktop {desktop} --startmenu {startMenu} --launch {launch} --dir \"{installPath}\""
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

    private sealed class ModernProgressForm : Form
    {
        private readonly string _verb;
        private readonly Label _statusLabel;
        private readonly RoundedProgressBar _progressBar;
        private readonly Label _percentLabel;

        public bool Succeeded { get; private set; }

        public ModernProgressForm(string verb, string? message = null)
        {
            _verb = verb;
            Text = "TricBar Setup";
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            ControlBox = false;
            ClientSize = new Size(460, 150);
            BackColor = Theme.Background;
            ForeColor = Theme.Text;
            Font = CreateUIFont(9.75f);

            var card = new Win11CardPanel
            {
                Location = new Point(18, 16),
                Size = new Size(424, 118),
                CornerRadius = 10,
                BackColor = Theme.Surface,
                BorderColor = Theme.Border
            };

            _statusLabel = new Label
            {
                Text = message ?? $"Aguarde enquanto o Setup conclui a operação de {verb} o TricBar...",
                AutoSize = false,
                Size = new Size(392, 40),
                Location = new Point(16, 14),
                ForeColor = Theme.Text,
                BackColor = Color.Transparent
            };

            _percentLabel = new Label
            {
                Text = "0%",
                AutoSize = false,
                Size = new Size(44, 18),
                Location = new Point(364, 64),
                TextAlign = ContentAlignment.MiddleRight,
                Font = CreateUIFont(10f, FontStyle.Bold),
                ForeColor = Theme.Yellow,
                BackColor = Color.Transparent
            };

            _progressBar = new RoundedProgressBar
            {
                Size = new Size(344, 18),
                Location = new Point(16, 64),
                TrackColor = Theme.Field,
                FillColor = Theme.Blue
            };

            card.Controls.AddRange([_statusLabel, _progressBar, _percentLabel]);
            Controls.Add(card);

            HandleCreated += (_, _) => ApplyWindows11Styling(Handle);
        }

        public void Report(int percent, string? step)
        {
            if (IsDisposed || Disposing)
                return;
            try
            {
                if (InvokeRequired)
                {
                    BeginInvoke(() => Report(percent, step));
                    return;
                }
            }
            catch (ObjectDisposedException)
            {
                return;
            }

            percent = Math.Clamp(percent, 0, 100);
            _progressBar.Value = percent;
            _percentLabel.Text = $"{percent}%";
            if (!string.IsNullOrWhiteSpace(step))
                _statusLabel.Text = step;
        }

        public void Complete(bool showSuccessMessage = true)
        {
            Succeeded = true;
            try
            {
                Report(100, $"O TricBar foi {(_verb == "instalar" ? "instalado" : "desinstalado")} com sucesso!");
            }
            catch { }
            if (showSuccessMessage)
            {
                string action = _verb == "instalar" ? "instalado" : "desinstalado";
                MessageBox.Show($"O TricBar foi {action} com sucesso!",
                    "TricBar Setup", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
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
