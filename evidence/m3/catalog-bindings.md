# M3A Codex catalog 与四槽 binding 证据

## Candidate and boundary

- Source candidate: `f6cd3c96a06751c4e2b988bc1313e23f73288220`
- Evidence level: static + automated + live local read-only catalog
- Physical keyboard: not connected, reset or flashed
- Secrets: no task UUID, title, prompt, rollout text, account, key or absolute user path in this evidence

本段只关闭 M3A。真实 Codex prompt 投递、per-task runner、rollout completion、Spark/TTS、PWA/Relay 和
实体键盘仍未由 catalog 证据替代；真实四槽 binding 也没有写入用户当前 Host 状态。

## Implemented contract

- Desktop pinned 保存次序优先，只有成功通过 UUID、未归档、结构化 user-thread 和 rollout regular-file
  验证的 pin 才占最多 16 个有效名额；recent 独立查询并最多返回 8 个。
- `source`、`thread_source` 与 `agent_role` 结构化排除 subagent；历史内部总结标题只作第二层防御。
- global/index/main DB/WAL/SHM、行数、单行、字段、SQLite VM step 和墙钟都有明确上限；未知 schema、
  symlink、普通 replacement 及 owner/type/identity 异常均 fail closed。main/WAL 的内容或 size 变化由 digest
  拒绝；SHM 只要求 identity 稳定且 size 不超过上限，不把同 inode 的限额内内容变化冒充数据库变化。
- SQLite 不直接打开 Codex WAL 源库。Host 把 main+WAL 流式复制到 managed private snapshot，复制后和
  查询后重新核对源 hash/identity；SQLite 只接触 `0700` root 下的 `0600` 副本，因此不创建或改写源 SHM。
- managed root 使用 owner marker、进程内 mutex 和跨进程 flock；正常 Drop 清理。复制中真实 `SIGKILL`
  留下的 partial snapshot 会在下一次 catalog acquire 持锁、按 UUID/文件 allow-list 清理。
- 四槽 binding 保存到 Host SQLite，`slot=1..4` 使用 generation CAS；bind 和 resolve 都重新经过当前 catalog。

## Automated evidence

最终工作树通过：

- macOS/当前本机 104 个 Host unit tests，exact CI Linux 101 个；另有 4 个本机 Host CLI tests、6 个
  Rust protocol tests；其中 catalog 16 tests、binding 1 test；
- Linux non-root Docker 的 16 个 catalog tests，覆盖 shared `/tmp` 与显式 fixture snapshot root；
- main+WAL+SHM 内容/目录项不变、缺 SHM 不创建、oversize、schema、subagent、pin/recent 和 query budget；
- 子进程复制到 8 MiB checkpoint 后真实 `SIGKILL`，下一次 acquire 只剩 owner marker 与 lock；
- TypeScript 36 tests、firmware host ctest、Rust fmt/Clippy、Prettier、typecheck/build、secret/source audit。

## Independent review

全部独立审查使用 `gpt-5.6-sol`、reasoning `high`、read-only，未使用 Oracle。审查先后发现并推动修复：

- pinned 被 recent top-80 截断、无效 pin 提前消耗 quota；
- 仅靠标题过滤导致正常标题的 subagent 混入；
- SQLite 主库/WAL/SHM、字段和 query work 无真实上限；
- read-only WAL connection 仍会写源 SHM；
- unmanaged TempDir 在 `SIGKILL` 后遗留大块任务数据库；
- Linux shared `/tmp` 不满足 owned-directory 创建合同；
- fixture `from_home()` 会触碰真实产品 runtime。

最终改为显式 `from_paths()` 测试隔离和生产 `from_environment()` 私有 runtime 后，增量复审报告
`NO_P0_P1_P2`、`VERDICT: PASS`。

## Live local evidence

最终 source binary 对当前本机 Codex 状态只输出脱敏计数：`tasks=17`、`pinned=9`、`recent=8`。Desktop
global state 当时有 11 个 pin 引用，其中 2 个没有 SQLite thread 行，按 fail-closed 合同不进入目录。

一次完整读取前后，global state、session index、SQLite main/WAL/SHM 的 SHA-256、size、inode 与 mtime 均
不变；managed snapshot pending 数为 0。此证据由 `cargo run` 的 source candidate 产生，当前 launchd
安装的常驻 Host 仍是 M2 release binary，不能把这次命令冒充 daemon 已接入 M3 catalog。

## CI and retained failure

- 初始源码提交 `fb64f4f` 的 CI `30776297424` 保留为失败证据：Linux `host-and-protocol` 暴露 shared
  `/tmp` owned-path 假设；其余三个 job 通过。
- 修复提交 `f6cd3c9` 的 exact-commit CI `30776818678` 全绿：`host-and-protocol`、
  `ble-peripheral-simulator`、`firmware`、`macos-desktop` 四个 job 均通过。

M3A 关闭；下一段是 M3B durable prompt runner，不提前声明真实 Codex task 已被写入或 Desktop 已热刷新。
