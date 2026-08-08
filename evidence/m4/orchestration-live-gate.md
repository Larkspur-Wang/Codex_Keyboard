# M4D 组合编排与真实 TTS 门禁

## Candidate boundary

- Status: pass; orchestration, automated recovery, live-service and user listening complete
- Voice preset: `Cherry` / `summary-cherry-v1`
- Model: `qwen3-tts-instruct-flash-realtime`
- Official equivalent snapshot: `qwen3-tts-instruct-flash-realtime-2026-01-22`
- Region: 华北 2（北京）
- Secrets: DashScope key 只从 App Support 私有 mode-0600 `.env` 读取；不记录 API key、Keychain account、
  task UUID、prompt/turn pack、summary 正文、PCM/WAV/EIAD 或用户绝对路径

官方依据：

- https://help.aliyun.com/zh/model-studio/qwen3-tts-instruct-flash-realtime
- https://help.aliyun.com/zh/model-studio/qwen-tts-voice-list

`Cherry` 是当前官方音色表中兼容目标 realtime Instruct 模型的中文女声。首版 instructions 固定为自然、
清晰、沉稳的普通话工作助理风格；真实试听不满意时必须建立新的版本化 preset，不能静默改变已有 cache
generation 的语义。

## Transaction and recovery contract

- 每个 claim 在外部 TTS 调用前先写 durable `summary_tts_attempts.started`。进程从该点到完整认证 cache
  publication 之间崩溃，重启后都进入 `ambiguous` / manual，不会自动再次计费。
- TTS 返回成功后，wrong voice、WAV/EIAD 编码、cache capacity、发布或认证读回任一失败都保留 attempt
  并进入 manual；只有明确发生在外部提交前的安全失败才允许 abandon claim。
- 完整 cache 已存在时，恢复路径在独占 CacheStore operation gate 内认证三个对象，校验 summary 的 ordered
  completion delta 精确匹配 immutable claim，然后在同一 gate 生命周期内提交 SQLite ledger；不再次调用
  Spark 或 TTS。
- ledger transaction 同时迁移旧 coverage、覆盖本批 completion、发布新 unread 并删除 attempt。旧 unread
  与 pending completion 在任何失败或崩溃路径中都不提前退出 current/coverage。

## Automated evidence

- 组合测试覆盖：旧未读累计、safe TTS failure、`AmbiguousAfterCommit`、wrong voice、cache capacity failure、
  已认证但 coverage 不匹配的 orphan、schema 原位升级和 attempt/claim 绑定。
- 七个 checkpoint 使用真实子进程 `SIGKILL`：claim、previous unread、Spark output、TTS attempt、TTS output、
  authenticated cache、ledger publish。恢复结果只可能是安全重做未产生副作用的步骤、从完整 cache 补提交、
  已发布幂等返回，或 manual；不存在自动重复 TTS。
- 本机聚焦结果：M4D orchestrator、私有 `.env`、极端 umask、旧配置保留、崩溃临时文件清理、legacy
  DashScope Keychain 精确删除和 descriptor-anchored parent replacement 均有回归；Host lib 204 tests pass，
  `cargo clippy -p easy-codex-host --all-targets -- -D warnings` pass。
- `.env` 配置后真实北京区 handshake pass，运行状态 `configured`、owner 为当前用户、mode `0600`；旧
  DashScope key/marker 两项已删除，cache/device/relay Keychain account 不在清理集合中。
- Sol-high 安全修复后的完整 `./scripts/eval-fast.sh` pass：Host 204、CLI 6、parent-death 6、protocol 6、
  runtime-gate 4、TypeScript 36、firmware host test、格式、Clippy、secret/source audit 全绿。

## Independent review

独立 `gpt-5.6-sol` high 首轮发现：TTS 已成功后 wrong voice/cache failure 会 abandon 并允许再次调用；恢复
authenticated orphan 时缺少 exact claim coverage 校验。`.env` 复审继续发现极端 umask 的先替换后失败、
legacy Keychain 遗留、crash temp 和 parent path TOCTOU。全部修复并新增回归；最终代码复审为
`P0=0/P1=0/P2=0`、`PASS`，未使用 Oracle、未修改文件、未操作实体键盘。

## Delivery gate

- M4D 首次候选 `c5807ee` 的 exact CI `30877244458` 保留为失败证据：firmware、macOS desktop、BLE
  peripheral simulator 通过；`host-and-protocol` 在 Linux `-D warnings` 下因 macOS-only legacy DashScope
  cleanup 的无条件 import 被判 unused，退出 101。业务测试尚未开始，因此不能把三项通过描述为候选全绿。
- 修复把 `remove_legacy_dashscope_items` import 与唯一调用放在相同 `target_os = "macos"` 边界；本机完整
  `eval-fast` 全绿，独立 `gpt-5.6-sol` high 增量复审 `P0=0/P1=0/P2=0`、`PASS`。
- 修复提交 `7518128f64e1d18e361391331d6f5fd5dee1ebcd` 已推送；exact CI `30877638039` 的 firmware、
  macOS desktop、BLE peripheral simulator、host-and-protocol 四个 job 全绿，M4 delivery gate 关闭。

## Live-service and listening gate

- 真实北京区组合调用：pass；completion -> Spark -> TTS -> WAV/EIAD -> encrypted cache -> ledger 全链通过
- 本阶段可计入的真实 TTS 成功次数：`20 / 20`；全部一次成功，计费字符合计 `1,920`
- 脱敏延迟：组合链 `10,334 ms`；TTS min/mean/max = `2,333 / 2,544 / 2,928 ms`
- 服务事实：requested/served model 均为 `qwen3-tts-instruct-flash-realtime`，transport=`https_proxy`
- preview：本机 mode-0600 canonical WAV，48 kHz / mono / PCM16 / 6.84375 s；不进 Git/evidence
- 用户试听确认：2026-08-04 明确确认“音色可以”；首版固定 `Cherry` / `summary-cherry-v1` 与当前 instructions

历史尝试边界：首次在锁屏时中止，未启动 Spark/TTS；解锁后的第二次通过 Keychain 后因临时 Spark root
被安全检查拒绝，TTS 仍为 0。改用产品 Spark 私有 root 后的第三次运行在尚未输出最终 receipt 时，用户
要求切换 `.env`，进程随即被中止；该窗口内可能已发生的调用数无法权威确认，因此不计入新的 20 次成功
门禁，也不声称零费用。当前 live gate 已彻底移除 DashScope/临时 cache 的 Keychain 访问；新的 20 次已
在无授权弹窗条件下完整结束，上述聚合数据不包含迁移前的不确定窗口。
