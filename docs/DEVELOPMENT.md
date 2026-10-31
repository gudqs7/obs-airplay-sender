# 开发与验证

## 工具链

- Windows 10/11 x64
- Visual Studio 2022，安装 `Desktop development with C++` 和 Windows SDK
- CMake 3.28 或更新版本
- Go 1.23 或更新版本
- Git
- OBS Studio 31.x，用于最终运行验证

构建配置固定在 `buildspec.json` 中，当前使用 OBS Studio 31.1.1。

第三方源码不进入本仓库。首次构建先执行：

```powershell
.\scripts\fetch-dependencies.ps1
```

脚本固定并获取以下上游提交：

- `omarroth/doubletake@5e2f1cbe3e1070bcefec8e549c7baf884727bc00`
- `mstorsjo/fdk-aac@c41c3c7516ea8d0d78a53b96b30bed3a29745cc6`

doubletake 会应用 `patches/doubletake-obs-airplay.patch`，并复制
`bridge/cmd/obsairplay-bridge` 到依赖模块中编译。

## 配置和构建

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --preset windows-x64
& $cmake --build --preset windows-x64 --config RelWithDebInfo --parallel
```

构建目标包括：

- `obs-airplay-cast`：OBS 插件 DLL。
- `obs-airplay-bridge`：Go bridge。
- `obs-airplay-aac-eld`：FDK AAC-ELD helper。

`obs-airplay-aac-eld-probe` 是显式测试目标，不会包含在默认构建中。

## Go bridge 编译检查

当前 bridge 使用 `CGO_ENABLED=0`，不依赖系统安装的 libfdk-aac：

```powershell
Push-Location .deps\source\doubletake
$env:CGO_ENABLED = '0'
go build -trimpath -o ..\..\build_x64\obs-airplay-cast-bridge-check.exe .\cmd\obsairplay-bridge
Remove-Item Env:CGO_ENABLED
Pop-Location
```

Windows 上直接运行整个上游 Go 测试集可能因 Linux-only 的 `Pdeathsig` 测试失败。可以
交叉编译测试二进制来检查 `internal/airplay` 的编译完整性：

```powershell
Push-Location .deps\source\doubletake
$env:CGO_ENABLED = '0'
$env:GOOS = 'linux'
go test -c -o ..\..\build_x64\airplay-tests-linux.test .\internal\airplay
Remove-Item Env:GOOS, Env:CGO_ENABLED
Pop-Location
```

## AAC-ELD 往返测试

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --build .\build_x64 --config RelWithDebInfo --target obs-airplay-aac-eld-probe --parallel
.\build_x64\RelWithDebInfo\obs-airplay-aac-eld-probe.exe
```

通过时应看到类似输出：

```text
encoded=348 bytes/frame decoded=19200 samples peak=16461 rms=11436.9
```

`decoded=19200` 等于 20 帧 × 480 samples × 2 声道。峰值必须明显大于 0，才能排除
编码器输出静音。

## 安装测试

不要在 OBS 运行时覆盖安装目录。先正常关闭 OBS，然后执行：

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --install build_x64 --config RelWithDebInfo --prefix "$env:ProgramData\obs-studio\plugins"
```

安装会带上 README、根许可证、第三方通知、doubletake 许可证和 FDK AAC 许可证。

## 一键安装包

`Package-Windows.ps1` 在 CI 中除了生成便携 ZIP，还会调用
`installer/Build-WindowsInstaller.ps1` 生成当前用户级 `Setup.exe`。本地构建时：

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --install build_x64 --config RelWithDebInfo --prefix .\release\RelWithDebInfo
.\installer\Build-WindowsInstaller.ps1 -Configuration RelWithDebInfo
```

安装器会嵌入完整插件载荷，并生成对应的 `.sha256` 文件。它安装到
`%ProgramData%\obs-studio\plugins\obs-airplay-cast`，启动时请求管理员权限。验证安装器时
至少检查：

1. OBS 运行时先确认关闭再覆盖安装。
2. 安装后 OBS 可以加载 `obs-airplay-cast.dll` 并显示 AirPlay 投屏菜单。
3. 再次运行安装包可以完成覆盖升级。
4. Windows 应用列表中的卸载入口可以移除插件。

## 合成输入探针

bridge 的 `READY <port>` 端口可直接用 Python 探针注入合成视频、音频和接收端目标：

```powershell
python .\tools\ipc_sender_probe.py `
  --port <bridge-ready-port> `
  --h264 .\build_x64\probe.h264 `
  --target '抖音直播伴侣[<device-suffix>]' `
  --fps 30 --bitrate 8000 --seconds 12 `
  --audio-tone-hz 1000 --audio-amplitude 0.5
```

探针输出是临时测试数据，不允许提交。

## 调试日志

在启动 OBS 之前设置：

```powershell
$env:OBS_AIRPLAY_DEBUG = '1'
```

重点日志：

```text
airplay session ready: ... audioCodec=8
[AUDIO-CAPTURE] levels peak=... rms=...
[AUDIO-TIMING] frame=... encoded=...
```

bridge 日志位于 `%TEMP%\obs-airplay-cast-bridge.log`，每次 bridge 启动时重建。

## 发布前检查

1. 运行 Windows CMake `RelWithDebInfo` 全量构建。
2. 运行 Go bridge 编译检查和 `internal/airplay` 交叉编译测试。
3. 运行 AAC-ELD 编码到解码往返测试，确认峰值非零。
4. 检查三个产物版本一致，且 AAC helper 与 bridge 被复制到同一目录。
5. 确认安装结果包含 `LICENSE`、`THIRD_PARTY_NOTICES.md`、
   `licenses\` 下的 doubletake、GPLv3 和 FDK AAC 许可证文本。
6. 审计提交内容，排除日志、用户目录、凭据、PDB、EXE、缓存和 `.deps`。
7. 若创建二进制发布包，核对 DLL、bridge、helper 哈希和 release 包内文件。

## 许可证发布检查

- 根插件使用 GPL v2 许可证文本。
- doubletake 使用 LGPL-3.0-or-later，源码和许可证必须保留。
- FDK AAC 要求保留完整 NOTICE；顶层 `METADATA` 还标有特殊许可条件。
- 二进制分发需要提供对应 FDK AAC 源码，并同时携带两份 NOTICE。
- FDK AAC 不授予专利许可，商业分发前应单独评估 MPEG AAC 专利授权。

## 代码结构

| 路径 | 内容 |
|---|---|
| `src/` | OBS 插件、bridge IPC 客户端和 AAC helper |
| `bridge/` | 项目自有的 Go bridge 入口和 IPC 协议 |
| `patches/` | 针对固定 doubletake 上游提交的最小协议补丁 |
| `scripts/` | 固定版本第三方依赖获取脚本 |
| `.deps/source/` | 构建时获取的 doubletake 和 FDK AAC，受 `.gitignore` 排除 |
| `tools/` | IPC、音频会话和窗口诊断探针 |
| `cmake/` | OBS 插件模板的 CMake 配置 |
| `.github/` | Windows 构建和格式检查工作流 |
