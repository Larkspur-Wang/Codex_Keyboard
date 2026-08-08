# L0 · 本地 Wi-Fi 仓库抽取

## 问题与目标

原 `easy-codex-input` 同时规划 LAN、PWA 和 Cloudflare。当前需要一个只服务于
`EasyInput V2 <-> same-Wi-Fi Mac` 的公开仓库，并在该仓继续完成真机播放。

## 冻结来源

- Source repo：`https://github.com/Larkspur-Wang/easy-codex-input`
- Source commit：`52949d33c51bac605a33cb8ff42ee3eeab37021e`
- Firmware upstream：`easy-input-keyboard@7a41ce1c5bd961f3be417498cee1a8131ab9d86e`
- License：Apache-2.0

原仓只读校验：抽取前后都必须保持 source worktree clean 且 HEAD 不变。

## 保留

- `app/host`：绑定、ASR、FIFO、Codex、observer、Spark、TTS、cache、LAN voice/playback。
- `app/desktop`：本地 Tauri 管理 App。
- `firmware`：V2 固件、USB/BLE 配网、Wi-Fi PCM、PSRAM、EIAD/I2S、按键和电源管理。
- 本地测试、固件侧 Codex device simulator、锁定 IDF build 和 M2-M5 脱敏证据。

## 删除

- PWA、Cloudflare Worker/Durable Object、远程 Relay、手机播放和热点代理。
- 只服务于远程 envelope 的 Rust/TypeScript shared protocol。
- PWA/Relay 专项计划、目标、证据和构建入口。

## 验收

1. `rg` 不再发现产品性 PWA/Cloudflare/Relay runtime 或依赖。
2. Cargo workspace 只有 Host 与 Desktop，pnpm workspace 只有 Desktop。
3. `eval-fast` 全绿，secret/source audit 全绿。
4. 原仓 HEAD/worktree 未变。
5. 新公开仓的 `main` 是无旧产品历史的根提交，并记录来源 commit。
6. Sol high 独立审查 `P0=0/P1=0` 后推送。

## 真机边界

本任务不烧录。下一任务重新生成候选 SHA，并在用户明确授权后测试 S6 可听播放和 exact deletion。
