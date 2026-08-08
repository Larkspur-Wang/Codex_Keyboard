# Codex Keyboard 协作约定

> 产品真相源是 `docs/总体方案.md`，执行顺序以 `flow/plan.md` 为准，最新交接见
> `flow/进展.md` 顶部。

## 产品边界

- 本仓只做同一局域网内的 `Keyboard <-> Mac Host` 产品。
- 不实现手机 PWA、Cloudflare、Relay、远程 WSS、热点代理或公网访问。
- 上排 `S1-S4` 是槽位 1-4 的 PTT；下排 `S5-S8` 是槽位 1-4 的未读总结播放。
- 固件不保存 Codex task UUID；`slot -> task` 的唯一真相源是 Mac Host SQLite。
- Tauri App 是本地 Host 的管理界面，不拥有第二份状态。
- TTS 固定由 Host 调用北京区 `qwen3-tts-instruct-flash-realtime`；ASR 默认
  `qwen3-asr-flash`。

## 不可破坏的语义

- 同一 Codex task 严格 FIFO，prompt 只能经 stdin 传给 `codex exec resume`。
- 只有 Codex rollout 权威 `task_complete` 才能触发总结。
- 未听总结必须滚入下一次总结；新 generation 完整发布后才能替换旧 generation。
- 只有固件实际播放完毕并回传匹配 generation，Host 才能标记 heard 和删除缓存。
- PTT 永远优先于播放；取消、断线、超时和失败不能消费未读总结。
- EIAU/EIP 控制和音频包必须认证；EIPD 音频必须加密，旧 generation/replay fail closed。
- 未经用户针对候选镜像精确 SHA 明确授权，不复位或烧录实体键盘。

## Secret 边界

- DashScope key：`~/Library/Application Support/EasyCodexInput/.env`，mode `0600`。
- cache/device secret：同一 App Support 根下的 mode `0600` 私有文件。
- 不把 key、真实 task UUID、prompt、transcript、录音或家庭 Wi-Fi 凭据写入仓库、日志或证据。

## 来源边界

- 抽取源：`easy-codex-input@52949d33c51bac605a33cb8ff42ee3eeab37021e`。
- 固件基线：`easy-input-keyboard@7a41ce1c5bd961f3be417498cee1a8131ab9d86e`。
- 原仓只读；后续产品开发只在 `Codex_Keyboard` 中进行。

## 工作方式

1. 开工读 `flow/charter.md`、`flow/plan.md`、`flow/进展.md` 顶部和本轮任务卡。
2. 先改计划再实现；跨 Host/固件协议先写共享 golden vector。
3. 软件、Host live service、ESP-IDF build 和真机 HIL 分开报告。
4. 每阶段完成后测试、独立审查、提交并推送，再进入下一阶段。
5. 收工在 `flow/进展.md` 顶部追加交接棒，并在回复中原样贴出。
