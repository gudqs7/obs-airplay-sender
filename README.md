# OBS AirPlay Sender

`obs-airplay-sender` 是面向 Windows x64 的非官方 OBS Studio 插件，用局域网 AirPlay
把 OBS 主画布和主混音发送到抖音直播伴侣的
`iOS投屏 → 无线投屏`，无需 NDI、二维码或手工填写接收端 IP。

> 当前目标只有抖音直播伴侣。项目不面向电视或 Apple TV 使用场景。

## 当前约束

以下约束来自已经实测通过的发送端实现和协议修复，不能为了兼容其他接收端而回退：

- FairPlay 默认使用 `mixed` 模式。抖音 raw pair-verify 路径的流密钥为
  `SHA-512(FairPlay AES key || X25519 shared secret)[:16]`。`raw` 仅用于诊断。
- 抖音屏幕镜像音频使用 AAC-ELD：`ct=8`、`spf=480`、
  `audioFormat=0x1000000`、256 kbps。
- AAC helper 输出 4 字节 little-endian 帧长度加原始 AAC-ELD 裸帧，不输出 ADTS。
- AAC helper 首次需要编码时才惰性启动；外部 PCM 捕获不能在第一次读取时直接返回 EOF。
- 接收端日志中的 `codecType=2` 不是 AAC-ELD 的协议 `ct`。判断抖音实际使用
  AAC-ELD 的决定性证据是 `CastMate Audio Open Multi ... samples : 480`。

协议细节见 [docs/PROTOCOL.md](docs/PROTOCOL.md)，进程和数据流见
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，构建与发布检查见
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 系统要求

- Windows 10/11 x64
- OBS Studio 31.x（构建配置使用 OBS Studio 31.1.1 头文件和依赖）
- 抖音直播伴侣已进入 `iOS投屏 → 无线投屏`
- PC 和手机处在同一局域网，且局域网内没有客户端隔离
- 构建时需要 Visual Studio 2022 C++ 工具链、Windows 10/11 SDK、CMake 3.28+、
  Go 1.23+ 和 Git

## AAC-ELD 编码器

