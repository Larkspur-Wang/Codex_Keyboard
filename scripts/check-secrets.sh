#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

files=()
while IFS= read -r -d '' file; do
  files+=("$file")
  if [[ "$file" =~ (^|/)(\.env($|\.)|secrets?/|evidence/private/) ]]; then
    echo "sensitive candidate path detected: $file" >&2
    exit 1
  fi
done < <(git ls-files --cached --others --exclude-standard -z)

pattern='sk-[A-Za-z0-9_-]{20,}|Bearer[[:space:]]+[A-Za-z0-9._-]{20,}|(DASHSCOPE_API_KEY|QWEN_API_KEY)[[:space:]]*=[[:space:]]*[^<`[:space:]]+|-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----'
scan_files=()
for file in "${files[@]}"; do
  if [[ "$file" != "scripts/check-secrets.sh" ]]; then
    scan_files+=("$file")
  fi
done

if (( ${#scan_files[@]} > 0 )) && rg -I -q -e "$pattern" -- "${scan_files[@]}"; then
  echo "credential-like candidate content detected" >&2
  exit 1
fi

echo "secret scan passed"
