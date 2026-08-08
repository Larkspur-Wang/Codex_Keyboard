# M3 Codex queue/observer 组合证据

## Candidate and boundary

- Source candidate: `41e9bcb5fae8109c28bca0ec034e490db6487a5b`
- Evidence level: static + automated + live Codex task + exact-commit CI + Desktop boundary observation
- Physical keyboard: M3 did not connect, reset, flash or perform keyboard HIL
- Secrets: no real task UUID, title, prompt, transcript, credential, rollout path or absolute user path in this evidence

用户授权后，本段以一个随后归档的专用普通用户 task 完成真实组合运行。所有运行状态位于隔离的 Host
SQLite，不写 Codex 内部数据库；当前安装的 M2 LaunchAgent 未被 M3 候选覆盖。公开证据只记录计数、状态
和边界，不保存 task UUID、标题、prompt、transcript、rollout 内容或本机绝对路径。

## Implemented contract

- observer 只 tail catalog 提供的同 UID regular rollout；打开 parent/file 时拒绝 symlink、相对路径和
  parent traversal。每次先验证首条 `session_meta` 的 task id，再使用 task、规范 path、device/inode、
  offset、generation 与 SHA-256 anchor 组成 cursor。
- 已持久 cursor 的 file identity 改变、文件截短到 cursor offset 之前，或 cursor anchor 不匹配时，会从
  0 重放并提升 generation，completion id ledger 提供幂等。只改到 cursor 之后未提交 range 的同 inode
  rewrite 会丢弃 RAM pending，并从 durable
  cursor 重新扫描，不会因此虚构持久 generation 变化。未完成 turn、半行或 streaming parser 状态只留在
  RAM，不推进持久 cursor。
- 只有 active turn id 匹配的 `event_msg.payload.type=task_complete` 是权威完成事件。CLI exit、启动、
  commentary、agent message、tool output 和错误 turn id 都不能提交 completion。
- 解析与 completion range 二次 SHA-256 复核共享每 poll 4 MiB 预算；多 MiB turn 跨 poll 验证，避免在
  完成点无界重读。ledger 与 next cursor 在同一 SQLite immediate transaction 内提交，并在事务中复核
  精确 slot/task/binding generation。
- observer 使用由 Host owner 创建、仍受 database fd identity guard 的第二 SQLite connection，catalog/
  rollout 读取不会持有 scheduler connection。1 秒阻塞 catalog fixture 期间 health 仍在 750 ms 门限内响应。
- 超过 256 KiB 的 JSONL record 使用固定深度、固定字段和有界 head/tail 的流式 parser；所有字符串包括
  ignored tool output 都严格验证 UTF-8，JSON whitespace、重复键 last-value 与 turn id
  `direct > internal metadata > metadata` 语义和 serde 小行路径一致。
- turn pack 只保留有界 public user/assistant 文本与 tool name/status。reasoning、raw tool input/output、binary、
  完整日志不进入 pack；本机路径、private key、常见 token、quoted JSON/YAML/env 短凭据及 URL userinfo
  在 SQLite 提交前脱敏，同时保留正常公共 URL 与 40 字符 Git commit SHA。

## Live Codex runtime gate

- 程序化 App `create_thread` 产生的 task 在 Codex index 中结构化标记为 subagent，catalog 正确拒绝；它被
  归档，没有用标题或手工放行绕过边界。最终测试使用官方 CLI 创建的普通 `source=exec`、
  `thread_source=user` task。
- gate 先观察该普通用户 task 的直接首轮 completion，再经 `BindingService`、`PromptQueueService`、
  `DurablePromptScheduler` 和真实 `CodexRunner` 执行一条 stdin-only queue prompt。结果是 direct/queued
  completion 各插入 1 次，ledger 总数 2、distinct 数 2；queue job 为 `completed`，task、prompt 与
  slot 1 / generation 1 精确匹配。