当前使用 [`ittiam-systems/libxaac`](https://github.com/ittiam-systems/libxaac)，固定到
上游提交 `2fbadd57bca46693b7a077bcf1513131106d960c`。第三方源码不存入本仓库，
构建前由 `scripts/fetch-dependencies.ps1` 获取，并应用
[`patches/libxaac-eld-core.patch`](patches/libxaac-eld-core.patch)。

该补丁关闭无 MPS AAC-ELD 路径中的 2:1 ELD-SBR，使 1920 字节 PCM 帧对应一个
44.1 kHz、480-sample、256 kbps 的原始 AAC-ELD access unit。当前实现已经通过
Windows x64 构建、helper stdio 协议测试和抖音直播伴侣实际投屏验收。

编码器实现和验证细节见
[docs/AAC_ELD_ENCODER.md](docs/AAC_ELD_ENCODER.md)。

## 构建

先从仓库根目录获取固定版本的上游依赖：

```powershell
.\scripts\fetch-dependencies.ps1
```

依赖会放在 `.deps\source`，不会进入 Git：

| 依赖 | 上游 | 固定提交 |
|---|---|---|
| doubletake | `omarroth/doubletake` | `5e2f1cbe3e1070bcefec8e549c7baf884727bc00` |
| libxaac | `ittiam-systems/libxaac` | `2fbadd57bca46693b7a077bcf1513131106d960c` |

doubletake 还需要应用本项目的最小补丁
[`patches/doubletake-obs-airplay.patch`](patches/doubletake-obs-airplay.patch)；脚本会把
`bridge/cmd/obsairplay-bridge` 复制到依赖模块中编译。

然后执行：

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --preset windows-x64
& $cmake --build --preset windows-x64 --config RelWithDebInfo --parallel
```

如果 Visual Studio 安装在其他位置，请使用本机实际的 `cmake.exe`。首次配置还会把
OBS 31.1.1、OBS 预编译依赖和 Qt6 下载到 `.deps`，然后构建插件、Go bridge 和
AAC-ELD helper。

主要产物：

```text
build_x64\RelWithDebInfo\obs-airplay-cast.dll
build_x64\RelWithDebInfo\obs-airplay-cast-bridge.exe
build_x64\RelWithDebInfo\obs-airplay-aac-eld.exe
build_x64\rundir\RelWithDebInfo\
```

完整验证命令见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 一键安装

从 [GitHub Releases](https://github.com/gudqs7/obs-airplay-sender/releases) 下载：

```text
obs-airplay-cast-<版本>-windows-x64-Setup.exe
```

先正常关闭 OBS Studio，然后双击安装包。安装程序会：

- 检查 64 位 Windows，并把插件安装到系统级目录
  `%ProgramData%\obs-studio\plugins\obs-airplay-cast`。
- 检测 OBS 是否正在运行，并在确认后先关闭 OBS 再安装。
- 覆盖升级已有版本，同时保留上一版安装目录用于回滚。
- 安装完成后直接启动 OBS。
- 在 Windows 的 `设置 → 应用 → 已安装的应用` 中注册卸载入口。

安装器会在启动时请求一次管理员权限，用于更新系统级 OBS 插件目录。当前构建没有
商业代码签名证书，首次运行时 Windows SmartScreen 可能显示提示；请从 Releases
下载，并用同一页面提供的 SHA-256 校验值核对文件。

自动部署或排查安装问题时，可以使用静默参数：

```powershell
.\obs-airplay-cast-<版本>-windows-x64-Setup.exe --silent
```

## 从源码安装

先正常关闭 OBS Studio，不要覆盖正在被 OBS 加载的 DLL。然后执行：

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --install build_x64 --config RelWithDebInfo --prefix "$env:ProgramData\obs-studio\plugins"
```

安装后的目录应包含：

```text
%ProgramData%\obs-studio\plugins\obs-airplay-cast\
├── bin\64bit\obs-airplay-cast.dll
├── bin\64bit\obs-airplay-cast-bridge.exe
├── bin\64bit\obs-airplay-aac-eld.exe
├── data\locale\
├── licenses\
├── LICENSE
├── README.md
└── THIRD_PARTY_NOTICES.md
```

重新启动 OBS Studio。

## 制作一键安装包

先完成 Windows x64 构建，并把插件安装到本地发布目录：

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --install build_x64 --config RelWithDebInfo --prefix .\release\RelWithDebInfo
.\installer\Build-WindowsInstaller.ps1
```

生成文件：

```text
release\obs-airplay-cast-<版本>-windows-x64-Setup.exe
release\obs-airplay-cast-<版本>-windows-x64-Setup.exe.sha256
```

## 使用

1. 打开抖音直播伴侣，进入 `iOS投屏 → 无线投屏`。
2. 在 OBS 选择 `工具 → AirPlay 投屏...`。
3. 等待接收端自动扫描完成。
4. 选择名称形如 `抖音直播伴侣[xxxxxx]` 的接收端。
5. 帧率通常选择 `60 fps`；如果 OBS 主画布不是 60 fps，也可以选择 `30 fps`。
6. 1080p60 建议从 `12000 kbps` 起步，在画质和无线稳定性之间取平衡。
7. 点击同一窗口中的 `开始投屏`。运行时按钮会变为 `停止投屏`。

设置窗口内的刷新按钮会重新扫描接收端。开始投屏后需要先停止，才能重新扫描。

## 推荐设置

| 参数 | 推荐值 | 说明 |
|---|---:|---|
| 帧率 | 60 fps | OBS 主画布为 60 fps 时最平滑 |
| 视频码率 | 12000-16000 kbps | 1080p60 的常用范围 |
| H.264 编码器 | 自动 | 优先 NVENC，其次 QSV，最后 x264 |
| 音频 | 44.1 kHz / 16-bit / stereo | OBS 主混音由 bridge 编码为 AAC-ELD |

## 工作方式

```text
OBS 主画布 / 主混音
  ├─ H.264 编码包 ─────────────────┐
  └─ 44.1 kHz / S16LE / stereo ────┤
                                     ▼
                        obs-airplay-cast-bridge.exe
                        ├─ mDNS 扫描
                        ├─ AirPlay 2 握手 / FairPlay
                        ├─ RTP 视频、音频和时间轴
                        └─ obs-airplay-aac-eld.exe
                                     │
                                     ▼
                          抖音直播伴侣无线投屏
```

OBS 插件 DLL 负责采集主画布、注册 H.264 编码器、接收主混音 PCM，并通过回环 TCP
把带时间戳的数据交给 Go bridge。bridge 负责 AirPlay 协议、FairPlay、RTP 和 AAC-ELD
helper 生命周期。数据只经过本机回环 IPC 和局域网 AirPlay，不经过公网。

## 日志与故障排查

### OBS 日志

从 OBS 的 `帮助 → 日志文件 → 查看当前日志` 打开。重点搜索：

```text
[obs-airplay-cast]
airplay session ready:
[AUDIO-CAPTURE]
[AUDIO-TIMING]
```

### Bridge 日志

bridge 日志位于：

```text
%TEMP%\obs-airplay-cast-bridge.log
```

需要在启动 OBS **之前**开启详细协议日志：

```powershell
$env:OBS_AIRPLAY_DEBUG = '1'
Start-Process 'C:\Program Files\obs-studio\bin\64bit\obs64.exe' `
  -WorkingDirectory 'C:\Program Files\obs-studio\bin\64bit'
```

OBS 启动后再设置环境变量不会影响已经启动的 bridge 子进程。

### 抖音接收端日志

日志目录：

```text
%APPDATA%\webcast_mate\logs\*.txt
```

一次成功的 AAC-ELD 音频会话通常包含：

```text
onAudioCodec ... codecType=2
CastMate Audio Open Multi ... samples : 480
InitAudioPlayer id=...
SendAudioPacket ... size=348
onAudioDecode ... lens=1920
call Audio_Process_Multi ... size=1920
```

判断顺序：

1. `samples : 480` 证明协商的是 AAC-ELD，而不是旧 ALAC 的 352 samples。
2. `SendAudioPacket` 大小约为 340-360 字节，符合 256 kbps AAC-ELD。
3. `onAudioDecode` 的 `lens=1920` 对应 `480 × 2 声道 × 2 字节`。
4. 最后以抖音调音台电平和实际声音验收。

不要把这些日志提交到仓库，也不要用单条 `codecType=2` 判断音频是否正常。

### 扫描列表为空

- 确认抖音直播伴侣当前在 `iOS投屏 → 无线投屏`，不是安卓页签。
- 确认 PC 和手机在同一局域网。
- 关闭 VPN、TUN 模式或虚拟网卡的客户端隔离。
- 先停止投屏，再点击刷新按钮。

### 能连接但没有画面

- 确认 OBS 主画布至少有一个正在渲染的场景。
- 检查 OBS 日志中的 `airplay session ready` 和编码器初始化信息。
- 确认 H.264 编码器可用；插件会优先选择 NVENC，再选择 QSV，最后选择 x264。
- 不要将 FairPlay 默认模式改回 `raw`。当前抖音路径需要混合 X25519 shared secret。

### 有画面但没有声音

- 确认 OBS 主混音电平不是静音，且 AirPlay 发送的是主混音。
- 确认 `obs-airplay-aac-eld.exe` 与 bridge 位于同一目录。
- 检查 bridge 日志中的 `audioCodec=8` 和 `[AUDIO-CAPTURE] levels`。
- 接收端日志必须出现 `samples : 480`；看到 352 说明仍在走旧 ALAC 路径。
- 不要把 AAC-ELD 改成 ADTS。AirPlay 此处要求原始裸帧。

### 辅助诊断工具

```powershell
# 查看某个 Windows 音频会话；它不是抖音调音台电平的替代品
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\windows-audio-session-probe.ps1 `
  -ProcessId <pid> -Samples 10 -IntervalMs 200 -AllDevices
```

`tools/ipc_sender_probe.py` 可向 bridge 注入合成 H.264 和测试音，具体参数见
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 已知限制

- 仅支持 Windows x64 上的 OBS Studio。
- 发送端固定使用 H.264；HEVC 不属于当前抖音验收路径。
- 接收端能力、抖音客户端版本和局域网环境都可能影响发现和连接。
- Windows 音频会话探针可能把实际有声的抖音会话报告为 `peak=0`，不能单独作为失败依据。
- 项目不提供对第三方接收端未来协议变化的兼容性保证。
- libxaac 依赖固定的本地 AAC-ELD 补丁；升级上游提交后必须重新运行 helper 协议和
  抖音实机验收。

## 许可证

- 本插件源码使用根目录 [LICENSE](LICENSE) 中的 GNU GPL v2 许可证文本。
- AirPlay 发送协议层来自
  [`omarroth/doubletake`](https://github.com/omarroth/doubletake)，使用
  `LGPL-3.0-or-later`；本项目只提交适配补丁，许可证文本见
  `licenses/doubletake-LICENSE.txt` 和 `licenses/GPL-3.0.txt`。
- AAC-ELD 编码器来自
  [`ittiam-systems/libxaac`](https://github.com/ittiam-systems/libxaac)，
  使用 Apache-2.0；许可证和 NOTICE 见 `licenses/libxaac-LICENSE.txt` 与
  `licenses/libxaac-NOTICE.txt`。
- OBS Studio API 依其 GPL 条款使用。

Apache-2.0 不替代 MPEG AAC 必要专利授权。分发包含 AAC 编码器的二进制包或将其用于
商业产品前，应单独评估 MPEG AAC 专利授权。
完整第三方说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 版本说明

- `0.1.2`：公开源码版本。包含 FairPlay `mixed` 流密钥修复、首帧延迟移除、
  AAC-ELD `ct=8/spf=480/audioFormat=0x1000000` 音频链路和惰性 helper 初始化。
- `0.1.1`：修复抖音 raw pair-verify 后的 FairPlay 流密钥和视频连续解码问题。
