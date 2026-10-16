import { spawnSync } from "node:child_process";
import { dirname } from "node:path";
import { createRequire } from "node:module";

if (process.platform !== "win32") throw new Error("Run this build on Windows");
const env = { ...process.env };
const pathKey =
  Object.keys(env).find((key) => key.toLowerCase() === "path") ?? "PATH";
const compiler = spawnSync("where.exe", ["windres.exe"], { encoding: "utf8" });
if (compiler.status === 0) {
  // cc1 is outside bin and resolves its DLLs through PATH. Another MinGW
  // distribution earlier in PATH can otherwise cause STATUS_ENTRYPOINT_NOT_FOUND.
  const bin = dirname(compiler.stdout.trim().split(/\r?\n/)[0]);
  env[pathKey] = `${bin};${env[pathKey] ?? ""}`;
}
const require = createRequire(import.meta.url);
const result = spawnSync(
  process.execPath,
  [require.resolve("@tauri-apps/cli/tauri.js"), "build", "--no-bundle"],
  {
    env,
    stdio: "inherit",
  },
);
if (result.error) throw result.error;
if (result.status !== 0) process.exit(result.status ?? 1);
await import("./package-windows.mjs");
