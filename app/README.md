# App

本地 Wi-Fi 版本只有两个应用单元：

- `host/`：Rust 常驻核心，负责绑定、ASR、durable FIFO、Codex rollout observer、Spark 总结、
  Qwen TTS、加密缓存、USB/BLE 配网和局域网键盘音频。
- `desktop/`：Tauri 2 macOS 管理界面，负责安装、四槽绑定、运行状态和设备诊断。

Host 作为用户级 LaunchAgent 独立运行，关闭桌面窗口不应停止队列、完成观察或总结生成。App 不含
PWA、Cloudflare Worker、Durable Object 或远程中继代码。
