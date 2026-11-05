# 协议

所有整数都是小端序。实现在 `core/`，手机端和 PC 测试工具共用同一份代码。

## 连接流程

1. 手机通过 TCP 连接端口 27183。USB 模式下目标是 `127.0.0.1`，由 `adb reverse` 转发到电脑。
2. 握手，见下文。
3. 手机发送 `Hello`。Wi-Fi 模式下 Host 回 `UdpSetup`，手机向该端口发探测包，Host 由此得知视频发往哪里。
4. Host 发送 `Config`，然后开始发帧，第一帧总是关键帧。

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
| 2 Config | H→C | 显示尺寸、编码尺寸、帧率、编码格式、码率、是否镜像、用户的手动设置。之后的第一帧是关键帧 |
| 3 Frame | H→C | 帧号、采集时间、是否关键帧、Annex B 码流 |
| 4 Stats | C→H | 约每 100ms：字节数、帧数、收包数、丢包数、丢帧数、延迟变化、解码耗时、RTT |
| 5 Touch | C→H | 多点触控，坐标为 0..65535 的归一化值 |
| 6 KeyframeRequest | C→H | 请求关键帧 |
| 7 SetDisplay | C→H | 手动设置分辨率、刷新率、码率上限（0 表示自动），以及副屏或镜像 |
| 8/9 Ping/Pong | 双向 | 测 RTT |
| 10 Nack | C→H | 请求重传指定分片 |
| 11 UdpSetup | H→C | Host 的 UDP 端口 |
| 12 Bye | 双向 | 主动断开 |
| 13 Viewport | C→H | 屏幕旋转或尺寸变化 |
| 14 DisplayOptions | H→C | 手机和电脑的尺寸、推荐和当前缩放、可选的分辨率列表 |

## UDP 分片和 FEC

每帧切成 1180 字节的分片，每 32 个数据分片为一组，按当前丢包率追加 5%~30% 的 Reed-Solomon（GF(256)，Cauchy 矩阵）校验分片。

分片包头 18 字节：帧号、帧大小、组号、组数、分片号、数据分片数、校验分片数、标志。

接收端按顺序交付：
- 分片丢失时，8ms 后发 Nack。
- 超过 120ms 仍不完整的帧会被丢弃，然后请求关键帧。关键帧也丢失时每 500ms 重新请求一次。

## 码率控制

`RateController` 不依赖时钟和 I/O，可以直接单元测试。

**拥塞信号：** 丢帧、单窗口丢包率 ≥10%、排队时延 >25ms，或发送端积压 >40ms。低于 10% 的随机丢包交给 FEC 处理，不降码率。

**降级顺序：** 码率（每次 ×0.8）→ 帧率（减半，最低 30）→ 编码分辨率（85% → 70% → 50%）。

**恢复：** 按相反顺序。码率每秒约上升 5%，并且只在实际用量超过目标一半时才上升，空闲桌面不算证据。

## 显示布局

通过 Windows CCD API（`QueryDisplayConfig` / `SetDisplayConfig`）管理，实现在 `host/src/topology.cpp`。

- **副屏：** 连接时先保存当前布局，再把虚拟屏放到独立的 source 上。Windows 可能把新显示器放进复制组，那时它和主屏共用一个 source，改模式会同时改到主屏，所以 Host 拒绝修改任何含有真实显示器的输出。
- **位置：** 用户在 Windows 显示设置里拖动虚拟屏后，Host 约 200ms 内发现新位置，更新触控映射并记下来。改分辨率、刷新率或转动手机时，它留在原来的一侧，贴着原来相邻的显示器；放不下时才移到所有显示器的右边。下次连接时回到记下的位置。
- **镜像：** 不使用 Windows 的复制模式，直接采集主屏，虚拟屏不上桌面，主屏的任何设置都不变。
- **恢复：** 手机离开 5 秒后恢复保存的布局，短暂掉线不会让窗口跳来跳去。布局同时写入 `%LOCALAPPDATA%\SideDroid\display-layout-to-restore.bin`，程序崩溃或断电后，下次启动时恢复。
- **缩放：** 通过 `DisplayConfigSetDeviceInfo` 的未公开类型 -3 和 -4 设置，和 Windows 显示设置里的缩放是同一项，只作用于虚拟屏。推荐值取满足“逻辑尺寸至少 1280×600”且不超过手机自身密度的最大档位。
- **开机：** 驱动的显示器开机就会出现，由 SideDroid 安装驱动时，Host 启动会把它从桌面取下。

## 控制台管道

`\\.\pipe\SideDroidHost` 每行一个 JSON，权限只开放给当前用户，并拒绝远程连接。最多同时服务 4 个客户端。

| cmd | 参数 |
|---|---|
| `status` | 无 |
| `set` | `width` `height` `hz` `maxKbps` `allowWireless` `mirror` `scale` |
| `pair_start` / `pair_cancel` | 无 |
| `unpair` | `id` |
| `disconnect` | 无 |
| `install_driver` | 无，会触发 UAC |
| `update_install` | 无，下载并校验新版本，然后启动 Windows Installer |

## 更新

仓库根目录的 `update.json`：

```json
{
  "version": "0.0.2",
  "notes": "更新说明，用 \n 换行",
  "msi": "SHA-256",
  "arm64-v8a": "SHA-256",
  "armeabi-v7a": "SHA-256",
  "x86_64": "SHA-256"
}
```

下载地址由客户端拼出，固定为 `https://github.com/Ryorin1337/SideDroid/releases/download/v<版本>/SideDroid-<版本>-x64.msi`（手机端是 `-android-<ABI>.apk`）。版本号必须是一到四段数字。校验和不一致的文件会被丢弃。

**信任范围：** 校验和和文件都来自同一个 GitHub 账号，所以账号被盗时校验和挡不住恶意 MSI。APK 还有一层保护：Android 只接受同一密钥签名的新版本。
