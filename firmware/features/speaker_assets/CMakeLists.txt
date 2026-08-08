idf_component_register(
  SRCS
    "diagnostic_link_anchor.cpp"
    "factory_boot_sound.cpp"
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
  # Keep every adapter/core object in diagnostic ELF proofs. Production also
  # references the runtime directly through SpeakerAssetsSupervisor; retaining
  # the complete archive prevents a release-only adapter from silently falling
  # out of the final link graph.
  WHOLE_ARCHIVE
)
