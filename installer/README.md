# Windows 一键安装器

`Build-WindowsInstaller.ps1` 把 CMake 安装目录中的插件文件打成 ZIP，嵌入一个
64 位 .NET Framework Windows Forms 安装程序，并输出可直接双击的 `Setup.exe`。

安装目标：

```text
%ProgramData%\obs-studio\plugins\obs-airplay-cast
```

安装器启动时请求一次管理员权限，用于更新系统级 OBS 插件目录。覆盖前会把上一版备份到
`%ProgramData%\obs-airplay-sender\previous-install`，并注册系统级卸载入口。

点击“安装”后才会显示当前步骤、百分比和进度条，鼠标不会切换成等待图标。ZIP 解压、
文件校验、备份、替换、卸载信息注册和启动 OBS 均在后台执行，安装期间窗口保持可重绘；
只有 OBS 启动调用返回后才显示 100%。完成前不能通过取消按钮或关闭按钮中断。

安装器从普通权限启动时会重新以管理员权限运行。UAC 结束后，新进程会短暂重试恢复自己的
前台焦点，成功后立即停止，不会持续抢占其他应用。

安装后的 OBS 设置中，`启动OBS自动连接抖音` 对首次安装和尚未保存该选项的用户默认
勾选。用户手动取消后该选择会持久保存，后续启动不会被默认值覆盖。

静默安装：

```powershell
.\obs-airplay-cast-<版本>-windows-x64-Setup.exe --silent
```

静默安装不会启动 OBS。需要安装后启动时追加 `--launch`。

安装器进度自测：

```powershell
.\installer\Test-Installer.ps1 -SetupPath .\release\obs-airplay-cast-<版本>-windows-x64-Setup.exe
```

UI 与提权焦点回归测试：

```powershell
.\installer\Test-InstallerUi.ps1 -SetupPath .\release\obs-airplay-cast-<版本>-windows-x64-Setup.exe
.\installer\Test-InstallerElevation.ps1 -SetupPath .\release\obs-airplay-cast-<版本>-windows-x64-Setup.exe
```

`Test-InstallerUi.ps1` 会模拟 OBS 启动阻塞，验证 98% 阶段窗口仍能响应且 100% 不提前出现。
追加 `-UseInstalledObs` 可验证本机真实 OBS 的启动路径。提权测试模拟父进程向子安装器转移
前台权限，不会自动操作 Windows UAC 安全桌面。

## Logo

`assets/logo.svg` 是源文件；运行 `Generate-Logo.ps1` 会生成 PNG 和 ICO：

```text
logo-64.png
logo-128.png
logo-256.png
logo-512.png
obs-airplay-sender.ico  (64 / 128 / 256)
```

只保留 64px 及以上的图标尺寸，不生成 16/32/48px 变体。构建安装器时会嵌入
64px 图标并设置 EXE 图标。
