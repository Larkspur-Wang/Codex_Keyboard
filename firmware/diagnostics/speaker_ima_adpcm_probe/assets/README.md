# EasyInput EIAD startup probe

`easyinput_boot_probe_eiad.h` is a self-contained diagnostic asset. Firmware
builds do not read the source WAV and do not need an audio tool at build time.

Source provenance:

- workspace source: `easy-input-music/web/audio/dingdongji_ding.wav`
- source SHA-256:
  `a7ad2114cfe07718c7de3e881fdcc4f20a981d36828892868195e2dec94842c4`
- source format: PCM16-LE, stereo, 32,000 Hz, 8,913 frames

Deterministic conversion:

1. Downmix each stereo frame with `(left + right) / 2`, truncating toward zero.
2. Resample 32 kHz to 48 kHz with exact 3:2 linear interpolation. At the
   `/3` interpolation phase, add `+1` to positive numerators and `-1` to
   negative numerators before division toward zero.
3. Encode standard IMA-ADPCM with low nibble first. The encoder step index
   starts at zero and continues across source frames; every EIAD frame stores
   its exact first PCM predictor and starting step index so decoding remains
   independently seekable.
4. Store 480 samples per 10 ms frame, with a shorter final frame.

Frozen result:

| Item | Value |
| --- | --- |
| EIAD version | 1 |
| PCM format | 48 kHz, mono, signed PCM16 |
| Frames | 27 × 480 samples + 1 × 409 samples |
| Total samples / duration | 13,369 / about 278.52 ms |
| Encoded bytes | 6,872 |
| Encoded SHA-256 | `c483ead293fba5321e5b22e7cfff699e56b3db8d7c157ec4b23eba8b8bd2de42` |
| Encoded FNV-1a64 | `7f4dc78c970b4054` |
| Decoded PCM bytes | 26,738 |
| Decoded PCM SHA-256 | `7f13f1e2b55daaef6b189b09c40576dbb17c809726c7f1661782399fe9491729` |
| Decoded PCM FNV-1a64 | `7abb5b0344f7014d` |
| Absolute peak | 15,465 |
| Sum of squares | 160,475,123,025 |
| RMS | about 3,464.61 PCM units / -19.52 dBFS |

The host decoder test freezes the container fields, decoded FNV, peak, sum of
squares, reset determinism and encoded-to-PCM size ratio.
