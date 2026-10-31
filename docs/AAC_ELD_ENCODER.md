# AAC-ELD 编码器

## 当前实现

`obs-airplay-aac-eld.exe` 使用 Ittiam `libxaac` 编码抖音要求的 AAC-ELD：

- 上游：`ittiam-systems/libxaac`
- 固定提交：`2fbadd57bca46693b7a077bcf1513131106d960c`
- 许可证：Apache-2.0
- 本地补丁：`patches/libxaac-eld-core.patch`
- 输入：44100 Hz、16-bit、双声道交错 PCM，每帧 1920 字节
- 输出：每帧约 346-347 字节的原始 AAC-ELD access unit
- 进程协议：`READY 480`，随后使用 4 字节 little-endian 长度前缀加裸帧

第三方源码不进入本仓库。`scripts/fetch-dependencies.ps1` 负责获取固定提交并应用补丁。

## 为什么需要补丁

libxaac 默认把无 MPS 的 AAC-ELD 配置当作 2:1 ELD-SBR。该模式使用 3840 字节
44.1 kHz PCM 生成一个 480-core access unit，时间基为 22050 Hz，不满足抖音已验证的
480-sample 帧约束。

本地补丁保留 MPS/ELDv2 的 ELD-SBR 行为，只让无 MPS 的 AAC-ELD 使用 core encoder：

```text
ASC:                  4 bytes
PCM per access unit:  1920 bytes (480 samples x 2 channels x 2 bytes)
Metadata timescale:   44100 Hz
Transport:            raw access unit, no ADTS
```

这是针对固定上游提交的行为补丁，不是 libxaac 的稳定公共配置。升级提交时必须重新
核对 `encoder/ixheaace_api.c` 中的 `aac_classic`、`eldsbr_found`、输入帧长度和时间基。

## API 适配

helper 使用 libxaac 的 C 编码接口：

```text
ixheaace_create()
ixheaace_process()
ixheaace_delete()
```

初始化时 libxaac 会在输出缓冲区生成 ASC。helper 只读取并用它确认初始化模式，
不会向 bridge 输出 ASC。每处理一个 1920 字节 PCM 帧，helper 都先验证输出长度，
再写入 4 字节长度和裸 access unit。

主要配置：

```text
AOT                   39 (AAC-ELD)
sample rate           44100
channels              2
samples per frame     480
bitrate               256000
MPS                   0
ADTS                  0
```

启动时还会校验 `input_size=1920`、`samp_freq=44100`、`down_sampling_ratio=1`
和 `ASC size=4`。任何一项不符都会在发送 `READY` 前退出，避免 bridge 在错误模式下
继续协商。

## 验证

Windows x64 构建和辅助解码验证结果：

```text
asc=4 input=1920 bytes/frame encoded=346-347 bytes/frame
decoded=19200 samples peak=16416 rms=11364.1
```

`decoded=19200` 等于 `20 x 480 x 2`。helper stdio 测试：

```text
ready=480 frames=20 bytes=7005 frame-bytes=346-347
```

当前实现已在抖音直播伴侣中完成实际投屏和声音验收。接收端日志的关键条件仍为：

```text
CastMate Audio Open Multi ... samples : 480
SendAudioPacket ... size≈346-347
onAudioDecode ... lens=1920
```

## 构建与测试

```powershell
.\scripts\fetch-dependencies.ps1

$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --preset windows-x64
& $cmake --build --preset windows-x64 --config RelWithDebInfo `
  --target obs-airplay-aac-eld --parallel
.\tools\aac-eld-helper-test\test-helper.ps1 `
  -HelperPath .\build_x64\RelWithDebInfo\obs-airplay-aac-eld.exe
```

helper 测试会发送 20 个合成 PCM 帧，并验证：

1. 首行必须是 `READY 480`。
2. 每个输出帧都有完整 4 字节长度前缀。
3. 每个裸帧长度在有效范围内。
4. 总帧数与输入 PCM 帧数一致。

## 维护约束

- 固定 libxaac 提交和补丁必须一起更新，不能只替换提交号。
- 不要重新启用无 MPS 的 ELD-SBR。
- 不要输出 ASC、ADTS 或额外分隔字节。
- 不要改变 helper 路径、`READY 480` 或长度前缀协议。
- 每次升级后重新运行 helper 测试，并检查抖音日志中的 480-sample 和 1920-sample
  解码结果。
- Apache-2.0 许可证和 NOTICE 必须随编码器二进制一起分发。
- MPEG AAC 必要专利授权需要单独评估。
