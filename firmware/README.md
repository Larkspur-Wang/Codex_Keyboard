# Firmware

本目录是 Codex Keyboard 的 EasyInput V2 ESP-IDF 固件。运行时只连接同一局域网内的 Mac Host。

## 来源

2026-08-04 以 `easy-input-keyboard@7a41ce1c5bd961f3be417498cee1a8131ab9d86e` 为审计基线，
复制成熟 V2 硬件实现后收敛为四槽 Codex 产品。详细来源和许可见 `UPSTREAM.md`。

## 硬件与键位

- ESP32-S3R8：8 MB Octal PSRAM。
- 外置 Flash：16 MB。
- S1-S4：槽位 1-4 PTT。
- S5-S8：槽位 1-4 未读总结播放。
- 固件不保存 Codex task UUID。

## 本地网络

- USB HID 或 BLE GATT 只负责下发 S3C Wi-Fi/Host 配置和读取状态。
- 运行时麦克风和扬声器音频只走 Wi-Fi，不通过 HID/BLE 传音频。
- 上行使用 EICC/EIAU 认证 PCM16；下行使用 EIP 控制 HMAC 和 EIPD AES-256-GCM。
- Host 不可达时 fail closed，不缓存 task prompt，也没有远程回退。

## 音频语义

- 麦克风：16 kHz mono PCM16，采集和发送 worker 隔离。
- 播放：48 kHz EIAD/IMA-ADPCM，复用现有 decoder/I2S worker。
- PTT 绝对优先于播放；抢占不能消费未读 summary。
- 只有 I2S 最后一帧和尾静音实际完成后才发送匹配 generation 的 EIPF。

当前候选先完整下载 EIAD 到 PSRAM，以便尽快做真实扬声器 HIL；最终版本还需有界 ring 边下边播。

## 验证

```bash
cmake -S firmware/host_test -B firmware/build-host-test
cmake --build firmware/build-host-test --parallel
ctest --test-dir firmware/build-host-test --output-on-failure
```

产品构建固定 ESP-IDF v5.5.5 和 `esp32s3`。未经用户针对精确镜像 SHA 授权，不烧录实体键盘。
