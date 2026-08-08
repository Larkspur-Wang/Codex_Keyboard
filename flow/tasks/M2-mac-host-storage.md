# Task: Mac Host 常驻与本地安全存储

> 2026-08-04 后续决定：DashScope API key 已迁到 App Support 私有 mode-0600 `.env`；本任务下方关于
> DashScope Keychain 导入的内容保留为当时已完成的实现记录。2026-08-04 真机运行再次证明 LaunchAgent
> 后台 Keychain 读取会中断自动总结；当前 cache/device/relay secret 也改为 App Support 私有文件，运行
> 合同见 `flow/decisions.md` 顶部。

- Milestone: `M2`
- Status: `complete`
- Owner: Codex
- Started: 2026-08-02
- Closed: 2026-08-03

## 目标

建立不依赖桌面窗口生命周期的 Rust Host，并把后续 FIFO、完成账本、未读总结和音频发布所需的
本地可靠性边界一次做对：SQLite transaction、macOS Keychain、加密 cache、LaunchAgent 和 health surface。

## 分段

1. `M2A`：应用目录权限、SQLite migration、绑定 CAS、FIFO claim、崩溃后 running job 回到 queued。
2. `M2B`：Keychain adapter、显式一次性 `.env` key 导入、内存清零和北京区握手。
3. `M2C`：AES-256-GCM cache、临时文件 fsync/rename、孤儿清理、容量审计和损坏 fail closed。
4. `M2D`：Unix health socket、LaunchAgent install/upgrade/uninstall、Tauri 最小 health UI。

每段必须依次完成实现、失败路径测试、不同模型只读审查、独立 commit/push 和 exact-commit CI；不得把
mock、静态 plist 或 debug 进程冒充登录启动/崩溃重启实测。

## M2A 验收

- 新数据库只以 `0600` 创建，父目录强制 `0700`，schema migration 幂等。
- 每 task 只 claim 自己最早 queued job；重复 request id 不重复入队。
- claim 与状态更新是 transaction；Host reopen 自动把 `running` 恢复为 `queued` 并累加恢复计数。
- binding 更新使用 slot generation CAS；失败 CAS 不改变原绑定。
- 单元测试、100 次 reopen/recovery 固定基准、fast eval、secret/source audit 和 Linux CI 全绿。

## M2C 验收

- 每次安装只使用 Keychain 中 32-byte 随机 cache root key；cache root 内的加密 key-check marker
  区分首次初始化与 key 丢失，损坏、不可读或缺失 key 必须 fail closed，不得静默换 key。
- generation 固定发布 `manifest.json.enc`、`qwen.wav.enc`、`device.eiad.enc` 三个 AES-256-GCM
  对象；每对象独立 salt/nonce，AAD 绑定 task hash、generation、对象类型和版本。
- 三个文件先在 mode-0700 临时目录完成 mode-0600 create-only 写入、文件 fsync 和目录 fsync，
  再以 no-replace rename 原子发布并 fsync 父目录；同 generation 不得覆盖。
- lifetime process lock 与进程内 mutex 串行化容量预留、发布、读取和协调；root/task directory fd
  锚定扫描与相对删除，引用缺失、路径替换、symlink、未知文件、认证失败和截断均 fail closed。
- 固定 512 MiB / 512 generation 总容量与 32 MiB 单 generation 上限；audit 使用磁盘密文
  逻辑字节计数，不把明文、task id 或 Keychain key 写入路径、日志和 evidence。
- 子进程 crash mode 覆盖每个文件 fsync、临时目录 fsync、rename 和父目录 fsync 边界；100 轮
  进程退出后重开/协调只能看到完整 generation 或无 generation；独立子进程验证极端 umask 下仍为
  精确 `0700/0600`。

## M2D 验收

- `easy-codex-host daemon` 独占打开 SQLite 后再发布 `run/host.sock`；runtime directory 固定 `0700`、
  socket 固定 `0600`。活动实例、symlink、非 socket、非当前用户对象和路径替换均 fail closed；只有持有
  Host instance lock 的进程可以清理已验证的陈旧 socket。
- health wire protocol 固定为一行、带版本、`deny_unknown_fields` 的有界 JSON request/response；连接、
  read、write 均有 deadline，超长、截断、未知命令和慢客户端不能阻塞后续 health。响应只含版本、PID、
  启动时间、schema migration、恢复 job 数和稳定状态，不含 task id、prompt、Keychain account 或 secret。