- queue 前重启与完成后重启均为 `inserted=0`、`replayed=0`。运行中 catalog transient 为 0，observer
  transient 为 1；有界重试后收敛，没有把 transient 隐藏成零故障。最终 JSON 明确报告
  `private_identifiers_emitted=false`。
- Desktop navigation 可解析并打开这个普通 CLI task，然后可返回源 task；structured hot-read API 没有返回
  task。故本门禁证明 Desktop navigation resolution，不宣称 structured hot refresh 已覆盖。
- 测试 task 随后归档；相同 gate 再运行时在入队或启动 Codex child 之前以 `failure=catalog` 失败。
- gate 私有运行目录使用独占锁、owner marker 原子发布和 UUID run directory；覆盖 `SIGTERM` 清理、
  `SIGKILL` 下次启动 sweep、部分 marker 发布、未知内容 fail closed 和八个创建失败 checkpoint。最终无
  gate 状态或 child 进程残留。

## Automated evidence

最终工作树和 source candidate 通过：

- 142 个 Host unit tests、4 个 Host CLI tests、4 个 macOS parent-death integration tests、6 个 Rust
  protocol tests，以及 4 个 runtime-gate example tests；
- 20 个 observer 聚焦测试覆盖 incomplete/restart、replacement/truncation/replay、同 inode rewrite、错误
  completion、5 MiB tool line、1 MiB user line、超过 4 MiB turn、严格 UTF-8、parser differential、
  credential/path redaction、binding 竞态和每 poll verification read counter；
- 100 jobs FIFO、global concurrency、Keychain/cache/LaunchAgent/health 等既有 Host 回归仍全绿；
- 36 个 TypeScript tests、firmware host ctest、Rust fmt/Clippy、Prettier、typecheck/build、secret scan 与
  source/license audit 全绿。

## Independent review

独立审查始终使用 `gpt-5.6-sol`、reasoning `high`、read-only，未使用 Oracle。多轮失败审查实际推动修复：

- completion 点同步重读整个任意长 turn，绕过 4 MiB/poll 并可能阻塞 shutdown；
- streaming JSON 对非法 UTF-8、VT/FF、重复 payload 和双 metadata turn id 的尺寸依赖语义；
- Markdown/JSON 内嵌绝对路径、quoted short credential 与 HTTP/WS URL userinfo 泄漏；
- 40 字符 Git SHA 被高熵规则误删；
- observer 与 scheduler 共用 connection，以及长 poll 后 binding 已改变仍可提交的竞态。
- runtime gate 的副作用前置条件、baseline ledger、signal/SIGKILL 残留、owner marker 部分发布、quiet poll
  计数和 transient catalog/observer 重试边界；Linux CI 进一步暴露 early-error lock 需要显式 unlock。

最终复审报告 `P0=0`、`P1=0`、`P2=0`、`VERDICT: PASS`。reviewer 未修改文件、未触碰实体键盘。

## Exact-commit CI and next gate

初始 runtime-gate 提交 `861185b230b755a59b180cdd4092384de0b2bff3` 的 CI run
[`30820252172`](https://github.com/Larkspur-Wang/easy-codex-input/actions/runs/30820252172) 在 Linux
`host-and-protocol` 发现 early-error 后下一次 gate lock 未显式释放，其余三个 job 通过；该失败保留为
跨平台运行证据。修复提交 `41e9bcb5fae8109c28bca0ec034e490db6487a5b` 的 GitHub Actions run
[`30821118374`](https://github.com/Larkspur-Wang/easy-codex-input/actions/runs/30821118374) 全绿：
`host-and-protocol`、`ble-peripheral-simulator`、`firmware`、`macos-desktop` 四个 job 均通过。

M3 关闭。它证明真实 Codex allow-list、binding、durable queue、官方 CLI resume、completion 去重和重启
收敛；没有替代 M4 Spark/TTS、M7 PWA、M8 Cloudflare、M6/M8 实体键盘与音频 HIL。
