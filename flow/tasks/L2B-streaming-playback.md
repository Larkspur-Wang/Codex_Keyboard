# L2B · 低等待流式播放

## 用户结果

按下 S5-S8 后，不再等待整条缓存音频传完才出声。设备收到并认证一小段预缓冲后开始播放，Host 在播放期间继续传输剩余 EIAD 数据。

## 当前增量

- 保留现有 EIP 身份、HMAC、顺序 offset、六包窗口、ACK、取消和 exact finished/ heard 契约。
- 第一阶段仍按声明总长一次性保留 PSRAM backing，但只等待 24 KiB 预缓冲便启动现有 EIAD decoder/I2S worker；decoder 读取尚未到达的范围时有界等待。
- 传输完成与播放完成是两个独立事实。只有两者都完成、样本数一致且 Host 收到认证 EIPF 后，才消费未读并删除缓存。
- 抢占、超时或 socket/decoder/speaker 失败必须唤醒等待中的 decoder、停止声音并保留未读。
- 第二阶段再将完整 backing 替换成真正有界 ring；不得为了首声速度恢复超过 lwIP mailbox 的 burst。

## 验收

- C++ 单测覆盖 streaming reader 的按需读取、边界、关闭和错误传播。
- 固件 host tests、production ESP-IDF build、Rust playback tests 全绿。
- 真机日志必须先出现 streaming start，后出现 transfer complete；用户确认首声不再等待整段传输。
- 中断测试证明没有 EIPF/Host heard 时缓存与灯保持。
