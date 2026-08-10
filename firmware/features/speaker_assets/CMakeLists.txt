idf_component_register(
  SRCS
    "diagnostic_link_anchor.cpp"
    "factory_boot_sound.cpp"
    "volume_prompt.cpp"
    "speaker_assets_protocol.cpp"
    "speaker_assets_flash_runner.cpp"
    "speaker_assets_runtime.cpp"
    "speaker_assets_session.cpp"
    "speaker_assets_store_executor.cpp"
    "speaker_assets_wifi_wire.cpp"
    "sound_asset_reader.cpp"
    "sound_asset_crypto.cpp"
    "sound_asset_format.cpp"
    "sound_asset_store.cpp"
    "esp_sound_bank_storage.cpp"
  INCLUDE_DIRS
    "include"
  REQUIRES
    esp_partition
  EMBED_FILES
    "assets/waytoagi.eiad"
    "assets/volume/volume_10.eiad"
    "assets/volume/volume_20.eiad"
    "assets/volume/volume_30.eiad"
    "assets/volume/volume_40.eiad"
    "assets/volume/volume_50.eiad"
    "assets/volume/volume_60.eiad"
    "assets/volume/volume_70.eiad"
    "assets/volume/volume_80.eiad"
    "assets/volume/volume_90.eiad"
    "assets/volume/volume_100.eiad"
  # Keep every adapter/core object in diagnostic ELF proofs. Production also
  # references the runtime directly through SpeakerAssetsSupervisor; retaining
  # the complete archive prevents a release-only adapter from silently falling
  # out of the final link graph.
  WHOLE_ARCHIVE
)