- Tauri 后端通过同一 Unix socket client 读取 Host，不复制数据库或建立第二份状态；最小 UI 显示 Host
  状态、版本、socket 与数据库 migration，并明确离线/协议错误，关闭窗口不向 Host 发送停止命令。
- 安装器把 Host 原子复制到固定的用户私有安装路径，生成 XML-escaped、固定 label 的用户级 plist；
  `RunAtLoad` + `KeepAlive` 由真实 `launchctl gui/<uid>` install/upgrade/uninstall 驱动。失败 upgrade 恢复
  上一 binary/plist，卸载只移除服务文件，不删除 SQLite、Keychain 或加密 cache。
- 自动化覆盖 stale/active socket、权限、oversize/slow/malformed request、并发 client、socket replacement、
  plist escaping、路径拒绝、install rollback 和 UI offline/healthy/protocol-error 状态；全仓 fast eval 全绿。
- macOS 运行证据必须分别证明 launchd 管理的启动、`SIGKILL` 后 PID 改变且 health 恢复、原位 upgrade、
  uninstall 后服务/socket 消失；登录后自动启动只能由真实 logout/login 或 reboot 证明，静态 plist、手动
  debug daemon、`bootstrap` 本身或自动化测试均不得冒充。

## 安全边界

- 测试 fixture 不使用真实 API key、真实 Codex task UUID、prompt 或用户绝对路径。
- SQLite 可以保存 Mac 本地工作状态；Keychain secret、cache key 和任何凭据不得进入数据库或日志。
- 本任务不连接、复位或烧录实体键盘。

## Evidence

- `M2A` complete: `evidence/m2/manifest.json`
- `M2A` implementation/review boundary: `evidence/m2/storage.md`
- Source candidate: `881b5624b98e15c836cbfeb2b2c307fd70e08970`
- Exact-commit CI: `30754424358` (`host-and-protocol`、`firmware`、`ble-peripheral-simulator` 全绿)
- Final independent review: `gpt-5.6-sol`, reasoning `high`, `NO_P0_P1_P2`; no Oracle.
- `M2B` complete: `evidence/m2/keychain-manifest.json`、`evidence/m2/keychain.md`
- Final source candidate: `57967cacda39d7aab7bb5f7feb05a45cdcb1b3c3`
- Exact-commit CI: `30763338506` (`host-and-protocol`、`firmware`、macOS Keychain/BLE job 全绿)
- Live service: Keychain `installed_verified`，北京区 `session.created`，模型
  `qwen3-tts-instruct-flash-realtime`，transport `https_proxy`；无 key/源路径/安装 UUID 入 evidence。
- M2B independent review: `gpt-5.6-sol`, reasoning `high`, `NO_P0_P1_P2`; no Oracle.
- `M2C` complete: `evidence/m2/cache-manifest.json`、`evidence/m2/cache.md`
- Final source candidate: `4b9f381d082fdd8b6abbd6adb6b069e9d5a29f40`
- Exact-commit CI: `30767392136` (`host-and-protocol`、`firmware`、macOS Keychain/BLE job 全绿)
- M2C independent review: `gpt-5.6-sol`, reasoning `high`, `NO_P0_P1_P2`; no Oracle.
- `M2D` complete: `evidence/m2/host-daemon-manifest.json`、`evidence/m2/host-daemon.md`
- M2D source candidate: `cffe0d20ec27e7cfa62c077e6e22245060ab2f2b`
- M2D exact-commit CI: `30773582976`（`host-and-protocol`、`ble-peripheral-simulator`、`firmware`、
  `macos-desktop` 四个 job 全绿）
- M2D source review: `gpt-5.6-sol`, reasoning `high`, `NO_P0_P1_P2`; no Oracle.
- M2D live macOS: launchd install、`SIGKILL` restart、upgrade、uninstall/state preservation、manual daemon
  rejection、真实 reboot/login startup、解锁后 87 Host tests/完整 eval，以及最终 `.app` healthy/offline/
  refresh/close 视觉验收均通过；另新增 100 轮 SQLite enqueue/claim 提交前后子进程 `SIGKILL`，逐轮重开
  证明完整回滚/提交与 running claim 单次恢复。

`M2` complete：M2A/M2B/M2C/M2D 各自独立的实现、自动化、Sol high 审查、运行证据、提交推送与
exact-commit CI 均已完成；该结论不包含 M3+ 或实体键盘 HIL。
