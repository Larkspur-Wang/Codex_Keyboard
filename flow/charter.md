# 项目宪章

- **项目名**：Codex Keyboard
- **目标**：让 EasyInput V2 在与 Mac 同一 Wi-Fi 时，通过八个物理键向四个 Codex 任务说入
  要求并收听未读完成总结。
- **目标用户**：在一台始终在线的 Mac 上并行使用多个 Codex 任务的个人用户。

## 当前范围

- macOS Rust Host + Tauri 管理 App。
- EasyInput V2 ESP-IDF 固件。
- USB/BLE 下发 Wi-Fi 与 Host 端点配置。
- 同局域网 Qwen ASR、Codex FIFO、rollout observer、Spark 累计总结、Qwen TTS、加密缓存和
  键盘扬声器播放。

## 明确不做

- 手机 PWA、手机播放、Cloudflare、Durable Object、Relay、远程 WSS 或热点代理。
- 把 Codex auth、task UUID、prompt、transcript 或总结正文写入固件。
- Mac 关机或 Host 离线时继续处理任务。
- 用构建、模拟器或 UDP 收包替代真实扬声器 HIL。

## 成功标准

1. Mac App 能把四个槽位绑定到 allow-list 中的四个 Codex task。
2. S1-S4 各完成 20 次 PTT -> Qwen ASR -> durable FIFO -> 对应 Codex task，不串槽。
3. 权威 completion 只触发一次总结，未听内容正确累计。
4. S5-S8 各完成 20 次对应播放；仅真实播放完成才删除 exact generation。
5. PTT 抢占播放不误标 heard，断线/重启/重放/旧 ACK 均 fail closed。
6. 本地 `eval-fast`、锁定 ESP-IDF build、Host live-service 和实体键盘 HIL 分别留证。
7. 仓库和历史不含 key、真实 task UUID、prompt、transcript、录音或 Wi-Fi 密码。
