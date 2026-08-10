cmake_minimum_required(VERSION 3.20)

project(easy_codex_input_firmware_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

add_executable(
  eci_protocol_test
  protocol_test.cpp
  ../components/eci_protocol/eci_protocol.cpp
)
target_include_directories(
  eci_protocol_test
  PRIVATE ../components/eci_protocol/include
)
target_link_libraries(eci_protocol_test PRIVATE OpenSSL::Crypto)
target_compile_options(
  eci_protocol_test
  PRIVATE -Wall -Wextra -Werror -Wpedantic
)
add_test(NAME eci_protocol_test COMMAND eci_protocol_test)

# These host executables use <cassert> as their test oracle, including calls
# with intentional side effects inside assert(...). Release generators define
# NDEBUG globally, which would otherwise remove both the checks and the calls,
# turning the suite into a false green (and causing undefined downstream test
# state in a few cases). Keep assertions enabled for every test executable.
function(enable_keyboard_host_test_assertions target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${target} PRIVATE -UNDEBUG)
  endif()
endfunction()

add_library(easy_input_keyboard_core
  ../components/keyboard/src/audio_io_arbiter.cpp
  ../components/keyboard/src/audio_session.cpp
  ../components/keyboard/src/audio_control_wire.cpp
  ../components/keyboard/src/battery_estimator.cpp
  ../components/keyboard/src/ble_advertising_state.cpp
  ../components/keyboard/src/ble_fixed_text_stream.cpp
  ../components/keyboard/src/ble_input_scheduler.cpp
  ../components/keyboard/src/ble_owner_recovery.cpp
  ../components/keyboard/src/config_payload.cpp
  ../components/keyboard/src/config_receiver.cpp
  ../components/keyboard/src/config_state.cpp
  ../components/keyboard/src/config_status.cpp
  ../components/keyboard/src/codex_slot_state.cpp
  ../components/keyboard/src/codex_playback_wire.cpp
  ../components/keyboard/src/debounce.cpp
  ../components/keyboard/src/diagnostic_command.cpp
  ../components/keyboard/src/encoder.cpp
  ../components/keyboard/src/held_keyboard_state.cpp
  ../components/keyboard/src/hid_report_queue.cpp
  ../components/keyboard/src/hid_keycode.cpp
  ../components/keyboard/src/input_feedback.cpp
  ../components/keyboard/src/input_test.cpp
  ../components/keyboard/src/keyboard_snapshot_delivery.cpp
  ../components/keyboard/src/keymap.cpp
  ../components/keyboard/src/mailbox_led.cpp
  ../components/keyboard/src/platform_selection.cpp
  ../components/keyboard/src/peripheral_power_lease.cpp
  ../components/keyboard/src/power_cycle.cpp
  ../components/keyboard/src/power_policy.cpp
  ../components/keyboard/src/speaker_probe_status.cpp
  ../components/keyboard/src/speaker_playback.cpp
  ../components/keyboard/src/speaker_service_startup.cpp
  ../components/keyboard/src/status_hid_protocol.cpp
  ../components/keyboard/src/transport_routing.cpp
)

target_include_directories(easy_input_keyboard_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/keyboard/include
)
target_compile_definitions(easy_input_keyboard_core PRIVATE EASY_CODEX_HOST_TEST=1)
target_link_libraries(easy_input_keyboard_core PUBLIC OpenSSL::Crypto)

function(add_keyboard_host_test target)
  add_executable(${target} ${target}.cpp)
  target_link_libraries(${target} PRIVATE easy_input_keyboard_core)
  enable_keyboard_host_test_assertions(${target})
  add_test(NAME ${target} COMMAND ${target})
endfunction()

add_keyboard_host_test(board_pins_tests)

add_executable(board_pins_v2_tests board_pins_tests.cpp)
target_link_libraries(board_pins_v2_tests PRIVATE easy_input_keyboard_core)
target_compile_definitions(board_pins_v2_tests PRIVATE EASY_INPUT_BOARD_V2=1)
enable_keyboard_host_test_assertions(board_pins_v2_tests)
add_test(NAME board_pins_v2_tests COMMAND board_pins_v2_tests)

add_keyboard_host_test(audio_io_arbiter_tests)
add_keyboard_host_test(audio_control_wire_tests)
add_keyboard_host_test(audio_packet_wire_tests)
add_keyboard_host_test(audio_session_tests)
add_keyboard_host_test(agent_status_tests)
add_keyboard_host_test(battery_estimator_tests)
add_keyboard_host_test(ble_advertising_state_tests)
add_keyboard_host_test(ble_connection_profile_tests)
add_keyboard_host_test(ble_input_scheduler_tests)
add_keyboard_host_test(ble_fixed_text_stream_tests)
add_keyboard_host_test(ble_owner_recovery_tests)
add_keyboard_host_test(ble_status_wire_tests)
add_keyboard_host_test(config_payload_tests)
add_keyboard_host_test(config_receiver_tests)
add_keyboard_host_test(config_state_tests)
add_keyboard_host_test(config_status_tests)
add_keyboard_host_test(codex_slot_state_tests)
add_keyboard_host_test(codex_playback_wire_tests)
add_keyboard_host_test(codex_firmware_wiring_tests)
target_compile_definitions(
  codex_firmware_wiring_tests
  PRIVATE EASY_INPUT_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_executable(
  codex_device_simulator_tests
  codex_device_simulator_tests.cpp
  ../simulator/codex_device_simulator.cpp
)
target_include_directories(
  codex_device_simulator_tests
  PRIVATE ../simulator
)
target_link_libraries(codex_device_simulator_tests PRIVATE easy_input_keyboard_core)
enable_keyboard_host_test_assertions(codex_device_simulator_tests)
add_test(NAME codex_device_simulator_tests COMMAND codex_device_simulator_tests)
add_keyboard_host_test(debounce_tests)
add_keyboard_host_test(diagnostic_command_tests)
add_keyboard_host_test(encoder_tests)
add_keyboard_host_test(firmware_source_contract_tests)
target_compile_definitions(
  firmware_source_contract_tests
  PRIVATE EASY_INPUT_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
target_include_directories(
  firmware_source_contract_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../features/speaker_assets/include
)
add_keyboard_host_test(held_keyboard_state_tests)
add_keyboard_host_test(hid_report_queue_tests)
add_keyboard_host_test(hid_keycode_tests)
add_keyboard_host_test(input_feedback_tests)
add_keyboard_host_test(input_test_tests)

add_executable(
  speaker_assets_wifi_policy_tests
  speaker_assets_wifi_policy_tests.cpp
)
target_include_directories(
  speaker_assets_wifi_policy_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../features/speaker_assets/include
)
enable_keyboard_host_test_assertions(
  speaker_assets_wifi_policy_tests)
add_test(
  NAME speaker_assets_wifi_policy_tests
  COMMAND speaker_assets_wifi_policy_tests
)

add_executable(
  ima_adpcm_decoder_tests
  ima_adpcm_decoder_tests.cpp
  ../diagnostics/speaker_ima_adpcm_probe/ima_adpcm_decoder.cpp
)
target_include_directories(
  ima_adpcm_decoder_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../diagnostics/speaker_ima_adpcm_probe
    ${CMAKE_CURRENT_SOURCE_DIR}/../diagnostics/speaker_ima_adpcm_probe/include
)
enable_keyboard_host_test_assertions(ima_adpcm_decoder_tests)
add_test(NAME ima_adpcm_decoder_tests COMMAND ima_adpcm_decoder_tests)

add_executable(
  sound_asset_store_tests
  sound_asset_store_tests.cpp
  ../features/speaker_assets/sound_asset_crypto.cpp
  ../features/speaker_assets/sound_asset_format.cpp
  ../features/speaker_assets/sound_asset_store.cpp
)
target_include_directories(
  sound_asset_store_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../features/speaker_assets/include
)
enable_keyboard_host_test_assertions(sound_asset_store_tests)
add_test(NAME sound_asset_store_tests COMMAND sound_asset_store_tests)

add_executable(
  sound_asset_reader_tests
  sound_asset_reader_tests.cpp
  ../features/speaker_assets/sound_asset_reader.cpp
  ../features/speaker_assets/sound_asset_crypto.cpp
  ../features/speaker_assets/sound_asset_format.cpp
  ../features/speaker_assets/sound_asset_store.cpp
)
target_include_directories(
  sound_asset_reader_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../features/speaker_assets/include
)
target_compile_definitions(
  sound_asset_reader_tests
  PRIVATE EASY_INPUT_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
enable_keyboard_host_test_assertions(sound_asset_reader_tests)
add_test(NAME sound_asset_reader_tests COMMAND sound_asset_reader_tests)

add_executable(
  speaker_assets_store_executor_tests
  speaker_assets_store_executor_tests.cpp
  ../features/speaker_assets/speaker_assets_store_executor.cpp
  ../features/speaker_assets/sound_asset_crypto.cpp
  ../features/speaker_assets/sound_asset_format.cpp
  ../features/speaker_assets/sound_asset_store.cpp
)
target_include_directories(
  speaker_assets_store_executor_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../features/speaker_assets/include
)
enable_keyboard_host_test_assertions(speaker_assets_store_executor_tests)
add_test(
  NAME speaker_assets_store_executor_tests
  COMMAND speaker_assets_store_executor_tests
)

add_executable(
  speaker_assets_flash_runner_tests
  speaker_assets_flash_runner_tests.cpp
  ../features/speaker_assets/speaker_assets_flash_runner.cpp
  ../features/speaker_assets/speaker_assets_store_executor.cpp
  ../features/speaker_assets/speaker_assets_runtime.cpp
  ../features/speaker_assets/speaker_assets_session.cpp
  ../features/speaker_assets/speaker_assets_protocol.cpp
  ../features/speaker_assets/sound_asset_reader.cpp
  ../features/speaker_assets/sound_asset_crypto.cpp
  ../features/speaker_assets/sound_asset_format.cpp
  ../features/speaker_assets/sound_asset_store.cpp
)
target_include_directories(
  speaker_assets_flash_runner_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../features/speaker_assets/include
)
target_compile_definitions(
  speaker_assets_flash_runner_tests
  PRIVATE EASY_INPUT_SPEAKER_ASSETS_RUNTIME_TEST_ACCESS=1
)
enable_keyboard_host_test_assertions(speaker_assets_flash_runner_tests)
add_test(
  NAME speaker_assets_flash_runner_tests
  COMMAND speaker_assets_flash_runner_tests
)

add_executable(
  speaker_assets_protocol_tests
  speaker_assets_protocol_tests.cpp
  ../features/speaker_assets/speaker_assets_protocol.cpp
  ../features/speaker_assets/sound_asset_crypto.cpp
)
target_include_directories(
  speaker_assets_protocol_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../features/speaker_assets/include
)
enable_keyboard_host_test_assertions(speaker_assets_protocol_tests)
add_test(NAME speaker_assets_protocol_tests COMMAND speaker_assets_protocol_tests)

add_executable(
  speaker_assets_wifi_wire_tests
  speaker_assets_wifi_wire_tests.cpp
  ../features/speaker_assets/speaker_assets_wifi_wire.cpp
  ../features/speaker_assets/speaker_assets_protocol.cpp
  ../features/speaker_assets/sound_asset_crypto.cpp
)
target_include_directories(
  speaker_assets_wifi_wire_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../features/speaker_assets/include
)
enable_keyboard_host_test_assertions(speaker_assets_wifi_wire_tests)
add_test(
  NAME speaker_assets_wifi_wire_tests
  COMMAND speaker_assets_wifi_wire_tests
)

add_executable(
  speaker_assets_session_tests
  speaker_assets_session_tests.cpp
  ../features/speaker_assets/speaker_assets_session.cpp
  ../features/speaker_assets/speaker_assets_protocol.cpp
  ../features/speaker_assets/sound_asset_crypto.cpp
)
target_include_directories(
  speaker_assets_session_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../features/speaker_assets/include
)
enable_keyboard_host_test_assertions(speaker_assets_session_tests)
add_test(NAME speaker_assets_session_tests COMMAND speaker_assets_session_tests)

add_executable(
  speaker_assets_runtime_tests
  speaker_assets_runtime_tests.cpp
  ../features/speaker_assets/speaker_assets_runtime.cpp
  ../features/speaker_assets/speaker_assets_session.cpp
  ../features/speaker_assets/speaker_assets_protocol.cpp
  ../features/speaker_assets/sound_asset_crypto.cpp
)
target_include_directories(
  speaker_assets_runtime_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../features/speaker_assets/include
)
target_compile_definitions(
  speaker_assets_runtime_tests
  PRIVATE EASY_INPUT_SPEAKER_ASSETS_RUNTIME_TEST_ACCESS=1
)
enable_keyboard_host_test_assertions(speaker_assets_runtime_tests)
add_test(NAME speaker_assets_runtime_tests COMMAND speaker_assets_runtime_tests)

add_keyboard_host_test(gatt_status_snapshot_tests)
add_keyboard_host_test(keyboard_snapshot_delivery_tests)
add_keyboard_host_test(keymap_tests)
add_keyboard_host_test(mailbox_led_tests)
add_keyboard_host_test(platform_selection_tests)
add_keyboard_host_test(peripheral_power_lease_tests)
add_keyboard_host_test(power_cycle_tests)
add_keyboard_host_test(power_policy_tests)
add_keyboard_host_test(speaker_audio_contract_tests)
add_keyboard_host_test(speaker_probe_status_tests)
add_keyboard_host_test(speaker_playback_tests)
add_keyboard_host_test(speaker_service_startup_tests)
add_keyboard_host_test(status_hid_protocol_tests)
add_keyboard_host_test(transport_routing_tests)
add_keyboard_host_test(usb_hid_endpoint_arbiter_tests)
