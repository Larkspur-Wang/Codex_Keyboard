# M2B Keychain 与 DashScope 导入证据

## Candidate and scope

- Final source candidate: `57967cacda39d7aab7bb5f7feb05a45cdcb1b3c3`
- Evidence level: static + automated + live service
- Live service: DashScope Beijing realtime WSS
- Physical keyboard: not connected, reset or flashed
- Secret boundary: key、源文件路径、安装 UUID 和可逆指纹均未写入仓库、SQLite、日志或 evidence

本段只关闭 M2B。加密 TTS cache、daemon/socket、LaunchAgent 和 Tauri health shell 仍属于 M2C/M2D。

## Implemented contract

- generic-password account 使用每次安装随机 UUID 隔离；UUID 由持锁的私有文件原子创建。
- 导入源必须是显式绝对路径，逐级 `dirfd/openat`，拒绝用户目录 symlink、`..`、非本人所有、
  group/world writable、非普通文件、超过 1 MiB 和读取期间元数据变化。
- 只接受唯一且精确的 `DASHSCOPE_API_KEY` 赋值；源缓冲与 secret readback 使用 zeroizing storage。
- active item 通过 `SecItemAdd` create-only 写入；随后丢弃源 candidate，并以禁止 UI 的后台模式从
  Keychain 读回，使用单一 12 秒绝对 deadline 完成北京区 WSS 验证。
- 只有 active item 存在且 verified marker 内容精确等于 `verified-v1` 才报告 installed；active-only、
  marker-only、损坏或未来 marker 会 fail closed，并在下次导入时协调清理重建。
- 支持直连、普通 HTTP CONNECT `HTTPS_PROXY/NO_PROXY` 和 macOS 静态 HTTPS proxy；非 UTF-8、认证、
  PAC、自动发现或 TLS-to-proxy 配置不会静默绕过。
- HTTP Upgrade、分片 text/control frame、服务 event/model、WebSocket Close 和 peer Close 都须完整验证；
  可打印服务 event id 使用 ASCII 标识符白名单，防止日志字段注入。

## Automated evidence

本地最终候选通过：

- 48 个 Host unit tests 和 2 个 CLI tests；
- `DashScopeHandshake::verify` 本地 TLS peer：DNS、direct、CONNECT、TLS、Upgrade、分片、401/403/407、
  invalid/missing Close、platform/connect stall 和总 deadline；
- Host all-targets Clippy `-D warnings`、全仓 fast eval、secret scan、source/license audit 和 diff check。

最终 exact-commit GitHub Actions
[`30763338506`](https://github.com/Larkspur-Wang/easy-codex-input/actions/runs/30763338506) 全绿：

- `host-and-protocol`: 70 seconds；
- `ble-peripheral-simulator`: 44 seconds，包含无 UI 的真实 macOS Keychain integration；
- `firmware`: 158 seconds。

保留两次失败历史：`30762291790` 暴露 Linux dead-code/`S_ISVTX` 类型差异；修复后的
`30762446018` 在旧的 interactive Keychain CI 未收敛时被主动取消。最终测试改为同一 adapter 的
background/no-UI 模式，并给 macOS job 设置 10 分钟硬超时；生产导入仍由用户动作使用 interactive create。
后续文档提交 `30762969976` 又暴露 macOS 并行调度下两个过紧的毫秒级测试上限；实现未超时，测试改为
以 2 秒阻塞对照 100 ms platform deadline；跨阶段测试先消耗 700 ms setup，再以 800 ms 总预算进入
connect stall，并要求 1.2 秒内返回，从而同时区分绝对 deadline 与错误的逐阶段重置。

## Independent review

所有独立审查均使用 `gpt-5.6-sol`、reasoning `high`，未使用 Oracle。审查依次发现并推动修复
WebSocket Close、close code 注册表、marker 值、非 UTF-8 proxy、真实 verify 集成链和 event-id 日志注入。
最终源码、portability 修复及 no-UI CI 修复的只读复审均为 `NO_P0_P1_P2`。

## Live service evidence

最终候选重新构建后执行一次性导入，结果为：

- Keychain status: `installed_verified`；
- region: `cn-beijing`；
- requested/accepted model: `qwen3-tts-instruct-flash-realtime`；
- authoritative server event: `session.created`，服务 event id 存在且通过 ASCII 白名单；
- transport: `https_proxy`；
- import 后再次运行 repository secret scan: pass。

这证明最终 Mac Host 可从 Keychain 读回并抵达真实 DashScope 握手，不证明后续 TTS PCM 生成、音频质量、
缓存发布、Cloudflare、手机或实体键盘链路。
