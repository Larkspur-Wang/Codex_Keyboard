# Board volume prompts

The ten frozen EIAD v1 files announce board-speaker levels from 10% to 100%.
They are embedded in the firmware app image and never fetched at runtime.

- model: `qwen-audio-3.0-tts-flash`
- region: China (Beijing)
- voice: `longanfengyue`
- source audio: 48 kHz mono PCM16 WAV
- device encoding: EIAD v1, 48 kHz mono, 480 samples per frame
- generation receipt and encoded SHA-256: `manifest.json`

Regenerate source WAVs with the official DashScope HTTP SDK, then freeze the
device assets with:

```sh
cargo run -p easy-codex-host --bin generate_volume_prompts -- \
  ../firmware/features/speaker_assets/assets/volume /path/to/source-wavs
```

API keys and source WAVs are not stored in the repository.
