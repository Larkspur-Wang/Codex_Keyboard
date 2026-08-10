# 进展日志（新的放最上面）

## 2026-08-10 · 优化 Spark 总结顺序与 Qwen TTS 听感 · Codex

- 总结内容：只调整 Spark prompt，不向 `spoken_text` 塞入固定正文。新播报立即以最后一条
  completion 的最新具体结果开头，再压缩融合仍相关的旧未听结果/决定；只在来源确有
  可执行待办时才以下一步收尾，不再加“可以继续”类空泛结尾。
- 播报长度：普通 1~5 条 completion 以约 120~180 个中文字符为听感目标，但有重要旧未听
  内容时允许超出，不为了追求短而丢掉有用信息；原 480 语义字符/150 秒安全边界未变。
- TTS 听感：保留 `Cherry` 和北京区 `qwen3-tts-instruct-flash-realtime`，指令改为自然同事式女声、
  中等偏快、句间短停顿，弱化播音腔/客服腔，并让英文技术词、数字和缩写按自然语境朗读。
- 验证与部署：Host 266/266 和 `./scripts/eval-fast.sh` 全绿，包括 CLI 6、parent-death 6、
  runtime gate 4、Desktop 4、firmware host 57、secret scan 和 source/license audit；release Host
  已升级为 PID 71113，health `ready`。
- 边界：已缓存的旧音频不会被重写；从下一条新完成任务开始使用新 prompt 和新音色指令，
  真实“更自然/更快”听感以下一条键盘播放为物理验收点。

## 2026-08-10 · 四槽语音信箱真机闭环完成 · Codex

- 真机验收：S1~S4 分别完成按住语音输入、ASR 和对应 Codex 任务入队；
  左起第 1~4 颗灯分别表示四槽未听数量，第 5 颗保持熄灯；S5~S8 分别播放对应槽位的
  Spark `spoken_text`，用户已确认四槽全部可用。
- 收敛证据：S3 generation 4 / coverage 4 与 S4 generation 4 / coverage 5 都完成
  `begin_ack -> transfer_complete -> device_finished -> heard`；四槽 `unread_generation` 均为 null，
  已听缓存删除且对应灯熄灯。
- 根因修复：旧 TTS `Ambiguous` 且无认证缓存时不再永久占用槽位；Host 原子释放
  claim 但保留 completion，下一轮可直接重新总结和合成，S3/S4 堵塞已现场解除。
- 自动验证：`./scripts/eval-fast.sh` 全绿，Host 265、CLI 6、parent-death 6、runtime gate 4、
  Desktop 4 和 firmware host 57 项测试全部通过；格式、Clippy、桌面构建、secret scan 与
  source/license audit 均通过。
- 下一步：不再增加摘要内容硬编码；根据真实听感小幅调整 Spark prompt 的“最新结果优先”和
  Qwen TTS 的自然女声、语速与停顿，保持未听内容累计语义。

## 2026-08-10 · 修复 S3/S4 摘要被旧 TTS 模糊状态永久阻塞 · Codex

- 现场根因：S3/S4 的 ASR 和 Codex 入队都成功，但两个任务各保留一条旧 generation 3 TTS
  `Ambiguous` 尝试。旧逻辑为避免重复调用 TTS，一直保留 `generating` 并要求人工对账，后续 completion
  因此只能持续得到 `another summary generation is already running`，不会产生缓存或亮灯。
- 修复：`SummaryOrchestrator` 现在先尝试恢复已认证缓存；如果根本没有可播放的认证产物，就原子
  abandon 旧 TTS 尝试、保留原 completion，并立即允许新 generation 重试。同样处理 TTS 模糊回执、
  错音色和无认证缓存的 commit 失败，不再把槽位永久占住。
- 自动验证：Host 265/265 测试通过，包含“已持久化 Ambiguous 在重启后释放”、“TTS 模糊回执无需
  重启即释放”、“缓存 commit 失败释放”和“错音色释放”回归；release Host 已升级到 PID 40959。
- 运行验证：两条旧 generation 3 均自动 abandon，随后 S3 发布 generation 4 / coverage 4，S4 发布
  generation 4 / coverage 5；dashboard 已同时标记两槽未读。
