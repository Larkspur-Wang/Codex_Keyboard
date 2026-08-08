# L1 · 固件烧录、四槽绑定与同 Wi-Fi HIL

## 目标

把当前软件候选变成可操作的本地产品切片：Mac App 选择四个 Codex task，常驻 Host 持久化绑定，
EasyInput V2 在同一 Wi-Fi 上完成 PTT、ASR、Codex FIFO、总结、TTS 和扬声器播放。

## 固件门禁

1. 只使用 ESP-IDF v5.5.5、`esp32s3`、16 MB Flash、8 MB Octal PSRAM 配置构建。
2. 记录 bootloader、partition table、app 三个烧录对象的 SHA-256 和地址。
3. 用户针对这组三个精确 SHA 授权后才操作实体板。
4. V2 的 BOOT 只在上电采样：USB 保持连接，电源关闭，按住 BOOT，再打开电源并继续保持；工具必须先轮询，
   抓住短暂出现的 ROM USB-Serial/JTAG 下载窗口。
5. 只写 `0x0`、`0x8000`、`0x10000`，不擦除 NVS、配网、蓝牙和音频数据分区。

## Desktop 最小闭环

- App 通过私有 Unix socket 向常驻 Host 请求任务目录、四槽绑定和 DashScope 状态。
- 绑定写入由持有 SQLite owner lock 的 Host 主循环执行，并使用 binding generation CAS。
- UI 每槽显示上排 PTT 键、下排播放键、任务名、项目名和保存状态。
- UI 不显示完整 task UUID、Wi-Fi 密码、API key、prompt、transcript 或录音。
- ASR 固定北京区 `qwen3-asr-flash`；TTS 固定北京区
  `qwen3-tts-instruct-flash-realtime`，音色继续使用 `Cherry`。

## HIL 顺序

1. 烧录后确认 PSRAM、USB HID、Wi-Fi 和 LAN Host 心跳。
2. 用 App 绑定四槽并回读 Host 权威状态。
3. 按住 S2 说话，松开后确认 DashScope ASR 文本只进入槽 2 task FIFO。
4. 等待该 task 权威完成、Spark 累计总结和 DashScope TTS 缓存发布。
5. S6 完整播放并确认 exact heard/cache deletion。
6. 新建未读后在 S6 播放期间按 S2，确认 PTT 抢占且未读仍保留。

## 验收

- 自动化、ESP-IDF build、Host live、真机日志和可听结果分开记录。
- Sol high 独立审查 `P0=0/P1=0`。
- 本阶段单独提交并推送后才进入长期压力和 ring streaming 收敛。

## 2026-08-08 脱敏证据

- ESP-IDF v5.5.5 候选已抓住上电 ROM download 窗口烧录，三个地址均完成写后校验；NVS、配网和音频数据分区未擦除。
- Host 从私有本地 `.env` 读取 DashScope 北京区配置；S2 真机 PTT 已走 `qwen3-asr-flash`，任务完成后 Spark 生成中文累计摘要并由 `qwen3-tts-instruct-flash-realtime` / `Cherry` 生成缓存。
- S6 真机回放完成了 authenticated begin、四分片窗口传输、device finished、exact heard 和缓存文件删除；用户确认键盘扬声器可听。
- 处理了长摘要上限、Host/固件 EIAD 格式边界、固件失败回执、传输超时、重复 gap ACK 重试上限、TTS ambiguous 人工恢复以及纯英文播报门禁。
- `eval-fast` 全绿：Host 248、CLI 6、parent-death 6、runtime gate 4、Desktop 4、firmware host 56；secret/source/license audit 全绿。Desktop 未签名 macOS App bundle 构建成功。
- Sol high 首审 `P0=0/P1=0`，两个 P2 修复后复审关闭；当前剩余真机门禁仅为“播放期间 S2 抢占且未读保留”。
