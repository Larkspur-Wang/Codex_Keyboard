# Codex Keyboard 本地 Wi-Fi 计划

## L0 仓库抽取与边界冻结

- [x] 从 `easy-codex-input@52949d3` 建立独立工作目录和公开 GitHub 仓库。
- [x] 删除 PWA、Cloudflare Relay、远程共享协议和相关构建入口。
- [x] 保留 Host、Tauri、固件、BLE/USB 配网和 LAN 语音/播放实现。
- [x] 重写本地 Wi-Fi 真相源、任务卡、目标和脱敏过程记录。
- [x] 更新 lockfile，执行 Host/desktop/firmware 全部快速测试。
- [x] 独立审查后形成无历史的新根提交并推送 `main`。

## L1 当前固件候选 HIL

- [ ] 锁定 ESP-IDF v5.5.5 重新构建并记录 bootloader/partition/app SHA-256。
- [ ] 用户针对精确 SHA 授权后进入 ROM download mode 并烧录。
- [ ] 验证同 Wi-Fi S6 播放保留的槽 2 未读音频。
- [ ] 验证完整播放后 exact heard/cache deletion；中断播放必须保留。
- [ ] 验证播放中按 S2，PTT 立即优先且不会误消费音频。
- [ ] 记录固件日志、Host lease、播放完成和缓存收敛的脱敏证据。

## L2 四槽本地产品收敛

- [ ] 桌面 App 完成四槽绑定、队列、未读、设备和网络诊断 UI。
- [ ] 固件从完整 EIAD 预下载候选升级为有界 jitter/ring 边下边播。
- [ ] EIP request replay floor 做有界生命周期治理和重启策略。
- [ ] S1-S8 各 20 轮 HIL，覆盖断网、Host 重启、键盘重启和旧 generation。
- [ ] 固化安装、升级、配网、回滚和故障排查文档。

## 阶段合同

每阶段必须依次完成：实现 -> 自动化 -> live/HIL（如适用）-> Sol high 独立审查 -> 提交 -> 推送。
原 `easy-codex-input` 不再接收本地 Wi-Fi 产品变更。
