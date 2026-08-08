# M2A Host local state evidence

## Candidate and scope

- Source candidate: `881b5624b98e15c836cbfeb2b2c307fd70e08970`
- Diff reviewed: `7fe16a7..881b562`
- Evidence level: static + automated only
- Physical keyboard: not connected, reset or flashed
- Secrets: no real key, Codex task UUID, prompt, source path or cache payload used

This stage establishes the Host state substrate. It does not claim that the daemon, LaunchAgent, Keychain import,
encrypted TTS cache or Tauri health shell exists yet.

## Implemented contract

- `~/Library/Application Support/EasyCodexInput` ownership boundary with `0700` directories and `0600` state,
  lock and SQLite sidecar files.
- User-writable path components reject symlinks; final files use `O_NOFOLLOW`; database and existing sidecars retain
  file guards and verify `dev + ino` identity around SQLite open/migration/recovery.
- SQLite schema v1 uses `IMMEDIATE` transactions, rollback journal `DELETE`, `synchronous=FULL`, foreign keys and a
  process-exclusive lock acquired before migration or interrupted-job recovery.
- Binding updates are generation CAS and fail rather than saturate. Duplicate request ids are either exact immutable
  replays or explicit conflicts.
- Jobs are ordered by monotonic sequence, permit one running row per task, and carry claim generation so a completion
  from an earlier recovered claim cannot complete the new claim.
- Reopen returns every `running` job to `queued` and increments its recovery counter exactly once per interrupted claim.
- Future schema versions are rejected before persistent PRAGMA changes.

## Automated evidence

Local `./scripts/eval-fast.sh` passed with:

- 14 Host unit tests, including nested symlink, file replacement, live rollback journal mode, future WAL schema,
  second Host exclusion, stale completion, request conflict, generation overflow and 100 reopen cycles.
- 6 Rust protocol tests, 34 TypeScript tests, 1 firmware host test, production PWA build and relay dry run.
- Rust formatting/clippy, secret scan and source/license audit.

Exact-commit GitHub Actions run
[`30754424358`](https://github.com/Larkspur-Wang/easy-codex-input/actions/runs/30754424358) passed:

- `host-and-protocol`: 56 seconds
- `firmware`: 167 seconds
- `ble-peripheral-simulator`: 21 seconds

## Independent review

The required final reviewer was an ephemeral, read-only `gpt-5.6-sol` session with reasoning effort `high`. Oracle,
network access, tests and edits were prohibited. Its first pass found three P2 findings: nested symlink traversal,
database/sidecar identity continuity and future-schema journal-mode mutation. After regression fixes and full eval,
the final review of `7fe16a7..881b562` reported `NO_P0_P1_P2`.

Preliminary Terra findings remain development history, not the M2A final release gate.

## Evidence boundary

The 100-cycle check repeatedly drops and reopens `StateStore`; it is deterministic automated recovery evidence, not
an OS `SIGKILL`, power-loss or filesystem fault test. M2 still owes separate-process kill points, hot journal/WAL,
disk-full/I/O/corruption behavior, LaunchAgent login/restart, Keychain import, Beijing DashScope handshake and encrypted
cache evidence. A malicious same-UID process that unlinks the cooperative lock or writes SQLite directly is outside the
`0700/0600` local boundary and remains a documented residual risk.
