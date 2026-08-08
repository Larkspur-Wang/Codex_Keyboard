# Ogg Opus diagnostic fixture

`easyinput_boot_probe.ogg` is a deterministic, diagnostic-only fixture. It is
not a product sound and is compiled only when both speaker diagnostic flags are
enabled.

- Source WAV: `easy-input-music/web/audio/dingdongji_ding.wav`
- Normalized source: 48,000 Hz, mono, PCM16, 13,369 logical samples
- Container / codec: Ogg Opus, mapping family 0, 15 audio packets
- Encoder: Xiph `libopus 1.6.1` + `libopusenc 0.3`
- Ogg serial: `0x45495031`
- File size: 1,734 bytes
- SHA-256:
  `d49de654f1b72f05a835299801950df202688f7adf70bbfac11d2dbb48941411`

Two independent encodes were byte-identical. An independent
`libogg` + `libopus` decode verified 48,000 Hz mono audio, 13,369 logical
samples after pre-skip, and non-zero signal.
