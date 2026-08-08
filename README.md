# Codex Keyboard

Codex Keyboard 是 EasyInput V2 的纯局域网版本。键盘和 Mac 必须连接同一个 Wi-Fi，不使用手机
PWA、Cloudflare Relay、公网端口或远程桥。

- `S1-S4`：按住说话，松开后由 Mac Host 调用 Qwen ASR，并把文本按任务 FIFO 投递到 Codex。
- `S5-S8`：播放对应任务最新的未读总结。
- Mac Host：持有四槽绑定、Codex 队列、完成观察、Spark 总结、Qwen TTS 和加密音频缓存。
- Firmware：只知道槽位号，负责 Wi-Fi 音频、麦克风、扬声器、按键、PSRAM 和 I2S。

## 目录

- `app/host/`：Rust LaunchAgent 常驻服务。
- `app/desktop/`：Tauri 2 macOS 管理 App。
- `firmware/`：ESP32-S3 / EasyInput V2 固件。
- `docs/总体方案.md`：本地 Wi-Fi 产品边界和数据所有权。
- `docs/端到端架构.md`：架构图、语音输入、总结和播放时序。
- `docs/协议草案.md`：当前 LAN 音频协议合同。
- `flow/`：计划、决策、任务卡和进展记录。

## 来源

本仓从 `Larkspur-Wang/easy-codex-input@52949d33c51bac605a33cb8ff42ee3eeab37021e`
抽取。新仓只保留本地 Wi-Fi 所需的电脑 App、Host、固件和测试；原仓继续独立存在且未被修改。

## 当前状态

本地 Host、语音上行、自动总结、加密音频缓存和 LAN 播放候选已具备自动化证据。实体键盘的
新播放镜像仍需要针对精确 SHA 单独授权烧录并完成 HIL，构建通过不能代替真实扬声器验收。

```bash
./scripts/eval-fast.sh
```

密钥只保存在 `~/Library/Application Support/EasyCodexInput/` 的私有文件中，不进入仓库。
