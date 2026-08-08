# M5G/M6B 同 Wi-Fi 总结播放候选

## 目标

把下排 `S5~S8` 的播放请求接到 Mac Host 当前未读总结：Host 只读取匹配槽位和 generation 的
认证加密 EIAD，设备在 PSRAM 中完成有界接收后通过现有 I2S 扬声器路径播放；只有设备报告同一
lease 的实际播放完成，Host 才把该 generation 标为 heard 并删除对应缓存。

## 本批边界

- 只做键盘与 Mac Host 同一 Wi-Fi 的第一版真机候选，不把它冒充 PWA、Cloudflare 或异地验收。
- 复用现有 UDP 17333 控制端点和设备 secret；控制报文带截断 HMAC-SHA256，音频 `EIPD` 使用
  AES-256-GCM 加密认证，局域网旁路监听者不能还原总结音频。
- 首个候选采用“完整 EIAD 有界下载到 PSRAM，再交给已验证的流式 EIAD 解码/I2S worker”。最大
  4 MiB，超限 fail closed。最终边下边播 jitter ring 留在 M6 发布门禁，不为赶真机测试隐藏此差异。
- 网络传输使用 stop-and-wait：Host 只有收到设备对下一 offset 的认证 ACK 才推进；重复块幂等，
  乱序、跨连接 generation、跨 lease 和错误 HMAC 一律拒绝。
- Host 在同一认证设备域按 `(slot, connection_generation)` 记住最高 request generation，拒绝旧
  `EIPR` 重放；新 generation 或新连接请求会先可靠取消旧 transport/DB lease，再开始替代请求。
- PTT 永远优先。PTT 抢占或网络中断会取消扬声器、释放设备 PSRAM，并使 Host lease 回到 unread；
  PTT 会先发认证 cancel，重连后的新 generation 也会收敛旧 lease；不发送 `finished`，因此绝不消费总结。

## 固定线协议

- `EIPR` request：`slot / request_generation / connection_generation / nonce`。
- `EIPB` begin：`slot / request_generation / connection_generation / summary_generation /
  lease / total_bytes / total_samples / chunk_bytes / request_nonce`；设备必须核对 Host 原样回显的随机 nonce，
  旧 request 即使在重启后被重放也不能启动 I2S。
- `EIPD` data：精确 identity + `offset / payload_length / AES-256-GCM ciphertext`；从 device secret、
  identity 和 request nonce 派生每次 transfer 的 data key，以 offset 生成唯一 nonce，40-byte header 作 AAD。
- `EIPA` ack：精确 identity + `next_offset / status`；`status=3` 是设备取消，Host 以 `EIPK status=1`
  确认，设备在确认丢失时有界重发，PTT 不必等待取消握手即可开始录音。
- `EIPF` finished：精确 identity + `played_samples`，设备重发直到 Host 返回 `EIPK status=0`；Host 若在
  heard 事务后 cache reconcile 暂时失败，重复 EIPF 会重新驱动幂等 cleanup，而不是留下永久 stale cache。
- 所有整数为 little-endian；控制报文最后 16 bytes 是 HMAC tag，`EIPD` 最后 16 bytes 是 AES-GCM
  tag。Rust/C++ 必须共享 golden vectors，且任何未经认证或无法解密的报文不得分配 PSRAM、开始
  I2S、改账本或删除缓存。

## 实现顺序

1. 先写 Rust/C++ wire codec 和固定 golden vectors。
2. 为 summary ledger 增加精确 playback lease：acquire、cancel、finish 三个事务；重启恢复 leased 为
   unread。cache 删除必须在 finish 事务后按当前引用集合清理。
3. Host UDP worker 处理 request/ACK/finished，解密读取 EIAD，执行 stop-and-wait 并报告账本事件。
4. 固件接收 begin/data，使用 PSRAM 有界缓冲，调用现有 `SpeakerOutput::request_embedded_asset`，在
   I2S worker 成功 drain 后发 finished；PTT/断线走取消路径。
5. 完成 host tests、device simulator、固件 host tests、secret/source audit 和锁定 ESP-IDF 构建。
6. 独立 Sol high 审查通过后提交推送，记录候选镜像 SHA-256，再向用户请求该候选的烧录授权。
7. 真机以已有未读槽位做一次 `S6 -> 听完 -> exact finished -> cache deleted` HIL；听不到或被抢占
   时缓存必须仍在。

## 验收证据

- 软件：Rust/C++ golden vectors、HMAC/GCM 失败、密文不含音频明文、乱序/重复、ACK 丢失、PTT
  抢占、EIPR replay/reconnect replacement、stale finished、重启 lease 恢复和幂等删除均有测试。
- 构建：ESP32-S3、16 MB Flash、8 MB Octal PSRAM、production speaker、镜像 hash 和 flash args。
- 真机：串口日志只记录槽位/generation/状态和计数，不记录任务 UUID、总结文本、明文音频或 secret；
  人耳确认播放完成后再核对 Host ledger/cache。
