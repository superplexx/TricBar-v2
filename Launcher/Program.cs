using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using WinRT;

namespace TricBarConfigurator;

internal static class LauncherEntryPoint
{
    private const string InstanceMutexName = @"Local\TricBarConfigurator_SingleInstance";
    private const int RestoreWindow = 9;

    [DllImport("user32.dll")]
    private static extern bool ShowWindowAsync(IntPtr window, int command);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr window);

    [STAThread]
    private static void Main(string[] args)
    {
        using var instanceMutex = new Mutex(true, InstanceMutexName, out bool isFirstInstance);
        if (!isFirstInstance)
        {
            ActivateExistingInstance();
            return;
        }

        ComWrappersSupport.InitializeComWrappers();
        Application.Start(p =>
        {
            var context = new DispatcherQueueSynchronizationContext(
                DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(context);
            new App();
        });
    }

    private static void ActivateExistingInstance()
    {
        for (int attempt = 0; attempt < 20; attempt++)
        {
            foreach (Process process in Process.GetProcessesByName("TricBarConfigurator"))
            {
                using (process)
                {
                    if (process.Id == Environment.ProcessId) continue;
                    IntPtr window = process.MainWindowHandle;
                    if (window == IntPtr.Zero) continue;

                    ShowWindowAsync(window, RestoreWindow);
                    SetForegroundWindow(window);
                    return;
                }
            }

            Thread.Sleep(100);
        }
    }
}
