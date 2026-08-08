# Evidence

这里保存可公开、可复现、已脱敏的验收证据。每个 milestone 建独立目录，并由对应
`flow/tasks/*.md` 指向具体文件。

允许：测试摘要、版本、hash、指标 CSV/JSON、无账号信息的截图、无个人内容的 fixture。

禁止：key/token、task UUID/标题、真实 prompt/transcript、麦克风录音、家庭网络凭据和云控制台
账号信息。私有调试材料放 `evidence/private/`，该目录不会进入 Git。

本地 Wi-Fi 仓只保留 M2-M5 中与 Host、Codex、TTS、缓存、固件和 LAN 直接相关的历史证据；PWA、
Cloudflare 和远程网络证据保留在来源仓 `easy-codex-input`。
