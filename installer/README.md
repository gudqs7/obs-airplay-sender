# Windows 一键安装器

`Build-WindowsInstaller.ps1` 把 CMake 安装目录中的插件文件打成 ZIP，嵌入一个
64 位 .NET Framework Windows Forms 安装程序，并输出可直接双击的 `Setup.exe`。

安装目标：

```text
%ProgramData%\obs-studio\plugins\obs-airplay-cast
```

安装器启动时请求一次管理员权限，用于更新系统级 OBS 插件目录。覆盖前会把上一版备份到
`%ProgramData%\obs-airplay-sender\previous-install`，并注册系统级卸载入口。

静默安装：

```powershell
.\obs-airplay-cast-<版本>-windows-x64-Setup.exe --silent
```

静默安装不会启动 OBS。需要安装后启动时追加 `--launch`。
