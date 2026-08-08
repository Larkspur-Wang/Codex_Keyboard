# M2D Host daemon、LaunchAgent 与 Tauri 证据

## Candidate and scope

- Source candidate: `cffe0d20ec27e7cfa62c077e6e22245060ab2f2b`
- Evidence level: static + automated + live macOS runtime
- Physical keyboard: not connected, reset or flashed
- Secrets: no key, task UUID, prompt, transcript, account or absolute user path in this evidence

本段只关闭 M2D。Codex task catalog/FIFO、Spark、TTS 音频生成、PWA 产品链、Cloudflare 正式 Relay 和
实体键盘仍属于后续里程碑，不能由本段 health 或 UI 证据代替。

## Implemented contract

- Host 先独占打开 SQLite，再把 line-delimited、versioned、bounded JSON health protocol 发布到私有
  `run/host.sock`；runtime directory 为 `0700`，socket 为 `0600`。
- 连接、read、write 使用单一 2 秒 absolute deadline；16 个并发 drip client 不能通过持续小包重置
  timeout。响应只含版本、PID、启动时间、schema 和恢复 job 数。
- socket 以短随机临时名 bind，经 parent fd nofollow chmod 与 identity 复核后 no-replace 发布；stale、
  drop 和 uninstall 清理使用 descriptor-relative retirement 与 identity-conditional unlink。
- LaunchAgent 使用固定 label、`RunAtLoad`、`KeepAlive` 和 XML-escaped 绝对路径。install/upgrade 成功须
  同时证明候选版本、新 PID，以及 health PID 精确等于 `launchctl print` service PID；失败 upgrade 恢复
  旧 binary/plist 并再次验证旧服务。
- Tauri v2 后端只调用同一 health socket，不读取 SQLite；UI 明确显示 healthy、offline、protocol error、
  Host 版本、PID、socket mode 和 database migration。关闭窗口不发送 Host stop。

## Automated evidence

当前工作树在真实重启并解锁 login Keychain 后通过：

- 87 个 Host tests（含 3 个真实 login Keychain integration）、4 个 Host CLI tests、6 个 Rust protocol tests；
- 100 轮独立子进程 `SIGKILL` 覆盖 SQLite enqueue/claim 提交前后，逐轮重开证明事务要么完整回滚、
  要么完整提交，running claim 只恢复一次且 FIFO job 不丢失；
- health 9/9、LaunchAgent 8/8，health suite 连续 20 轮通过；
- Linux 非 root 聚焦复现并修复 regular-file inode 即时复用，回归通过；
- pnpm typecheck/test/build、desktop UI 2 tests、PWA 24 tests、firmware host ctest；
- Rust fmt、workspace all-targets Clippy `-D warnings`、Prettier、secret scan、source/license audit 和 diff check；
- unsigned macOS `.app` bundle 成功；CI 新增独立 macOS desktop bundle job。
- 完整 `./scripts/eval-fast.sh` 全绿，未跳过 Keychain integration。

## Independent review

全部独立审查使用 `gpt-5.6-sol`、reasoning `high`、read-only，未使用 Oracle。审查依次发现并推动修复：

- launchctl 返回成功但候选 Host 未健康；
- per-read timeout 可被 drip client 无限续期；
- stale/drop socket 和 bind/chmod/publish 的路径替换竞态；
- 未加载 LaunchAgent 时同版本手动 daemon 冒充候选；
- uninstall 检查后删除及 Linux inode 即时复用；
- 750 ms deadline 在高负载调度下的 flake，以及失效的临时文件残留断言。

最终实现复审与 fd-held inode 增量复审均报告 `NO_P0_P1_P2`。提交前证据复核随后发现计划把普通 reopen
过报为进程崩溃门禁；实现新增上述 100 轮真实 `SIGKILL`，完整 eval 重跑全绿，并交回同一 reviewer
做最终增量复审。复审确认测试只声明 enqueue/claim 提交前后进程 kill 边界，不冒充断电、内核或磁盘
故障，最终报告 `NO_P0_P1_P2`、`VERDICT: PASS`。

## Live macOS runtime evidence

最终 release 候选已完成：

- 真实用户级 `launchctl` install，`loaded=true`、`healthy=true`；
- 对 service PID 执行 `SIGKILL` 后，launchd 以不同 PID 恢复相同版本 health；
- 原位 upgrade 后 installed binary inode 与 PID 均改变，source/installed SHA-256 相同；
- uninstall 后 service、binary、plist、socket 均消失，SQLite 与 installation id hash 保持不变；
- 未加载 service 但手动 daemon 占用 health 时，install 在写 binary/plist 前以通用 activation error 拒绝；
- 重新 install 后恢复 `loaded=true`、`healthy=true`；关闭 Tauri 窗口后 Host health 仍可读。
- 用户执行真实 Mac reboot/login，未手动启动 Host 或 App；重启后的 LaunchAgent 自动达到
  `loaded=true`、`healthy=true`，随后 login Keychain integration 全绿。
- 最终 `.app` 实际显示绿色 healthy、版本、PID、`run/host.sock · 0600` 与 schema v1；手动刷新更新时间，
  关闭窗口不停止 Host。卸载 Host 后同一 App 自动显示琥珀色 offline，重新安装后恢复 healthy。

## Release gate result

- Source candidate 已推送到 `main`。
- Exact-commit GitHub Actions run `30773582976` 全绿：`host-and-protocol`、
  `ble-peripheral-simulator`、`firmware`、`macos-desktop` 四个 job 均通过。

M2D 与整个 M2 的约定门禁均已完成；M3+ 与实体键盘证据仍按各自里程碑独立推进。
