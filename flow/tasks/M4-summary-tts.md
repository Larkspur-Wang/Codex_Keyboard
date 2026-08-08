# Task: 累计未读总结、Spark 与 DashScope TTS

- Milestone: `M4`
- Status: `complete`
- Owner: Codex
- Started: 2026-08-03
- Closed: 2026-08-04

## 问题与假设

每个已绑定 Codex task 只能有一个 current unread summary。新 completion 到达时必须把旧未读内容和按
顺序领取的有界一批未覆盖 turn pack 一起交给隔离 Spark，积压由后续 generation 继续累计；只有结构化
总结、DashScope PCM、WAV、EIAD 和加密 cache 全部成功后，才能原子发布新 generation 并让旧
generation 退出 current。任何失败或崩溃都必须保留旧未读音频与未覆盖 completion。

## 范围与非目标

- `M4A`：summary schema、pending completion claim、generation/CAS、累计替换与崩溃恢复。`complete`
- `M4B`：独立临时 `CODEX_HOME`、安全 auth 引用、Spark stdin/output schema、ephemeral/read-only/no hooks。
  `complete`
- `M4C`：北京区 realtime WebSocket、事件状态机、PCM 48 kHz、WAV、EIAD/IMA-ADPCM 和 cache publication。
  `complete`
- `M4D`：completion -> Spark -> TTS -> cache 组合运行、真实 API 20 次、模型快照、voice/instructions
  用户试听与选择。`complete`
- 本阶段不实现 PWA/Cloudflare 播放面，不连接、复位或烧录实体键盘；播放 lease 与真机扬声器属于后续
  M5-M8，不能用本阶段 cache/audio 自动化替代。

每段都必须实现、覆盖失败路径、运行完整 fast eval、由 `gpt-5.6-sol` high 独立只读审查、脱敏记录、
独立 commit/push 并等待 exact-commit CI；全程不使用 Oracle。

## 实现

### M4A cumulative summary ledger

- 定义严格有界的 `facts`、`pending`、`decisions`、`spoken_text` 与 `covers_new_completions` schema；该字段
  只表示本 generation 新领取的有序 completion delta（最多 32 条），累计 coverage 的唯一真相源是 SQLite
  `completion_ledger`。canonical JSON 进入加密 cache；本地 SQLite 保存有界 task/request/generation 标识、
  状态、覆盖关系与 cache reference，不保存总结正文。
- 单 task 只允许一个 generation 处于生成流程；claim 必须在 transaction 中固定旧 current generation、
  最多 32 条且总 turn pack 不超过 1 MiB 的顺序 completion 批次和下一 generation；重复 request 幂等，
  stale claim 不能发布，未领取积压保持 pending。
- M4A 的 SQLite publish 只接受按 task/generation 推导的精确 cache reference；同一 transaction 标记
  coverage、新 generation `unheard` 并让旧 current 退出 current。M4D orchestration 必须在独占 CacheStore
  gate 内先证明对象已存在且认证通过，再调用此 transaction；TTS/cache 失败只释放 claim，不覆盖旧 unread。
- Host 重启把 `generating` 变为保留同一 request/generation/completion 集合的 `interrupted`；同 request 重试
  必须恢复完全相同的 claim，其他 request 继续看到 busy；冷启动 orchestrator 只凭 task id 即可事务发现并
  恢复该 claim，不依赖崩溃前内存中的 request id。显式 abandon 只释放 task ownership，永久保留 request
  tombstone，旧 request 不得重用；generation 与 completion 集合不能饱和、跳代或跨 task。

### M4B isolated Spark runner

- 固定模型 `gpt-5.3-codex-spark`，prompt 只走 stdin；使用 `--ephemeral --ignore-user-config
  --ignore-rules --sandbox read-only --config 'approval_policy="never"' --output-schema`。
- 为每次调用创建私有临时 `CODEX_HOME`，只把现有 auth 做成 mode-0400 的 auth-only 临时快照，调用前后
  复核源文件 identity/SHA 并在内存中 zeroize；不复制用户 config、rules、hooks、skills、memory 或本项目
  observer。预建空的 mode-0500 `skills/`，同时关闭 skills/plugins/memory/goals/hooks 等 feature；独立
  `debug prompt-input` 门禁必须证明模型可见输入中 `skills_instructions` 与 `SKILL.md` 均为 0。
