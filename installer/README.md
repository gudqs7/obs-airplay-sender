# Windows 一键安装器

安装包由 Inno Setup 6 生成，脚本为 `obs-airplay-cast.iss`。
`Build-WindowsInstaller.ps1` 只负责校验发布目录、调用 `ISCC.exe` 和生成 SHA-256，
不执行安装测试。

默认安装目标：

```text
%ProgramData%\obs-studio\plugins\obs-airplay-cast
```

该目录由当前用户创建和维护，因此首次安装、升级和卸载都不需要管理员权限。Inno Setup
使用固定的 `AppId` 和 `UsePreviousAppDir=yes`，已有用户目录会被继续沿用。

如果目标目录中检测到旧版 `bin\64bit\obs-airplay-cast.dll`，安装器会在
`PrepareToInstall` 阶段调用该目录中的旧版 `Uninstall.exe`。这个旧卸载器自身可能需要
一次 UAC；清理完成后 Inno Setup 继续安装到同一目录。

构建：

```powershell
.\installer\Build-WindowsInstaller.ps1 -Configuration RelWithDebInfo
```

生成文件：

```text
release\obs-airplay-cast-<版本>-windows-x64-Setup.exe
release\obs-airplay-cast-<版本>-windows-x64-Setup.exe.sha256
release\obs-airplay-cast-<版本>-windows-x64.zip
release\obs-airplay-cast-<版本>-windows-x64.zip.sha256
```

ZIP 是手动安装包，顶层目录为 `obs-airplay-cast`，解压到
`%ProgramData%\obs-studio\plugins` 即可。ZIP 不包含 PDB、ILK 或 EXP 文件。

如果 Inno Setup 不在默认位置，可以指定编译器：

```powershell
.\installer\Build-WindowsInstaller.ps1 -CompilerPath 'C:\path\to\ISCC.exe'
```

静默安装沿用 Inno Setup 参数：

```powershell
.\obs-airplay-cast-<版本>-windows-x64-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

## Logo

`assets/logo.svg` 是源文件；运行 `Generate-Logo.ps1` 会生成 PNG 和 ICO：

```text
logo-64.png
logo-128.png
logo-256.png
logo-512.png
obs-airplay-sender.ico  (64 / 128 / 256)
```

构建安装包时使用 `assets/obs-airplay-sender.ico`。
