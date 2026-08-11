# Codex Keyboard

Codex Keyboard 是 EasyInput V2 的纯局域网版本。键盘和 Mac 必须连接同一个 Wi-Fi，不使用手机
PWA、Cloudflare Relay、公网端口或远程桥。

- `S1-S4`：按住说话，松开后由 Mac Host 调用 Qwen ASR，并把文本按任务 FIFO 投递到 Codex。
- `S5-S8`：播放对应任务最新的未读总结。
- 旋钮：逆时针降低板载扬声器音量，顺时针提高；短按播报当前音量，长按 3 秒进入配置模式。
- 状态灯：左起四灯对应槽 1-4，各槽未听 completion 越多越亮，完整听完后熄灭；第五灯显示
  0-4 个运行任务（绿/黄/橙/紫/红）。
- Mac Host：持有四槽绑定、Codex 队列、完成观察、Spark 总结、Qwen TTS 和加密音频缓存。
- Firmware：只知道槽位号，负责 Wi-Fi 音频、麦克风、扬声器、按键、PSRAM 和 I2S。

## 目录

- `app/host/`：Rust LaunchAgent 常驻服务。
- `app/desktop/`：Tauri 2 macOS 管理 App。
- `firmware/`：ESP32-S3 / EasyInput V2 固件。
- `docs/总体方案.md`：本地 Wi-Fi 产品边界和数据所有权。
- `docs/端到端架构.md`：架构图、语音输入、总结和播放时序。
- `docs/协议草案.md`：当前 LAN 音频协议合同。
- `flow/`：计划、决策、任务卡和进展记录。

## 来源

本仓从 `Larkspur-Wang/easy-codex-input@52949d33c51bac605a33cb8ff42ee3eeab37021e`
抽取。新仓只保留本地 Wi-Fi 所需的电脑 App、Host、固件和测试；原仓继续独立存在且未被修改。

## 当前状态

本地 Host、语音上行、累计自动总结、150 秒加密音频缓存和 LAN 播放已经连通，桌面 App 安装于
`/Applications/Codex Keyboard.app`。当前固件已烧录并完成真机验证：四路任务可并发到红灯，板载
十档音量、短按音量播报和四槽语音信箱均可用；长稳、断网和播放中 PTT 抢占仍属于后续压力测试。

```bash
./scripts/eval-fast.sh
```

密钥只保存在 `~/Library/Application Support/EasyCodexInput/` 的私有文件中，不进入仓库。

## 训练营材料

- 学员手册：`docs/训练营学员手册.md`
- PowerPoint：`projects/codex-radio_ppt169_20260810/exports/Codex任务电台_课程案例_Lark.pptx`
- PDF：`projects/codex-radio_ppt169_20260810/exports/Codex任务电台_课程案例_Lark.pdf`

课程案例名为《Codex 任务电台：一把键盘，远程开发》。课件共 36 页，包含产品动机、真实键盘与
App、端到端链路、关键实现、踩坑过程、现场演示流程和技术附录。
