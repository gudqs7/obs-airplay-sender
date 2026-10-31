# 协议约束

本文记录当前已通过抖音直播伴侣实测的发送端约束。修改协议相关代码时，必须同时运行
Go 包编译检查和 AAC-ELD 往返测试。

## FairPlay 与 pair-verify

抖音直播伴侣会完成 raw pair-verify，但 FairPlay 流密钥不能直接使用 FairPlay helper
返回的原始 key。默认 `mixed` 路径会加入 X25519 shared secret：

```text
stream_key = SHA-512(fairplay_aes_key || x25519_shared_secret)[:16]
```

实现位置：

- `patches/doubletake-obs-airplay.patch`
- 获取依赖后的 `.deps/source/doubletake/internal/airplay/pairing.go`
- 获取依赖后的 `.deps/source/doubletake/internal/airplay/fairplay.go`

约定：

- 空 `FairPlayKeyMode` 使用能力默认值；raw pair-verify 当前默认混合 shared secret。
- `FairPlayKeyMode=mixed` 强制混合。
- `FairPlayKeyMode=raw` 只用于确认某个接收端确实需要未混合旧路径。
- 不要为了让新问题看起来有进展而把默认值改为 `raw`。

同一轮修复还取消了多余的首帧延迟。当前 H.264 路径不应再出现
`16003008` / `16003013` 这类接收端连续解码失败。

## 视频

- 编码格式：H.264。
- profile：High。
- 码率控制：CBR。
- 关键帧间隔：1 秒。
- 输出尺寸：按接收端协商画布等比缩放，宽高保持偶数。
- OBS 编码包通过回环 IPC 传送到 bridge，bridge 转换为 Annex-B access unit。

HEVC 不属于当前抖音验收路径。

## AAC-ELD 协商

抖音屏幕镜像使用 AAC-ELD：

| 项 | 值 |
|---|---|
| Go 枚举 | `AudioCodecAACELD = 8` |
| RTSP `ct` | `8` |
| RTSP `spf` | `480` |
| RTSP `audioFormat` | `0x1000000` |
| PCM | 44100 Hz、16-bit、双声道交错 |
| 每帧 PCM | `480 × 2 × 2 = 1920` 字节 |
| 编码码率 | 256000 bit/s |
| FDK AOT | `AOT_ER_AAC_ELD` (39) |
| FDK transmux | `TT_MP4_RAW` |

旧 ALAC 链路使用 `ct=2`、`spf=352`。它在抖音侧可能完成部分协商，但没有可用的音频
解码输出，因此不能作为抖音无声问题的回退方案。

抖音日志中的 `codecType=2` 是接收端内部编号，不代表 RTSP 的 `ct=2`。真实 AAC-ELD
会话的决定性标记是：

```text
CastMate Audio Open Multi ... samples : 480
```

## AAC-ELD helper 进程协议

helper 文件名必须是 `obs-airplay-aac-eld.exe`。bridge 按以下顺序查找：

1. 环境变量 `OBS_AIRPLAY_ELD_ENCODER`。
2. bridge 可执行文件同目录。
3. 当前工作目录。

启动后：

1. helper 初始化 FDK encoder。
2. stdout 输出一行 ASCII `READY 480\n`。
3. bridge 每次向 stdin 写入恰好 1920 字节 S16LE 交错 PCM。
4. helper 每次向 stdout 写入 4 字节 little-endian AAC 帧长度。
5. 紧接着写入该长度的原始 AAC-ELD 帧。

不得输出 ADTS 头，不得自动增加额外分隔符，也不要在 `READY` 之前输出编码数据。

FDK 配置：

```text
AACENC_AOT             = AOT_ER_AAC_ELD (39)
AACENC_SAMPLERATE      = 44100
AACENC_CHANNELMODE     = MODE_2
AACENC_CHANNELORDER    = 1
AACENC_BITRATE         = 256000
AACENC_TRANSMUX        = TT_MP4_RAW
AACENC_SBR_MODE        = 0
AACENC_GRANULE_LENGTH  = 480
AACENC_AFTERBURNER     = 1
```

## 惰性初始化

`NewExternalAudioCapture()` 只保存编码类型和 PCM 源，不启动 helper。第一次读取 PCM
帧时，`readFramePosition()` 必须在 `eld == nil` 时调用 `newELDEncoder()`。

不要在 `NewExternalAudioCapture()` 中硬编码 helper 路径，也不要在首次读取时返回
`EOF`。后一种行为会造成 AirPlay 协商成功、只出现 `Audio_Open_Multi`，随后第一帧音频
立即结束。

## 音频验收日志

一次成功的会话通常依次出现：

```text
onAudioCodec ... codecType=2
CastMate Audio Open Multi ... samples : 480
InitAudioPlayer id=...
SendAudioPacket ... size=348
onAudioDecode ... lens=1920
call Audio_Process_Multi ... size=1920
```

判断规则：

1. `samples : 480`：选择了 AAC-ELD。
2. `SendAudioPacket size≈340-360`：256 kbps AAC-ELD 帧进入接收端。
3. `onAudioDecode lens=1920`：解出 480 samples × 2 channels × 2 bytes。
4. `Audio_Process_Multi size=1920`：PCM 进入抖音音频处理链。
5. 最后以抖音调音台电平和实际声音确认。

Windows 音频会话探针只能检查会话、音量和端点，不能替代接收端验收。一次成功会话中，
该探针仍可能在默认 HDMI 端点报告 `peak=0`。

## 禁止回退

- 不要用 ALAC `spf=352` 解决抖音无声。
- 不要依据 `codecType=2` 判断 AAC-ELD。
- 不要把 AAC helper 改成 ADTS。
- 不要在 Go 中写死 helper 的绝对路径。
- 不要删除外部音频捕获中的惰性编码器初始化。
- 不要覆盖正在被 OBS 加载的 DLL。
- 不要把电视或显示器端点作为本项目目标。
