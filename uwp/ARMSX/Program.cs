using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Activation;
using Windows.Storage;
using SDL2;

namespace ARMSX
{
    class Program
    {
        // external_main is the 2-arg main-shaped symbol; external_main_ex is the 4-arg form
        // for hosts that own the SDL_Window/SDL_Renderer.
        [DllImport("libarmsx.dll", CallingConvention = CallingConvention.Cdecl, EntryPoint = "external_main_ex")]
        private static extern int external_main(int argc, IntPtr argv, IntPtr external_window, IntPtr external_renderer);

        [DllImport("libarmsx.dll", CallingConvention = CallingConvention.Cdecl, EntryPoint = "psxe_enqueue_launch_argument")]
        private static extern void psxe_enqueue_launch_argument([MarshalAs(UnmanagedType.LPStr)] string argument);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern void OutputDebugStringW(string message);

        private static string[] launchArgs = Array.Empty<string>();
        private static readonly object logLock = new object();
        private static string nativeLogPath = string.Empty;

        static void Main(string[] args)
        {
            launchArgs = ResolveLaunchArgs(args ?? Array.Empty<string>());
            nativeLogPath = BuildNativeLogPath();
            InstallExceptionHandlers();
            InstallActivationHandlers();
            LogHost($"ARMSX UWP host starting. Native log path: {nativeLogPath}");

            try
            {
                SDL.SDL_SetHint("SDL_WINRT_HANDLE_BACK_BUTTON", "1");
                SDL.SDL_main_func mainFunction = SDLMain;
                SDL.SDL_WinRTRunApp(mainFunction, IntPtr.Zero);
            }
            catch (Exception ex)
            {
                LogManagedException("Main", ex);
                throw;
            }
        }

        private static int SDLMain(int argc, IntPtr argv)
        {
            string[] nativeArgs = BuildNativeArgs();
            IntPtr[] argvPtrs = new IntPtr[nativeArgs.Length];
            IntPtr nativeArgv = IntPtr.Zero;

            try
            {
                for (int i = 0; i < nativeArgs.Length; i++)
                {
                    argvPtrs[i] = Marshal.StringToHGlobalAnsi(nativeArgs[i]);
                }

                nativeArgv = Marshal.AllocHGlobal(IntPtr.Size * nativeArgs.Length);
                for (int i = 0; i < nativeArgs.Length; i++)
                {
                    Marshal.WriteIntPtr(nativeArgv, i * IntPtr.Size, argvPtrs[i]);
                }

                LogHost($"Invoking external_main argc={nativeArgs.Length}");
                try
                {
                    int ret = external_main(nativeArgs.Length, nativeArgv, IntPtr.Zero, IntPtr.Zero);
                    LogHost($"external_main returned: {ret}");
                    return ret;
                }
                catch (Exception ex)
                {
                    LogManagedException("SDLMain/external_main", ex);
                    return -1;
                }
            }
            finally
            {
                foreach (IntPtr ptr in argvPtrs)
                {
                    if (ptr != IntPtr.Zero)
                    {
                        Marshal.FreeHGlobal(ptr);
                    }
                }
                if (nativeArgv != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(nativeArgv);
                }
            }
        }

        private static string[] BuildNativeArgs()
        {
            string[] nativeArgs = new string[launchArgs.Length + 1];
            nativeArgs[0] = "armsx";
            Array.Copy(launchArgs, 0, nativeArgs, 1, launchArgs.Length);
            return nativeArgs;
        }

        private static string[] ResolveLaunchArgs(string[] args)
        {
            try
            {
                string protocolUri = ExtractProtocolUri(AppInstance.GetActivatedEventArgs());
                if (!string.IsNullOrWhiteSpace(protocolUri))
                {
                    LogHost($"Protocol activation URI: {protocolUri}");
                    return new[] { protocolUri };
                }
            }
            catch (Exception ex)
            {
                LogManagedException("ResolveLaunchArgs", ex);
            }

            return args ?? Array.Empty<string>();
        }

