# M4A cumulative summary ledger 证据

## Candidate and boundary

- Source candidate: `9612d81fa19c7c3a01d3dd933f0c066b39002602`
- Evidence level: static + automated + crash injection + exact-commit CI
- Independent review: `gpt-5.6-sol` high, read-only, no Oracle
- Secrets: evidence 不含真实 task UUID、prompt、turn pack、summary、音频、credential、rollout path 或用户绝对路径

M4A 只建立 summary 的事务真相源，不调用 Spark/DashScope，不写真实 cache 对象，也不执行键盘、PWA、
Cloudflare 或扬声器 HIL。已安装的 M2 Host 未被本候选替换。

## Implemented contract

- `SummaryDocument` 使用 deny-unknown-fields 的 v1 schema；`facts`、`pending`、`decisions`、
  `spoken_text` 和 `covers_new_completions` 都有固定条目/字节上限、严格 UUID coverage 与 canonical JSON。
- `covers_new_completions` 只表示本 generation 新领取的有序 delta，最多 32 条；累计 coverage 的唯一
  Source of Truth 是 SQLite `completion_ledger.summarized_generation`，因此多代累计可超过 32。
- claim 在 immediate transaction 内冻结 previous current、下一 generation、本批 completion id 与原始
  redacted turn pack，总输入不超过 1 MiB；单 task 只有一个 generating/interrupted claim。
- publish 重新验证 immutable claim、previous current、completion id/turn pack 和精确推导的 cache reference；
  旧 unread 变 superseded，旧 leased 保持 lease 但失去 current，历史与本批 coverage、新 unread 在同一
  transaction 提交，任何 stale/trigger failure 全量 rollback。
- M4A 只验证 deterministic cache reference。M4D 必须持有独占 CacheStore gate，先证明对象存在且认证通过，
  再进入 SQLite publish transaction。

## Crash and idempotency evidence

- Host open 把 `generating` 改为 `interrupted`，不删除 request/generation/previous/coverage。冷启动调用者只凭
  task id 即可事务恢复完全相同 claim；恢复前其他 request 返回 busy。
- SIGKILL after-commit 后再插入一个新 completion，恢复 claim 仍只含崩溃前冻结的 completion；before-commit
  则不存在 durable claim，可正常创建第一次 claim。
- 显式 abandon 释放 active ownership但保留永久 request tombstone；同 task 重用返回 abandoned，跨 task
  重用返回 conflict。published request replay 不重新运行外部副作用。
- 旧 summary schema migration 保留可识别 active claim 为 interrupted；重建表后重新建立三个 partial/unique
  index，避免 `ALTER TABLE RENAME` 时旧 index 名占用导致新表无约束。

## Automated and review evidence

本机最终 `./scripts/eval-fast.sh` exit 0：154 Host unit tests、4 Host CLI tests、4 macOS parent-death tests、
6 Rust protocol tests、4 runtime-gate example tests、36 TypeScript tests、firmware host ctest、Rust
fmt/Clippy、Prettier、typecheck/build、secret scan 与 source/license audit 全绿。summary 聚焦测试 11 个，
覆盖累计替换、request replay/busy/published、cache reference、restart/abandon、rollback、batch backlog、
leased replacement、generation overflow、旧 schema migration 和真实子进程 SIGKILL。

独立 Sol high 审查先后发现：失败/重启删除 request identity；`covers_completions` 对 delta/累计语义不清；
迁移时旧 index 名占用；冷启动只知道 task 时无法发现旧 request。全部修复后最终结论为
`P0=0`、`P1=0`、`P2=0`、`PASS`，reviewer 未修改文件、未操作实体键盘。

## Exact-commit CI and next gate

GitHub Actions run [`30824483037`](https://github.com/Larkspur-Wang/easy-codex-input/actions/runs/30824483037)
精确对应 source candidate，`host-and-protocol`、`ble-peripheral-simulator`、`firmware`、`macos-desktop`
四个 job 全绿。

M4A 关闭，下一段是 M4B isolated Spark runner。真实 Spark、北京区 DashScope TTS、WAV/EIAD、cache
object authentication、20 次 API 运行和用户试听仍未执行，不能由本证据替代。
