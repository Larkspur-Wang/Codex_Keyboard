# EIAD v1：EasyInput 帧独立 IMA-ADPCM

EIAD 是 Host 缓存并发送给 EasyInput V2 的设备播放格式。它不是网络信封；本地 EIPD 使用
AES-256-GCM 对 EIAD 数据块加密并通过 application ACK 确认。缓存外层由 `CacheStore` 做独立
AES-256-GCM，EIAD 文件本身不重复加密。

## 固定音频参数

- PCM 输入：48,000 Hz、单声道、signed PCM16 little-endian。
- 最长 90 秒，即最多 4,320,000 samples / 8,640,000 PCM bytes。
- 每帧最多 960 samples（20 ms）；最后一帧可短，不补假 sample。
- 每帧独立携带 predictor 和 step index，固件从任意帧边界都能开始解码。
- IMA nibble 先写低四位，再写高四位；奇数个 delta 的最后高四位必须为 0。

## 文件头

所有整数均为 little-endian。文件头固定 32 bytes：

| Offset | Bytes | Field | v1 value |
|---:|---:|---|---:|
| 0 | 4 | magic | ASCII `EIAD` |
| 4 | 2 | version | `1` |
| 6 | 2 | header bytes | `32` |
| 8 | 4 | sample rate | `48000` |
| 12 | 2 | channels | `1` |
| 14 | 2 | encoded bits/sample | `4` |
| 16 | 2 | maximum frame samples | `960` |
| 18 | 2 | frame header bytes | `8` |
| 20 | 8 | total samples | `1..4320000` |
| 28 | 4 | frame count | `ceil(total_samples / 960)` |

## 帧

每帧由 8-byte header 和紧随其后的 packed nibbles 组成：

| Offset | Bytes | Field | Rule |
|---:|---:|---|---|
| 0 | 2 | samples in frame | `1..960`；非尾帧必须为 960 |
| 2 | 2 | initial predictor | 本帧第一个原始 PCM sample |
| 4 | 1 | initial step index | `0..88` |
| 5 | 1 | reserved | 必须为 `0` |
| 6 | 2 | payload bytes | `ceil((samples - 1) / 2)` |

predictor 已经表示第一个 sample，因此 payload 只编码后续 `samples - 1` 个 delta。编码器可根据本帧首个
delta 选择 initial step index；解码器不得继承前一帧的 predictor 或 step index。

## Fail-closed 规则

- magic、版本、固定参数、reserved、frame count、sample count、payload length 或文件总长任一不一致即拒绝。
- trailing bytes、截断帧、超过 90 秒和 step index 大于 88 均拒绝。
- WAV 必须是 canonical 44-byte RIFF/WAVE PCM header，且 data 长度与同一份 PCM 完全一致。
- 新 summary generation 只有在 manifest、WAV、EIAD 三个对象全部加密、fsync、原子 rename 并读回认证后
  才可交给 SQLite ledger 发布；同 task/generation 的不同明文永远不能覆盖已发布对象。

当前 Rust 真相实现与边界测试位于 `app/host/src/audio.rs`；M6 固件 decoder 必须以相同 golden vectors
做跨语言验收，不能凭本文件手写一个“近似兼容”版本。首个冻结向量位于
`protocol/golden-vectors/eiad-v1.json`。