- CLI 仍会在全新 HOME 建空的 state/log/memory/goal schema 与模型目录缓存；Host 对 HOME 与独立 process
  TMPDIR 的精确白名单、owner、mode、inode/link、最多 32 个节点和 8 MiB 逻辑总量做运行中审计，并在
  接受 summary 前证明 memory/goal/log/thread 持久表全为空。只允许 identity 绑定的 `tmp/arg0` 下短暂出现
  指向当前受信 Codex binary 的三条固定 PATH alias 和空 lock；process TMPDIR 必须保持为空。任意 system
  skill、plugin、未知文件、额外链接或越界写入都杀进程并 fail closed。
- `RLIMIT_FSIZE=16 MiB` 是子进程的单文件硬上限，8 MiB 是 Host watchdog 对所有 child-writable HOME/TMPDIR
  的可接受总量门禁；最终 summary 只接受 64 KiB。startup、stdin/stdout drain、workspace/output watchdog
  和 supervisor 都服从同一
  绝对 deadline/cancel；同 task 跨线程、跨 Host 和 parent-death cleanup 期间都保持 single-flight。正常
  结束立即删除，`SIGKILL` 残留由下一次持锁扫描清理。
- 覆盖 missing auth/CLI、timeout、malformed/oversized output、schema mismatch、SIGTERM/SIGKILL 和 parent death。

### M4C DashScope realtime and codecs

- Key 只从 App Support 的 mode-0600 私有 `.env` 临时读取，连接固定北京区 endpoint/model；验证 HTTP upgrade、authoritative
  `session.created`、event id、`response.audio.delta`、`response.audio.done`、`session.finished` 与 peer close。
- 固定 `response_format=pcm`、`sample_rate=48000`，限制事件/PCM/时长；401/403、429/5xx、乱序、重复、非法
  base64、缺 done、timeout 和中断均 fail closed，不记录 Authorization 或原始响应。
- PCM16 mono 封装 WAV，再编码带独立帧边界的 EIAD/IMA-ADPCM；验证 sample/frame count、尾帧和 90 秒上限。

### M4D orchestration and live gate

- 用 fake Spark/DashScope 逐 checkpoint 注入崩溃，证明旧 unread 与 completion coverage 不丢、cache 不半发布。
- 使用真实 completion fixture 和隔离 Host 状态跑完整组合链；随后在用户已配置私有 `.env` key 的前提下完成
  20 次真实北京区 TTS，记录脱敏延迟/事件/模型快照，不保存文本或音频到 evidence。
- 用户试听 voice/instructions 候选并明确选择后关闭 M4。

## 验证

| 等级 | 环境/版本 | 命令或操作 | 结果 | Evidence |
|---|---|---|---|---|
| static | Rust/SQLite/schema | M4A bounds、CAS、secret/path review | M4A pass | `evidence/m4/summary-ledger.md` |
| automated | macOS + Linux CI | M4A unit/integration/crash matrix + `eval-fast` | M4A pass | `evidence/m4/summary-ledger.md` |
| live-service | Codex Spark + Beijing DashScope | isolated Spark + 20 TTS calls | pass | `evidence/m4/orchestration-live-gate.md` |
| HIL | N/A for M4 | keyboard/PWA playback deferred | not run | M5-M8 |
| user-accepted | target phone/Mac audio | voice/instructions listening choice | pass (`Cherry`) | `evidence/m4/orchestration-live-gate.md` |

## 安全与脱敏

- 不把 DashScope key 或私有 `.env`、Codex auth、task UUID、prompt/turn pack、summary text、PCM/WAV/EIAD、rollout path 或
  绝对用户路径写入 Git、SQLite 日志或公开 evidence。
- Spark/DashScope 子进程和连接只输出有界状态分类、计数、延迟、event id 与模型名；公开 event id 如来自
  真实服务只记录不可逆脱敏值或不记录。
- Cache plaintext 只在受限内存/临时文件生命周期中存在；持久音频和 manifest 沿用 M2 加密 cache。

## 结论与下一步

M4A-M4D 全部完成：累计未读事务、隔离 Spark、北京区 realtime TTS、WAV/EIAD、认证 cache、组合崩溃
恢复和真实 API `20/20` 均通过；用户试听确认首版采用 `Cherry` 与当前 instructions。M4D 完整增量已通过
Sol high 独立审查和本机完整 `eval-fast` 均通过；Linux 条件编译修复提交
`7518128f64e1d18e361391331d6f5fd5dee1ebcd` 已推送，exact-commit CI `30877638039` 四个 job 全绿。M4
正式关闭，下一步进入 M5 模拟设备纵向切片；M5-M8 才验证播放 lease、PWA/Cloudflare 与实体键盘。