        private static void InstallActivationHandlers()
        {
            try
            {
                AppInstance.GetCurrent().Activated += (_, args) =>
                {
                    try
                    {
                        string protocolUri = ExtractProtocolUri(args);
                        if (string.IsNullOrWhiteSpace(protocolUri))
                        {
                            return;
                        }

                        LogHost($"Runtime protocol activation URI: {protocolUri}");
                        psxe_enqueue_launch_argument(protocolUri);
                    }
                    catch (Exception ex)
                    {
                        LogManagedException("AppInstance.Activated", ex);
                    }
                };
            }
            catch (Exception ex)
            {
                LogManagedException("InstallActivationHandlers", ex);
            }
        }

        private static string ExtractProtocolUri(object activatedArgs)
        {
            if (activatedArgs is ProtocolActivatedEventArgs protocolArgs && protocolArgs.Uri != null)
            {
                return protocolArgs.Uri.OriginalString;
            }

            if (activatedArgs is IActivatedEventArgs uwpArgs)
            {
                return ExtractProtocolUri(uwpArgs);
            }

            object data = activatedArgs?.GetType().GetProperty("Data")?.GetValue(activatedArgs);
            if (data is IActivatedEventArgs dataArgs)
            {
                return ExtractProtocolUri(dataArgs);
            }

            return string.Empty;
        }

        private static string ExtractProtocolUri(IActivatedEventArgs activatedArgs)
        {
            if (activatedArgs is ProtocolActivatedEventArgs protocolArgs && protocolArgs.Uri != null)
            {
                return protocolArgs.Uri.OriginalString;
            }

            return string.Empty;
        }

        private static void InstallExceptionHandlers()
        {
            AppDomain.CurrentDomain.UnhandledException += (_, eventArgs) =>
            {
                if (eventArgs.ExceptionObject is Exception exception)
                {
                    LogManagedException("AppDomain.CurrentDomain.UnhandledException", exception);
                }
                else
                {
                    LogHost($"Unhandled exception object: {eventArgs.ExceptionObject}");
                }
            };

            TaskScheduler.UnobservedTaskException += (_, eventArgs) =>
            {
                LogManagedException("TaskScheduler.UnobservedTaskException", eventArgs.Exception);
                eventArgs.SetObserved();
            };
        }

        private static string BuildNativeLogPath()
        {
            try
            {
                string localStatePath = ApplicationData.Current?.LocalFolder?.Path;
                if (string.IsNullOrEmpty(localStatePath))
                {
                    return string.Empty;
                }

                string prefPath = Path.Combine(localStatePath, "nanodata", "armsx");
                string logDirectory = Path.Combine(prefPath, "logs");
                Directory.CreateDirectory(logDirectory);
                return Path.Combine(logDirectory, "armsx-uwp.log");
            }
            catch (Exception ex)
            {
                OutputDebugStringW($"ARMSX UWP host log path init failed: {ex}{Environment.NewLine}");
                Debug.WriteLine($"ARMSX UWP host log path init failed: {ex}");
                return string.Empty;
            }
        }

        private static void LogManagedException(string source, Exception exception)
        {
            if (exception == null)
            {
                LogHost($"{source}: managed exception unavailable.");
                return;
            }

            LogHost($"{source}: {exception}");
        }

        private static void LogHost(string message)
        {
            string line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} [uwp-host] {message}";
            OutputDebugStringW(line + Environment.NewLine);

            if (string.IsNullOrEmpty(nativeLogPath))
            {
                Debug.WriteLine(line);
                return;
            }

            lock (logLock)
            {
                try
                {
                    File.AppendAllText(nativeLogPath, line + Environment.NewLine, Encoding.UTF8);
                }
                catch (Exception ex)
                {
                    OutputDebugStringW($"ARMSX UWP host file logging disabled: {ex}{Environment.NewLine}");
                    Debug.WriteLine($"ARMSX UWP host file logging disabled: {ex}");
                    nativeLogPath = string.Empty;
                }
            }

            Debug.WriteLine(line);
        }

    }
}
