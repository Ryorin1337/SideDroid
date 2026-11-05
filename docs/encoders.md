# 编码器

## 怎么选择

控制台的“编码器”默认是自动，按下面的顺序尝试，每种都先试 HEVC，再试 H.264：

1. 画面所在显卡上的专用接口：NVIDIA 用 NVENC，AMD 用 AMF，Intel 用 Quick Sync。
2. 同一张显卡上的 Media Foundation（MF），它是 Windows 的通用接口，由驱动提供实现。
3. 其他显卡，顺序同上。这时每帧都要经系统内存拷贝。
4. CPU 上的软件编码，用 Windows 自带的编码器，优先 H.264。

运行中的保护：

- **反复失败：** 同一种编码器在前 120 帧内失败 3 次，这台设备不再用它，换下一种。
- **不响应关键帧：** 请求后 30 帧内没有关键帧，就重建编码器。新编码器的第一帧总是关键帧。
- **调码率失败：** 用新码率重建编码器。
- **用户选择的编码器不可用：** 自动选择，控制台提示已改用自动。

## 实测情况

| 编码器 | 实测情况 |
|---|---|
| NVENC | 在 RTX 4070 上实测 |
| Quick Sync | 在 Intel UHD Graphics（设备 ID A788）上实测，包括跨显卡 |
| MF | 在 Intel 和 NVIDIA 上实测 |
| 软件编码 | 在 Windows 11 上实测 |
| **AMF** | 用户在 AMD Radeon Vega 8 上实际跑通（0.0.4，USB，HEVC，1920×1080 和 3200×1440，120 Hz），日志里没有属性被拒绝、编码失败或关键帧没响应的记录。丢包、限速和 H.264 还没有在 AMD 上测过 |

实测内容：5% 丢包和 8 Mbps 限速下降码率、强制关键帧、每个关键帧都带参数集，以及用 ffprobe 检查码流。

## AMF 的依据

