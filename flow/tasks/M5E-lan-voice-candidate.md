# M5E / M6A · 同 Wi-Fi 单槽语音候选

## 目标

把已经复制进本仓的 V2 麦克风/Wi-Fi 基线与 Easy Codex 四槽状态机接起来，形成第一个可以安全烧录的
候选：按住上排槽键采集 16 kHz PCM，经同一 Wi-Fi 发给 Mac Host，默认由北京区
`qwen3-asr-flash` 转写后只通过既有 binding 与 durable FIFO 投递到对应 Codex task；本地
whisper.cpp 只作离线兜底。

## 配置链路

- 正式产品仍由 Easy Codex App/PWA 复用 EasyInput 的 `S3C` 配置 service UUID 与分片/CRC 合同。
- 第一轮真板使用本仓 CLI 走已连接 USB HID Feature `0x10`；payload 与 BLE GATT 完全相同，避免在
  语音链路尚未通过前同时调试 CoreBluetooth 权限和广播窗口。
- CLI 参数只包含 SSID、Mac LAN 地址和端口；Wi-Fi 密码从 stdin 读取。固件保存到 `config_v2` NVS；
  Host 不保存 Wi-Fi 密码，仓库与日志不得出现它。

## 运行合同

- PTT start 只有在当前 Wi-Fi route generation 可用时成立；session identity 必须可无歧义还原
  slot、capture generation 和 route generation。
- 每个 EIAU v3 frame 必须携带 device secret 派生的 HMAC-SHA256 截断 tag，并且是 16 kHz、mono、
  PCM16、严格连续 sequence；伪造、缺帧、乱序、重复冲突、超限或 session identity 非法都 fail closed，
  不得形成 Codex prompt。
- 松开后固件停止精确 session，等待 capture worker 结束并按序排空已采集尾帧，再重复发送经过认证的
  `EIAE` terminal；Host 只在收到同 session/source/identity 的 terminal 且所有连续帧齐全后收敛，
  15 秒 idle 只能中止残缺 capture，不能生成 prompt。默认把内存 WAV 以 Base64 Data URL 同步提交给
  北京区 `qwen3-asr-flash`，不把录音上传到公共 URL；只有未配置 credential、429 或远端/传输不可用
  时才使用 mode-0600 临时 WAV + 本地 whisper.cpp，401/403、坏音频和协议错误必须原样 fail closed；
  成功或失败后都删除本地临时音频。
- 转写结果必须非空、UTF-8 且有界；request id 只由 session identity 确定，设备每次启动以硬件随机数
  播种 route/capture generation，重复数据只能得到同一 durable
  enqueue outcome，不能产生第二个 Codex turn。
- 本批不实现下排播放、EIAD 下行、Cloudflare、PWA bridge 或最终相互认证；这些仍由 M5/M6 后续任务
  关闭，不能用本批 HIL 冒充。

## 验收

1. 纯软件 fixture 覆盖 PTT -> authenticated PCM -> Qwen/whisper adapter -> slot enqueue，以及所有
   fail-closed 边界；另有一次北京区真实 Qwen ASR 调用证据。
2. Firmware Host tests、Rust tests、`eval-fast`、锁定 ESP-IDF build 全绿。
3. Sol high 独立审查无 P0/P1；提交、推送、exact CI 全绿。
4. 用户对精确镜像授权后烧录；记录设备身份、固件 SHA-256、SSID 仅记脱敏标签、Host LAN IP/port。
5. 槽 1 真机连续 20 轮无串槽、无丢 release、无重复 job；失败轮保留原因，不重跑掩盖。
