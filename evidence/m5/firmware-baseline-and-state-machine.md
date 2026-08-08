# M5-M6 firmware baseline and shared state machine evidence

- Date: 2026-08-04
- Imported source: `AIoTWan/easy-input-keyboard@7a41ce1c5bd961f3be417498cee1a8131ab9d86e`
- ESP-IDF: `/Users/lark/esp/esp-idf-v5.5.5@b774170ff46c393eeb5e495ea37936038d3f4f4f`
- Target: `esp32s3`, V2, 8 MB Octal PSRAM, 16 MB Flash
- Hardware evidence: read-only USB enumeration only; no reset, download mode, device handle or flash

### Check: Complete repository eval after baseline import
**Command run:**
  `./scripts/eval-fast.sh`
**Output observed:**
  `204 Host tests, CLI 6, parent-death 6, protocol 6, runtime-gate 4, TypeScript 36, firmware 55/55; secret scan passed; source and license audit passed; fast eval passed`
**Result: PASS**

### Check: Adversarial slot state under sanitizers
**Command run:**
  `cmake -S firmware/host_test -B firmware/build-host-test-adversarial -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Wpedantic' -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' && cmake --build firmware/build-host-test-adversarial --target codex_slot_state_tests codex_device_simulator_tests codex_firmware_wiring_tests --parallel && ctest --test-dir firmware/build-host-test-adversarial --output-on-failure -R 'codex_(slot_state|device_simulator|firmware_wiring)_tests'`
**Output observed:**
  `3/3 passed; ASan/UBSan reported no failure`
**Result: PASS**

### Check: Non-happy-path state traces
**Command run:**
  `firmware/build-host-test-adversarial/codex_slot_state_tests && firmware/build-host-test-adversarial/codex_device_simulator_tests`
**Output observed:**
  `exit 0; 1,000 seeds x 100 operations plus stale finished ACK, forged generation, wrong connection, disconnect, dropped frame, out-of-order frame and offline PTT preemption assertions all held`
**Result: PASS**

### Check: Locked ESP-IDF product build
**Command run:**
  `source /Users/lark/esp/esp-idf-v5.5.5/export.sh >/dev/null && idf.py -C firmware -B firmware/build-idf-v5.5.5-import build`
**Output observed:**
  `easy_codex_input.bin size 0x18bca0; factory partition 0x300000; 0x174360 bytes (48%) free; build complete`
**Result: PASS**

### Check: Candidate artifact identity
**Command run:**
  `shasum -a 256 firmware/build-idf-v5.5.5-import/easy_codex_input.bin firmware/build-idf-v5.5.5-import/bootloader/bootloader.bin firmware/build-idf-v5.5.5-import/partition_table/partition-table.bin`
**Output observed:**
  `easy_codex_input.bin b0f5ee5c28ba1f787a5f80d23c46a5ad7839480a714bd071e684a49f8e172013; bootloader c96a16af81e195685e0a3215c7d2dbba481217525a68c9ddb6a71404976b2c12; partition table 7c541b70dcac8f920c2d11589f06745e1b033fa9b95b8343de2748bb8312a278`
**Result: PASS**

### Check: Physical board presence without mutation
**Command run:**
  `ioreg -p IOUSB -l -w 0 | rg -A 14 -B 3 'EasyInput|easy-input-v2|12346|4102'`
**Output observed:**
  `EasyInput AI; AIOTWAN; serial easy-input-v2; VID 12346 (0x303A); PID 4102 (0x1006); 12 Mbps; location 0x01140000; normal HID runtime; no USB serial port`
**Result: PASS**

### Check: Independent Sol high review
**Command run:**
  `Independent gpt-5.6-sol high read-only review plus full ASan/UBSan Host suite`
**Output observed:**
  `Initial review found route-lifetime, offline PTT priority, stale-ACK injection and trace-count gaps. After correction, final Sol high re-review found no remaining technical blocker; its independent ASan/UBSan tests passed 3/3 and fresh ESP-IDF v5.5.5 build produced size 0x18bca0 with 48% free. No hardware operation was performed.`
**Result: PASS**

The image is not a flash candidate: encrypted `eci.v1` transport, real PCM uplink, EIAD streaming/I2S tail-silence completion and HIL remain open. The source repository is private and has no root license; public push requires explicit rights-holder authorization.

VERDICT: PASS
