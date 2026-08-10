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

## 2026-08-10 · 旋钮只控制键盘扬声器

- 旋钮旋转固定控制板载扬声器十档 PCM 增益，不发送 USB/BLE Consumer Control，也不改变 Mac 音量。
- 物理逆时针减小、顺时针增大；默认 70%，停止旋转 1 秒后保存到 NVS，避免每个脉冲都写 Flash。
- 短按播放固件内预生成的当前音量提示，运行时不依赖 Host 或网络；长按 3 秒继续进入配置模式。
- 最低档为 10% 而不是静音，避免用户完整播放总结却因无声而被误认为已经听过。

## 2026-08-10 · 四个槽位允许四路 Codex 并发

- Host prompt scheduler 的全局运行上限从 2 提升为 4，每个 task 仍严格 FIFO，同一 task 不重叠。
- 原因：产品有四个独立物理槽，第五灯又明确用黄/橙/紫/红表示 1/2/3/4 个运行任务；上限为 2 会让
  紫色和红色只能靠用户在 Codex App 手工启动任务触发，键盘自身无法完整表达四槽产品能力。
- 后果：Mac 最多同时运行四个 Codex 子进程；每 task 的待处理上限 12、失败恢复和完成观察合同不变。

## 2026-08-10 · 总结质量交给 Luna，Host 只守运行底线

- Luna High 只读取每次任务的权威助手最终回复，并在存在旧未听摘要时把它一并纳入新一代总结。
- Host 不再用事实字数、中文评分、证据摘录、旧数组逐项复述或新旧文本差异判断内容好坏，避免可用
  总结被永久挡在 TTS 之前。
- Host 仍严格校验 JSON/字段尺寸、completion 归属、脱敏、缓存事务和有效音频；这些是运行边界，
  不是替模型打内容分。
