# 协议

所有整数都是小端序。实现在 `core/`，手机端和 PC 测试工具共用同一份代码。

## 连接流程

1. 手机通过 TCP 连接端口 27183。USB 模式下目标是 `127.0.0.1`，由 `adb reverse` 转发到电脑。
2. 握手，见下文。
3. 手机发送 `Hello`。Wi-Fi 模式下 Host 回 `UdpSetup`，手机向该端口发探测包，Host 由此得知视频发往哪里。
4. Host 发送 `Config`，然后开始发帧，第一帧总是关键帧。

Host 最多同时服务 4 台设备，每台各有自己的连接。

- **第 5 台：** 握手和 `Hello` 之后，Host 回一个带原因的 `Bye` 并关闭连接。
- **同一台设备再次连接：** 按 `clientId` 识别，新连接接管旧连接，不占新名额。旧连接往往是已经失效、但 Host 还不知道的那条。
- **全零的 `clientId`：** 只有测试工具会用，不做识别，设置也不保存。

控制消息始终走 TCP。视频在 USB 模式下走 TCP，在 Wi-Fi 模式下走 UDP。

## 握手

X25519 临时密钥结合预共享密钥（PSK）：

```
C -> H: "SDH1" | mode(1) | clientId(16) | clientPub(32)
H -> C: hostPub(32)
C -> H: confirmClient(32)
H -> C: confirmHost(32)
```

- `transcript = BLAKE2b(magic | mode | id | clientPub | hostPub)`
- `master = BLAKE2b(key=psk, X25519(...) | transcript)`
- 各子密钥为 `BLAKE2b(key=master, label)`。

| mode | PSK 来源 | 限制 |
|---|---|---|
| 0 USB | 全零 | Host 只接受来自回环地址的连接 |
| 1 配对 | Argon2id（16 MiB，2 轮）作用于一次性配对码 | 配对码 40 位，2 分钟有效，最多尝试 5 次 |
| 2 已配对 | 上次配对保存的 `pairSecret` | 按 `clientId` 查找 |

客户端先证明自己知道 PSK，所以猜错配对码的一方拿不到任何可用于离线验证的 Host 响应。Host 的 PSK 确认只在客户端通过验证后才发出。

**已知局限：**
- USB 模式不认证 Host。手机上的恶意应用如果抢先监听本地端口，可以冒充 Host；电脑上的本地程序也能以 USB 模式连接。
- 这套握手是为本项目定制的，用的都是成熟算法，但没有经过第三方审计。

## 加密

- **TCP 记录：** `len(4) | mac(16) | ciphertext`，算法 XChaCha20-Poly1305，nonce 由递增序列号生成，长度作为附加认证数据。
- **UDP 包：** `seq(8) | mac(16) | ciphertext`，序列号作为附加认证数据，接收端用滑动窗口拒绝重放。
- 手机的探测包使用 2^63 以上的序列号，与 Host 的序列号空间分开，避免 nonce 重复。Host 只有在探测包通过认证后才会更新视频目标地址。

## 消息

第一个字节是类型。

| 类型 | 方向 | 内容 |
|---|---|---|
| 1 Hello | C→H | 协议版本、设备名、屏幕尺寸、DPI、解码能力、刷新率列表、是否要 UDP |
| 2 Config | H→C | 显示尺寸、编码尺寸、帧率、编码格式、码率、是否镜像、用户的手动设置、画面适配。之后的第一帧是关键帧。只改画面适配时会单独重发，流本身不重建 |
| 3 Frame | H→C | 帧号、采集时间、是否关键帧、Annex B 码流 |
| 4 Stats | C→H | 约每 100ms：字节数、帧数、收包数、丢包数、丢帧数、延迟变化、解码耗时、RTT |
| 5 Touch | C→H | 多点触控，坐标为 0..65535 的归一化值 |
| 6 KeyframeRequest | C→H | 请求关键帧 |
| 7 SetDisplay | C→H | 手动设置分辨率、刷新率、码率上限（0 表示自动），副屏或镜像，以及画面适配（旧客户端不发，表示不变） |
| 8/9 Ping/Pong | 双向 | 测 RTT |
| 10 Nack | C→H | 请求重传指定分片 |
| 11 UdpSetup | H→C | Host 的 UDP 端口 |
| 12 Bye | 双向 | 主动断开。Host 发出时可带 1 字节原因，1 表示设备数已满，旧客户端会忽略它 |
| 13 Viewport | C→H | 屏幕旋转或尺寸变化 |
| 14 DisplayOptions | H→C | 手机和电脑的尺寸、推荐和当前缩放、可选的分辨率列表 |

