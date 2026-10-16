import { copyFile, access } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

if (process.platform !== "win32") {
  throw new Error("Windows desktop packaging requires Windows");
}

const release = new URL("../../../target/release/", import.meta.url);
// The GNU build loads this DLL beside the executable. Keep both exe names
// in this directory so an old product-name copy cannot survive an upgrade.
await access(new URL("WebView2Loader.dll", release));
try {
  await copyFile(
    new URL("easy-codex-desktop.exe", release),
    new URL("DeepSeekHarness KeyBroad.exe", release),
  );
  await copyFile(
    new URL("easy-codex-desktop.exe", release),
    new URL("Codex Keyboard.exe", release),
  );
} catch (cause) {
  throw new Error(
    "Close DeepSeekHarness KeyBroad before replacing the desktop exe",
    {
      cause,
    },
  );
}
for (const name of [
  "easy-codex-desktop.exe",
  "DeepSeekHarness KeyBroad.exe",
  "Codex Keyboard.exe",
]) {
  const label = spawnSync(
    "icacls.exe",
    [fileURLToPath(new URL(name, release)), "/setintegritylevel", "M"],
    { encoding: "utf8" },
  );
  if (label.status !== 0)
    throw new Error(
      "Cannot set normal Medium integrity on the desktop exe; build from a normal user terminal",
    );
}
console.log(
  `Windows desktop ready: ${fileURLToPath(new URL("DeepSeekHarness KeyBroad.exe", release))}`,
);
