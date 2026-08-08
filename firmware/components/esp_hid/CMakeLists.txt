# SPDX-FileCopyrightText: 2026 EasyInput contributors
#
# SPDX-License-Identifier: Apache-2.0

# Keep this override intentionally thin: only the NimBLE HID-device backend is
# patched locally.  Every other source and all upstream public/private headers
# come from the exact ESP-IDF component that this project component overrides.
idf_component_get_property(upstream_esp_hid_dir esp_hid COMPONENT_OVERRIDEN_DIR)
if(NOT upstream_esp_hid_dir)
    message(FATAL_ERROR
        "EasyInput esp_hid safety adapter must override an ESP-IDF esp_hid component")
endif()

idf_build_get_property(idf_ver IDF_VER)
if(NOT idf_ver STREQUAL "v5.5.5")
    message(FATAL_ERROR
        "EasyInput esp_hid safety adapter is pinned to ESP-IDF v5.5.5 "
        "(got '${idf_ver}'). Re-audit the adapter before changing ESP-IDF.")
endif()

set(expected_upstream_nimble_hidd_sha256
    "b117a0236c2086d07b74fb6ca356be62d5e94335b020abc18f53a2ec9dec7428")
set(upstream_nimble_hidd "${upstream_esp_hid_dir}/src/nimble_hidd.c")
if(NOT EXISTS "${upstream_nimble_hidd}")
    message(FATAL_ERROR
        "ESP-IDF v5.5.5 esp_hid backend not found at '${upstream_nimble_hidd}'")
endif()
file(SHA256 "${upstream_nimble_hidd}" upstream_nimble_hidd_sha256)
if(NOT upstream_nimble_hidd_sha256 STREQUAL expected_upstream_nimble_hidd_sha256)
    message(FATAL_ERROR
        "ESP-IDF esp_hid/nimble_hidd.c does not match the audited v5.5.5 source. "
        "Expected ${expected_upstream_nimble_hidd_sha256}, "
        "got ${upstream_nimble_hidd_sha256}.")
endif()

idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    return()
endif()

set(srcs
    "${upstream_esp_hid_dir}/src/esp_hidd.c"
    "${upstream_esp_hid_dir}/src/esp_hidh.c"
    "${upstream_esp_hid_dir}/src/esp_hid_common.c")

if(CONFIG_BT_ENABLED)
    if(CONFIG_BT_BLUEDROID_ENABLED)
        list(APPEND srcs
            "${upstream_esp_hid_dir}/src/ble_hidd.c"
            "${upstream_esp_hid_dir}/src/ble_hidh.c"
            "${upstream_esp_hid_dir}/src/bt_hidh.c"
            "${upstream_esp_hid_dir}/src/bt_hidd.c")
    endif()
    if(CONFIG_BT_NIMBLE_ENABLED)
        list(APPEND srcs
            "${CMAKE_CURRENT_LIST_DIR}/src/nimble_hidd.c"
            "${upstream_esp_hid_dir}/src/nimble_hidh.c")
    endif()
endif()

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS
        "include"
        "${upstream_esp_hid_dir}/include"
    PRIV_INCLUDE_DIRS
        "${upstream_esp_hid_dir}/private"
    REQUIRES esp_event bt)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
