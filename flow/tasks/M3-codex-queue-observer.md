# Task: Codex task、durable FIFO 与 completion observer

- Milestone: `M3`
- Status: `complete`
- Owner: Codex
- Started: 2026-08-03
- Closed: 2026-08-03

## 目标

让 Mac Host 在不修改 Codex 内部数据库的前提下，建立四槽可绑定的本地 allow-list、持久任务队列、
受控 `codex exec resume` 执行器，以及只由 rollout `task_complete` 驱动的完成账本和有界 turn pack。

## 分段与提交门禁

1. `M3A`：只读 Codex catalog、pin/recent 次序、归档过滤、四槽 CAS binding 与换绑语义。`complete`
2. `M3B`：durable prompt job、每 task 严格 FIFO、全局并发 2、每 task 上限 12、stdin-only Codex runner。`complete`
3. `M3C`：rollout path/identity/cursor、rotation/truncation、`task_complete` 去重与有界 turn pack。
   `complete`

每段都必须完成实现、失败路径自动化、`gpt-5.6-sol` high 独立只读审查、脱敏证据、独立 commit/push
和 exact-commit CI；M3C 关闭前再做一次组合运行验证。全程不使用 Oracle。

## M3A 验收

- Codex 来源只读：有界读取 `.codex-global-state.json`、`session_index.jsonl` 与 `state_5.sqlite`；SQLite
  只对经过 main+WAL 前后 hash/identity 一致性验证的 Host 私有快照使用 read-only/query-only connection，
  不让 WAL reader 接触源 SHM，也不执行 migration、PRAGMA journal mutation 或写事务。managed snapshot
  root 使用 owner marker、进程内/跨进程锁；正常结束删除，`SIGKILL` 残留在下次 catalog 启动先清理。
- pinned 按 Desktop 保存次序，随后最多 8 个 recent；总 catalog 有界。只有合法 UUID、未归档且存在
  rollout path 的 thread 能进入 allow-list；内部/子 Agent/临时总结 task、未知 schema、超大、普通非竞争
  替换路径或 SQLite sidecar identity 变化 fail closed。
- task 名、project 与 UUID 只通过本机私有 Host surface 返回；公开日志/evidence 不保存真实值。
- binding 写入 Host SQLite，`slot=1..4` 使用 generation CAS；保存前和使用前都必须重新验证 catalog。
  换绑只影响新请求，已经入队的 job 继续锁定原 task/generation。
- 本地安全边界沿用 M2 的用户私有目录与同 UID 信任假设：恶意同 UID 进程对 Codex SQLite 做快速
  `A -> B -> A` 路径置换或直接写库不在本阶段防护范围；现有 guard 负责普通 symlink、非竞争 replacement、
  owner/type/size 与 main/WAL/SHM 前后 identity，不能把它描述成防住同 UID 主动攻击者。

## M3B 验收

- enqueue 在 SQLite transaction 中锁定 request id、slot、binding generation、task id 与 prompt；同 request
  精确重放幂等，不同载荷冲突。每 task 最多 12 个 queued/running job，超过后明确拒绝。
- scheduler 每 task 只运行最早 sequence，全局最多 2 个 child；Host 重启把 running 精确恢复 queued，旧
  claim generation 不能完成新 claim。
- 只执行 argv `codex exec resume --json <task-id> -`；prompt 只走 stdin，禁 shell；cwd 必须来自 enqueue
  时 catalog 快照且仍是受信目录。stdout JSONL 有界消费，stderr 只保留脱敏错误分类。
- CLI 缺失、auth/archived/active-session、timeout、SIGKILL、Host restart 和不同 task 并发均有失败测试。

## M3C 验收

- observer 只 tail catalog 给出的 rollout regular file；cursor 绑定 task、规范路径、device/inode、offset 和
  generation。replacement/truncation 先重新验证 `session_meta` task id，再从 0 重放并靠 ledger 去重。
- 未完成 turn 不推进持久 cursor；重启后可重建同一 turn。超大行、非法 UTF-8/JSON、缺失字段、隐藏
  reasoning、未知 event 和无限工具输出不能进入 turn pack 或造成无界内存。
- 只有 `event_msg.payload.type=task_complete` 且 turn id 匹配时才生成 completion；进程启动、tool end、
  commentary 或 CLI exit 都不能替代。completion ledger 与 cursor 在同一 transaction 内提交。
- turn pack 仅含有界 user/assistant 公开文本、tool name/状态和测试/提交事实；不含 reasoning、secret、
  原始 binary、完整日志或未脱敏绝对路径。

## 运行证据边界

- 自动化 fixture 只能证明解析、队列和 child argv/stdin；不能冒充真实 Codex task 持久化或 Desktop 热刷新。
- 真实运行验证必须使用专门测试 task，记录脱敏 completion/result，不把 task UUID、标题、prompt 或 rollout
  内容写入仓库。若需要用户确认目标 task 或产生可见 turn，先停在操作门禁请求用户参与。
- 本任务不连接、复位或烧录实体键盘；PWA、Cloudflare、Spark/TTS 和播放属于后续里程碑。

## 真实组合运行结果

- 用户授权后，用官方 CLI 创建一个普通 `thread_source=user` 专用测试 task；程序化 App
  `create_thread` 生成的是结构化 subagent，catalog 按合同拒绝，未拿它绕过 allow-list。
- 隔离 Host SQLite 绑定 slot 1 / generation 1，先观察直接用户 task 首轮，再通过真实
  `codex exec resume --json <task-id> -` 的 stdin-only 路径投递一条 Host queue prompt。两次完成各提交
  一次，completion 总数与 distinct 总数均为 2；投递前、投递后重启均无重复插入或 replay。
- 持久 job 为 `completed`，task、prompt 与 binding 精确匹配；对外 JSON 不含 task UUID、prompt、rollout
  内容或绝对路径。归档测试 task 后重跑门禁，在 binding、enqueue 或启动 Codex child 前以 `catalog`
  fail closed；catalog 自身仍会创建并清理私有 snapshot/lock。
- Desktop navigation 能解析并打开该普通 CLI task；structured hot read 没有返回 task，因此只记录为
  Desktop 集成边界，不宣称热读 API 已覆盖。测试 task 随后归档。
- runtime gate 的 crash-safe 私有状态覆盖 `SIGTERM`、`SIGKILL`、owner marker 部分发布与八个创建失败点；
  Linux CI 暴露错误路径锁未显式释放后，改为 guard `Drop` 配对 `unlock`，最终 exact-commit CI 全绿。
- M3 候选只在隔离状态中运行，没有替换当前已安装的 M2 LaunchAgent，也没有操作实体键盘。

## 参考来源

- 概念参考：Heard `d56b10f990e065d5b8aa18576e78d382e837a52b` 的
  `heard/mobile/codex_tasks.py` 与对应 tests，Apache-2.0。
- 本项目用 Rust 重新实现，不复制 Python 源码；差异是队列/游标进入 Host SQLite，后台 daemon 是唯一 owner，
  并增加全局并发、claim generation、rollout identity 与 completion transaction。

## Evidence

- M3A: `evidence/m3/catalog-bindings.md`、`evidence/m3/catalog-bindings-manifest.json`
- M3B: `evidence/m3/durable-runner.md`、`evidence/m3/durable-runner-manifest.json`
- M3C: `evidence/m3/rollout-observer.md`、`evidence/m3/rollout-observer-manifest.json`
