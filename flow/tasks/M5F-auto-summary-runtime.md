# Task: 常驻完成总结与本地未读音频

- Milestone: `M5F`
- Status: `complete`
- Owner: Codex
- Started: 2026-08-04

## 目标

把 M3 的权威 completion observer 与 M4 的 Spark/TTS orchestrator 接入同一个 Mac Host 生命周期：
completion 落账后自动生成累计未读总结，并将 WAV/EIAD 作为认证加密 cache 发布，为键盘 S5-S8 与手机
播放提供唯一来源。

## 合同

- 总结 worker 复用 Host 的 SQLite 实例所有权，但使用独立连接；不能启动第二个 Host 或绕过进程锁。
- 只处理 `interrupted` claim 或尚未覆盖的 completion；每个 task 单飞，旧未读必须进入下一代总结。
- `ambiguous` TTS 永不自动重试，必须经显式人工 reconcile；日志不出现 task UUID、正文或音频。
- Spark/TTS 慢调用不得阻塞健康检查、语音入队或 Codex FIFO。
- cache 继续位于 App Support 私有目录并加密；DashScope key 只读私有 `.env`。

## 验收

- 自动化覆盖共享实例锁、task 去重、重启恢复、失败退避与 ambiguous 不重试。
- 真实 completion 触发 Spark 与北京区 TTS，SQLite 发布 `unheard`，cache 三对象认证读回。
- Sol high 独立审查无 P0/P1；本机完整测试通过后独立提交并推送。

## 当前证据

- S1 真实语音已形成 Codex completion。
- 首次真实总结已完成 Spark，但 TTS 在 commit 后进入 `ambiguous`；未自动重试、未发布不完整 cache，符合
  失败安全合同。下一步需用户批准一次人工 abandon/retry，再完成真实 `unheard` 发布门禁。

## 关闭结果

- 真机 completion 已触发常驻 worker；S2 generation 1 成功发布为 `unheard`，加密 manifest/WAV/EIAD
  三对象已落盘。S1/S3/S4 的不确定 TTS 均冻结为 ambiguous，没有自动重试或半发布。
- 完整 `eval-fast`：Host 226、CLI 6、parent-death 6、protocol 6、runtime gate 4、TypeScript 36、Firmware
  Host 55/55、secret/source audit 全绿。
- Sol high 最终 `P0=0/P1=0`、`VERDICT: PASS`；发现的 32-task 饥饿 P1 已用 cursor 分页和 35-task
  回归关闭。剩余 P2 是 shutdown detach、人工 reconcile 需离线和 worker 组合层专项测试，留到播放批次加固。
