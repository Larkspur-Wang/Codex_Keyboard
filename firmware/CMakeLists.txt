cmake_minimum_required(VERSION 3.16)

set(EASY_INPUT_BOARD "v2" CACHE STRING "EasyInput production board revision (V2/V2.1)")
set_property(CACHE EASY_INPUT_BOARD PROPERTY STRINGS v2)
if(NOT EASY_INPUT_BOARD STREQUAL "v2")
  message(FATAL_ERROR
    "EASY_INPUT_BOARD='${EASY_INPUT_BOARD}' is no longer a supported release target. "
    "EasyInput V2 includes the former V2.1 hardware revision; use -DEASY_INPUT_BOARD=v2.")
endif()
message(STATUS "EasyInput board revision: ${EASY_INPUT_BOARD}")

# Explicit bring-up image only. Production playback is controlled by the
# separate asset-product gate below; this switch retains the fixed diagnostic
# tone/probe and must remain off in release images.
set(
  EASY_INPUT_SPEAKER_DIAGNOSTIC
  OFF
  CACHE BOOL
  "Build the one-shot low-volume V2 speaker diagnostic probe"
)
message(STATUS
  "EasyInput speaker diagnostic: ${EASY_INPUT_SPEAKER_DIAGNOSTIC}")

# Phase-2 codec gate. Keep the adapter outside ESP-IDF's default component
# search path so a production build neither resolves nor downloads the codec.
# The Opus probe deliberately depends on the already-isolated speaker path.
set(
  EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC
  OFF
  CACHE BOOL
  "Decode one embedded 48 kHz mono Ogg Opus speaker diagnostic"
)
set(
  EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC
  OFF
  CACHE BOOL
  "Decode one embedded 48 kHz mono framed IMA-ADPCM speaker diagnostic"
)
if(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC AND
   NOT EASY_INPUT_SPEAKER_DIAGNOSTIC)
  message(FATAL_ERROR
    "EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC requires "
    "-DEASY_INPUT_SPEAKER_DIAGNOSTIC=ON.")
endif()
if(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC AND
   NOT EASY_INPUT_SPEAKER_DIAGNOSTIC)
  message(FATAL_ERROR
    "EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC requires "
    "-DEASY_INPUT_SPEAKER_DIAGNOSTIC=ON.")
