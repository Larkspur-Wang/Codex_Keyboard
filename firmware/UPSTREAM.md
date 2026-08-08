# Firmware upstream provenance

## Source snapshot

- Repository: `https://github.com/AIoTWan/easy-input-keyboard`
- Commit: `7a41ce1c5bd961f3be417498cee1a8131ab9d86e`
- Imported: 2026-08-04
- Destination: this repository's `firmware/`

The import copied the production V2 firmware roots `components/esp_hid`, `components/keyboard`, `diagnostics`,
`features`, `main`, and `host_test`, plus the root firmware `CMakeLists.txt`, `sdkconfig.defaults`, and
`partitions.csv`. The existing Easy Codex Input `components/eci_protocol` and its Host test were retained and
merged into the imported test build. The source repository remains read-only.

## License boundary

The source snapshot did not contain a repository-root license file. On 2026-08-04 the user explicitly confirmed
that they hold the rights needed to publish this firmware in `easy-codex-input`; on 2026-08-08 they explicitly
requested this extracted repository to be public. `Codex_Keyboard` therefore publishes the imported and modified
product firmware under the repository Apache-2.0 license. This authorization is project-specific and must not be
inferred for unrelated copies of the upstream repository.

`components/esp_hid` contains its own Apache-2.0 `LICENSE` and `UPSTREAM.md`; both are preserved verbatim. Any
other third-party component added later must keep its license and modification notice in this repository.

## Product boundary

The imported snapshot is a buildable V2 hardware baseline, not proof that the Easy Codex Input behavior is
complete. At import time it still contains the former generic eight-key configuration and sound-asset behavior.
The target product maps S1-S4 to four PTT slots and S5-S8 to the corresponding summary playback requests. Device
firmware never stores Codex task identifiers.
