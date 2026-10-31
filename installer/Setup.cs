using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace ObsAirPlaySender.Setup
{
    internal static class Program
    {
        private const string PluginFolderName = "obs-airplay-cast";
        private const string ProductName = "OBS AirPlay Sender";
        private const string DisplayName = "OBS AirPlay Sender (obs-airplay-cast)";
        private const string Publisher = "gudqs7";
        private const string PayloadResourceName = "OBSAirPlay.Payload.zip";
        private const string UninstallerResourceName = "OBSAirPlay.Uninstaller.exe";
        private const string LogoResourceName = "OBSAirPlay.Logo.64.png";
        private const string UninstallRegistryPath =
            @"Software\Microsoft\Windows\CurrentVersion\Uninstall\obs-airplay-cast";
        private const string ElevatedInstanceArgument = "--elevated-instance";
        private static bool activateAfterElevation;

        [STAThread]
        private static int Main(string[] args)
        {
            try
            {
                activateAfterElevation = HasArgument(args, ElevatedInstanceArgument);
                if (RequiresElevation())
                {
                    return RelaunchElevated(args);
                }

#if UNINSTALLER
                return Uninstaller.Run(args);
#else
                return RunSetup(args);
#endif
            }
            catch (Win32Exception ex)
            {
                if (ex.NativeErrorCode == 1223)
                {
                    ShowFatal("安装或卸载需要管理员权限。", IsSilent(args));
                }
                else
                {
                    ShowFatal(ex.Message, IsSilent(args));
                }
                return 1;
            }
            catch (Exception ex)
            {
                ShowFatal(ex.Message, IsSilent(args));
                return 1;
            }
        }

#if !UNINSTALLER
        private static int RunSetup(string[] args)
        {
            if (!Environment.Is64BitOperatingSystem)
            {
                throw new InvalidOperationException("此安装包仅支持 64 位 Windows。");
            }

            if (HasArgument(args, "--uninstall"))
            {
                return Uninstaller.Run(args);
            }

            string version = GetProductVersion();
            if (IsSilent(args))
            {
                InstallerEngine.Install(version, HasArgument(args, "--launch"), true);
                return 0;
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new SetupForm(version, activateAfterElevation));
            return 0;
        }
#endif

        private static bool RequiresElevation()
        {
            if (activateAfterElevation)
            {
                return false;
            }

            if (String.Equals(
                    Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_TEST_RELAUNCH"),
                    "1",
                    StringComparison.Ordinal))
            {
                return true;
            }

            if (!String.IsNullOrWhiteSpace(
                    Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_INSTALL_DIR")) ||
                String.Equals(
                    Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_SKIP_ELEVATION"),
                    "1",
                    StringComparison.Ordinal))
            {
                return false;
            }

            using (WindowsIdentity identity = WindowsIdentity.GetCurrent())
            {
                WindowsPrincipal principal = new WindowsPrincipal(identity);
                return !principal.IsInRole(WindowsBuiltInRole.Administrator);
            }
        }

        private static int RelaunchElevated(string[] args)
        {
            ProcessStartInfo startInfo = new ProcessStartInfo(
                Assembly.GetExecutingAssembly().Location);
            string[] childArgs = new string[args.Length + 1];
            Array.Copy(args, childArgs, args.Length);
            childArgs[args.Length] = ElevatedInstanceArgument;
            startInfo.Arguments = JoinArguments(childArgs);
            startInfo.WorkingDirectory = Path.GetDirectoryName(
                Assembly.GetExecutingAssembly().Location);
            startInfo.UseShellExecute = true;
            if (!String.Equals(
                    Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_TEST_RELAUNCH"),
                    "1",
                    StringComparison.Ordinal))
            {
                startInfo.Verb = "runas";
            }

            using (Process process = Process.Start(startInfo))
            {
                // The unelevated parent owns the foreground activation right
                // while the UAC consent UI is shown. Transfer that right once
                // the elevated child has started so only its setup window can
                // come forward after the prompt closes.
                NativeMethods.AllowSetForegroundWindow(process.Id);
                process.WaitForExit();
                return process.ExitCode;
            }
        }

        private static string JoinArguments(string[] args)
        {
            string result = String.Empty;
            foreach (string arg in args)
            {
                if (result.Length > 0)
                {
                    result += " ";
                }
                result += "\"" + arg.Replace("\"", "\\\"") + "\"";
            }
            return result;
        }

        internal static string GetProductVersion()
        {
            Assembly assembly = Assembly.GetExecutingAssembly();
            AssemblyInformationalVersionAttribute informational =
                (AssemblyInformationalVersionAttribute)Attribute.GetCustomAttribute(
                    assembly,
                    typeof(AssemblyInformationalVersionAttribute));

            if (informational != null && !String.IsNullOrWhiteSpace(informational.InformationalVersion))
            {
                return informational.InformationalVersion.Trim();
            }

            Version version = assembly.GetName().Version;
            return version == null ? "0.0.0" : version.ToString(3);
        }

        internal static string PluginsRoot
        {
            get
            {
                if (!String.IsNullOrWhiteSpace(
                    Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_INSTALL_DIR")))
                {
                    return Path.GetDirectoryName(InstallDirectory);
                }

                return Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                    "obs-studio",
                    "plugins");
            }
        }

        internal static string InstallDirectory
        {
            get
            {
                string testDirectory =
                    Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_INSTALL_DIR");
                if (!String.IsNullOrWhiteSpace(testDirectory))
                {
                    return Path.GetFullPath(testDirectory);
                }

                return Path.Combine(
                    Path.Combine(
                        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                        "obs-studio",
                        "plugins"),
                    PluginFolderName);
            }
        }

        internal static string UninstallRegistryKey
        {
            get
            {
                string testKey =
                    Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_UNINSTALL_KEY");
                return String.IsNullOrWhiteSpace(testKey) ? UninstallRegistryPath : testKey;
            }
        }

        internal static RegistryKey InstallRegistryRoot
        {
            get
            {
                string testHive =
                    Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_REGISTRY_HIVE");
                return String.Equals(testHive, "HKCU", StringComparison.OrdinalIgnoreCase)
                    ? Registry.CurrentUser
                    : Registry.LocalMachine;
            }
        }

        internal static string StateRoot
        {
            get
            {
                if (!String.IsNullOrWhiteSpace(
                    Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_INSTALL_DIR")))
                {
                    return Path.Combine(
                        Path.GetDirectoryName(InstallDirectory),
                        ".obs-airplay-sender");
                }

                return Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                    "obs-airplay-sender");
            }
        }

        internal static string PayloadResource
        {
            get { return PayloadResourceName; }
        }

        internal static string UninstallerResource
        {
            get { return UninstallerResourceName; }
        }

        internal static string LogoResource
        {
            get { return LogoResourceName; }
        }

        internal static string FriendlyName
        {
            get { return DisplayName; }
        }

        internal static string ProductDisplayName
        {
            get { return ProductName; }
        }

        internal static string ProductPublisher
        {
            get { return Publisher; }
        }

        internal static bool IsSilent(string[] args)
        {
            return HasArgument(args, "--silent") || HasArgument(args, "/S");
        }

        internal static bool HasArgument(string[] args, string expected)
        {
            foreach (string arg in args)
            {
                if (String.Equals(arg, expected, StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }
            }
            return false;
        }

        internal static void ShowFatal(string message, bool silent)
        {
            if (!silent)
            {
                MessageBox.Show(
                    message,
                    ProductName + " 安装程序",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Error);
            }
            else
            {
                Console.Error.WriteLine(message);
            }
        }
    }

    internal static class NativeMethods
    {
        internal const int SwRestore = 9;
        private const uint SetWindowPosNoSize = 0x0001;
        private const uint SetWindowPosNoMove = 0x0002;
        private const uint SetWindowPosShowWindow = 0x0040;

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool AllowSetForegroundWindow(int processId);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetForegroundWindow(IntPtr windowHandle);

        [DllImport("user32.dll")]
        internal static extern IntPtr GetForegroundWindow();

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ShowWindowAsync(IntPtr windowHandle, int command);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool BringWindowToTop(IntPtr windowHandle);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetWindowPos(
            IntPtr windowHandle,
            IntPtr insertAfter,
            int x,
            int y,
            int width,
            int height,
            uint flags);

        internal static bool ForceInstallerWindowToFront(IntPtr windowHandle)
        {
            if (GetForegroundWindow() == windowHandle)
            {
                return true;
            }

            ShowWindowAsync(windowHandle, SwRestore);
            BringWindowToTop(windowHandle);
            SetWindowPos(
                windowHandle,
                IntPtr.Zero,
                0,
                0,
                0,
                0,
                SetWindowPosNoSize | SetWindowPosNoMove | SetWindowPosShowWindow);
            return SetForegroundWindow(windowHandle) ||
                GetForegroundWindow() == windowHandle;
        }
    }

    internal sealed class InstallProgress
    {
        public InstallProgress(int percent, string message)
        {
            Percent = percent;
            Message = message;
        }

        public int Percent { get; private set; }

        public string Message { get; private set; }
    }

    internal sealed class InstallResult
    {
        public InstallResult(bool obsLaunched)
        {
            ObsLaunched = obsLaunched;
        }

        public bool ObsLaunched { get; private set; }
    }

    internal static class InstallerEngine
    {
        private static readonly Stopwatch ProgressClock = Stopwatch.StartNew();

        public static InstallResult Install(
            string version,
            bool launchObs,
            bool silent,
            IProgress<InstallProgress> progress = null,
            bool obsAlreadyClosed = false)
        {
            ReportProgress(progress, obsAlreadyClosed ? 8 : 0, "正在准备安装...");
            if (!obsAlreadyClosed)
            {
                EnsureObsIsClosed(silent, progress);
            }

            string workRoot = Path.Combine(
                Program.StateRoot,
                "install-" + Guid.NewGuid().ToString("N"));
            string stagingPlugin = Path.Combine(workRoot, "payload");
            string backupRoot = Path.Combine(Program.StateRoot, "previous-install");
            bool movedExisting = false;

            try
            {
                ReportProgress(progress, 10, "正在创建临时安装目录...");
                Directory.CreateDirectory(stagingPlugin);
                ExtractPayload(stagingPlugin, progress);
                ReportProgress(progress, 52, "正在校验插件文件...");
                ValidatePayload(stagingPlugin);
                ReportProgress(progress, 58, "正在准备卸载程序...");
                CopyUninstaller(stagingPlugin);
                ReportProgress(progress, 64, "正在准备 OBS 插件目录...");
                Directory.CreateDirectory(Program.PluginsRoot);

                if (Directory.Exists(Program.InstallDirectory))
                {
                    ReportProgress(progress, 70, "正在备份上一版插件...");
                    if (Directory.Exists(backupRoot))
                    {
                        Directory.Delete(backupRoot, true);
                    }
                    Directory.CreateDirectory(Path.GetDirectoryName(backupRoot));
                    Directory.Move(Program.InstallDirectory, backupRoot);
                    movedExisting = true;
                }

                try
                {
                    ReportProgress(progress, 80, "正在安装插件文件...");
                    Directory.Move(stagingPlugin, Program.InstallDirectory);
                    ReportProgress(progress, 90, "正在注册卸载信息...");
                    RegisterUninstaller(version);
                }
                catch
                {
                    InstallerEngine.TryDeleteDirectory(Program.InstallDirectory);
                    if (movedExisting && Directory.Exists(backupRoot))
                    {
                        Directory.Move(backupRoot, Program.InstallDirectory);
                    }
                    throw;
                }
            }
            finally
            {
                ReportProgress(progress, 96, "正在清理临时文件...");
                TryDeleteDirectory(workRoot);
            }

            if (launchObs)
            {
                ReportProgress(progress, 98, "正在启动 OBS Studio...");
                bool launched = LaunchObs();
                ReportProgress(progress, 100, "安装完成");
                return new InstallResult(launched);
            }

            ReportProgress(progress, 100, "安装完成");
            return new InstallResult(false);
        }

        public static bool IsObsRunning()
        {
            if (String.Equals(
                Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_SKIP_PROCESS_CHECK"),
                "1",
                StringComparison.Ordinal))
            {
                return false;
            }
            return Process.GetProcessesByName("obs64").Length > 0;
        }

        public static void EnsureObsIsClosed(
            bool silent,
            IProgress<InstallProgress> progress = null)
        {
            if (String.Equals(
                Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_SKIP_PROCESS_CHECK"),
                "1",
                StringComparison.Ordinal))
            {
                return;
            }

            if (!IsObsRunning())
            {
                return;
            }

            if (silent)
            {
                throw new InvalidOperationException("OBS Studio 正在运行，静默安装已取消。");
            }

            DialogResult answer = MessageBox.Show(
                "检测到 OBS Studio 正在运行。\r\n\r\n" +
                "安装插件前需要关闭 OBS。是否现在自动关闭并继续？",
                Program.ProductDisplayName + " 安装程序",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning,
                MessageBoxDefaultButton.Button1);

            if (answer != DialogResult.Yes)
            {
                throw new InvalidOperationException(
                    "安装已取消。请先关闭 OBS Studio，然后再运行安装程序。");
            }

            CloseObsAndWait(progress);
        }

        public static void CloseObsAndWait(IProgress<InstallProgress> progress = null)
        {
            Process[] processes = Process.GetProcessesByName("obs64");
            foreach (Process process in processes)
            {
                try
                {
                    if (!process.HasExited)
                    {
                        process.CloseMainWindow();
                    }
                }
                catch
                {
                    // The wait below reports whether OBS really exited.
                }
            }

            ReportProgress(progress, 5, "正在等待 OBS Studio 关闭...");
            DateTime deadline = DateTime.UtcNow.AddSeconds(25);
            while (DateTime.UtcNow < deadline)
            {
                if (Process.GetProcessesByName("obs64").Length == 0)
                {
                    return;
                }
                ReportProgress(progress, 5, "正在等待 OBS Studio 关闭...");
                System.Threading.Thread.Sleep(250);
            }

            throw new InvalidOperationException(
                "OBS Studio 未能在 25 秒内关闭。请手动退出 OBS 后重试。");
        }

        private static void ReportProgress(
            IProgress<InstallProgress> progress,
            int percent,
            string message)
        {
            InstallProgress value = new InstallProgress(
                Math.Max(0, Math.Min(100, percent)),
                message);
            if (progress != null)
            {
                progress.Report(value);
            }

            int delayMs;
            if (Int32.TryParse(
                    Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_PROGRESS_DELAY_MS"),
                    out delayMs) &&
                delayMs > 0 &&
                delayMs <= 1000)
            {
                Thread.Sleep(delayMs);
            }

            string progressFile =
                Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_PROGRESS_FILE");
            if (!String.IsNullOrWhiteSpace(progressFile))
            {
                try
                {
                    File.AppendAllText(
                        progressFile,
                        value.Percent + "\t" + value.Message + "\t" +
                        ProgressClock.ElapsedMilliseconds + Environment.NewLine);
                }
                catch
                {
                    // Progress logging is a test/diagnostic aid only.
                }
            }
        }

        private static void ExtractPayload(
            string destination,
            IProgress<InstallProgress> progress)
        {
            Assembly assembly = Assembly.GetExecutingAssembly();
            using (Stream payload = assembly.GetManifestResourceStream(Program.PayloadResource))
            {
                if (payload == null)
                {
                    throw new InvalidOperationException("安装包损坏：缺少插件文件。");
                }

                using (ZipArchive archive = new ZipArchive(payload, ZipArchiveMode.Read))
                {
                    string destinationRoot = Path.GetFullPath(destination) + Path.DirectorySeparatorChar;
                    int total = Math.Max(archive.Entries.Count, 1);
                    int current = 0;
                    foreach (ZipArchiveEntry entry in archive.Entries)
                    {
                        current++;
                        int percent = 12 + (int)Math.Round(36.0 * current / total);
                        ReportProgress(
                            progress,
                            percent,
                            "正在解压插件文件 " + current + "/" + total + "...");
                        string relative = entry.FullName
                            .Replace('/', Path.DirectorySeparatorChar)
                            .Replace('\\', Path.DirectorySeparatorChar);
                        string target = Path.GetFullPath(Path.Combine(destination, relative));
                        if (!target.StartsWith(destinationRoot, StringComparison.OrdinalIgnoreCase))
                        {
                            throw new InvalidDataException("安装包包含无效路径。");
                        }

                        if (String.IsNullOrEmpty(entry.Name))
                        {
                            Directory.CreateDirectory(target);
                            continue;
                        }

                        Directory.CreateDirectory(Path.GetDirectoryName(target));
                        entry.ExtractToFile(target, true);
                    }
                }
            }
        }

        private static void ValidatePayload(string root)
        {
            RequireFile(root, @"bin\64bit\obs-airplay-cast.dll");
            RequireFile(root, @"bin\64bit\obs-airplay-cast-bridge.exe");
            RequireFile(root, @"bin\64bit\obs-airplay-aac-eld.exe");
            RequireFile(root, @"data\locale\en-US.ini");
            RequireFile(root, @"data\locale\zh-CN.ini");
            RequireFile(root, @"locale\en-US.ini");
            RequireFile(root, @"bin\64bit\locale\en-US.ini");
            RequireFile(root, @"data\obs-plugins\obs-airplay-cast\locale\en-US.ini");
            RequireFile(root, "LICENSE");
            RequireFile(root, "README.md");
            RequireFile(root, "THIRD_PARTY_NOTICES.md");
        }

        private static void RequireFile(string root, string relativePath)
        {
            string path = Path.Combine(root, relativePath);
            FileInfo file = new FileInfo(path);
            if (!file.Exists || file.Length == 0)
            {
                throw new InvalidDataException("安装包不完整，缺少文件：" + relativePath);
            }
        }

        private static void CopyUninstaller(string stagingPlugin)
        {
            Assembly assembly = Assembly.GetExecutingAssembly();
            using (Stream resource = assembly.GetManifestResourceStream(Program.UninstallerResource))
            {
                if (resource == null)
                {
                    throw new InvalidOperationException("安装包损坏：缺少卸载程序。");
                }

                string target = Path.Combine(stagingPlugin, "Uninstall.exe");
                using (FileStream file = File.Create(target))
                {
                    resource.CopyTo(file);
                }
            }
        }

        private static void RegisterUninstaller(string version)
        {
            string uninstaller = Path.Combine(Program.InstallDirectory, "Uninstall.exe");
            long estimatedSizeKb = GetDirectorySize(Program.InstallDirectory) / 1024;

            using (RegistryKey key = Program.InstallRegistryRoot.CreateSubKey(
                Program.UninstallRegistryKey))
            {
                if (key == null)
                {
                    throw new InvalidOperationException("无法写入卸载信息。");
                }

                key.SetValue("DisplayName", Program.FriendlyName);
                key.SetValue("DisplayVersion", version);
                key.SetValue("Publisher", Program.ProductPublisher);
                key.SetValue("InstallLocation", Program.InstallDirectory);
                key.SetValue("DisplayIcon", uninstaller);
                key.SetValue("UninstallString", Quote(uninstaller) + " --uninstall");
                key.SetValue("QuietUninstallString", Quote(uninstaller) + " --uninstall --silent");
                key.SetValue("EstimatedSize", (int)Math.Min(estimatedSizeKb, Int32.MaxValue), RegistryValueKind.DWord);
                key.SetValue("NoModify", 1, RegistryValueKind.DWord);
                key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            }
        }

        internal static bool LaunchObs()
        {
            string obs = FindObsExecutable();
            if (obs == null)
            {
                return false;
            }

            int delayMs;
            if (Int32.TryParse(
                    Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_LAUNCH_DELAY_MS"),
                    out delayMs) &&
                delayMs > 0 &&
                delayMs <= 5000)
            {
                Thread.Sleep(delayMs);
            }

            string workingDirectory = Path.GetDirectoryName(obs);
            try
            {
                if (TryLaunchObsViaShell(obs, workingDirectory))
                {
                    return true;
                }
            }
            catch
            {
                // Fall back to a direct shell launch below.
            }

            try
            {
                ProcessStartInfo startInfo = new ProcessStartInfo(obs);
                startInfo.WorkingDirectory = workingDirectory;
                startInfo.UseShellExecute = true;
                Process.Start(startInfo);
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static bool TryLaunchObsViaShell(string obs, string workingDirectory)
        {
            Type shellType = Type.GetTypeFromProgID("Shell.Application");
            if (shellType == null)
            {
                return false;
            }

            object shell = Activator.CreateInstance(shellType);
            try
            {
                shellType.InvokeMember(
                    "ShellExecute",
                    BindingFlags.InvokeMethod,
                    null,
                    shell,
                    new object[] { obs, String.Empty, workingDirectory, "open", 1 });
                return true;
            }
            finally
            {
                if (shell != null && Marshal.IsComObject(shell))
                {
                    Marshal.FinalReleaseComObject(shell);
                }
            }
        }

        private static string FindObsExecutable()
        {
            string testPath =
                Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_OBS_PATH");
            if (!String.IsNullOrWhiteSpace(testPath) && File.Exists(testPath))
            {
                return Path.GetFullPath(testPath);
            }

            string[] candidates = new string[]
            {
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                    @"obs-studio\bin\64bit\obs64.exe"),
                Path.Combine(Environment.GetEnvironmentVariable("ProgramW6432") ?? String.Empty,
                    @"obs-studio\bin\64bit\obs64.exe")
            };

            foreach (string candidate in candidates)
            {
                if (!String.IsNullOrEmpty(candidate) && File.Exists(candidate))
                {
                    return candidate;
                }
            }
            return null;
        }

        private static long GetDirectorySize(string root)
        {
            long total = 0;
            foreach (string file in Directory.GetFiles(root, "*", SearchOption.AllDirectories))
            {
                try
                {
                    total += new FileInfo(file).Length;
                }
                catch
                {
                    // Estimated size is informational only.
                }
            }
            return total;
        }

        private static string Quote(string value)
        {
            return "\"" + value + "\"";
        }

        internal static void TryDeleteDirectory(string path)
        {
            try
            {
                if (Directory.Exists(path))
                {
                    Directory.Delete(path, true);
                }
            }
            catch
            {
                // Temporary directories are best-effort cleanup.
            }
        }
    }

    internal static class Uninstaller
    {
        public static int Run(string[] args)
        {
            bool silent = Program.IsSilent(args);

            if (!Directory.Exists(Program.InstallDirectory))
            {
                if (!silent)
                {
                    MessageBox.Show(
                        "OBS AirPlay Sender 尚未安装。",
                        Program.ProductDisplayName + " 卸载程序",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Information);
                }
                return 0;
            }

            try
            {
                InstallerEngine.EnsureObsIsClosed(silent);
                Program.InstallRegistryRoot.DeleteSubKeyTree(
                    Program.UninstallRegistryKey,
                    false);

                foreach (string file in Directory.GetFiles(Program.InstallDirectory))
                {
                    if (!String.Equals(
                        Path.GetFileName(file),
                        "Uninstall.exe",
                        StringComparison.OrdinalIgnoreCase))
                    {
                        File.Delete(file);
                    }
                }

                foreach (string directory in Directory.GetDirectories(Program.InstallDirectory))
                {
                    Directory.Delete(directory, true);
                }

                ScheduleDirectoryRemoval(Program.InstallDirectory, Program.StateRoot);

                if (!silent)
                {
                    MessageBox.Show(
                        "OBS AirPlay Sender 已卸载。",
                        Program.ProductDisplayName + " 卸载程序",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Information);
                }
                return 0;
            }
            catch (Exception ex)
            {
                Program.ShowFatal(ex.Message, silent);
                return 1;
            }
        }

        private static void ScheduleDirectoryRemoval(params string[] directories)
        {
            string command = "/d /c timeout /t 2 /nobreak >nul";
            foreach (string directory in directories)
            {
                command += " & rmdir /s /q \"" + directory + "\"";
            }

            ProcessStartInfo startInfo = new ProcessStartInfo();
            startInfo.FileName = "cmd.exe";
            startInfo.Arguments = command;
            startInfo.CreateNoWindow = true;
            startInfo.WindowStyle = ProcessWindowStyle.Hidden;
            startInfo.UseShellExecute = false;
            Process.Start(startInfo);
        }
    }

    internal sealed class SetupForm : Form
    {
        private readonly string version;
        private readonly bool activateAfterElevation;
        private readonly CheckBox launchCheckBox;
        private readonly Button installButton;
        private readonly Button cancelButton;
        private readonly TableLayoutPanel progressPanel;
        private readonly ProgressBar progressBar;
        private readonly Label progressStatus;
        private readonly System.Windows.Forms.Timer elevationFocusTimer;
        private int elevationFocusAttempts;
        private bool installing;

        public SetupForm(string version, bool activateAfterElevation)
        {
            this.version = version;
            this.activateAfterElevation = activateAfterElevation;
            Text = "OBS AirPlay Sender 安装程序";
            ClientSize = new Size(680, 450);
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            Font = new Font("Microsoft YaHei UI", 9F);
            BackColor = Color.White;
            Padding = new Padding(30, 24, 30, 22);
            AutoScaleDimensions = new SizeF(96F, 96F);
            AutoScaleMode = AutoScaleMode.Dpi;

            TableLayoutPanel layout = new TableLayoutPanel();
            layout.Dock = DockStyle.Fill;
            layout.AutoSize = true;
            layout.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            layout.MinimumSize = new Size(620, 0);
            layout.ColumnCount = 2;
            layout.RowCount = 9;
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 88F));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 62F));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 16F));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 72F));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            Controls.Add(layout);

            PictureBox logo = new PictureBox();
            logo.Size = new Size(64, 64);
            logo.SizeMode = PictureBoxSizeMode.Zoom;
            logo.BackColor = Color.Transparent;
            logo.Margin = new Padding(0, 2, 24, 0);
            logo.TabStop = false;
            Image logoImage = LoadEmbeddedLogo();
            if (logoImage != null)
            {
                logo.Image = logoImage;
                logo.Disposed += delegate { logoImage.Dispose(); };
            }
            layout.Controls.Add(logo, 0, 0);
            layout.SetRowSpan(logo, 2);

            Label title = new Label();
            title.Text = "OBS AirPlay Sender";
            title.Font = new Font("Microsoft YaHei UI", 18F, FontStyle.Bold);
            title.ForeColor = Color.FromArgb(24, 32, 40);
            title.AutoSize = true;
            title.Margin = new Padding(0, 0, 0, 2);
            layout.Controls.Add(title, 1, 0);

            Label subtitle = new Label();
            subtitle.Text = "安装 AirPlay 投屏插件到 OBS Studio";
            subtitle.Font = new Font("Microsoft YaHei UI", 10F);
            subtitle.ForeColor = Color.FromArgb(75, 86, 96);
            subtitle.AutoSize = true;
            subtitle.Margin = new Padding(2, 4, 0, 12);
            layout.Controls.Add(subtitle, 1, 1);

            Label details = new Label();
            details.Text =
                "版本 " + version + "  |  Windows 10/11 x64  |  OBS Studio 31.x+\r\n" +
                "安装到系统级 OBS 插件目录，启动时会请求管理员权限。";
            details.ForeColor = Color.FromArgb(75, 86, 96);
            details.AutoSize = false;
            details.Dock = DockStyle.Fill;
            details.Margin = new Padding(2, 0, 0, 0);
            details.TextAlign = ContentAlignment.TopLeft;
            layout.Controls.Add(details, 0, 2);
            layout.SetColumnSpan(details, 2);

            Label pathLabel = new Label();
            pathLabel.Text = "安装位置";
            pathLabel.ForeColor = Color.FromArgb(52, 61, 70);
            pathLabel.AutoSize = true;
            pathLabel.Margin = new Padding(2, 0, 0, 0);
            layout.Controls.Add(pathLabel, 0, 4);
            layout.SetColumnSpan(pathLabel, 2);

            TextBox pathBox = new TextBox();
            pathBox.Text = Program.InstallDirectory;
            pathBox.ReadOnly = true;
            pathBox.BackColor = Color.FromArgb(246, 248, 250);
            pathBox.ForeColor = Color.FromArgb(65, 75, 85);
            pathBox.BorderStyle = BorderStyle.FixedSingle;
            pathBox.Dock = DockStyle.Fill;
            pathBox.Margin = new Padding(0, 10, 0, 18);
            layout.Controls.Add(pathBox, 0, 5);
            layout.SetColumnSpan(pathBox, 2);

            launchCheckBox = new CheckBox();
            launchCheckBox.Text = "安装完成后启动 OBS Studio";
            launchCheckBox.Checked = true;
            launchCheckBox.AutoSize = true;
            launchCheckBox.Margin = new Padding(2, 0, 0, 0);
            layout.Controls.Add(launchCheckBox, 0, 6);
            layout.SetColumnSpan(launchCheckBox, 2);

            progressPanel = new TableLayoutPanel();
            progressPanel.ColumnCount = 1;
            progressPanel.RowCount = 2;
            progressPanel.AutoSize = true;
            progressPanel.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            progressPanel.Dock = DockStyle.Fill;
            progressPanel.Margin = new Padding(0, 8, 0, 0);
            progressPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            progressPanel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            progressPanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 18F));
            progressPanel.Visible = false;
            layout.Controls.Add(progressPanel, 0, 7);
            layout.SetColumnSpan(progressPanel, 2);

            progressStatus = new Label();
            progressStatus.Text = "准备就绪（0%）";
            progressStatus.ForeColor = Color.FromArgb(52, 61, 70);
            progressStatus.AutoSize = true;
            progressStatus.Margin = new Padding(2, 0, 0, 4);
            progressPanel.Controls.Add(progressStatus, 0, 0);

            progressBar = new ProgressBar();
            progressBar.Minimum = 0;
            progressBar.Maximum = 100;
            progressBar.Value = 0;
            progressBar.Style = ProgressBarStyle.Continuous;
            progressBar.Dock = DockStyle.Fill;
            progressBar.Margin = new Padding(0);
            progressPanel.Controls.Add(progressBar, 0, 1);

            FlowLayoutPanel actions = new FlowLayoutPanel();
            actions.Dock = DockStyle.Fill;
            actions.AutoSize = true;
            actions.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            actions.FlowDirection = FlowDirection.RightToLeft;
            actions.WrapContents = false;
            actions.Margin = new Padding(0);
            actions.Padding = new Padding(0, 12, 0, 0);
            layout.Controls.Add(actions, 0, 8);
            layout.SetColumnSpan(actions, 2);

            installButton = new Button();
            installButton.Text = "安装";
            installButton.Size = new Size(104, 36);
            installButton.Margin = new Padding(0);
            installButton.Click += InstallButtonClick;
            AcceptButton = installButton;

            cancelButton = new Button();
            cancelButton.Text = "取消";
            cancelButton.Size = new Size(104, 36);
            cancelButton.Margin = new Padding(8, 0, 0, 0);
            cancelButton.Click += CancelButtonClick;
            actions.Controls.Add(cancelButton);
            actions.Controls.Add(installButton);
            CancelButton = cancelButton;

            elevationFocusTimer = new System.Windows.Forms.Timer();
            elevationFocusTimer.Interval = 75;
            elevationFocusTimer.Tick += ActivateAfterElevationTick;
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            if (activateAfterElevation)
            {
                elevationFocusTimer.Start();
            }
        }

        private void ActivateAfterElevationTick(object sender, EventArgs e)
        {
            elevationFocusAttempts++;
            if (WindowState == FormWindowState.Minimized)
            {
                WindowState = FormWindowState.Normal;
            }

            Activate();
            BringToFront();
            if (NativeMethods.ForceInstallerWindowToFront(Handle) ||
                elevationFocusAttempts >= 20)
            {
                elevationFocusTimer.Stop();
            }
        }

        private static Image LoadEmbeddedLogo()
        {
            using (Stream stream = Assembly.GetExecutingAssembly()
                       .GetManifestResourceStream(Program.LogoResource))
            {
                if (stream == null)
                {
                    return null;
                }

                using (Image source = Image.FromStream(stream))
                {
                    return new Bitmap(source);
                }
            }
        }

        private void CancelButtonClick(object sender, EventArgs e)
        {
            DialogResult = DialogResult.Cancel;
            Close();
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (installing)
            {
                e.Cancel = true;
                return;
            }
            base.OnFormClosing(e);
        }

        private async void InstallButtonClick(object sender, EventArgs e)
        {
            installing = true;
            installButton.Enabled = false;
            launchCheckBox.Enabled = false;
            cancelButton.Enabled = false;
            progressPanel.Visible = true;
            progressBar.Value = 0;
            progressStatus.Text = "准备就绪（0%）";
            IProgress<InstallProgress> progress = new Progress<InstallProgress>(value =>
            {
                progressBar.Value = value.Percent;
                progressStatus.Text = value.Message + "（" + value.Percent + "%）";
            });

            try
            {
                if (InstallerEngine.IsObsRunning())
                {
                    DialogResult answer = MessageBox.Show(
                        this,
                        "检测到 OBS Studio 正在运行。\r\n\r\n" +
                        "安装插件前需要关闭 OBS。是否现在自动关闭并继续？",
                        Program.ProductDisplayName + " 安装程序",
                        MessageBoxButtons.YesNo,
                        MessageBoxIcon.Warning,
                        MessageBoxDefaultButton.Button1);
                    if (answer != DialogResult.Yes)
                    {
                        throw new InvalidOperationException(
                            "安装已取消。请先关闭 OBS Studio，然后再运行安装程序。");
                    }

                    progress.Report(new InstallProgress(3, "正在关闭 OBS Studio..."));
                    await Task.Run(() => InstallerEngine.CloseObsAndWait(progress));
                }

                InstallResult result = await Task.Run(() =>
                    InstallerEngine.Install(
                        version,
                        launchCheckBox.Checked,
                        true,
                        progress,
                        true));
                progressBar.Value = 100;
                progressStatus.Text = "安装完成（100%）";
                bool launched = !launchCheckBox.Checked || result.ObsLaunched;
                string resultText;
                if (launchCheckBox.Checked && !launched)
                {
                    resultText =
                        "插件已安装，但没有找到 OBS Studio。\r\n\r\n" +
                        "请从开始菜单启动 OBS。";
                }
                else if (launchCheckBox.Checked)
                {
                    resultText =
                        "插件已安装，OBS 正在启动。\r\n\r\n" +
                        "启动后在 OBS 菜单中打开：\r\n" +
                        "工具 → AirPlay 投屏";
                }
                else
                {
                    resultText =
                        "插件已安装。\r\n\r\n" +
                        "请在 OBS 菜单中打开：\r\n" +
                        "工具 → AirPlay 投屏";
                }
                MessageBox.Show(
                    this,
                    resultText,
                    Program.ProductDisplayName,
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Information);
                installing = false;
                DialogResult = DialogResult.OK;
                Close();
            }
            catch (Exception ex)
            {
                progressStatus.Text = "安装失败";
                MessageBox.Show(
                    this,
                    ex.Message,
                    Program.ProductDisplayName + " 安装失败",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Error);
                installButton.Enabled = true;
                launchCheckBox.Enabled = true;
                cancelButton.Enabled = true;
                installing = false;
            }
        }
    }
}
