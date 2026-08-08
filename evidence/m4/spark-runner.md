# M4B isolated Spark runner 证据

## Candidate and boundary

- Final candidate: `751f8d22f2957f2dca5c842812176fbac6aebca0`
- Evidence level: static + automated + parent-death + live Codex service
- Runtime: `codex-cli 0.145.0`, model `gpt-5.3-codex-spark`
- Independent review: `gpt-5.6-sol` high, read-only, no Oracle; `P0=0/P1=0/P2=0`, `PASS`
- Secrets: evidence 不含 auth、真实 task UUID、prompt/turn pack、summary 正文、rollout 或用户绝对路径

M4B 只实现 cumulative summary 的隔离 Spark 运行器，不调用 DashScope、不生成或缓存音频，也不连接、复位
或烧录实体键盘。已安装的 M2 Host 没有被本候选替换。

## False-green discovery and fix

第一版真实 gate 虽能返回合规 summary，但外部运行中审查发现全新 `CODEX_HOME` 自动展开系统 skills 和
plugin cache，最多观察到 1,247 个文件、39 MiB；`--disable skill_search/plugins/memories` 不能阻止 bundled
skills 进入模型可见 prompt，因此该 gate 判为 false green，没有提交。

最终实现预建空的 mode-0500 `skills/`，保留 feature disable 作为第二层防线，并把 `codex debug
prompt-input` 变成真实 gate 的前置断言。当前门禁观察到 `prompt_items=5`、`skills_instructions=0`、
`SKILL.md mentions=0`、system skill files=0、plugin files=0。

Codex 自身仍会在隔离 HOME 创建空的 state/log/memory/goal SQLite schema、模型目录缓存和短时 PATH alias。
Host 每 10 ms 审计 HOME 与独立 process TMPDIR 的精确路径/type/owner/mode/inode/link，最多 32 个节点和
8 MiB 逻辑总量；PATH alias 只能位于 identity 绑定的 `tmp/arg0`，名称固定且目标必须等于本次受信 Codex
binary，process TMPDIR 必须保持为空。接受 summary 前再次确认
memory/goal/log/thread 持久表行数全为 0。未知文件、system skill、plugin、错误 symlink、额外节点或总量越界
都会终止进程并返回稳定的 fail-closed 分类。

## Automated and live evidence

- Spark focused：13 unit tests + 2 macOS parent-death tests 通过；覆盖 argv/stdin/auth snapshot、空 skills、
  prompt skill injection、HOME 污染/总量、runtime DB 非空、精确 alias target、output/auth replacement、
  timeout/cancel/signal、single-flight、stale sweep 和 parent SIGKILL。
- Clippy `--all-targets -D warnings` 和 `cargo fmt --check` 通过。
- 真实 gate 连续 2 次通过；加入 process TMPDIR 门禁后的复跑为 26 nodes / 2,171,757 bytes，prompt probe 为
  12 nodes /
  483 bytes；system skill/plugin/persistent runtime rows 全为 0。summary 内容没有输出到证据。
- 两次 gate 后 active Spark run directories=0、prompt gate directories=0；只保留无正文的 sweep/task lock。
- 完整 `./scripts/eval-fast.sh` exit 0：167 Host unit、4 Host CLI、6 macOS integration/parent-death、6 Rust
  protocol、4 runtime-gate example、36 TypeScript tests、firmware host ctest，以及 fmt/Clippy、Prettier、
  typecheck/build、secret scan、source/license audit 全绿。

## Exact-CI correction

首次 source exact-commit CI `30834577645` 对
`b436bef023d32646a70938420d567367e3a259bf` 的四个 job 中，macOS desktop、BLE peripheral simulator 与
firmware 通过；Linux `host-and-protocol` 因 `UnixListener` 只在 macOS 使用却被无条件导入，被
`-D warnings` 拒绝。该结果保留为真实失败证据，不重跑或覆盖。

修复只把 `UnixListener` import 放入 `cfg(target_os = "macos")`，共享路径仍无条件导入 `UnixStream`；本地
fmt、全 targets Clippy、13 个 Spark focused unit、2 个 parent-death test 和 `git diff --check` 通过。第二次
`gpt-5.6-sol` high 独立只读复审为 `P0=0/P1=0/P2=0`、`PASS`。

修复后的 exact-commit CI `30835041538` 精确对应
`751f8d22f2957f2dca5c842812176fbac6aebca0`，`host-and-protocol`、`macos-desktop`、
`ble-peripheral-simulator` 和 `firmware` 四个 job 全部通过。M4B 软件切片关闭；M4C DashScope realtime、
WAV/EIAD、真实 TTS 20 次和用户试听均未由此证据替代。
