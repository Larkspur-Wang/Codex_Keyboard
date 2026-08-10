# 开发过程与方法

本项目同时交付 App、固件和一条可公开复盘但不泄露个人数据的开发记录。

## 记录位置

| 内容 | 路径 |
|---|---|
| 产品范围与成功标准 | `flow/charter.md` |
| 阶段合同 | `flow/plan.md` |
| 单项实现和验收 | `flow/tasks/*.md` |
| 关键选择 | `flow/decisions.md` |
| 每轮交接棒 | `flow/进展.md` |
| 脱敏测试和 HIL | `evidence/` |
| 当前产品/协议真相 | `docs/` |

每张任务卡记录问题、范围、自动化、live/HIL、结论和下一步。`static`、`automated`、
`live-service`、`HIL`、`user-accepted` 必须分开，低等级不能替代高等级。

## 仓库抽取方法

`Codex_Keyboard` 从 `easy-codex-input@52949d33c51bac605a33cb8ff42ee3eeab37021e` 抽取：

1. 冻结并验证来源 worktree clean。
2. 只保留 Host、Desktop、Firmware 和 LAN 测试。
3. 删除 PWA、Cloudflare、Relay 和远程协议入口。
4. 重写真相源、计划和证据索引，不让旧路线继续出现在产品合同中。
5. 运行 lockfile、编译、测试、secret/source audit 和独立审查。
6. 公开仓使用新的根提交，README 保留来源 commit 和 Apache-2.0 边界。

原仓保留完整多网络开发历史，新仓从此独立推进本地 Wi-Fi 产品。

## 可公开与禁止公开

可以提交模型名、region、公开 endpoint、依赖版本、来源 commit、脱敏 request id、延迟、计数、
音频格式、固件 SHA 和不含账号信息的真机结论。

禁止提交 API key、Codex auth、真实 task UUID/标题、prompt、transcript、总结正文、麦克风录音、
Wi-Fi 密码、device secret、代理配置或本机绝对私有路径。私有材料放 `evidence/private/` 或 App Support，
并由 `.gitignore` 排除。

## 密钥方法

- DashScope key：App Support 私有 `.env`，只含 `DASHSCOPE_API_KEY`，owner 当前用户、regular、0600。
- cache root key 和 device secret：App Support 下独立私有文件，0600。
- 配网密码：只从 stdin 读取并经 USB HID/BLE GATT 下发到固件 NVS，不进入 argv、日志或数据库。
- 日志只记录 provider、model、阶段、稳定错误分类和脱敏 generation。

每次提交前运行 `scripts/check-secrets.sh`；公开 HIL 证据还需人工检查任务名、录音和网络信息。

## 阶段关闭

每个阶段按以下顺序关闭：

```text
plan -> implementation -> automated tests -> live/HIL -> Sol high review -> commit -> push
```

固件烧录是单独的人机门：形成精确 app SHA 后再次请求用户授权，不能沿用旧候选的授权。
