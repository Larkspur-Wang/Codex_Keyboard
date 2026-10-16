# L4R Windows 启动与语音连接故障

- 用户报告 exe 闪一下退出，语音无法连接；以最新进展中的 Windows 修改为现场基线。
- 已确认 Host live health=ready、UDP 17333 正在监听、DashScope key 已配置；沙箱内 socket 拒绝不能当作现场故障。
- 分别定位 CLI 双击行为和 Tauri/WebView2 启动；检查键盘认证链与网络，不输出凭据、任务 UUID、口述或录音。
- 不清空绑定、数据库、缓存或 WebView 用户目录；不更改固件，不复位/烧录实体键盘。
- 软件检查、Host live 和实体 C4 验收分别记录；本轮不调用会修改真实绑定或发送模型请求的 live 测试。

## 已确认的现场结果

- 用户确认窗口正常保持，最终 S1 完成后第 1 颗灯亮，S5 可以听到播报。
- Host 记录真实 ASR、DeepSeek final/成功退出、generation 1 发布以及匹配的 device_finished/heard；槽 1 未读清零。
- 最终根因是 exe 和应用目录的 Low MIC 标签；恢复 Medium 后原生 DeepSeek 可正常使用自己的用户目录。
- 用户明确保留 DeepSeek 后，改用 stdin 原生 CLI、并发读管道、超时取消与 Host 私有完成事务；不再改写 Codex rollout。
- Windows 测试使用仓库临时目录，默认测试移除真实账号、绑定和模型调用；人工诊断使用脱敏 connection_probe。
- C4 其余项目未代填。无固件改动、ESP-IDF build、复位或烧录。
