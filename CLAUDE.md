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

## 测试

- 单元测试：`ctest --test-dir build -C Release`，控制台用 `ui/console` 的 `gradlew test`。
- 界面截图：`ui/console` 的 `gradlew renderScreens`，图片输出到 `build/screens`。
- 没有手机时用 `sd-loopback` 当客户端，`--commands` 可以从标准输入发送 `tap`、`size` 和 `quit`。
- 测试 Host 要用非默认端口，如 `--headless --port 27999 --no-adb --no-discovery`，日志写到 `host-test.log`，不影响已安装的 Host。
    - `--test-virtual-display` 会真实改动显示布局，并写入真实的 `settings.ini`，要先备份。
    - `--update-test-base <URL>` 可以把更新器指向本地服务器，只校验不安装。
- 注入触摸前，先用自己的窗口盖住落点，否则可能点到真实桌面。
- 改动 APK 或安装流程后，要在真机上验证，loopback 覆盖不到手机端。

## 工具上的坑

- **Gradle：** 不要用 `Start-Process -Wait` 调用 Gradle。它会等 Gradle 守护进程，构建完了也不返回。用 `&` 调用。
- **WSL：** 在 Git Bash 里 `wsl.exe --cd /mnt/c/...` 的路径会被改写而失败，改用 PowerShell，或者 `wsl.exe -e bash -lc 'cd ...'`。
- **PowerShell 5.1：** 原生程序写 stderr 时，`2>&1` 配合 `$ErrorActionPreference='Stop'` 会中断脚本，keytool 就会这样。
- **历史记录：** 重写 `main` 的历史或强制推送前要先征得同意。