endif()
if(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC AND
   EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  message(FATAL_ERROR
    "EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC and "
    "EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC are mutually exclusive.")
endif()
if(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
  list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/diagnostics/speaker_opus_probe")
endif()
if(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/diagnostics/speaker_ima_adpcm_probe")
endif()
message(STATUS
  "EasyInput speaker Opus diagnostic: "
  "${EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC}")
message(STATUS
  "EasyInput speaker IMA-ADPCM diagnostic: "
  "${EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC}")

# A/B storage gates. The canonical partition table reserves both banks in every
# image. The diagnostic gate keeps the isolated link/ELF proof, while the
# production gate enables the reviewed USB sync and boot-playback path.
set(
  EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC
  OFF
  CACHE BOOL
  "Build the isolated speaker A/B asset-store diagnostic component"
)
set(
  EASY_INPUT_SPEAKER_ASSETS_PRODUCT
  ON
  CACHE BOOL
  "Enable production speaker asset sync and boot playback"
)
if(EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC OR
   EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/features/speaker_assets")
endif()
message(STATUS
  "EasyInput speaker A/B asset storage diagnostic: "
  "${EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC}")
message(STATUS
  "EasyInput speaker asset sync + boot playback: "
  "${EASY_INPUT_SPEAKER_ASSETS_PRODUCT}")

# The production PCB is ESP32-S3 only. Reject a stale caller/cache target before
# ESP-IDF loads any target-specific Kconfig, then keep the cache explicit so a
# release command never relies on idf.py guessing from a developer-local
# source-tree sdkconfig.
if(DEFINED IDF_TARGET AND
   NOT "${IDF_TARGET}" STREQUAL "" AND
   NOT "${IDF_TARGET}" STREQUAL "esp32s3")
  message(FATAL_ERROR
    "EasyInput V2 only supports IDF_TARGET=esp32s3 "
    "(got '${IDF_TARGET}'). Remove the stale target cache or pass "
    "-D IDF_TARGET=esp32s3.")
endif()
set(
  IDF_TARGET
  "esp32s3"
  CACHE STRING
  "EasyInput production SoC target"
  FORCE
)

# Never let ESP-IDF's implicit `${CMAKE_SOURCE_DIR}/sdkconfig` default select
# the repository's ignored developer-local file for a production image.
# Every build directory owns a freshly generated effective config unless the
# caller explicitly supplies another out-of-tree SDKCONFIG path.
if(NOT DEFINED SDKCONFIG OR "${SDKCONFIG}" STREQUAL "")
  set(
    SDKCONFIG
    "${CMAKE_BINARY_DIR}/sdkconfig"
    CACHE FILEPATH
    "Generated EasyInput sdkconfig (build-directory scoped)"
  )
endif()
if(NOT DEFINED SDKCONFIG_DEFAULTS OR "${SDKCONFIG_DEFAULTS}" STREQUAL "")
  set(
    SDKCONFIG_DEFAULTS
    "${CMAKE_SOURCE_DIR}/sdkconfig.defaults"
    CACHE STRING
    "Canonical EasyInput sdkconfig defaults"
  )
endif()
get_filename_component(
  easy_input_effective_sdkconfig
  "${SDKCONFIG}"
  ABSOLUTE
  BASE_DIR "${CMAKE_SOURCE_DIR}"
)
get_filename_component(
  easy_input_ignored_source_sdkconfig
  "${CMAKE_SOURCE_DIR}/sdkconfig"
  ABSOLUTE
)
if(easy_input_effective_sdkconfig STREQUAL easy_input_ignored_source_sdkconfig)
  message(FATAL_ERROR
    "The ignored source-tree sdkconfig is not a valid EasyInput release input. "
    "Use a new build directory (default) or pass an explicit out-of-tree "
    "-D SDKCONFIG=<path> generated from sdkconfig.defaults.")
endif()
message(STATUS "EasyInput effective sdkconfig: ${easy_input_effective_sdkconfig}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(
  EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC_ENABLED
  "${EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC}"
)
idf_build_set_property(
  EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC_ENABLED
  "${EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC}"
)
idf_build_set_property(
  EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC_ENABLED
  "${EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC}"
)
idf_build_set_property(
  EASY_INPUT_SPEAKER_ASSETS_PRODUCT_ENABLED
  "${EASY_INPUT_SPEAKER_ASSETS_PRODUCT}"
)
if(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
  # The active component graph differs only for this diagnostic image. Keep
  # its generated lock in the build directory so running the probe cannot
  # rewrite the production dependency lock.
  idf_build_set_property(
    DEPENDENCIES_LOCK
    "${CMAKE_BINARY_DIR}/dependencies.opus.lock"
  )
endif()
project(easy_codex_input)

if(NOT CONFIG_IDF_TARGET_ESP32S3)
  message(FATAL_ERROR
    "EasyInput V2 requires CONFIG_IDF_TARGET_ESP32S3=y. Regenerate the "
    "effective sdkconfig from sdkconfig.defaults before building or flashing.")
endif()

if(NOT CONFIG_ESPTOOLPY_FLASHSIZE_16MB)
  message(FATAL_ERROR
    "EasyInput V2 hardware has 16 MB flash, but the effective sdkconfig is not "
    "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y. Remove stale build/sdkconfig caches or "
    "regenerate them from sdkconfig.defaults before building or flashing.")
endif()

if(CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE)
  message(FATAL_ERROR
    "EasyInput release images must retain their verified 16 MB image header; "
    "CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE must remain disabled.")
endif()

if(NOT CONFIG_PARTITION_TABLE_CUSTOM OR
   NOT "${CONFIG_PARTITION_TABLE_CUSTOM_FILENAME}" STREQUAL "partitions.csv" OR
   NOT "${CONFIG_PARTITION_TABLE_OFFSET}" STREQUAL "0x8000")
  message(FATAL_ERROR
    "EasyInput V2 requires the repository partitions.csv at offset 0x8000 "
    "(24 KiB NVS, 4 KiB PHY, 3 MiB factory app, two 576 KiB sound banks). "
    "Do not change the partition layout without an explicit upgrade plan.")
endif()

# A build can otherwise succeed with the right CSV filename but stale content.
# Normalize whitespace/case and freeze all five entries, including the custom
# type/subtypes and empty flags. Existing NVS/PHY/factory offsets must never
# move when speaker storage evolves.
file(STRINGS
  "${CMAKE_SOURCE_DIR}/partitions.csv"
  easy_input_partition_lines
)
set(easy_input_partition_entries)
foreach(easy_input_partition_line IN LISTS easy_input_partition_lines)
  string(REGEX REPLACE
    "[ \t\r]"
    ""
    easy_input_partition_compact
    "${easy_input_partition_line}"
  )
  if(NOT easy_input_partition_compact STREQUAL "" AND
     NOT easy_input_partition_compact MATCHES "^#")
    string(TOLOWER
      "${easy_input_partition_compact}"
      easy_input_partition_compact
    )
    list(APPEND
      easy_input_partition_entries
      "${easy_input_partition_compact}"
    )
  endif()
endforeach()
set(easy_input_expected_partition_entries
  "nvs,data,nvs,0x9000,0x6000,"
  "phy_init,data,phy,0xf000,0x1000,"
  "factory,app,factory,0x10000,0x300000,"
  "sound_a,0x40,0x00,0x310000,0x90000,"
  "sound_b,0x40,0x01,0x3a0000,0x90000,"
)
if(NOT easy_input_partition_entries STREQUAL
       easy_input_expected_partition_entries)
  message(FATAL_ERROR
    "EasyInput partitions.csv drifted. Preserve the exact NVS/PHY/factory "
    "layout and sound_a/sound_b custom A/B banks; regenerate only under an "
    "explicit migration plan.")
endif()

idf_build_get_property(easy_input_idf_version IDF_VER)
if(NOT easy_input_idf_version STREQUAL "v5.5.5")
  message(FATAL_ERROR
    "EasyInput production firmware requires ESP-IDF v5.5.5 "
    "(got '${easy_input_idf_version}'). Re-audit the local esp_hid adapter "
    "before changing SDK versions.")
endif()

# Audio reserves its sender/capture workers and PCM frame queue during begin(),
# before NimBLE and variable-length configuration strings can fragment the
# internal heap. I2S/Wi-Fi/NimBLE still share the same finite internal-RAM
# budget. A stale, ignored sdkconfig once doubled the NimBLE host stack,
# enlarged its mbuf pool and enabled unrelated IRAM/coredump options; the image
# still built, but the keyboard microphone could no longer admit a session.
# Keep the release memory profile explicit and fail at configure time instead
# of producing a subtly broken binary. sdkconfig.defaults carries the
# canonical values for clean builds.
if(NOT CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE EQUAL 4096 OR
   NOT CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT EQUAL 12 OR
   NOT CONFIG_BT_NIMBLE_MAX_CONNECTIONS EQUAL 3)
  message(FATAL_ERROR
    "EasyInput release BLE memory profile drifted "
    "(host_stack=${CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE}, "
    "msys1=${CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT}, "
    "connections=${CONFIG_BT_NIMBLE_MAX_CONNECTIONS}). "
    "Do not reuse an ignored/stale sdkconfig; regenerate a clean sdkconfig "
    "from sdkconfig.defaults.")
endif()

# V2 is an ESP32-S3R8 design. Keep non-DMA bulk buffers in the chip's 8 MB
# Octal PSRAM and preserve internal SRAM for I2S DMA, Wi-Fi/LwIP, NimBLE and
# task stacks. CAPS_ALLOC is deliberate: ordinary malloc/new must not silently
# migrate protocol state, task objects or secrets into external RAM.
if(NOT CONFIG_SPIRAM OR
   NOT CONFIG_SPIRAM_MODE_OCT OR
   NOT CONFIG_SPIRAM_SPEED_80M OR
   NOT CONFIG_SPIRAM_USE_CAPS_ALLOC OR
   CONFIG_SPIRAM_IGNORE_NOTFOUND)
  message(FATAL_ERROR
    "EasyInput V2 PSRAM memory-domain profile drifted "
    "(spiram=${CONFIG_SPIRAM}, octal=${CONFIG_SPIRAM_MODE_OCT}, "
    "speed80=${CONFIG_SPIRAM_SPEED_80M}, "
    "caps_alloc=${CONFIG_SPIRAM_USE_CAPS_ALLOC}, "
    "ignore_notfound=${CONFIG_SPIRAM_IGNORE_NOTFOUND}). "
    "Use the mandatory ESP32-S3R8 Octal/80MHz/CAPS_ALLOC profile from "
    "sdkconfig.defaults.")
endif()

if(CONFIG_BT_NIMBLE_EXT_SCAN OR
   CONFIG_BT_NIMBLE_CRYPTO_STACK_MBEDTLS OR
   CONFIG_PM_SLEEP_FUNC_IN_IRAM OR
   CONFIG_PM_SLP_IRAM_OPT OR
   CONFIG_PM_RTOS_IDLE_OPT OR
   NOT CONFIG_ESP_COREDUMP_ENABLE_TO_NONE OR
   NOT CONFIG_FREERTOS_ISR_STACKSIZE EQUAL 1536)
  message(FATAL_ERROR
    "EasyInput release internal-RAM profile drifted. Extended scan, mbedTLS "
    "NimBLE crypto, PM IRAM options and core dumps must stay disabled, and "
    "the ISR stack must remain 1536 bytes. Regenerate sdkconfig from "
    "sdkconfig.defaults before building or flashing.")
endif()

# These values are not the cause of the microphone allocation regression, but
# they are part of the verified BLE/Wi-Fi runtime contract. Keep them separate
# from the memory gate so a future failure reports the correct class of drift.
if(NOT CONFIG_BT_NIMBLE_SVC_GAP_PPCP_MIN_CONN_INTERVAL EQUAL 12 OR
   NOT CONFIG_BT_NIMBLE_SVC_GAP_PPCP_MAX_CONN_INTERVAL EQUAL 36 OR
   NOT CONFIG_BT_NIMBLE_SVC_GAP_PPCP_SLAVE_LATENCY EQUAL 0 OR
   NOT CONFIG_BT_NIMBLE_SVC_GAP_PPCP_SUPERVISION_TMO EQUAL 400 OR
   NOT CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0)
  message(FATAL_ERROR
    "EasyInput release BLE/Wi-Fi behavior profile drifted. The verified "
    "PPCP is 12/36/0/400 and the Wi-Fi task must remain pinned to core 0. "
    "Regenerate sdkconfig from sdkconfig.defaults before building or flashing.")
endif()
