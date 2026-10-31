using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Principal;
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
        private const string UninstallRegistryPath =
            @"Software\Microsoft\Windows\CurrentVersion\Uninstall\obs-airplay-cast";

        [STAThread]
        private static int Main(string[] args)
        {
            try
            {
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
            Application.Run(new SetupForm(version));
            return 0;
        }
#endif

        private static bool RequiresElevation()
        {
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
            startInfo.Arguments = JoinArguments(args);
            startInfo.WorkingDirectory = Path.GetDirectoryName(
                Assembly.GetExecutingAssembly().Location);
            startInfo.UseShellExecute = true;
            startInfo.Verb = "runas";
            using (Process process = Process.Start(startInfo))
            {
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

    internal static class InstallerEngine
    {
        public static void Install(string version, bool launchObs, bool silent)
        {
            EnsureObsIsClosed(silent);

            string workRoot = Path.Combine(
                Program.StateRoot,
                "install-" + Guid.NewGuid().ToString("N"));
            string stagingPlugin = Path.Combine(workRoot, "payload");
            string backupRoot = Path.Combine(Program.StateRoot, "previous-install");
            bool movedExisting = false;

            try
            {
                Directory.CreateDirectory(stagingPlugin);
                ExtractPayload(stagingPlugin);
                ValidatePayload(stagingPlugin);
                CopyUninstaller(stagingPlugin);
                Directory.CreateDirectory(Program.PluginsRoot);

                if (Directory.Exists(Program.InstallDirectory))
                {
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
                    Directory.Move(stagingPlugin, Program.InstallDirectory);
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
                TryDeleteDirectory(workRoot);
            }

            if (launchObs)
            {
                LaunchObs();
            }
        }

        public static void EnsureObsIsClosed(bool silent)
        {
            if (String.Equals(
                Environment.GetEnvironmentVariable("OBS_AIRPLAY_SENDER_SKIP_PROCESS_CHECK"),
                "1",
                StringComparison.Ordinal))
            {
                return;
            }

            Process[] processes = Process.GetProcessesByName("obs64");
            if (processes.Length == 0)
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

            DateTime deadline = DateTime.UtcNow.AddSeconds(25);
            while (DateTime.UtcNow < deadline)
            {
                if (Process.GetProcessesByName("obs64").Length == 0)
                {
                    return;
                }
                System.Threading.Thread.Sleep(250);
            }

            throw new InvalidOperationException(
                "OBS Studio 未能在 25 秒内关闭。请手动退出 OBS 后重试。");
        }

        private static void ExtractPayload(string destination)
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
                    foreach (ZipArchiveEntry entry in archive.Entries)
                    {
                        string relative = entry.FullName.Replace('/', Path.DirectorySeparatorChar);
                        string target = Path.GetFullPath(Path.Combine(destination, relative));
                        if (!target.StartsWith(destinationRoot, StringComparison.OrdinalIgnoreCase))
                        {
                            throw new InvalidDataException("安装包包含无效路径。");
                        }

                        if (entry.FullName.EndsWith("/", StringComparison.Ordinal))
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

            try
            {
                // Hand the launch back to Explorer so OBS does not inherit this
                // installer's elevated administrator token.
                ProcessStartInfo startInfo = new ProcessStartInfo("explorer.exe");
                startInfo.Arguments = Quote(obs);
                startInfo.UseShellExecute = false;
                Process.Start(startInfo);
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static string FindObsExecutable()
        {
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
        private readonly CheckBox launchCheckBox;
        private readonly Button installButton;

        public SetupForm(string version)
        {
            this.version = version;
            Text = "OBS AirPlay Sender 安装程序";
            ClientSize = new Size(520, 310);
            MinimumSize = new Size(536, 349);
            MaximumSize = new Size(536, 349);
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            Font = new Font("Microsoft YaHei UI", 9F);
            BackColor = Color.White;
            AutoScaleMode = AutoScaleMode.Dpi;

            Label title = new Label();
            title.Text = "OBS AirPlay Sender";
            title.Font = new Font("Microsoft YaHei UI", 18F, FontStyle.Bold);
            title.ForeColor = Color.FromArgb(24, 32, 40);
            title.AutoSize = true;
            title.Location = new Point(28, 24);
            Controls.Add(title);

            Label subtitle = new Label();
            subtitle.Text = "安装 AirPlay 投屏插件到 OBS Studio";
            subtitle.Font = new Font("Microsoft YaHei UI", 10F);
            subtitle.ForeColor = Color.FromArgb(75, 86, 96);
            subtitle.AutoSize = true;
            subtitle.Location = new Point(31, 68);
            Controls.Add(subtitle);

            Label details = new Label();
            details.Text =
                "版本 " + version + "  |  Windows 10/11 x64  |  OBS Studio 31.x+\r\n" +
                "安装到系统级 OBS 插件目录，启动时会请求管理员权限。";
            details.ForeColor = Color.FromArgb(75, 86, 96);
            details.AutoSize = false;
            details.Size = new Size(458, 46);
            details.Location = new Point(31, 101);
            Controls.Add(details);

            Label pathLabel = new Label();
            pathLabel.Text = "安装位置";
            pathLabel.ForeColor = Color.FromArgb(52, 61, 70);
            pathLabel.AutoSize = true;
            pathLabel.Location = new Point(31, 163);
            Controls.Add(pathLabel);

            TextBox pathBox = new TextBox();
            pathBox.Text = Program.InstallDirectory;
            pathBox.ReadOnly = true;
            pathBox.BackColor = Color.FromArgb(246, 248, 250);
            pathBox.ForeColor = Color.FromArgb(65, 75, 85);
            pathBox.BorderStyle = BorderStyle.FixedSingle;
            pathBox.Location = new Point(31, 187);
            pathBox.Size = new Size(458, 27);
            pathBox.TabStop = false;
            Controls.Add(pathBox);

            launchCheckBox = new CheckBox();
            launchCheckBox.Text = "安装完成后启动 OBS Studio";
            launchCheckBox.Checked = true;
            launchCheckBox.AutoSize = true;
            launchCheckBox.Location = new Point(31, 232);
            Controls.Add(launchCheckBox);

            installButton = new Button();
            installButton.Text = "安装";
            installButton.Size = new Size(98, 34);
            installButton.Location = new Point(292, 267);
            installButton.Click += InstallButtonClick;
            Controls.Add(installButton);
            AcceptButton = installButton;

            Button cancelButton = new Button();
            cancelButton.Text = "取消";
            cancelButton.Size = new Size(98, 34);
            cancelButton.Location = new Point(396, 267);
            cancelButton.DialogResult = DialogResult.Cancel;
            Controls.Add(cancelButton);
            CancelButton = cancelButton;
        }

        private void InstallButtonClick(object sender, EventArgs e)
        {
            installButton.Enabled = false;
            launchCheckBox.Enabled = false;
            Cursor previousCursor = Cursor;
            Cursor = Cursors.WaitCursor;
            Application.DoEvents();

            try
            {
                InstallerEngine.Install(version, false, false);
                bool launched = !launchCheckBox.Checked || InstallerEngine.LaunchObs();
                Cursor = previousCursor;
                MessageBox.Show(
                    "OBS AirPlay Sender 已安装。\r\n\r\n" +
                    (launchCheckBox.Checked && !launched
                        ? "没有找到 OBS Studio，请从开始菜单手动启动。"
                        : "在 OBS 中打开：工具 → AirPlay 投屏..."),
                    Program.ProductDisplayName,
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Information);
                DialogResult = DialogResult.OK;
                Close();
            }
            catch (Exception ex)
            {
                Cursor = previousCursor;
                MessageBox.Show(
                    ex.Message,
                    Program.ProductDisplayName + " 安装失败",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Error);
                installButton.Enabled = true;
                launchCheckBox.Enabled = true;
            }
        }
    }
}
