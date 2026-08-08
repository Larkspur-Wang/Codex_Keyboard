#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo test -p easy-codex-host --example m3_runtime_gate

pnpm exec prettier --check \
  'app/**/*.{ts,tsx,js,mjs,css,html,json}' \
  '*.{json,yaml}' \
  '.github/**/*.yml'
pnpm typecheck
pnpm test
pnpm build

cmake -S firmware/host_test -B firmware/build-host-test
cmake --build firmware/build-host-test --parallel
ctest --test-dir firmware/build-host-test --output-on-failure

./scripts/check-secrets.sh
./scripts/audit-sources.sh

echo "fast eval passed"