- 下一步：用户确认物理第 3/4 灯已亮，再按 S7/S8 分别播放至完成；Host 验证两槽精确
  `device_finished -> heard`、缓存删除且对应灯熄灭，之后再提交本批。

## 2026-08-10 · 用户经 EasyInput 恢复键盘 LAN 配置 · Codex

- 做了什么：用户直接用 EasyInput 重新写入键盘配置；Codex 不再访问钥匙或改写配置，只通过
  BLE 状态和 Host 进程网络计数做只读验证。
- 验证：键盘回报新配置 `saved=true` / 1123 bytes / CRC16 3878；Host PID 29801 每 2 秒稳定收到
  80 bytes 心跳并回复 32 bytes，连续四轮一致，说明键盘到 `192.168.1.70:17333` 的 LAN 控制链已恢复。
- 信箱状态：Host dashboard 仍保留 S1 generation 12 / coverage 1，S2-S4 无未读；本轮未重启 Host、
  未消费音频。
- 下一步：先由用户确认物理左一灯已恢复，再按 S5 播放 S1 的旧未听；Host 同步追踪
  `begin_ack -> transfer_complete -> device_finished -> heard`，最后确认缓存删除和左一灯熄灭。

## 2026-08-10 · 通过 BLE 读取重启后的键盘真实状态 · Codex

- 做了什么：不改键盘配置，通过已连接的管理 BLE 特征直接读取固件状态；确认运行固件为
  `0.5.0-easy-codex-lan-playback`，保存配置 `saved=true` / 1124 bytes / CRC16 15215，音频能力已启用且麦克风
  `mic_ready`。
- 根因：键盘 NVS 中的 Host 是 `192.168.1.71:17333`，而 Mac 当前是 `192.168.1.70`；重启后固件
  状态停在 `wifi_preparing`，Host 收不到心跳。Wi-Fi 密码未被判定为变更，真正需要更新的只是
  Host 地址。
- 安全边界：现有 `provision-lan` 是整份原子配置，为了保留原密码必须原样带回；尝试从
  System Keychain 读取时发现 Mac 已锁屏，因此立即中止等待。本轮未读出/打印密码、未改 `.env`、
  未下发配置、未重启 Host，S1 未听仍保留。
- 下一步：用户解锁 Mac 后，一次性读取原密码并更新 Host 为 `192.168.1.70`，写入 0600 本机
  `.env` 并经 USB HID 收到精确保存 ACK；随后验证键盘心跳、左一灯恢复和 S5 听完后灯熄灭。

## 2026-08-09 · 板子重启后信箱灯不恢复的现场定位 · Codex

- 做了什么：通过 Host 唯一的 dashboard IPC 确认 S1 的 generation 12 仍是未听，缓存没有丢；
  键盘 USB HID 也仍正常枚举。同时确认 Host PID 29801 正在 UDP 17333 监听，但连续 5 秒
  进程网络采样为 0 bytes，说明重启后键盘没有向当前 Mac 发心跳，灯不亮不是信箱状态被清除。
- 怎么理解：板子 `192.168.1.15` 可 ping，Mac 当前是 `192.168.1.70`；故障点收窄到板子 NVS 中
  保存的 Host 端点/鉴权配置没有收敛到当前 Host，需要通过 USB HID 重新下发完整 LAN 配置。
- 问题→解决：macOS 钥匙读取连续等待授权但未获得结果，已中止等待进程，没有打印密码、
  没有下发空密码，也没有重启 Host 或消费 S1 未听。
- 下一步：用户完成一次“始终允许”钥匙授权后，立即把 SSID/密码/Host/Port 保存到
  `~/Library/Application Support/EasyCodexInput/.env`（0600）并 USB HID 下发；然后先验证左一灯恢复，
  再按 S5 完整追踪 `device_finished -> heard -> 灯熄灭`，期间不重启 Host。

## 2026-08-08 · 摘要收窄为助手最终回复并优化预下载等待 · Codex

