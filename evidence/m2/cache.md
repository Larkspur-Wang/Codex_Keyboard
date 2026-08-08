# M2C 加密 TTS Cache 证据

## Candidate and scope

- Final source candidate: `4b9f381d082fdd8b6abbd6adb6b069e9d5a29f40`
- Evidence level: static + automated
- Exact-commit CI: `30767392136`
- Physical keyboard: not connected, reset or flashed
- Sensitive boundary: cache key、明文 task id、真实音频、prompt、API key 和用户源路径均未进入 evidence

本段只关闭 M2C 的本地加密 cache。真实 DashScope TTS 生成、音质、扬声器/手机播放、daemon/socket、
LaunchAgent 和 Tauri health UI 仍未由本段证明。

## Implemented contract

- 每次安装的 32-byte cache root key 只存于 macOS Keychain；cache root 内仅保存经该 key 认证的
  `.key-check.enc`。marker、Keychain item 或既有密文不一致时 fail closed，不静默换 key。
- task id 只以 SHA-256 hash 进入路径；generation 只接受无符号、非零、无前导零的 ASCII 十进制规范形式。
- 每个 generation 固定包含 `manifest.json.enc`、`qwen.wav.enc`、`device.eiad.enc`。每对象使用独立
  16-byte salt、12-byte nonce 和 HKDF-SHA256 派生 AES-256-GCM key；AAD 绑定版本、task hash、
  generation 和对象类型，交换、截断、错 key 或篡改均拒绝。
- 发布使用 mode-0700 临时目录和 mode-0600 create-only 文件；三个文件分别 fsync、临时目录 fsync、
  no-replace rename、父目录 fsync。同 generation 永不覆盖。
- cache root 的进程锁与进程内 mutex 串行化扫描、容量预留、发布、读取和协调。扫描/删除锚定稳定
  root/task directory fd；孤儿 final 目录先原子改名为可识别临时目录，再做相对删除。
- 容量固定为 512 MiB / 512 finalized generations，单 generation 明文最多 32 MiB。已超限状态仍可
  协调清理，但不能继续发布。
- `ExplicitFileLock` 在 flock 成功后立即接管 Cache、StateStore 和 ImportLock；错误返回和正常析构都
  显式 unlock，避免 fork 后、exec 前继承的重复描述符延长 owner 生命周期。SQLite connection 先于
  实例锁销毁，互斥窗口没有缩短。

## Automated evidence

最终候选在 macOS 本地通过：

- 68 个 Host unit tests、2 个 CLI tests；
- 真实隔离 background Keychain cache-key roundtrip；
- 100 个独立子进程 crash cycles，轮换覆盖三个文件 fsync、临时目录 fsync、rename、父目录 fsync；
- 极端 umask 子进程验证精确 `0700/0600`；
- descriptor replacement sentinel、symlink/未知文件/损坏引用/错误 key/对象交换/截断 fail closed；
- cache、SQLite 和导入锁的 duplicate-descriptor 回归，以及包装后 early-error 解锁回归；
- 全仓 fast eval：Rust protocol 6、TypeScript 34、firmware host 1，Clippy `-D warnings`、secret scan、
  source/license audit 和 diff check 全绿。

Linux 非 root Docker 曾对锁修复运行 20 轮并行 Host 全量测试；最终源码另由独立审查运行 65/65，随后
exact-commit GitHub Actions
[`30767392136`](https://github.com/Larkspur-Wang/easy-codex-input/actions/runs/30767392136) 全绿：

- `host-and-protocol`: 62 seconds；
- `ble-peripheral-simulator`: 43 seconds，包含 macOS Host/Keychain integration；
- `firmware`: 161 seconds。

保留失败 run `30765679212`：初始候选在 Linux 并行测试中偶发 `AlreadyOpen`。本地 Docker 在第 4 轮
复现后确认，子进程在 exec 前可短暂继承 flock 的 open-file-description；只等待 descriptor close 会让
新 owner 瞬时失败。修复 cache 后压力测试又依次暴露 SQLite 与 import lock 的同类问题，因此最终使用
共享显式解锁 guard，而不是只给失败测试加重试或串行化。

文档提交的取消/未通过 run `30766714903` 也被保留：Linux 与固件成功，macOS 在 68 项中完成 67 项后，
绝对 deadline 测试的 peer 因尚未收到连接而永久阻塞在 `accept()`，最终触发 10 分钟 job timeout。生产
deadline 已正确返回；问题是测试随后无条件 `join()`。最终测试使用带 5 秒上限的 nonblocking accept、
release channel 和 `accepted` 断言，并把 3 秒总预算中的 1.5 秒明确消耗在 setup；错误的逐阶段重置约需
4.5 秒，仍会被 3.75 秒上限拒绝。修复后 macOS exact CI 43 秒完成。

## Independent review

所有独立审查均使用 `gpt-5.6-sol`、reasoning `high`，未使用 Oracle。审查依次发现并推动修复：
silent key rotation、容量并发、伪 crash 测试、路径 TOCTOU、umask、非规范 generation，以及显式锁在
`set_len/sync_all` 之前尚未接管的错误路径。最终 diff 只读复审为 `NO_P0_P1_P2`。

这些证据证明的是本地密文 cache 的完整性、原子性、容量和恢复边界；真实 TTS 音频、物理断电、磁盘满、
手机或实体键盘链路仍需后续阶段单独验收。
