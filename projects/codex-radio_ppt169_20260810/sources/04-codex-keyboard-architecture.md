# Codex Keyboard 端到端架构

## 拓扑

```mermaid
flowchart TB
    subgraph Keyboard["EasyInput V2"]
        KEYS["S1-S4 PTT / S5-S8 Play"]
        MIC["I2S microphone"]
        ESP["ESP32-S3 + Wi-Fi + 8 MB PSRAM"]
        SPK["I2S amplifier and speaker"]
        KEYS --> ESP
        MIC --> ESP
        ESP --> SPK
    end

    subgraph Mac["Mac"]
        APP["Tauri Desktop App"]
        HOST["easy-codex-host LaunchAgent"]
        DB["SQLite + encrypted WAV/EIAD cache"]
        CODEX["Codex CLI + rollout observer"]
        APP <--> HOST
        HOST <--> DB
        HOST <--> CODEX
    end

    ASR["DashScope qwen3-asr-flash"]
    SPARK["gpt-5.6-luna / high"]
    TTS["DashScope qwen-audio-3.0-tts-flash"]

    ESP <-->|"same LAN: EICC/EIAU/EIP"| HOST
    HOST --> ASR
    HOST --> SPARK
    HOST --> TTS
```

## PTT 时序

```mermaid
sequenceDiagram
    participant K as Keyboard
    participant H as Host
    participant A as Qwen ASR
    participant C as Codex task

    K->>H: EICC start(slot, capture generation)
    H-->>K: authenticated accept
    loop 16 kHz PCM16 frames
        K->>H: EIAU(sequence, timestamp, HMAC)
    end
    K->>H: authenticated end(last sequence)
    H->>A: bounded WAV
    A-->>H: transcript
    H->>H: durable FIFO transaction
    H->>C: codex exec resume via stdin
    C-->>H: rollout task_complete
```

## 总结和播放时序

```mermaid
sequenceDiagram
    participant C as Codex rollout
    participant H as Host
    participant S as Spark
    participant T as Qwen TTS
    participant D as Encrypted cache
    participant K as Keyboard

    C->>H: authoritative task_complete
    H->>S: previous unread + new bounded turn pack
    S-->>H: cumulative spoken summary
    H->>T: one complete spoken_text request
    T-->>H: temporary complete WAV URL
    H->>T: HTTPS GET complete 48 kHz WAV
    H->>D: fsync and atomic WAV/EIAD generation publish
    K->>H: EIPR(slot, request generation, nonce)
    H->>D: acquire exact unread lease
    H-->>K: EIPB + AES-GCM EIPD chunks
    K->>K: PSRAM -> EIAD decode -> I2S
    K->>H: EIPF(actual drain complete)
    H->>D: commit heard + exact cleanup
    H-->>K: EIPK
```

## 本地文件

```text
~/Library/Application Support/EasyCodexInput/
├── .env                  # DashScope key, 0600
├── cache-secret.hex      # cache root key, 0600
├── device-secret.hex     # LAN device secret, 0600
├── state.sqlite3
├── bin/easy-codex-host
├── run/host.sock
├── logs/host.log
└── cache/
```

Host 是唯一后台服务。Tauri 关闭后，Codex observer、FIFO、Spark/TTS 和未读缓存仍继续运行。

## 故障语义

- Host 离线：固件不缓存 task 请求，不猜测绑定。
- Wi-Fi 断线：当前采集/播放 generation 失败并保留未读。
- PTT 抢占：固件立即停止播放，向 Host 重发 cancel，录音不等待 cancel ACK。
- final data ACK/EIPF/EIPK 丢失：双方以相同 identity 幂等重发。
- Host 重启：未完成 playback lease 恢复为 unread；不把“传输完成”当“听完”。
- cache 损坏：fail closed，不删除引用；需要重新生成或人工处理。
