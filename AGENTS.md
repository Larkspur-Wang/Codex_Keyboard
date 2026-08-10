# Codex Keyboard 协作约定

> 产品真相源是 `docs/总体方案.md`，执行顺序以 `flow/plan.md` 为准，最新交接见
> `flow/进展.md` 顶部。

## 产品边界

- 本仓只做同一局域网内的 `Keyboard <-> Mac Host` 产品。
- 不实现手机 PWA、Cloudflare、Relay、远程 WSS、热点代理或公网访问。
- 上排 `S1-S4` 是槽位 1-4 的 PTT；下排 `S5-S8` 是槽位 1-4 的未读总结播放。
- 固件不保存 Codex task UUID；`slot -> task` 的唯一真相源是 Mac Host SQLite。
- Tauri App 是本地 Host 的管理界面，不拥有第二份状态。
- TTS 固定由 Host 调用北京区 `qwen3-tts-instruct-flash-realtime`；ASR 默认
  `qwen3-asr-flash`。
- 左起第 1-4 颗灯只表示槽 1-4 的未听信箱；最右第 5 颗灯只表示不同已绑定任务的运行数，
  0/1/2/3/4 个依次为绿/黄/橙/紫/红，全部完成后回绿。

## 不可破坏的语义

- 同一 Codex task 严格 FIFO，prompt 只能经 stdin 传给 `codex exec resume`。
- 只有 Codex rollout 权威 `task_complete` 才能触发总结。
- 未听总结必须滚入下一次总结；新 generation 完整发布后才能替换旧 generation。
- 只有固件实际播放完毕并回传匹配 generation，Host 才能标记 heard 和删除缓存。
- PTT 永远优先于播放；取消、断线、超时和失败不能消费未读总结。
- EIAU/EIP 控制和音频包必须认证；EIPD 音频必须加密，旧 generation/replay fail closed。
- `EIMB v3` 的运行数只能来自 rollout 权威 `task_started -> task_complete` 区间；固件不得猜测。
- 未经用户针对候选镜像精确 SHA 明确授权，不复位或烧录实体键盘。

## Secret 边界

- DashScope key：`~/Library/Application Support/EasyCodexInput/.env`，mode `0600`。
- cache/device secret：同一 App Support 根下的 mode `0600` 私有文件。
- 不把 key、真实 task UUID、prompt、transcript、录音或家庭 Wi-Fi 凭据写入仓库、日志或证据。

## ESP32-S3 烧录 SOP（强制）

这块 V2 板的 BOOT 进入时序不稳定。烧录方必须先启动等待与 ESP32-S3 原生 USB
debug reset 握手，用户只执行一次“断电，再上电”；不要默认要求用户按住 BOOT。

1. 使用仓库固定的 ESP-IDF v5.5.5 构建，并核对目标为 `esp32s3`、Flash 为 16 MB。
2. 对 `bootloader.bin`、`partition-table.bin`、`easy_codex_input.bin` 分别计算 SHA-256；
   报告三个完整 SHA，并得到用户针对这三个精确镜像的明确授权后才可烧录。
3. 先停止遗留的端口 watcher/esptool，随后由烧录方先启动下面的等待命令；命令必须使用
   `--before usb_reset --connect-attempts 0`，不能退回 `--before no_reset`。
4. 命令已运行并等待后，只请用户断电再上电。端口出现时立即由 `usb_reset` 抓取 ROM
   downloader；只有该路径被实证失败后，才单独诊断 BOOT 键，不把按 BOOT 当成默认步骤。
5. 只写 `0x0` bootloader、`0x8000` partition table、`0x10000` app；不得擦除 NVS、
   Wi-Fi 配置或开机/提示音数据分区。
6. 三段都出现 `Hash of data verified` 才算写入成功；自动 hard reset 后还要检查设备重新枚举，
   并把真机按键、灯光和扬声器行为作为独立 HIL 门禁，不能用构建或日志代替。

标准命令模板（从 `firmware/` 执行）：

```sh
python -m esptool --chip esp32s3 -p "$PORT" -b 460800 \
  --before usb_reset --after hard_reset --connect-attempts 0 write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/easy_codex_input.bin
```

已知错误做法：对瞬时 `/dev/cu.usbmodem*` 使用 `--before no_reset` 会在上电枚举后报
`Device not configured`，既抓不到下载窗口，也不会写入任何镜像；不得重复这种方法。

## 来源边界

- 抽取源：`easy-codex-input@52949d33c51bac605a33cb8ff42ee3eeab37021e`。
- 固件基线：`easy-input-keyboard@7a41ce1c5bd961f3be417498cee1a8131ab9d86e`。
- 原仓只读；后续产品开发只在 `Codex_Keyboard` 中进行。

## 工作方式

1. 开工读 `flow/charter.md`、`flow/plan.md`、`flow/进展.md` 顶部和本轮任务卡。
2. 先改计划再实现；跨 Host/固件协议先写共享 golden vector。
3. 软件、Host live service、ESP-IDF build 和真机 HIL 分开报告。
4. 每阶段完成后测试、真机验收、提交并推送，再进入下一阶段；当前按用户要求不启动子智能体或额外独立审查。
5. 收工在 `flow/进展.md` 顶部追加交接棒，并在回复中原样贴出。
