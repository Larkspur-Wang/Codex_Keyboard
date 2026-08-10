# L2A · 信箱灯、摘要质量、长播报与桌面安装

## 用户结果

1. 左起四颗灯分别代表槽 1-4；对应槽有未听语音信箱时显示绿色，槽内累计总结覆盖的 completion 越多越亮，听完后该槽熄灭。第五颗灯不参与信箱显示。
2. Spark 摘要必须说明具体完成内容、结果、待办和决策。新 completion 到来而旧摘要未听时，旧摘要参与下一代累计总结。
   每个新 completion 只取该次任务的助手最终回复；用户输入、工具调用、过程消息和测试日志不进入 Spark prompt。
3. 单条播报允许到 150 秒；这是当前完整 EIAD 预下载和 4 MB 设备传输上限下的安全范围，不用 90 秒过早截断有意义内容。
4. macOS App 使用项目专属图标，并实际安装到 `/Applications/Codex Keyboard.app`。

## 协议边界

- 键盘周期性 EIHB 心跳作为 challenge；Host 返回固定长度、HMAC 认证的 EIMB 状态，回显刚收到的 heartbeat sequence。
- EIMB v2 只携带四槽未读 bitmask 和四个逐槽 coverage byte，不含 task UUID、摘要正文或缓存标识。
- 固件只接受已配置 Host IPv4、正确 HMAC、匹配最近 heartbeat sequence 的状态，旧包和伪造包不能改变灯光。
- 信箱灯是持久背景状态；左起四灯逐槽显示，第五灯熄灭。物理输入、录音、播放、配置和错误反馈可以临时覆盖，结束后恢复当前信箱状态。

## 验收

- Rust/C++ 共享 golden vector，覆盖认证失败、序号不匹配、coverage 饱和和亮度单调性。
- 摘要测试覆盖：具体性门禁、一次有界修复、旧未听摘要进入 prompt、二次失败不调用 TTS且 completion 保留。
- prompt 隔离测试必须证明只含每个 completion 的助手最终回复，且不含同一 turn 的用户消息、工具事实或中间助手消息。
- 完整预下载阶段把 UDP 窗口和设备单轮接收批次提升到有界值，并在 Host 记录无正文的传输耗时；边下边播继续作为 L2B 独立改造。
- 150 秒 PCM/WAV/EIAD 边界接受，151 秒拒绝；设备 EIAD 仍不超过 4 MB。
- ImageGen 原始图、Tauri icon set、bundle 和 `/Applications` 安装结果分别留证。
- 真机至少验证槽 2 仅第二灯亮、coverage 1 与 coverage 2+ 的亮度递增、第五灯不亮，以及听完后第二灯熄灭。