- 做了什么：确认刚才 S1/S2 实际绑定的是测试任务 `hi`/`hi-2`，不是当前 `easy-codex-input`；旧实现会把
  turn 的用户消息、工具事实和多条过程回复一起交给 Spark。现在以权威 `task_complete.last_agent_message`
  覆盖中间助手消息，并在 Spark prompt 构造时只序列化每个 completion 的最后助手回复，旧 turn pack
  也按同一规则读取；未听旧摘要仍按原有精确累计合同保留。
- 延迟原因与优化：TTS 已预生成并加密缓存于 Mac，但固件在 PSRAM 收完整段 EIAD 后才启动扬声器；本次
  S1 为 67 秒、1.66 MB，因此首声前包含完整 LAN 传输。Host 有界发送窗口由 4 包提升到 16 包，固件单轮
  接收由 8 包提升到 16 包，并新增无正文的 `elapsed_ms` 传输计时；真正边下边播仍归 L2B。
- 验证与部署：`eval-fast` 全绿（Host 259、CLI 6、parent-death 6、runtime gate 4、Desktop 4、firmware
  host 57），secret/source/license audit 全绿；ESP-IDF v5.5.5 production build 成功，Mac LaunchAgent 已
  升级且 health ready。听完的 S1/S2 已收到 exact `device_finished -> heard`，缓存删除且两槽无未读。
- 当前边界与下一步：新固件 app SHA 为
  `b66d886f09ef83c9135481e772bf82e1ff686040994344411eac55ac534e2c69`（bootloader/partition 未变）；
  等用户对三个精确 SHA 授权后按 `usb_reset` SOP 烧录，再用新生成的助手最终回复摘要测量首声等待与内容可懂度。

## 2026-08-08 · 固化 USB debug reset 烧录法并写入 L2A 固件 · Codex

- 做了什么：把 V2 板的强制烧录 SOP 写入根 `AGENTS.md`；烧录方先运行端口等待与
  `esptool --before usb_reset --connect-attempts 0`，用户只需断电再上电，不再默认要求按 BOOT，
  并明确禁止已两次导致 `Device not configured` 的 `--before no_reset` 做法。
- 固件与验证：按用户对精确 SHA 的授权，仅写入 `0x0` bootloader
  `4f53dc9147728131b891a5a8a2524ad2bb60c43cb573db8827b29b2bdba0d314`、`0x8000` partition
  `7c541b70dcac8f920c2d11589f06745e1b033fa9b95b8343de2748bb8312a278`、`0x10000` app
  `62c8488db4876f2cbfaabbadc0d44343dceeda05cddc3ef44c1b42b55203ed1f`；三段均完成
  `Hash of data verified`，未擦 NVS/声音数据，复位后已重新枚举为 `EasyInput AI`。
- 自动化：本批完整 eval 全绿，Host 259 项、firmware host 57 项及 secret/source/license audit 均通过。
- 当前边界与下一步：S1 音频已完成 Host 到设备的传输，未再出现重复按键导致的 `status=3`；仍需用户
  确认真机完整可听，并等待匹配的 `device_finished -> heard -> cache delete -> 第一槽灯熄灭`，之后再单独测 S6。

## 2026-08-08 · L1 同 Wi-Fi 中文语音闭环真机可听 · Codex

- 做了什么：完成四槽 Desktop/Host IPC、DashScope 北京区状态、S2 PTT/ASR、Codex task FIFO、Spark 中文累计摘要、Qwen realtime TTS 加密缓存和 S6 键盘扬声器回放；用户已确认真机有声音。
- 固件与运行时：最终烧录 `bootloader=6ba36341b4f47b1e10d6d6017c12fbdb102fa66eccec8ce42841c99e803e2d07`、`partition=7c541b70dcac8f920c2d11589f06745e1b033fa9b95b8343de2748bb8312a278`、`app=b06934cfc8f3636bac779bda24485c87afe592734703c5fbd04c7d317ab18873`；保留 NVS。Host live health 正常，Desktop 未签名 macOS bundle 构建成功。
- 问题与解决：修正长摘要被开机音上限拒绝、Host/固件 EIAD 布局不一致、失败后 Host 长等、单分片停等过慢、TTS ambiguous 恢复卡住和英文摘要；播放改为四分片窗口，重复 gap ACK 有界重试，纯英文在 TTS 前仅允许一次有界再生成，否则保留 completion 且不合成。
- 验证：S6 实机完成 begin/ack、270686-byte 传输、device finished、exact heard 和缓存文件删除；`eval-fast` 全绿（Host 248、CLI 6、parent-death 6、runtime gate 4、Desktop 4、firmware host 56），secret/source/license audit 全绿。Sol high 首审 `P0=0/P1=0`，两个 P2 修复后复审关闭。
- 当前边界与下一步：本批代码可提交推送；L1 仍保留最后一个真机门禁，需生成新未读并在 S6 播放时按 S2，确认 PTT 立即抢占且未读不被误消费，之后再进入 ring streaming 和长稳测试。

