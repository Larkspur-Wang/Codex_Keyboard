#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
./scripts/eval-fast.sh

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH is required for the full firmware eval" >&2
  exit 1
fi

rm -rf firmware/build-full-eval
idf.py -C firmware -B "$PWD/firmware/build-full-eval" build

echo "full software eval passed; live-service and HIL gates remain separate"