### Stats 字段

类型字节之后依次是以下 9 个 32 位字段，消息总长 37 字节。

| 字段 | 类型 | 含义 |
|---|---|---|
| `windowMs` | uint32 | 统计窗口长度，毫秒 |
| `bytes` | uint32 | 窗口内完整 `Frame` 消息的字节数 |
| `frames` | uint32 | 收到的完整帧数，不等于实际呈现帧数 |
| `packetsReceived` | uint32 | 收到并通过认证、重放检查的 UDP 包数 |
| `packetsLost` | uint32 | 按 UDP 序列号缺口估计的丢包数，乱序到达会回补 |
| `framesDropped` | uint32 | 丢帧反馈，包括无法恢复的传输丢帧和 Android 本地解码过载；Surface、配置切换不计入 |
| `delayGradientUs` | int32 | 相邻非空窗口平均到达延迟的变化，微秒；首个窗口或空窗口为 0 |
| `decodeUs` | uint32 | 最近一次完整帧接收到解码输出可用的时间，微秒，含本地排队和输入缓冲等待，不含屏幕实际呈现和扫描 |
| `rttUs` | uint32 | 最近一次 Ping/Pong 的往返时间，微秒 |

`Frame.captureUs` 属于 Host 的单调时钟，客户端不把它与本地时间直接相减作为绝对单程延迟。延迟变化的计算见 [码率控制](ratecontrol.md)，日志和界面数值的口径见 [延迟诊断](troubleshooting.md)。

## UDP 分片和 FEC

每帧切成 1180 字节的分片，每 32 个数据分片为一组，按当前丢包率追加 5%~30% 的 Reed-Solomon（GF(256)，Cauchy 矩阵）校验分片。

分片包头 18 字节：帧号、帧大小、组号、组数、分片号、数据分片数、校验分片数、标志。

接收端按顺序交付：
- 分片丢失时，8ms 后发 Nack。
- 超过 120ms 仍不完整的帧会被丢弃，然后请求关键帧。关键帧也丢失时每 500ms 重新请求一次。

## 控制台管道

`\\.\pipe\SideDroidHost` 每行一个 JSON，权限只开放给当前用户，并拒绝远程连接。最多同时服务 4 个客户端。

| cmd | 参数 |
|---|---|
| `status` | 无 |
| `set` | `width` `height` `hz` `maxKbps` `allowWireless` `mirror` `scale` `fill` `encoder`，可选 `id`。带 `id` 时显示参数只改那台设备；不带时作为新的默认值应用到所有设备，并清除各设备单独的选择（位置保留）。`allowWireless` 始终是全局的 |
| `set_gpu` | `gpu`，显卡名，空表示自动。只对 SideDroid 安装的驱动生效 |
| `pair_start` / `pair_cancel` | 无 |
| `unpair` | `id` |
| `disconnect` | 可选 `id`，不带时断开所有设备 |
| `install_driver` | 无，会触发 UAC |
| `update_install` | 无，下载并校验新版本，然后启动 Windows Installer |

`status` 的 `devices` 数组列出每台已连接的设备，包含 `id`、`name`、串流数据、`settings` 和 `options`。顶层的 `settings` 是默认值，`error` 只表示 Host 自身的问题，设备的问题在各自的 `error` 里。
