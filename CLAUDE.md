# SideDroid 开发须知

把 Android 手机或平板变成 Windows 第二块屏幕的开源工具。功能和用法见 [README.md](README.md)，协议、显示布局和更新机制见 [docs/protocol.md](docs/protocol.md)。

## 环境

- **Windows 端：** Visual Studio 2026（C++ 和 Windows SDK）、JDK 17，用 PowerShell 构建。
- **Android：** 只在 WSL 里构建，SDK 和 NDK 默认在 `/root/android-sdk`。Windows 上没有 Android SDK。
- **完整构建：** `.\scripts\package.ps1 -Notes "更新说明"`，产物输出到 `out\`。它会跑完所有测试，也会检查各处版本号是否一致。

## 约定

- 界面文字用中文，代码、注释和提交信息用英文。
- 提交信息和 PR 描述**不要写 `Claude-Session` 或任何会话链接**，这是公开仓库。`Co-Authored-By` 可以保留。
- 改动协议、显示布局或更新机制时，同步更新 `docs/protocol.md`。用户能感知的变化同步更新 `README.md`。
- 不支持 32 位 Windows：虚拟显示驱动和 Compose 控制台都没有 32 位版本。

## 版本号

发版时这些地方要一起改，`package.ps1` 的检查不过就会停下：

| 文件 | 内容 |
|---|---|
| `CMakeLists.txt` | `project(... VERSION x.y.z)`，Host 的版本由它决定 |
| `android/app/build.gradle.kts` | `versionName`，以及 `versionCode`（每次发版加一，只能增加） |
| `ui/console/build.gradle.kts` | `version` 和 `packageVersion` |
| `ui/design/src/com/sidedroid/design/About.kt` | `AppInfo.VERSION`，手机端更新检查和它比较 |
| `scripts/package.ps1` | `$Version` 默认值 |
| `README.md` | “当前状态”里的版本号 |

**不要用相同版本号重新发布：** 同版本的 MSI 不会升级，Android 也不接受相同或更低的 `versionCode`，已安装的客户端也不会提示更新。

## 发布流程

1. 改版本号，运行 `package.ps1`。
2. 核对产物：`sha256sum -c SHA256SUMS.txt`，APK 用 `apksigner verify --print-certs` 看证书。
3. 提交并推送，用 `gh release create --draft --target <提交>` 建草稿。
4. 上传 MSI、4 个 APK 和 `SHA256SUMS.txt`，失败就重试。MSI 约 58 MB，上传可能要 20 多分钟。
5. 全部传完后再公开，下载回来核对校验和。
6. **Release 公开之后**，才把 `out\update.json` 复制到仓库根目录，提交并推送。

第 6 步一定要最后做：客户端读到 `update.json` 就会提示更新，如果 Release 还没公开，用户会下载失败。

## 签名密钥

- 密钥是 `android/sidedroid-release.jks`，配置和密码在 `android/keystore.properties`，两者都被 git 忽略，绝不能提交，也不要输出到日志。
- `git clean -fdx` 或删除项目文件夹都会丢失它们，要先另外备份。
- 丢失或更换密钥后，所有用户都得卸载重装。
- 缺少密钥时构建会直接报错，不要退回调试签名。

## 显示和触控的坑

- 虚拟屏只按硬件 ID `MTT1337` 识别。电脑上可能有 VR 串流或 ToDesk 的虚拟显示器，不能只匹配 “virtual”，也绝不能碰它们。
- `InjectSyntheticPointerInput` 的坐标相对**虚拟屏幕左上角**，不是主屏。不减去这个原点，显示器在主屏左边或上方时，触摸会被夹到主屏边缘。
- 改模式时必须保留虚拟屏位置（`placeDisplay`），主屏决定桌面原点，不能移动。
- 一个进程不能对同一输出调用两次 `DuplicateOutput`（`0x80070057`）。多台设备镜像主屏要走 `CaptureHub`。
- 改动显示布局的代码都要持有 `Host::lockDisplays()`：`SetDisplayConfig` 提交的是整个布局，多个会话会互相覆盖。
- 虚拟屏的 GDI 名称在驱动重载后可能变化，要按显示器设备路径识别。
- **虚拟屏只在连接时存在：** 平时驱动设备是禁用的，`Host::prepareDriver` 通过 `sidedroid-service` 启用。判断驱动是否安装用 `vddDeviceExists`，不能看显示器是否存在。没有安装服务时，这些功能都不可用。
- **不要用驱动的 `RELOAD_DRIVER` 和 `SETDISPLAYCOUNT`：** 每次都会让驱动崩溃（`mttvdd.dll` 访问冲突），Windows 倒数重启次数，用完进入代码 43。改配置后用服务重启设备，不会崩溃。
- **重启驱动前要先停掉虚拟屏上的采集（`Host::stopVirtualCaptures`）：** 否则 Windows 会把重启推迟到下次开机，只在安装参数里标记 `DI_NEEDRESTART`，调用本身仍返回成功。表现是时有时无，第二台设备一直等不到显示器。
- **驱动重启后 Windows 还会调整一轮布局，可能把刚放好的虚拟屏挤掉：** 这时另一台设备可能分到同一个 source，第一台就会继续采集别人的画面。所以 `prepareOutput` 结束前和串流中都要核对 `vddMonitorSource(monitor_)`。“设置缩放失败”常常就是这个信号。
- 驱动重启后，Windows 可能把虚拟屏接回桌面，也可能把它们克隆到主屏，所以之后要核对桌面。克隆时虚拟屏的路径会被报告在主屏的适配器上，只有主屏的 source 可选，要先解除克隆才能分配独立的 source。
- **分辨率不能无限增加：** `vdd_settings.xml` 超过 14 种分辨率时（4 个显示器、6 个刷新率下实测），驱动没有任何显示器，但设备状态仍是正常。`addModesToVddXml` 会按上限淘汰旧分辨率。

## 编码器

- 选择顺序、每种编码器的依据和实测情况见 [docs/encoders.md](docs/encoders.md)。
- **AMF 只在一位用户的 Radeon Vega 8 上跑通过**（`host.log` 的 `pipeline:` 行写的是 `AMF`）。丢包、限速和 H.264 还没有在 AMD 上测过。
- 改动 `encoder_*.cpp` 后，要测丢包和限速下的码率、强制关键帧，以及每个关键帧是否带参数集，不能只看有没有画面。

## 多设备

- **分配带宽不需要额外的仲裁器：** 用共享限速实测，各设备自己的码率控制就能分得公平（4 台公平性约 0.998），后来的设备也能拿到份额。不要在没有测到问题时加。
- **显示锁不能挡住连接：** `Host::detachIfIdle` 在 accept 循环里，只能 `try_lock`。否则一台设备做显示操作时，其他设备连不进来，同时到达的设备也无法一次算进显示器数量。
- 禁用驱动时偶尔有一次崩溃（`WUDFVerifierFailure`，在 `mttvdd.dll` 所属的框架里），Windows 会自动恢复，下次启用正常。原因在上游驱动，还没有深究。

## 测试

- 单元测试：`ctest --test-dir build -C Release`，控制台用 `ui/console` 的 `gradlew test`。
- 界面截图：`ui/console` 的 `gradlew renderScreens`，图片输出到 `build/screens`。
- 没有手机时用 `sd-loopback` 当客户端，`--commands` 可以从标准输入发送 `tap`、`size` 和 `quit`。模拟多台设备时用 `--client-id` 和 `--name` 区分，不带 `--client-id` 的客户端不保存设置。
- 不想动显示布局时，用 `--source \\.\DISPLAY1` 读主屏，可以测试多连接、上限和接管。
- 会真实改动显示布局或重载驱动的测试，运行前先告诉用户。
- 测试 Host 要用非默认端口，如 `--headless --port 27999 --no-adb --no-discovery`，日志写到 `host-test.log`，不影响已安装的 Host。
    - `--test-virtual-display` 会真实改动显示布局，并写入真实的 `settings.ini`，要先备份。它需要已安装的 `SideDroidService`，否则驱动无法启用。
    - `--update-test-base <URL>` 可以把更新器指向本地服务器，只校验不安装。
- 注入触摸前，先用自己的窗口盖住落点，否则可能点到真实桌面。
- 改动 APK 或安装流程后，要在真机上验证，loopback 覆盖不到手机端。

## 工具上的坑

- **Gradle：** 不要用 `Start-Process -Wait` 调用 Gradle。它会等 Gradle 守护进程，构建完了也不返回。用 `&` 调用。
- **WSL：** 在 Git Bash 里 `wsl.exe --cd /mnt/c/...` 的路径会被改写而失败，改用 PowerShell，或者 `wsl.exe -e bash -lc 'cd ...'`。
- **管理员权限：** 这台电脑关了 UAC 弹窗，写成脚本后用 `Start-Process -Verb RunAs` 运行即可，输出写到日志文件。
- **PowerShell 5.1：** 原生程序写 stderr 时，`2>&1` 配合 `$ErrorActionPreference='Stop'` 会中断脚本，keytool 就会这样。
- **cmake 的 MSBuild 参数：** `/m:4` 在 Git Bash 里会被改写成路径，在 PowerShell 里写 `-m:4` 又会被解析错，都会让编译失败。要在 PowerShell 里写 `'--' '/m:4' '/v:m'`。过滤输出时注意看有没有 `MSB` 错误，否则测试跑的是旧程序。
- **Bash 工具：** 内嵌在命令里的 heredoc 可能吞掉反斜杠或解析失败。含反斜杠或大量引号的脚本先用 Write 写成文件再运行。
- **历史记录：** 重写 `main` 的历史或强制推送前要先征得同意。
