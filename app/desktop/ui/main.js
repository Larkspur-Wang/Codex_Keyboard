import { presentProbe } from "./view-model.js";

/** @param {string} selector @returns {HTMLElement} */
function requiredElement(selector) {
  const element = document.querySelector(selector);
  if (!(element instanceof HTMLElement)) {
    throw new Error(`missing UI element: ${selector}`);
  }
  return element;
}

const elements = {
  refresh: /** @type {HTMLButtonElement} */ (requiredElement("#refresh")),
  band: requiredElement("#status-band"),
  dot: requiredElement("#status-dot"),
  title: requiredElement("#status-title"),
  detail: requiredElement("#status-detail"),
  version: requiredElement("#host-version"),
  pid: requiredElement("#host-pid"),
  socket: requiredElement("#host-socket"),
  schema: requiredElement("#database-schema"),
  updated: requiredElement("#last-updated"),
};

/** @param {import("./view-model.js").HostView} view */
function render(view) {
  elements.band.className = `status-band ${view.tone}`;
  elements.dot.className = `status-dot ${view.tone}`;
  elements.title.textContent = view.title;
  elements.detail.textContent = view.detail;
  elements.version.textContent = view.version;
  elements.pid.textContent = view.pid;
  elements.socket.textContent = view.socket;
  elements.schema.textContent = view.schema;
  elements.updated.textContent = `更新于 ${new Intl.DateTimeFormat("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).format(new Date())}`;
}

async function refresh() {
  elements.refresh.disabled = true;
  elements.refresh.classList.add("spinning");
  try {
    const invoke = window.__TAURI__?.core?.invoke;
    if (!invoke) {
      throw new Error("tauri_unavailable");
    }
    const probe = await invoke("host_health");
    render(presentProbe(probe));
  } catch {
    render(presentProbe({ connection: "offline", reason: "invoke_failed" }));
  } finally {
    elements.refresh.disabled = false;
    elements.refresh.classList.remove("spinning");
  }
}

elements.refresh.addEventListener("click", refresh);
void refresh();
window.setInterval(refresh, 3000);