## 2026-08-08 · L1 固件候选已构建，等待精确 SHA 烧录授权 · Codex

- 做了什么：在新仓用 ESP-IDF v5.5.5 从 `db738c9` 构建 `esp32s3` / 16 MB Flash / 8 MB Octal
  PSRAM 候选；app 大小 `0x18eb40`。新增 L1 任务卡，把四槽 Desktop 绑定、DashScope 状态和同 Wi-Fi
  真机闭环纳入同一阶段。
- 候选：`0x0 bootloader=434b1831f7cb0ab04fc5402022bf90f6ab738ce69be78927e5ac5d3d8c52ea3d`；
  `0x8000 partition=7c541b70dcac8f920c2d11589f06745e1b033fa9b95b8343de2748bb8312a278`；
  `0x10000 app=50e402e271b4a60ea53610bf49a35315f705f864799b38505b2ddc8574813663`。
- 当前边界：尚未复位或写入实体板；板子处于 TinyUSB HID 状态，没有稳定串口属于预期。Host live health、
  Codex catalog 和 DashScope 私有 `.env` 均已确认 ready，未读取或输出 key。
- 下一步：用户确认上述精确 SHA 后，先启动 ROM 端口轮询，再执行“USB 保持连接、关电源、按住 BOOT、
  开电源并继续按住”抓上电窗口，只写三个固件地址且不擦 NVS；烧录后继续四槽 App 和 HIL。

## 2026-08-08 · 从多网络工程抽取 Codex Keyboard 本地 Wi-Fi 仓 · Codex

- 做了什么：以 `easy-codex-input@52949d33c51bac605a33cb8ff42ee3eeab37021e` 为冻结来源，建立
  `Codex_Keyboard` 独立目录并连接公开仓库 `Larkspur-Wang/Codex_Keyboard`；原仓保持 clean 且 HEAD
  不变。新树删除 PWA、Cloudflare Relay、远程 envelope 和相关证据/计划，只保留 Mac Host、Tauri、
  ESP-IDF 固件、USB/BLE 配网、LAN PTT、自动总结、加密缓存和 LAN 扬声器播放。
- 为什么：当前优先目标是尽快把同 Wi-Fi 真键盘闭环跑通，不再让远程网络、手机代理和浏览器生命周期
  扩大调试面。公开新仓使用无旧历史的根提交，但在 README/任务卡保留精确来源 commit 和许可证边界。
- 验证：`eval-fast` 全绿（Host 238、CLI 6、parent-death 6、runtime gate 4、Desktop 2、固件 56），
  secret/source audit 全绿。Sol high 首轮发现旧历史和 `source` push 跟踪两个 P1；已改为单一无父根提交、
  `main -> origin/main`，并禁用 `source` push，复审为 `P0=0/P1=0`。
- 当前边界：新仓 `main` 已推送到公开仓；实体键盘保持正常 HID，本轮不复位、不烧录、不消费保留的
  未读音频。误建的空私有仓 `easy-codex-input-local-wifi` 已归档，当前 token 无删除权限。
- 下一步：在新仓锁定 ESP-IDF v5.5.5 重新构建候选镜像，记录精确 SHA，请求烧录授权后进行 S6
  同 Wi-Fi 可听播放 HIL。
