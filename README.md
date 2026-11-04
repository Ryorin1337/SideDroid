# SideDroid

把 Android 设备变成 Windows 的扩展副屏：支持 USB 和 Wi-Fi、自适应和手动分辨率、可调刷新率、多点触控和自适应码率。两端界面都使用 Material 3 Expressive。

- **Windows：** Windows 10 或更高版本（x64），需要硬件视频编码器（NVIDIA、Intel 或 AMD）
- **Android：** Android 8 或更高版本。Android 11 起有官方低延迟解码接口，更老的设备延迟可能更高

验收步骤见 [docs/ACCEPTANCE.md](docs/ACCEPTANCE.md)，当前状态和已知限制见 [docs/STATUS.md](docs/STATUS.md)。

## 使用

1. 运行 `SideDroid-<版本>.msi`，首次打开控制台时按提示安装虚拟显示驱动。
2. 在手机上安装 `SideDroid-<版本>.apk`。
3. 连接：
   - **USB：** 打开手机的 USB 调试并连接电脑，在手机应用里点“USB 连接”。
   - **Wi-Fi：** 在控制台点“添加设备”，在手机上选择这台电脑并输入配对码。之后再连接不需要配对码。

## 目录

| 路径 | 内容 |
|---|---|
| `core/` | C++20 协议核心，Windows 和 Android 共用：封包、加密、FEC、组帧、码率控制 |
| `host/` | Windows 后台程序：采集、编码、触控注入、虚拟屏控制、adb、mDNS、控制管道 |
| `ui/design/` | Windows 和 Android 共用的 MD3E 主题、组件和手机界面 |
| `ui/console/` | Compose Desktop 控制台 |
| `android/` | Android 应用和 NDK 媒体层 |
| `installer/` | WiX MSI 定义 |
| `third_party/vdd/` | 固定版本、未修改的 Virtual Display Driver |
| `scripts/` | 打包和测试脚本 |
| `docs/` | 协议、技术验证结论、验收清单 |

## 构建

需要 VS 2026（C++ 和 Windows SDK）、JDK 17，以及装有 Android SDK 和 NDK 的 WSL（默认路径 `/root/android-sdk`）。

```powershell
.\scripts\package.ps1          # 全部产物输出到 out\
.\scripts\verify_artifacts.ps1 # 检查产物，不安装任何东西
```

## 测试

```powershell
ctest --test-dir build -C Release   # 协议和 Host 逻辑
.\scripts\stream_tests.ps1          # 端到端串流、丢包和限速，不需要手机
.\scripts\console_self_check.ps1    # 打包后的控制台能否启动并连上 Host
cd ui\console; .\gradlew.bat test renderScreens   # 控制台测试，把所有界面渲染成 PNG
```

Android 应用在 WSL 中构建：`bash scripts/build_apk.sh`。

## 许可

第三方组件见 [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md)。