文档版本是 AMF SDK v1.5.3，来自 [GPUOpen-LibrariesAndSDKs/AMF](https://github.com/GPUOpen-LibrariesAndSDKs/AMF)。

- **[Ref]** `amf/doc/AMF_API_Reference.md`
- **[AVC]** `amf/doc/AMF_Video_Encode_API.md`
- **[HEVC]** `amf/doc/AMF_Video_Encode_HEVC_API.md`
- **[Sample]** `amf/public/samples/CPPSamples/SimpleEncoder/SimpleEncoder.cpp`
- **[Wiki]** [Guide for Video CODEC Encoder App Developers](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/wiki/Guide-for-Video-CODEC-Encoder-App-Developers)

| 步骤 | 做法 | 依据 |
|---|---|---|
| 加载运行库 | 运行时从系统目录加载 `amfrt64.dll`，没有就跳过 | [Ref] 2.2.1，文件名见 `core/Factory.h` 的 `AMF_DLL_NAME` |
| 初始化 | `AMFQueryVersion` 取版本，再传给 `AMFInit` | [Ref] 2.2.1：`AMF_RESULT initRes = init(version, &pFactory);` |
| 绑定设备 | `CreateContext`，再 `InitDX11(我们的设备)` | [Ref] `AMFContext`；[Sample] 的 DX11 分支 |
| 创建编码器 | `CreateComponent`，AVC 用 `AMFVideoEncoderVCE_AVC`，HEVC 用 `AMFVideoEncoder_HEVC` | [HEVC] 第 2 节；组件头文件 |
| 先设 `USAGE` | 设为 `LOW_LATENCY` | [HEVC] 1.2：Low Latency 的用途是 “Video collaboration, remote desktop”。Ultra-low latency 面向 “Video game streaming”，会限制每帧比特数 |
| 设置时机 | 静态属性都在 `Init` 之前设置 | [HEVC] 2.2.1：“must be defined before the `Init()` function is called” |
| 低延迟 | `LOWLATENCY_MODE` 设为 true | [HEVC]：“Enables low latency mode in the encoder.” |
| 速度预设 | `QUALITY_PRESET` 设为 `SPEED` | [HEVC]：Low latency 用途的默认值就是 `SPEED`，这里明确写出 |
| 码率控制 | `RATE_CONTROL_METHOD` 设为 `CBR`，目标码率等于峰值码率 | [HEVC]、[AVC] 列出的取值。选 CBR 是为了和其他编码器一致，上层码率控制按此假设 |
| 填充数据 | 保持关闭 | [HEVC]：默认 `false`，“Enable filler data for CBR usage”。关闭后，桌面静止时几乎不发数据 |
| HEVC 关键帧周期 | `GOP_SIZE` 设为 0 | [HEVC]：“`0` means only insert the first IDR/CRA (infinite GOP size)” |
| AVC 关键帧周期 | `IDR_PERIOD` 设为 0，设置失败就放弃 AMF | [AVC] `FORCE_PICTURE_TYPE`：“to use this feature, set `AMF_VIDEO_ENCODER_IDR_PERIOD` to `0`” |
| AVC 不用 B 帧 | `B_PIC_PATTERN` 设为 0 | [AVC]：“`0` indicates that B-pictures are not used”，“not supported by VCE 1.0”，所以失败时只记日志 |
| 等待输出 | `QUERY_TIMEOUT` 设为 50ms | [HEVC]：“will reduce polling load when `QueryOutput` is called” |
| 初始化编码器 | `Init(AMF_SURFACE_NV12, 宽, 高)` | [Ref] `AMFComponent::Init`；[Sample] 的默认输入格式就是 NV12 |
| 传入画面 | `CreateSurfaceFromDX11Native` 包装纹理，不拷贝 | [Ref] `AMFContext` |
| 强制关键帧 | 在该帧的表面上设 `FORCE_PICTURE_TYPE` 为 `IDR` | [HEVC] Table A-2；[AVC] 对应表格 |
| 参数集 | 关键帧上设 `INSERT_HEADER`（HEVC），或 `INSERT_SPS` 和 `INSERT_PPS`（AVC） | [HEVC] Table A-2：“Inserts SPS, PPS and VPS.” 没有周期性 IDR，所以逐帧请求 |
| 提交 | `AMF_INPUT_FULL` 和 `AMF_DECODER_NO_FREE_SURFACES` 时等 1ms 重试，`AMF_NEED_MORE_INPUT` 不处理 | [Sample] 提交循环 |
| 取输出 | 循环调用 `QueryOutput`，`AMF_REPEAT` 表示还没好 | [Wiki] |
| 复用纹理 | 拿到输出后才处理下一帧 | [Ref]：“should not make any assumptions about any relationship between input and output samples”。纹理下一帧会被覆盖，所以必须等，500ms 没有结果就算失败 |
| 判断关键帧 | 读输出的 `OUTPUT_DATA_TYPE` | [HEVC] Table A-2 之后的输出属性；[AVC] 同名属性 |
| 改码率 | 运行时设 `TARGET_BITRATE` 和 `PEAK_BITRATE` | [HEVC] 2.2.2：“flushed to encoder only before the next `Submit()` call”；[Wiki] 列为动态属性 |
| 释放 | 先 `Terminate` 编码器，再 `Terminate` 上下文 | [Sample]：“cleanup in this order”，“context is the last” |

**文档没有明确、需要在 AMD 上确认的地方：**

- `Low Latency` 和 `CBR` 组合时的实际码率表现。
- HEVC 的 `INSERT_HEADER` 是否每次都生效。文档只说明了属性本身。如果没有生效，手机会等不到参数集，表现为 30 帧后重建编码器。
- 输出是否一定是 Annex B 格式。

## Quick Sync 的依据

依据是 [oneVPL 规范](https://intel.github.io/libvpl/latest/) 和 libvpl v2.17.0 的 `hello-sharing-dx11` 示例。

- **需要 API 2.10：** 不拷贝传入纹理用的是 `ImportFrameSurface`。旧 Intel 显卡的运行库不支持，会走 MF。
- **实验性接口：** 头文件把这个接口标为 `ONEVPL_EXPERIMENTAL`，但官方示例就在用。如果某个运行库不支持，导入会失败，并自动换到 MF。
- **改码率不产生关键帧：** 规范说明，不要求 HRD 一致性时，CBR 模式下改 `TargetKbps` “will not result in the generation of a new keyframe or sequence header”。

## 软件编码

使用 Windows 自带的 H.264 和 HEVC 编码器（[H.264](https://learn.microsoft.com/windows/win32/medfound/h-264-video-encoder)、[H.265](https://learn.microsoft.com/windows/win32/medfound/h-265---hevc-video-encoder)）。

- **在 CPU 上转 NV12：** 微软文档说明，WARP 和 Basic Display Adapter 上取不到 `ID3D11VideoDevice`，不能用 GPU 做转换。
- **码率不可靠：** 实测用 1080p 以上的噪声画面要求 8 Mbps，实际约 80 Mbps，换码率模式或上限都没有改善；普通桌面约 0.7 Mbps。所以 Host 按实际产出的字节数限制编码频率，画面复杂时掉帧率，不会超出码率。
- **已知限制：** 噪声这样的极端画面，在 8 Mbps 且有丢包的 UDP 线路上收不到画面，因为关键帧传输时间超过了接收端 120ms 的组帧期限。USB 不受影响。

## 微软虚拟显卡（WARP）

[WARP](https://learn.microsoft.com/windows/win32/direct3darticles/directx-warp) 是 Windows 自带的软件渲染器，文档列出的适用情况包括没有 Direct3D 硬件、显卡驱动不可用。

- **作用：** 给没有显卡的电脑（比如虚拟机）一条能用的路径，这时用 WARP 采集，用 CPU 编码。
- **实测：** 在这台电脑上把虚拟屏指定到 WARP，配合软件编码或跨显卡的 NVENC，都能出画面。真实的虚拟机环境没有测过。
- **没有 GPU 也能显示的前提：** 虚拟显示驱动能在该环境下运行，这一点没有验证。
