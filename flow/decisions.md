# 决策日志

## 2026-08-08 · 新仓只保留本地 Wi-Fi 产品

- 决定：`Codex_Keyboard` 只支持键盘与 Mac 同一局域网，不携带 PWA、Cloudflare、Relay、手机播放
  或热点代理实现。
- 原因：先把已经具备的实体键盘、Mac Host、Codex 和音频纵向链变成可重复验收的独立产品，减少
  网络变量和发布面。
- 后果：公开仓以本地 Wi-Fi 当前树建立新根提交；完整多网络路线继续保留在原仓历史中。

## 2026-08-08 · 状态和敏感信息仍由 Mac Host 唯一拥有

- `slot -> task`、FIFO、completion cursor、summary generation、playback lease 和 cache reference
  均以 Host SQLite/文件系统为真相源。
- 固件只持有槽位、连接 generation、短时传输状态和音频 buffer，不保存 task UUID 或总结正文。

## 2026-08-08 · LAN 音频安全合同保持不降级

- EIAU 上行和 EIP 控制使用设备 secret 认证。
- EIPD 下行音频使用按 transfer identity 派生的 AES-256-GCM；控制包使用 HMAC-SHA256。
- 只有匹配 generation 的实际播放完成确认才能消费未读缓存；PTT、失败、取消和超时均保留。
