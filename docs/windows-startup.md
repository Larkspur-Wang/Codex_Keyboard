# Windows 启动与连接诊断

在仓库根目录双击 `Start Codex Keyboard.cmd`。该入口先检查常驻 Host，离线时安装并启动，再打开管理窗口。失败会保留错误提示。关闭管理窗口不会停止 Host。

开发时构建：

```powershell
cargo build --release -p easy-codex-host --bin easy-codex-host
pnpm --dir app/desktop run bundle:win
```

构建前关闭本产品的桌面窗口。Windows 锁定正在运行的 exe，Host 升级前应等待任务完成后停止旧实例。
`bundle:win` 同步生成 `target\release\easy-codex-desktop.exe` 和 `target\release\Codex Keyboard.exe`；不要复制旧的同名 exe。GNU 构建需要同目录的 `WebView2Loader.dll`。构建脚本将所用 MinGW 的 bin 目录置于 PATH 最前，避免其他发行版的 DLL 让 gcc/cc1 静默失败。

修复版将 WebView2 数据目录固定为 `%APPDATA%\EasyCodexInput\desktop-webview-v2`，使用现有可写的应用支持目录；此前的 LocalAppData 目录在 Windows 受限令牌下只有读取权限。旧目录保留，Host SQLite、绑定、未读缓存和设备密钥仍由原 Host 目录管理。若旧版 exe 闪退或驻留却没有窗口，请重新执行正式构建。

产品 exe 必须保持 Windows 正常的 Medium 完整性。此前把应用目录递归设为 Low，会连带把 Host/GUI 及其子进程降为 Low，导致 WebView 和 DeepSeek 无法写入正常用户目录。打包、双击入口和 Host 安装现在会校正 exe 标签；不要通过把 `.dsh`、用户工作目录或整个 AppData 降为 Low 来解决。正常用户桌面运行即可，无需以管理员运行。

管理界面的“北京区已就绪”仅表示本机已配置 DashScope 凭据。“键盘语音”独立显示 UDP 等待、收到包但无认证心跳、已认证或心跳中断。

只读现场检查：

```powershell
.\target\release\easy-codex-host.exe health
cargo build -p easy-codex-host --example connection_probe
.\target\debug\examples\connection_probe.exe
```

需要单独验证北京区凭据网络握手时，运行 `connection_probe.exe --verify-cloud`。该选项会向 DashScope 发起认证连接，但不上传录音或生成音频；默认诊断只访问本机。

诊断不输出任务标识、口述、录音或密钥。无数据包时检查键盘电源、相同 Wi-Fi、电脑当前 IPv4 和防火墙；有数据包但无认证心跳时检查设备配置与 secret 是否匹配。不要通过清空 SQLite、换 secret 或重烧固件排障。

C4 的按键、灯光、扬声器、PTT 抢占、FIFO 和旋钮断电记忆必须逐项实测。Host 就绪与软件测试不能替代实机结果。
