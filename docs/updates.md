# 更新机制与发布

电脑端每 12 小时、手机端每次启动时读取仓库根目录的 `update.json`。有新版本时提示，用户确认后下载、校验并调用系统安装程序。

## 更新清单

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

## 发布顺序

1. 改版本号：`CMakeLists.txt`、`android/app/build.gradle.kts`（`versionCode` 也要加一）、`ui/console/build.gradle.kts`、`ui/design/.../About.kt`、`scripts/package.ps1` 的默认值和 README 的“当前状态”。完整位置见 [开发须知](../CLAUDE.md)。
2. 运行 `.\scripts\package.ps1 -Notes "更新说明"`，构建和测试通过后核对 `out\` 里的产物与校验和。
3. 提交、打标签 `v<版本号>`，把 MSI、APK 和 `SHA256SUMS.txt` 发布到 Release。APK 保持同一发布密钥签名。
4. **Release 公开后**，把 `out\update.json` 复制到仓库根目录，提交并推送。客户端读到它就会提示更新，顺序不能反，否则用户会下载失败。

不要用相同版本号重新发布。同版本 MSI 不会升级，Android 更新还受 `versionCode` 限制，客户端也不会提示同版本更新。签名密钥管理和草稿发布的操作约定见 [开发须知](../CLAUDE.md)。
