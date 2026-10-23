using Microsoft.UI.Xaml;
using System.Diagnostics;
using System.IO;

namespace TricBarConfigurator;

public partial class App : Application
{
    private static readonly string ErrorLogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "TricBar",
        "launcher-error.log");

    private Window? _window;

    public App()
    {
        UnhandledException += LogUnhandledException;
        InitializeComponent();
    }

    private static void LogUnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs args)
    {
        LogError($"Unhandled XAML exception (0x{args.Exception.HResult:X8}): {args.Exception}");
    }

    internal static void LogError(string message)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(ErrorLogPath)!);
            File.AppendAllText(ErrorLogPath, $"{DateTimeOffset.Now:O} {message}{Environment.NewLine}");
        }
        catch (IOException ex)
        {
            Trace.TraceError("Unable to write the TricBar Launcher error log: {0}", ex);
        }
        catch (UnauthorizedAccessException ex)
        {
            Trace.TraceError("Unable to write the TricBar Launcher error log: {0}", ex);
        }
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _window = new MainWindow();
        _window.Activate();
    }
}
