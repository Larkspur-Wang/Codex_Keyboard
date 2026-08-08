import { access } from "node:fs/promises";

const required = ["index.html", "main.js", "styles.css", "view-model.js"];
await Promise.all(
  required.map((file) => access(new URL(`../ui/${file}`, import.meta.url))),
);
console.log("desktop UI assets verified");
