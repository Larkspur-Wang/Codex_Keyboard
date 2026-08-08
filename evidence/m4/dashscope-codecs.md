# M4C DashScope realtime、WAV/EIAD 与 cache publication 证据

## Candidate boundary

- Status: M4C source and deterministic Linux timing fix committed; exact-commit CI pass
- Model alias: `qwen3-tts-instruct-flash-realtime`
- Official equivalent snapshot: `qwen3-tts-instruct-flash-realtime-2026-01-22`
- Region/endpoint: 华北 2（北京），`dashscope.aliyuncs.com/api-ws/v1/realtime`
- Evidence level: official-protocol review + static + automated local TLS peer + encrypted-cache integration
- Secrets: 不记录 Authorization、API key、summary text、PCM/WAV/EIAD、task UUID 或用户绝对路径

官方协议依据：

- https://help.aliyun.com/zh/model-studio/qwen-tts-realtime-client-events
- https://help.aliyun.com/zh/model-studio/qwen-tts-realtime-server-events
- https://help.aliyun.com/zh/model-studio/qwen3-tts-instruct-flash-realtime
- https://help.aliyun.com/zh/model-studio/qwen-tts-voice-list

## Implemented contract

- key 只从安装 marker 匹配的 `SecretStore`/macOS Keychain 临时读取；握手请求与序列化 client event 使用
  zeroizing buffer，不把 key、文本、instructions 或原始响应交给日志。
- WebSocket 固定北京 endpoint/model，验证 TLS/HTTP Upgrade、`session.created -> session.updated ->
  input_text_buffer.committed -> response.created -> output_item.added -> content_part.added ->
  response.audio.delta+ -> response.audio.done -> content_part.done -> output_item.done -> response.done ->
  session.finished -> peer close`，并绑定唯一 event/response/item id、index、voice、PCM 与 48 kHz。每个
  scaffold 恰好一次；重复、缺失、乱序、错误 identity、非法 base64、奇数字节 PCM 和提前 close 全部 fail closed。
- 401/403 终止；HTTP 429/5xx、DNS/connect/TLS 和提交文本前的断线最多三次有界退避。文本 commit 后任何
  模糊断线都不自动重试，避免重复合成和重复计费。
- PCM 最长 90 秒；Host 生成 canonical PCM16 mono WAV 和 `docs/EIAD-v1.md` 定义的 20 ms 帧独立
  IMA-ADPCM。尾帧保留精确 sample count，不补假 sample。
- manifest（含累计 summary）、WAV 和 EIAD 作为三个独立 AES-GCM 对象原子发布；同 generation 重试只接受
  完全相同明文并读回认证，不同明文返回冲突，不能替换旧缓存。

## Current checks

- Host unit：186 pass（其中 audio 8、DashScope 26、TTS cache 2）；CLI 4、macOS parent-death 6 pass。
- `cargo clippy -p easy-codex-host --all-targets -- -D warnings`：pass。
- 完整 `./scripts/eval-fast.sh`：最终候选独占运行 pass，含 Rust、TypeScript、firmware host test、格式、
  secret scan 和 source/license audit。
- Independent `gpt-5.6-sol` high read-only review：`P0=0/P1=0/P2=0`、`VERDICT: PASS`；未使用 Oracle。
- 源码候选 `005cd70e64fbfb1d6592d016fd377cdb3c54a63a` 的 exact CI `30838401361` 在 Linux 暴露
  150 ms stall fixture 尚未发完 delta 时客户端已到 deadline，测试 peer 因 BrokenPipe panic；产品断言本身
  已返回预期 `AmbiguousAfterCommit`。修复后的 mock 在 delta 后必须收到客户端 Pong，证明客户端已消费音频
  才进入三秒 stall，并为 Linux 调度保留独立 deadline 余量。修复提交
  `1df414630ae9f8dfc4d49b72e22ca2b0d3f4efcc` 的 exact CI `30839110987` 四个 job 全绿。
- 真实 DashScope TTS、20 次 API、音色/instructions 试听：M4D，当前未运行，不能用本地 TLS peer 替代。
- 实体键盘/PWA/Cloudflare/扬声器 HIL：M5-M8，当前未运行。
