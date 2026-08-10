import { access } from "node:fs/promises";

const required = ["index.html", "main.js", "styles.css", "view-model.js"];
const icons = [
  "../assets/codex-keyboard-icon-source.png",
  "../src-tauri/icons/icon.png",
  "../src-tauri/icons/icon.icns",
];
await Promise.all(
  [
    required.map((file) => access(new URL(`../ui/${file}`, import.meta.url))),
    icons.map((file) => access(new URL(file, import.meta.url))),
  ].flat(),
);
console.log("desktop UI assets verified");
