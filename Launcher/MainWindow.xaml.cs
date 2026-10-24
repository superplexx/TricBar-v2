using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Windowing;
using Microsoft.UI.Dispatching;
using Microsoft.Win32;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using Windows.Foundation;
using Windows.UI;
using Windows.UI.ViewManagement;
using Windows.Graphics;
using WinRT.Interop;

namespace TricBarConfigurator;

public sealed partial class MainWindow : Window
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ConfigDirectoryName = "TricBar";
    private const string ConfigFileName = "config.ini";
    private const uint WmClose = 0x0010;
    private static readonly uint ReloadMessage = RegisterWindowMessageW("TricBar_Reload");
    private static readonly string[] ModeKeys = ["normal", "clear", "blur", "acrylic", "liquidglass", "color"];
    private static readonly bool IsEnglish = CultureInfo.CurrentUICulture.Name.StartsWith(
        "en", StringComparison.OrdinalIgnoreCase);
    private static readonly IReadOnlyDictionary<string, string> EnglishText =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["Início"] = "Home",
            ["Aparência"] = "Appearance",
            ["Configurações"] = "Settings",
            ["Iniciar TricBar"] = "Start TricBar",
            ["Verificando..."] = "Checking...",
            ["Bem-vindo ao TricBar"] = "Welcome to TricBar",
            ["Sua barra de tarefas, do seu jeito."] = "Your taskbar, your way.",
            ["ESTADO DO MOTOR"] = "ENGINE STATUS",
            ["APARÊNCIA ATUAL"] = "CURRENT APPEARANCE",
            ["Sistema operacional"] = "Operating system",
            ["Modo da barra"] = "Taskbar mode",
            ["Compatibilidade"] = "Compatibility",
            ["Pronto"] = "Ready",
            ["Desfoque suave"] = "Subtle blur",
            ["Transparência com textura"] = "Textured transparency",
            ["Transparente"] = "Transparent",
            ["Revela o papel de parede"] = "Reveals your wallpaper",
            ["Personalizar aparência"] = "Customize appearance",
            ["Escolha o estilo e ajuste a tonalidade da barra."] = "Choose a style and adjust the taskbar tint.",
            ["Estilo da barra de tarefas"] = "Taskbar style",
            ["Normal (padrão do Windows)"] = "Normal (Windows default)",
            ["Blur (desfoque)"] = "Blur",
            ["Liquid Glass"] = "Liquid Glass",
            ["Tonalidade Liquid Glass"] = "Liquid Glass tone",
            ["Escuro"] = "Dark",
            ["Claro"] = "Light",
            ["Liquid Glass (Escuro)"] = "Dark Liquid Glass",
            ["Liquid Glass (Claro)"] = "Light Liquid Glass",
            ["Selecione o tom escuro ou claro do Liquid Glass."] =
                "Choose the dark or light Liquid Glass tone.",
            ["Usa o visual padrão do Windows."] = "Uses the default Windows appearance.",
            ["Barra transparente que revela o papel de parede."] =
                "Transparent taskbar that reveals your wallpaper.",
            ["Aplica desfoque ao fundo da barra."] = "Applies blur to the taskbar background.",
            ["Aplica uma cor sólida com opacidade ajustável."] =
                "Applies a solid color with adjustable opacity.",
            ["Vidro translúcido flutuante"] = "Floating translucent glass",
            ["Liquid Glass requer Windows 11 22H2 ou posterior. No Windows 10, o TricBar usa Acrylic como alternativa."] =
                "Liquid Glass requires Windows 11 22H2 or later. On Windows 10, TricBar uses Acrylic as a fallback.",
            ["Cor sólida"] = "Solid color",
            ["Tonalidade e opacidade"] = "Tint and opacity",
            ["O cinza neutro se mistura ao fundo nos modos Blur e Acrylic."] =
                "Neutral gray blends into the background in Blur and Acrylic modes.",
            ["Escolher cor"] = "Choose color",
            ["Opacidade da cor:"] = "Color opacity:",
            ["Prévia da tonalidade"] = "Tint preview",
            ["A prévia mostra a cor escolhida; o efeito aparece na barra de tarefas."] =
                "The preview shows your selected color; the effect appears on the taskbar.",
            ["Inicialização e comportamento do TricBar."] = "Startup and TricBar behavior.",
            ["Opções"] = "Options",
            ["Iniciar o TricBar com o Windows"] = "Start TricBar with Windows",
            ["Centralizar ícones da barra (Windows 10)"] = "Center taskbar icons (Windows 10)",
            ["Estado do TricBar"] = "TricBar status",
            ["Não foi possível carregar as configurações."] = "Could not load settings.",
            ["Não foi possível salvar as configurações."] = "Could not save settings.",
            ["TricBar.exe não foi encontrado ao lado do Launcher."] =
                "TricBar.exe was not found next to the Launcher.",
            ["Não foi possível iniciar o TricBar."] = "Could not start TricBar.",
            ["TricBar em execução"] = "TricBar is running",
            ["TricBar parado"] = "TricBar is stopped",
            ["TricBar está ativo"] = "TricBar is active",
            ["TricBar está parado"] = "TricBar is stopped",
            ["Em execução"] = "Running",
            ["Parado"] = "Stopped",
            ["Parar TricBar"] = "Stop TricBar",
            ["Windows 11 22H2+: o hook TricBar.dll é necessário. Não use junto com TranslucentTB nem com o Windows 11 Taskbar Styler do Windhawk."] =
                "Windows 11 22H2+: the TricBar.dll hook is required. Do not use with TranslucentTB or Windhawk's Windows 11 Taskbar Styler.",
            ["TricBar.dll não foi encontrada. Ela é necessária para efeitos de transparência, Blur, Acrylic e cor no Windows 11 22H2+."] =
                "TricBar.dll was not found. It is required for transparency, Blur, Acrylic, and color effects on Windows 11 22H2+.",
            ["Não use junto com TranslucentTB nem com o Windows 11 Taskbar Styler do Windhawk."] =
                "Do not use with TranslucentTB or Windhawk's Windows 11 Taskbar Styler.",
            ["Verifique os avisos"] = "Check warnings",
            ["Normal"] = "Normal",
            ["Não foi possível alterar a inicialização com o Windows."] =
                "Could not change the startup setting.",
            ["Selecionar"] = "Select",
            ["Cancelar"] = "Cancel",
            ["OK"] = "OK"
        };

    private readonly string _configDirectory = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), ConfigDirectoryName);
    private readonly string _configPath;
    private readonly DispatcherTimer _saveTimer = new() { Interval = TimeSpan.FromMilliseconds(180) };
    private readonly DispatcherTimer _statusTimer = new() { Interval = TimeSpan.FromSeconds(1) };
    private readonly UISettings _uiSettings = new();
    private DispatcherQueue _dispatcherQueue = null!;
    private AppWindow _appWindow = null!;
    private Color _accentColor;
    private int _rgb;
    private bool _loading;

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "FindWindowExW")]
    private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string? className, string? windowName);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern uint RegisterWindowMessageW(string message);
    [DllImport("user32.dll")]
    private static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint desiredAccess, bool inheritHandle, uint processId);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool QueryFullProcessImageName(
        IntPtr process, uint flags, StringBuilder executableName, ref uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    private static IntPtr FindEngine() => FindWindowEx(new IntPtr(-3), IntPtr.Zero, "TricBarEngineHidden", null);

    private static string GetProcessImagePath(uint processId)
    {
        const uint processQueryLimitedInformation = 0x1000;
        IntPtr process = OpenProcess(processQueryLimitedInformation, false, processId);
        if (process == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Não foi possível abrir o processo do motor.");

        try
        {
            var path = new StringBuilder(32768);
            uint size = (uint)path.Capacity;
            if (!QueryFullProcessImageName(process, 0, path, ref size))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Não foi possível obter o caminho do motor.");
            return path.ToString();
        }
        finally
        {
            CloseHandle(process);
        }
    }

    public MainWindow()
    {
        InitializeComponent();
        _dispatcherQueue = DispatcherQueue.GetForCurrentThread();
        SystemBackdrop = new DesktopAcrylicBackdrop();
        var windowId = Win32Interop.GetWindowIdFromWindow(WindowNative.GetWindowHandle(this));
        _appWindow = AppWindow.GetFromWindowId(windowId);
        if (_appWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.SetBorderAndTitleBar(true, true);
            presenter.IsMinimizable = true;
            presenter.IsMaximizable = true;
            presenter.IsResizable = false;
        }
        _appWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "icon.ico"));
        _appWindow.Resize(new SizeInt32(1000, 700));
        _appWindow.Title = string.Empty;

        Color baseTint = Color.FromArgb(170, 12, 15, 21);
        Color darkerTint = Color.FromArgb(185, 9, 12, 17);
        Color hoverTint = Color.FromArgb(190, 20, 23, 30);
        Color pressedTint = Color.FromArgb(205, 26, 29, 37);

        _appWindow.TitleBar.BackgroundColor = baseTint;
        _appWindow.TitleBar.InactiveBackgroundColor = darkerTint;
        _appWindow.TitleBar.ForegroundColor = Color.FromArgb(255, 229, 229, 232);
        _appWindow.TitleBar.InactiveForegroundColor = Color.FromArgb(165, 229, 229, 232);
        _appWindow.TitleBar.ButtonBackgroundColor = baseTint;
        _appWindow.TitleBar.ButtonInactiveBackgroundColor = darkerTint;
        _appWindow.TitleBar.ButtonForegroundColor = Color.FromArgb(255, 229, 229, 232);
        _appWindow.TitleBar.ButtonHoverForegroundColor = Color.FromArgb(255, 255, 255, 255);
        _appWindow.TitleBar.ButtonPressedForegroundColor = Color.FromArgb(255, 255, 255, 255);
        _appWindow.TitleBar.ButtonHoverBackgroundColor = hoverTint;
        _appWindow.TitleBar.ButtonPressedBackgroundColor = pressedTint;
        _uiSettings.ColorValuesChanged += (_, _) => _dispatcherQueue.TryEnqueue(ApplySystemAccent);
        ApplySystemAccent();
        _configPath = Path.Combine(_configDirectory, ConfigFileName);
        Title = string.Empty;
        _saveTimer.Tick += (_, _) =>
        {
            _saveTimer.Stop();
            SaveConfig();
        };
        _statusTimer.Tick += (_, _) => UpdateStatus();
    }

    private static string T(string portuguese) =>
        IsEnglish && EnglishText.TryGetValue(portuguese, out string? english) ? english : portuguese;

    private static void ApplyLocalization(DependencyObject element)
    {
        switch (element)
        {
            case TextBlock textBlock:
                textBlock.Text = T(textBlock.Text);
                break;
            case CheckBox checkBox when checkBox.Content is string checkBoxText:
                checkBox.Content = T(checkBoxText);
                break;
            case Button button when button.Content is string buttonText:
                button.Content = T(buttonText);
                break;
            case ComboBoxItem item when item.Content is string itemText:
                item.Content = T(itemText);
                break;
            case TextBox textBox:
                textBox.PlaceholderText = T(textBox.PlaceholderText);
                break;
        }

        int childCount = VisualTreeHelper.GetChildrenCount(element);
        for (int index = 0; index < childCount; index++)
            ApplyLocalization(VisualTreeHelper.GetChild(element, index));
    }

    private void Content_Loaded(object sender, RoutedEventArgs e)
    {
        ApplyLocalization(RootGrid);
        foreach (ComboBoxItem item in ModeSelector.Items)
            if (item.Content is string text)
                item.Content = T(text);
        foreach (ComboBoxItem item in LiquidGlassToneSelector.Items)
        if (item.Content is string text)
            item.Content = T(text);
        LoadConfig();
        ShowPage(HomePage);
        UpdateEnabled();
        UpdateModeSummary();
        UpdateWarning();
        EnsureEngineVersion();
        UpdateStatus();
        _statusTimer.Start();
    }

    private void EnsureEngineVersion()
    {
        IntPtr engineWindow = FindEngine();
        if (engineWindow == IntPtr.Zero)
        {
            StartEngine();
            return;
        }

        GetWindowThreadProcessId(engineWindow, out uint processId);
        if (processId == 0)
        {
            ShowMessage(T("Não foi possível identificar o processo do motor TricBar."));
            return;
        }

        try
        {
            string runningPath = GetProcessImagePath(processId);
            if (string.Equals(Path.GetFullPath(runningPath), Path.GetFullPath(EnginePath),
                    StringComparison.OrdinalIgnoreCase))
                return;

            using Process? engine = Process.GetProcessById((int)processId);
            if (!PostMessageW(engineWindow, WmClose, IntPtr.Zero, IntPtr.Zero) ||
                !engine.WaitForExit(3000))
            {
                ShowMessage(T("Feche o TricBar antigo antes de usar esta versão."));
                return;
            }

            StartEngine();
        }
        catch (Exception ex)
        {
            App.LogError($"Engine version check failed: {ex}");
            ShowError(T("Não foi possível atualizar o motor TricBar em execução."), ex);
        }
    }

    private void LoadConfig()
    {
        _loading = true;
        try
        {
            string mode = "acrylic";
            string liquidGlassTone = "dark";
            int color = 0x808080;
            int opacity = 60;
            bool center = true;
            if (File.Exists(_configPath))
            {
                foreach (string line in File.ReadLines(_configPath))
                {
                    int equals = line.IndexOf('=');
                    if (equals <= 0) continue;
                    string key = line[..equals].Trim();
                    string value = line[(equals + 1)..].Trim();
                    switch (key.ToLowerInvariant())
                    {
                        case "mode": mode = value.ToLowerInvariant(); break;
                        case "color":
                            string hex = value.TrimStart('#');
                            if (hex.Length == 6 && int.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int parsed))
                                color = parsed;
                            break;
                        case "opacity":
                            if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out int parsedOpacity))
                                opacity = Math.Clamp(parsedOpacity, 0, 100);
                            break;
                        case "center": center = value != "0"; break;
                    }
                }
            }
            if (mode is "lightliquidglass" or "mica" or "macos")
            {
                mode = "liquidglass";
                liquidGlassTone = "light";
            }
            else if (mode == "darkliquidglass")
            {
                mode = "liquidglass";
            }
            else if (mode == "liquidglass")
            {
                liquidGlassTone = "light";
            }
            int index = Array.IndexOf(ModeKeys, mode);
            ModeSelector.SelectedIndex = index < 0 ? 3 : index;
            LiquidGlassToneSelector.SelectedIndex = liquidGlassTone == "light" ? 1 : 0;
            _rgb = color;
            HexColor.Text = FormatColor(_rgb);
            SetPreview(_rgb);
            OpacitySlider.Value = opacity;
            UpdateTonalPreview();
            OpacityLabel.Text = $"{T("Opacidade da cor:")} {opacity}%";
            CenterCheck.IsChecked = center;
            CenterCheck.IsEnabled = Environment.OSVersion.Version.Build < 22000;

            using RegistryKey? runKey = Registry.CurrentUser.OpenSubKey(RunKey);
            StartupCheck.IsChecked = runKey?.GetValue("TricBar") is not null;
        }
        catch (Exception ex)
        {
            ShowError(T("Não foi possível carregar as configurações."), ex);
        }
        finally
        {
            _loading = false;
        }
    }

    private void SaveConfig()
    {
        try
        {
            Directory.CreateDirectory(_configDirectory);
            string mode = GetSelectedMode();
            string text =
                $"mode={mode}\r\n" +
                $"color={_rgb.ToString("X6", CultureInfo.InvariantCulture)}\r\n" +
                $"opacity={(int)OpacitySlider.Value}\r\n" +
                $"center={(CenterCheck.IsChecked == true ? 1 : 0)}\r\n";
            string temporary = _configPath + ".tmp";
            File.WriteAllText(temporary, text);
            File.Move(temporary, _configPath, true);
            IntPtr engine = FindEngine();
            if (engine != IntPtr.Zero && ReloadMessage != 0)
                PostMessageW(engine, ReloadMessage, IntPtr.Zero, IntPtr.Zero);
        }
        catch (Exception ex)
        {
            ShowError(T("Não foi possível salvar as configurações."), ex);
        }
    }

    private static string EnginePath => Path.Combine(AppContext.BaseDirectory, "TricBar.exe");

    private void StartEngine()
    {
        try
        {
            if (!File.Exists(EnginePath))
            {
                ShowMessage(T("TricBar.exe não foi encontrado ao lado do Launcher."));
                return;
            }
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(EnginePath)
            {
                UseShellExecute = false,
                WorkingDirectory = AppContext.BaseDirectory
            });
        }
        catch (Exception ex)
        {
            ShowError(T("Não foi possível iniciar o TricBar."), ex);
        }
    }

    private void OpenConfigButton_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            Directory.CreateDirectory(_configDirectory);
            Process.Start(new ProcessStartInfo
            {
                FileName = _configPath,
                UseShellExecute = true,
                WorkingDirectory = _configDirectory
            });
        }
        catch (Exception ex)
        {
            ShowError(T("Não foi possível abrir o arquivo de configuração."), ex);
        }
    }

    private void ToggleButton_Click(object sender, RoutedEventArgs e)
    {
        IntPtr engine = FindEngine();
        if (engine != IntPtr.Zero) PostMessageW(engine, WmClose, IntPtr.Zero, IntPtr.Zero);
        else StartEngine();
        UpdateStatus();
    }

    private void UpdateStatus()
    {
        bool running = FindEngine() != IntPtr.Zero;
        StatusText.Text = running ? T("TricBar em execução") : T("TricBar parado");
        SidebarStatusText.Text = running ? T("TricBar está ativo") : T("TricBar está parado");
        HomeEngineStatus.Text = running ? T("Em execução") : T("Parado");
        EngineStatusDot.Fill = new SolidColorBrush(running
            ? Color.FromArgb(255, 91, 190, 130)
            : Color.FromArgb(255, 196, 106, 106));
        EngineActionText.Text = running ? T("Parar TricBar") : T("Iniciar TricBar");
        EngineIcon.Symbol = running ? Symbol.Stop : Symbol.Play;
        SettingsToggleButton.Content = running ? T("Parar TricBar") : T("Iniciar TricBar");
    }

    private void UpdateWarning()
    {
        string warning = string.Empty;
        if (Environment.OSVersion.Version.Build >= 22621)
        {
            warning = File.Exists(Path.Combine(AppContext.BaseDirectory, "TricBar.dll"))
                ? T("Windows 11 22H2+: o hook TricBar.dll é necessário. Não use junto com TranslucentTB nem com o Windows 11 Taskbar Styler do Windhawk.")
                : T("TricBar.dll não foi encontrada. Ela é necessária para efeitos de transparência, Blur, Acrylic e cor no Windows 11 22H2+.");
        }
        else if (Environment.OSVersion.Version.Build >= 22000)
        {
            warning = T("Não use junto com TranslucentTB nem com o Windows 11 Taskbar Styler do Windhawk.");
        }
        WarningText.Text = warning;
        SettingsWarningText.Text = warning;
        CompatibilityText.Text = warning.Length == 0 ? T("Pronto") : T("Verifique os avisos");
        CompatibilityCard.Visibility = warning.Length == 0 ? Visibility.Collapsed : Visibility.Visible;
    }

    private void UpdateEnabled()
    {
        string mode = (ModeSelector.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "clear";
        bool useColor = ModeSelector.SelectedIndex >= 2;
        HexColor.IsEnabled = useColor;
        foreach (var child in Presets.Children)
            if (child is Button button) button.IsEnabled = useColor;
        OpacitySlider.IsEnabled = useColor;
        LiquidGlassTonePanel.Visibility = mode == "liquidglass" ? Visibility.Visible : Visibility.Collapsed;
        LiquidGlassCompatibilityWarning.Text = T(
            "Liquid Glass requer Windows 11 22H2 ou posterior. No Windows 10, o TricBar usa Acrylic como alternativa.");
        bool liquidGlassCompat = mode == "liquidglass" && Environment.OSVersion.Version.Build < 22621;
        LiquidGlassCompatibilityWarning.Visibility = liquidGlassCompat ? Visibility.Visible : Visibility.Collapsed;
    }

    private string GetSelectedMode()
    {
        string mode = (ModeSelector.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "clear";
        if (mode != "liquidglass") return mode;
        string tone = (LiquidGlassToneSelector.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "dark";
        return tone == "light" ? "lightliquidglass" : "darkliquidglass";
    }

    private void ScheduleSave()
    {
        if (_loading) return;
        _saveTimer.Stop();
        _saveTimer.Start();
    }

    private void SetColor(int color)
    {
        _rgb = color & 0xFFFFFF;
        _loading = true;
        HexColor.Text = FormatColor(_rgb);
        SetPreview(_rgb);
        _loading = false;
        ScheduleSave();
    }

    private static string FormatColor(int color) => $"#{color:X6}";

    private void SetPreview(int color)
    {
        ColorPreview.Background = new SolidColorBrush(Color.FromArgb(
            255, (byte)(color >> 16), (byte)(color >> 8), (byte)color));
        UpdateTonalPreview();
        HomeColorPreview.Background = new SolidColorBrush(Color.FromArgb(
            255, (byte)(color >> 16), (byte)(color >> 8), (byte)color));
    }

    private void ModeSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ModeSelector is null) return;
        UpdateEnabled();
        UpdateModeSummary();
        if (!_loading) ScheduleSave();
    }

    private void LiquidGlassToneSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LiquidGlassToneSelector is null) return;
        UpdateModeSummary();
        if (!_loading) ScheduleSave();
    }

    private void UpdateModeSummary()
    {
        if (ModeSelector.SelectedItem is not ComboBoxItem selected) return;
        string mode = GetSelectedMode();
        string style = selected.Tag?.ToString() ?? "clear";
        string label = mode switch
        {
            "normal" => "Normal",
            "clear" => T("Transparente"),
            "blur" => "Blur",
            "acrylic" => "Acrylic",
            "lightliquidglass" => T("Liquid Glass (Claro)"),
            "darkliquidglass" => T("Liquid Glass (Escuro)"),
            "color" => T("Cor sólida"),
            _ => "Acrylic"
        };
        HomeModeText.Text = label;
        HomeModeDetailText.Text = label;
        ModeDescriptionText.Text = T(style switch
        {
            "normal" => "Usa o visual padrão do Windows.",
            "clear" => "Barra transparente que revela o papel de parede.",
            "blur" => "Aplica desfoque ao fundo da barra.",
            "acrylic" => "Fundo translúcido com desfoque e textura Acrylic para a taskbar.",
            "liquidglass" => "Selecione o tom escuro ou claro do Liquid Glass.",
            "color" => "Aplica uma cor sólida com opacidade ajustável.",
            _ => "Fundo translúcido com desfoque e textura Acrylic para a taskbar."
        });
        HomeColorPreview.Background = new SolidColorBrush(Color.FromArgb(
            255, (byte)(_rgb >> 16), (byte)(_rgb >> 8), (byte)_rgb));
        WindowsVersionText.Text = Environment.OSVersion.Version.Build >= 22000
            ? $"Windows 11 · {Environment.OSVersion.Version.Build}"
            : $"Windows 10 · {Environment.OSVersion.Version.Build}";
    }

    private void ShowPage(FrameworkElement page)
    {
        HomePage.Visibility = page == HomePage ? Visibility.Visible : Visibility.Collapsed;
        AppearancePage.Visibility = page == AppearancePage ? Visibility.Visible : Visibility.Collapsed;
        SettingsPage.Visibility = page == SettingsPage ? Visibility.Visible : Visibility.Collapsed;
        var selectedBackground = new SolidColorBrush(Color.FromArgb(
            0x55, _accentColor.R, _accentColor.G, _accentColor.B));
        var transparentBackground = new SolidColorBrush(Color.FromArgb(0, 0, 0, 0));
        HomeNavButton.Background = page == HomePage ? selectedBackground : transparentBackground;
        AppearanceNavButton.Background = page == AppearancePage ? selectedBackground : transparentBackground;
        SettingsNavButton.Background = page == SettingsPage ? selectedBackground : transparentBackground;
    }

    private void ApplySystemAccent()
    {
        _accentColor = _uiSettings.GetColorValue(UIColorType.Accent);
        ((SolidColorBrush)RootGrid.Resources["SystemAccentBrush"]).Color = _accentColor;
        _appWindow.TitleBar.ButtonHoverBackgroundColor = Color.FromArgb(
            0xCC, _accentColor.R, _accentColor.G, _accentColor.B);
        _appWindow.TitleBar.ButtonPressedBackgroundColor = Color.FromArgb(
            0xFF, _accentColor.R, _accentColor.G, _accentColor.B);
        if (HomePage is not null && HomePage.Visibility == Visibility.Visible)
            ShowPage(HomePage);
        else if (AppearancePage is not null && AppearancePage.Visibility == Visibility.Visible)
            ShowPage(AppearancePage);
        else if (SettingsPage is not null && SettingsPage.Visibility == Visibility.Visible)
            ShowPage(SettingsPage);
    }

    private void HomeNavButton_Click(object sender, RoutedEventArgs e) => ShowPage(HomePage);

    private void AppearanceNavButton_Click(object sender, RoutedEventArgs e) => ShowPage(AppearancePage);

    private void SettingsNavButton_Click(object sender, RoutedEventArgs e) => ShowPage(SettingsPage);

    private void QuickMode_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string mode }) return;
        if (mode is "lightliquidglass" or "darkliquidglass")
        {
            LiquidGlassToneSelector.SelectedIndex = mode == "lightliquidglass" ? 1 : 0;
            mode = "liquidglass";
        }
        for (int index = 0; index < ModeSelector.Items.Count; index++)
        {
            if (ModeSelector.Items[index] is ComboBoxItem item &&
                string.Equals(item.Tag?.ToString(), mode, StringComparison.Ordinal))
            {
                ModeSelector.SelectedIndex = index;
                break;
            }
        }
    }

    private void ToggleEngine_Click(object sender, RoutedEventArgs e) => ToggleButton_Click(sender, e);

    private void HexColor_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (_loading) return;
        string value = HexColor.Text.Trim().TrimStart('#');
        if (value.Length == 6 && int.TryParse(value, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int color))
        {
            _rgb = color;
            SetPreview(color);
            ScheduleSave();
        }
    }

    private void OpacitySlider_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (OpacityLabel is not null)
            OpacityLabel.Text = $"{T("Opacidade da cor:")} {(int)e.NewValue}%";
        UpdateTonalPreview();
        ScheduleSave();
    }

    private void UpdateTonalPreview()
    {
        if (TonalPreview is null || OpacitySlider is null) return;
        byte opacity = (byte)Math.Clamp((int)Math.Round(OpacitySlider.Value * 2.55), 0, 255);
        TonalPreview.Background = new SolidColorBrush(Color.FromArgb(
            opacity, (byte)(_rgb >> 16), (byte)(_rgb >> 8), (byte)_rgb));
    }

    private void CenterCheck_Changed(object sender, RoutedEventArgs e) => ScheduleSave();

    private void StartupCheck_Changed(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        try
        {
            using RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKey, true);
            if (StartupCheck.IsChecked == true)
                key.SetValue("TricBar", $"\"{EnginePath}\"");
            else
                key.DeleteValue("TricBar", false);
        }
        catch (Exception ex)
        {
            ShowError(T("Não foi possível alterar a inicialização com o Windows."), ex);
        }
    }

    private void Preset_Click(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: string value } &&
            int.TryParse(value, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int color))
            SetColor(color);
    }

    private async void PickColor_Click(object sender, RoutedEventArgs e)
    {
        var picker = new ColorPicker { Color = Color.FromArgb(255, (byte)(_rgb >> 16), (byte)(_rgb >> 8), (byte)_rgb) };
        var dialog = new ContentDialog
        {
            Title = T("Escolher cor"),
            Content = picker,
            PrimaryButtonText = T("Selecionar"),
            CloseButtonText = T("Cancelar"),
            XamlRoot = Content.XamlRoot
        };
        if (await dialog.ShowAsync() == ContentDialogResult.Primary)
            SetColor((picker.Color.R << 16) | (picker.Color.G << 8) | picker.Color.B);
    }

    private async void ShowMessage(string message)
    {
        await new ContentDialog
        {
            Title = "TricBar",
            Content = message,
            CloseButtonText = T("OK"),
            XamlRoot = Content.XamlRoot
        }.ShowAsync();
    }

    private async void ShowError(string message, Exception exception)
    {
        await new ContentDialog
        {
            Title = "TricBar",
            Content = $"{message}\n{exception.Message}",
            CloseButtonText = T("OK"),
            XamlRoot = Content.XamlRoot
        }.ShowAsync();
    }
}
