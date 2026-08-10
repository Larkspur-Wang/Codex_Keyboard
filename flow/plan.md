# Codex Keyboard 本地 Wi-Fi 计划

## L0 仓库抽取与边界冻结

- [x] 从 `easy-codex-input@52949d3` 建立独立工作目录和公开 GitHub 仓库。
- [x] 删除 PWA、Cloudflare Relay、远程共享协议和相关构建入口。
- [x] 保留 Host、Tauri、固件、BLE/USB 配网和 LAN 语音/播放实现。
- [x] 重写本地 Wi-Fi 真相源、任务卡、目标和脱敏过程记录。
- [x] 更新 lockfile，执行 Host/desktop/firmware 全部快速测试。
- [x] 独立审查后形成无历史的新根提交并推送 `main`。

## L1 当前固件候选 HIL

- [x] 锁定 ESP-IDF v5.5.5 重新构建并记录 bootloader/partition/app SHA-256。
- [x] 用户针对精确 SHA 授权后进入 ROM download mode 并烧录。
- [x] Desktop 增加四槽任务列表、当前绑定和绑定操作；写入必须经常驻 Host IPC，不能直接争用 SQLite。
- [x] Desktop 明示 DashScope 北京区 ASR/TTS 就绪状态，ASR 固定为 `qwen3-asr-flash`。
- [x] 验证同 Wi-Fi S6 播放保留的槽 2 未读音频。
- [x] 验证完整播放后 exact heard/cache deletion；中断播放必须保留。
- [ ] 验证播放中按 S2，PTT 立即优先且不会误消费音频。
- [x] 记录固件日志、Host lease、播放完成和缓存收敛的脱敏证据。

## L2 四槽本地产品收敛

### L2A 信箱、摘要质量与桌面安装

- [x] Host/固件增加认证的信箱状态同步；左起四灯一一对应槽 1-4，各槽未听 completion coverage 越多越亮，第五灯不参与，短时输入反馈仍可覆盖后恢复。
- [x] Spark 输出质量门禁要求具体完成事项、结果、待办和决策；上一版未听摘要必须作为下一代输入并保留仍相关内容。
- [x] Spark 的新内容输入只允许每次已完成任务的助手最终回复；用户消息、工具调用、过程消息和测试日志不得进入摘要 prompt。
- [x] 将 TTS/缓存/设备播放的单条摘要上限从 90 秒提升到当前 4 MB PSRAM 预下载链可安全支持的 150 秒，提示词优先完整意义而非过度压缩。
- [x] 将 Host/固件 UDP 窗口共同限制为 production lwIP mailbox 的 6 包，并记录传输完成耗时；不再用超出接收队列的 burst 换取表面吞吐。
- [x] 用 ImageGen 生成独立 macOS App 图标，产出完整 Tauri icon set 并接入 bundle。
- [x] 构建并安装 `Codex Keyboard.app` 到 `/Applications`，升级常驻 Host，验证 App 可启动且四槽状态可读。
- [x] 自动化、ESP-IDF build、真机信箱灯亮度/熄灭 HIL 完成后提交并推送。
- [x] 优化 Spark 播报文本：最新结果放在最前，压缩仍相关的旧未听内容，只在确有行动时以下一步收尾；只改 prompt，不新增正文硬编码。
- [x] 首版 Qwen TTS 听感优化：Cherry 通用女声采用自然同事式、中等偏快、短停顿，减少播音腔/客服腔；后续模型替换见下项。
- [x] 将总结模型切换为 `gpt-5.6-luna` + `high`，并按实际信息量自适应摘要长度，短结果不再为了字数扩写。
- [x] 将总结播报切换为北京区 `qwen-audio-3.0-tts-flash` + `longanfengyue`，完整 `spoken_text` 只提交一次并缓存一段完整 WAV，不再分句生成后拼接。
- [x] 完成 Host 自动化、一次真实 48 kHz TTS 冒烟和 live Host 升级，再提交推送本阶段。
- [x] 将最右第 5 颗灯改为整机任务活动灯：0 个已绑定任务运行时绿色常亮，1/2/3/4 个运行任务分别显示黄/橙/紫/红；全部完成后回到绿色。左起四颗继续只表示四槽未听信箱，短时输入反馈仍可覆盖后恢复。
- [x] 扩展 Host/固件认证信箱状态协议，运行数只来自绑定任务 rollout 的权威 `task_started -> task_complete` 区间；补 Host、wire golden、LED 映射和 ESP-IDF build 验证。
- [x] 同步更新《Codex 任务电台》课程 PPT 中的交互草图、灯光语义、架构和完成时序，并重新导出、逐页渲染检查 PPTX/PDF。
- [ ] 获候选镜像精确 SHA 授权后烧录并升级 Host，真机依次验证绿/黄/橙/紫/红、全部完成回绿，以及前四颗信箱灯不受影响。

### L2B 长稳与流式播放

- [ ] 桌面 App 完成四槽绑定、队列、未读、设备和网络诊断 UI。
- [ ] 固件先在已认证的顺序 EIAD 传输上加入 24 KiB 预缓冲后边收边播，把按播放键到首声从整段传输时间中解耦；随后再把完整 PSRAM backing 收敛为有界 jitter/ring。
- [ ] 流式播放必须覆盖慢网、断包重传、播放抢占、Host 中断和传输未完成时 speaker 失败，只有完整播放后才允许 heard/cache deletion。
- [ ] EIP request replay floor 做有界生命周期治理和重启策略。
- [ ] S1-S8 各 20 轮 HIL，覆盖断网、Host 重启、键盘重启和旧 generation。
- [ ] 固化安装、升级、配网、回滚和故障排查文档。

## 阶段合同

每阶段必须依次完成：实现 -> 自动化 -> live/HIL（如适用）-> 提交 -> 推送。当前按用户要求不启动子智能体或额外独立审查。
原 `easy-codex-input` 不再接收本地 Wi-Fi 产品变更。

## L3 课程案例交付

- [x] 读取 EasyInput V2 飞书硬件资料与本仓产品、架构、开发过程文档。
- [x] 完成《Codex 任务电台：一把键盘，远程开发》36 页课程 PPT，含 30 页主讲与 6 页技术附录。
- [x] 使用真实键盘图片、脱敏 App 截图和 4 张概念配图；不把生成图冒充真实产品。
- [x] 为 36 页写入可直接讲的中文演讲备注，明确同局域网边界和真机验收口径。
- [x] 通过 SVG 质量检查、36 页 PPTX 导出、PowerPoint 结构检查和浏览器逐页渲染检查。
