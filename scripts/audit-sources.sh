#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

cargo metadata --format-version 1 | node -e '
let input = "";
process.stdin.on("data", (chunk) => (input += chunk));
process.stdin.on("end", () => {
  const metadata = JSON.parse(input);
  const missing = metadata.packages
    .filter((pkg) => !pkg.license && !pkg.license_file)
    .map((pkg) => pkg.name);
  if (missing.length) {
    console.error(`Rust packages without licenses: ${missing.join(", ")}`);
    process.exit(1);
  }
  const forbidden = metadata.packages
    .filter((pkg) => /(^|[^L])AGPL|SSPL/i.test(pkg.license ?? ""))
    .map((pkg) => `${pkg.name}:${pkg.license}`);
  if (forbidden.length) {
    console.error(`Forbidden Rust dependency licenses: ${forbidden.join(", ")}`);
    process.exit(1);
  }
  const unexpectedSources = metadata.packages
    .filter((pkg) => pkg.source && !pkg.source.startsWith("registry+https://github.com/rust-lang/crates.io-index"))
    .map((pkg) => `${pkg.name}:${pkg.source}`);
  if (unexpectedSources.length) {
    console.error(`Unexpected Rust dependency sources: ${unexpectedSources.join(", ")}`);
    process.exit(1);
  }
});
'

pnpm licenses list --json | node -e '
let input = "";
process.stdin.on("data", (chunk) => (input += chunk));
process.stdin.on("end", () => {
  const report = JSON.parse(input);
  const licenses = Object.keys(report);
  const forbidden = licenses.filter((license) => /(^|[^L])AGPL|SSPL/i.test(license));
  if (forbidden.length) {
    console.error(`Forbidden dependency licenses: ${forbidden.join(", ")}`);
    process.exit(1);
  }
});
'

echo "source and license audit passed"
