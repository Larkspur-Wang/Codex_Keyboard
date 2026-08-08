set(easy_input_main_sources
  "app_main.cpp"
  "platform/battery_adc.cpp"
  "platform/ble_hid.cpp"
  "platform/gpio_keys.cpp"
  "platform/keyboard_audio.cpp"
  "platform/led_strip_status.cpp"
  "platform/nvs_store.cpp"
  "platform/peripheral_power.cpp"
  "platform/usb_hid.cpp"
)

set(easy_input_main_requires
  keyboard
  bt
  esp_hid
  esp_adc
  esp_event
  esp_driver_gpio
  esp_driver_i2s
  esp_driver_rmt
  esp_netif
  esp_pm
  esp_psram
  esp_timer
  esp_wifi
  espressif__esp_tinyusb
  lwip
  mbedtls
  nvs_flash
)

idf_build_get_property(
  easy_input_speaker_assets_product_enabled
  EASY_INPUT_SPEAKER_ASSETS_PRODUCT_ENABLED
)

if(EASY_INPUT_SPEAKER_DIAGNOSTIC OR
   easy_input_speaker_assets_product_enabled)
  list(APPEND easy_input_main_sources "platform/speaker_output.cpp")
endif()
if(easy_input_speaker_assets_product_enabled)
  list(APPEND easy_input_main_sources
    "platform/codex_lan_playback.cpp"
    "platform/speaker_assets_supervisor.cpp"
    "platform/speaker_assets_wifi.cpp")
endif()

idf_build_get_property(
  easy_input_speaker_opus_diagnostic_enabled
  EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC_ENABLED
)
if(easy_input_speaker_opus_diagnostic_enabled)
  list(APPEND easy_input_main_requires speaker_opus_probe)
endif()

idf_build_get_property(
  easy_input_speaker_ima_adpcm_diagnostic_enabled
  EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC_ENABLED
)
if(easy_input_speaker_ima_adpcm_diagnostic_enabled)
  list(APPEND easy_input_main_requires speaker_ima_adpcm_probe)
endif()

idf_build_get_property(
  easy_input_speaker_assets_diagnostic_enabled
  EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC_ENABLED
)
if(easy_input_speaker_assets_diagnostic_enabled)
  list(APPEND easy_input_main_requires speaker_assets)
endif()
if(easy_input_speaker_assets_product_enabled AND
   NOT easy_input_speaker_assets_diagnostic_enabled)
  list(APPEND easy_input_main_requires speaker_assets)
endif()

idf_component_register(
  SRCS
    ${easy_input_main_sources}
  INCLUDE_DIRS
    "."
  REQUIRES
    ${easy_input_main_requires}
)

if(EASY_INPUT_BOARD STREQUAL "v2")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE EASY_INPUT_BOARD_V2=1)
endif()

if(EASY_INPUT_SPEAKER_DIAGNOSTIC)
  target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE EASY_INPUT_SPEAKER_DIAGNOSTIC=1
  )
endif()

if(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
  target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC=1
  )
endif()

if(easy_input_speaker_ima_adpcm_diagnostic_enabled)
  target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC=1
  )
endif()

if(easy_input_speaker_assets_diagnostic_enabled)
  target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC=1
  )
endif()

if(easy_input_speaker_assets_product_enabled)
  target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE EASY_INPUT_SPEAKER_ASSETS_PRODUCT=1
  )
endif()
