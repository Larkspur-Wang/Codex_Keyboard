# M3B durable Codex prompt runner 证据

## Candidate and boundary

- Source candidate: `2db0cd6eaf9ff6ae85beacd66257a1dffa5278bf`
- Evidence level: static + automated + live local rollback compatibility
- Physical keyboard: M3B did not connect, reset, flash or perform keyboard HIL
- Secrets: no task UUID, title, prompt, transcript, auth, key or absolute user path in this evidence

本段只关闭 M3B。自动化 fake CLI 证明 argv、stdin、队列、失败分类和进程清理，不冒充真实 Codex task
已经产生 turn、Desktop 已热刷新或 rollout completion 已被观察。当前 launchd 安装的仍是 M2 release binary，
没有用源码候选替换常驻 Host。

## Implemented contract

- enqueue transaction 锁定 request id、slot、binding generation、task id、prompt 和 catalog cwd snapshot；
  精确重放幂等，改变任一载荷即冲突。每 task 最多 12 个 queued/running job。
- scheduler 只 claim 每 task 最早 sequence，全局最多 2 个 running；旧 claim generation 不能完成重启后
  的新 claim。Host 非正常退出后，SQLite 将 running 恢复为 queued，提供 at-least-once 重试而非虚假的
  exactly-once 承诺。
- runner 只执行 `codex exec resume --json <task-id> -`，prompt 只写 stdin，不经过 shell。cwd 在 enqueue
  时来自 allow-list，执行前按 owner/type/no-symlink 打开目录链，并用已打开 fd 进入工作目录。
- stdout 只接受有界 JSON object lines，默认单行 256 KiB、总计 4 MiB；stderr 最多保留 64 KiB 用于
  分类，超出部分继续排空但不保留，只落
  `cli_missing`、`authentication`、`task_archived`、`active_session`、`timeout`、`invalid_output`、
  `output_too_large`、`unsafe_working_directory`、`process_io` 或 `exit_failure` 分类。
- macOS Host 以独立 supervisor、private control socket、kqueue `NOTE_EXIT` 和进程组覆盖 Host `SIGKILL`
  清理。Codex leader 退出后 supervisor 仍守护后代，直到 Host 清理进程组；control fd 在启动 Codex 前恢复
  `FD_CLOEXEC`。Linux 直接 child 使用 `PR_SET_PDEATHSIG(SIGKILL)`。
- schema 的有效版本为 2，但 raw SQLite `user_version` 暂留 1，新增列采用 additive/default migration。
  这是为了让当前已安装 M2 binary 仍可打开同一数据库；没有 cwd 的旧 queued/running job 在升级时
  fail closed 为 `unsafe_working_directory`。

## Automated evidence

最终候选通过：

- macOS 116 个 Host unit tests、4 个 Host CLI tests、4 个 supervisor parent-death integration tests、
  6 个 Rust protocol tests；
- 同 task 100 jobs 在容量为 12 的有界批次中保持 FIFO；不同 task 可以并发，同 task 不重叠；
- request replay/conflict、容量 transaction、global concurrency 2、restart recovery、stale claim、
  CLI missing/auth/archive/active、timeout、invalid/oversize output、child `SIGKILL` 和 missing/unsafe cwd；
- Host `SIGKILL` 后 leader/后台 descendant 均退出；Host 被 `SIGSTOP`、leader 先退出后再杀 Host 的
  handoff 路径也不留下 descendant；fake Codex 看不到 supervisor 的精确 control fd；
- 当前仓库 `eval-fast`：36 个 TypeScript tests、firmware host ctest、Rust fmt/Clippy、Prettier、
  typecheck/build、secret scan 和 source/license audit 全绿。

## Independent review

全部独立审查使用 `gpt-5.6-sol`、reasoning `high`、read-only，未使用 Oracle。多轮审查推动修复：

- macOS 直接 child 无法在 Host `SIGKILL` 后可靠清除子孙进程；
- supervisor 注册、leader 快速退出和 Host 收到结果后的三个生命周期竞态；
- control socket 继承给 Codex 形成可伪造结果通道；
- schema v2 会让当前 M2 LaunchAgent 无法回滚；
- 空 stdout 过早覆盖 auth/archive/active stderr，及 direct CLI 退出码可碰撞 supervisor 内部分类；
- CI 的非 macOS import lint 和“任意 socket 即泄漏”的过宽断言。

最终增量复审无 P0/P1/P2，`VERDICT: PASS`。剩余边界是同 UID 主动进程可暂停/杀死 supervisor，或恶意
child 主动 `setsid` 逃离进程组；本阶段不把普通本地进程故障防护描述成对抗同 UID 恶意代码。

## Live local compatibility and CI

- 在隔离临时 HOME/数据库副本上建立 M3 additive columns 后，当前安装的 M2 release binary 仍能启动并
  返回 healthy，raw schema 仍报告 1。该检查没有触碰产品数据库。
- 本机确认 launchd 的最小 PATH 下仍可从用户 NVM 目录发现 native `codex-cli 0.145.0`；没有向真实 task
  投递 prompt。
- 首个源码提交 `dfaaa56` 的 CI `30779360765` 保留为失败证据：Linux 暴露 macOS-only import lint，
  macOS 暴露测试把无关继承 socket 误报为 control fd 泄漏；firmware 与 desktop job 已通过。
- 修复提交 `2db0cd6` 的 exact-commit CI `30779604869` 全绿：`host-and-protocol`、
  `ble-peripheral-simulator`、`firmware`、`macos-desktop` 四个 job 均通过。

M3B 关闭；下一段是 M3C rollout observer。真实测试 task 投递与 Desktop 可见性仍保留在 M3 组合运行门禁。
