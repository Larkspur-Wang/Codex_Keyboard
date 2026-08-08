# Factory boot sound

`waytoagi.eiad` is the one product fallback sound embedded in the firmware
application image. It is not an App preset library and is never copied into
the `sound_a` / `sound_b` user banks.

Frozen provenance:

- source preset: EasyInput App `speaker-presets/waytoagi.mp3`
- source MP3 SHA-256:
  `88dc68a670bb2c8d696d4c6cfdf9e0f1e4b7bf5a57ec338b86c3cf957e732085`
- conversion: App `import_boot_sound_bytes` followed by
  `encode_device_sound`
- decoder/resampler versions: Symphonia 0.5.5 and Rubato 0.15.0
- format: EIAD v1, 48 kHz mono, 480 samples per frame
- decoded length: 82,755 samples (173 frames)
- encoded size: 42,435 bytes
- encoded SHA-256:
  `f29312efa6cb78eb1ac43ca762acbbfefa81769f00dee0930f81fd53bc311751`
- firmware-decoded PCM16LE SHA-256:
  `431c9f6bebb6eaa44e386252c49a2af9fc647da7a79a5796bbab2e1ea48fbd3f`

The binary is frozen rather than regenerated during ESP-IDF builds so host
floating-point decoder/resampler differences cannot silently change a
production firmware sound.
