import { presentProbe, presentSlotStatus, sortedTasks } from "./view-model.js";

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
  slots: requiredElement("#slots"),
  slotTemplate: /** @type {HTMLTemplateElement} */ (
    requiredElement("#slot-template")
  ),
  taskCount: requiredElement("#task-count"),
  providerDot: requiredElement("#provider-dot"),
  providerState: requiredElement("#provider-state"),
  asrModel: requiredElement("#asr-model"),
  ttsModel: requiredElement("#tts-model"),
  ttsVoice: requiredElement("#tts-voice"),
};

/** @type {import("./view-model.js").DashboardSnapshot | null} */
let dashboard = null;
const dirtySlots = new Set();

/** @param {import("./view-model.js").HostView} view */
function renderHealth(view) {
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

/** @param {Element | null} element @returns {HTMLElement} */
function rowElement(element) {
  if (!(element instanceof HTMLElement))
    throw new Error("invalid slot template");
  return element;
}

/** @param {HTMLElement} row @param {string} selector @returns {HTMLElement} */
function childElement(row, selector) {
  const element = row.querySelector(selector);
  if (!(element instanceof HTMLElement))
    throw new Error("invalid slot template");
  return element;
}

/** @param {number} slot @returns {import("./view-model.js").DashboardSlot | undefined} */
function slotSnapshot(slot) {
  return dashboard?.slots.find((candidate) => candidate.slot === slot);
}

/** @param {import("./view-model.js").DashboardSnapshot} snapshot */
function renderDashboard(snapshot) {
  dashboard = snapshot;
  const tasks = sortedTasks(snapshot.tasks);
  elements.taskCount.textContent = `${tasks.length} 个任务`;
  elements.providerDot.className = `provider-dot ${snapshot.provider.configured ? "ready" : "offline"}`;
  elements.providerState.textContent = snapshot.provider.configured
    ? "北京区 · 已就绪"
    : "北京区 · 未配置";
  elements.asrModel.textContent = snapshot.provider.asr_model;
  elements.ttsModel.textContent = snapshot.provider.tts_model;
  elements.ttsVoice.textContent = snapshot.provider.voice;

  const existingRows = new Map(
    Array.from(elements.slots.querySelectorAll(".slot-row")).map((row) => [
      Number(rowElement(row).dataset.slot),
      rowElement(row),
    ]),
  );
  for (const slot of [...snapshot.slots].sort(
    (left, right) => left.slot - right.slot,
  )) {
    let row = existingRows.get(slot.slot);
    if (!row) {
      const fragment = elements.slotTemplate.content.cloneNode(true);
      if (!(fragment instanceof DocumentFragment))
        throw new Error("invalid slot template");
      const newRow = rowElement(fragment.querySelector(".slot-row"));
      row = newRow;
      newRow.dataset.slot = String(slot.slot);
      childElement(newRow, ".talk-key").textContent = `S${slot.slot}`;
      childElement(newRow, ".play-key").textContent = `S${slot.slot + 4}`;
      const label = /** @type {HTMLLabelElement} */ (
        childElement(newRow, ".slot-label")
      );
      const select = /** @type {HTMLSelectElement} */ (
        childElement(newRow, ".task-select")
      );
      const button = /** @type {HTMLButtonElement} */ (
        childElement(newRow, ".bind-button")
      );
      const selectId = `slot-${slot.slot}-task`;
      label.htmlFor = selectId;
      label.textContent = `槽位 ${slot.slot}`;
      select.id = selectId;
      select.ariaLabel = `槽位 ${slot.slot} Codex 任务`;
      button.ariaLabel = `绑定槽位 ${slot.slot}`;
      select.addEventListener("change", () => dirtySlots.add(slot.slot));
      button.addEventListener("click", () => void bindSlot(slot.slot, newRow));
      elements.slots.append(newRow);
    }
    const select = /** @type {HTMLSelectElement} */ (
      childElement(row, ".task-select")
    );
    const selected = dirtySlots.has(slot.slot)
      ? select.value
      : (slot.task_id ?? "");
    const optionSignature = tasks.map((task) => task.task_id).join("|");
    if (select.dataset.options !== optionSignature) {
      select.replaceChildren(new Option("选择 Codex 任务", "", true, false));
      const placeholder = select.options.item(0);
      if (placeholder) placeholder.disabled = true;
      for (const task of tasks) {
        const marker = task.pinned ? "置顶 · " : "";
        select.add(
          new Option(`${marker}${task.name} · ${task.project}`, task.task_id),
        );
      }
      select.dataset.options = optionSignature;
    }
    if (
      selected &&
      !Array.from(select.options).some((option) => option.value === selected)
    ) {
      select.add(
        new Option(`${slot.task_name ?? "不可用任务"} · 已移出列表`, selected),
      );
    }
    select.value = selected;
    childElement(row, ".slot-meta").textContent = presentSlotStatus(slot);
  }
}

/** @param {number} slot @param {HTMLElement} row */
async function bindSlot(slot, row) {
  const select = /** @type {HTMLSelectElement} */ (
    childElement(row, ".task-select")
  );
  const button = /** @type {HTMLButtonElement} */ (
    childElement(row, ".bind-button")
  );
  if (!select.value || !dashboard) return;
  const current = slotSnapshot(slot);
  button.disabled = true;
  row.classList.add("saving");
  try {
    const invoke = window.__TAURI__?.core?.invoke;
    if (!invoke) throw new Error("tauri_unavailable");
    const updated = await invoke("bind_slot", {
      slot,
      taskId: select.value,
      expectedGeneration: current?.binding_generation ?? null,
    });
    dirtySlots.delete(slot);
    renderDashboard(updated);
    row.classList.add("saved");
    window.setTimeout(() => row.classList.remove("saved"), 900);
  } catch {
    row.classList.add("failed");
    window.setTimeout(() => row.classList.remove("failed"), 1500);
    await refreshDashboard();
  } finally {
    button.disabled = false;
    row.classList.remove("saving");
  }
}

async function refreshDashboard() {
  const invoke = window.__TAURI__?.core?.invoke;
  if (!invoke) throw new Error("tauri_unavailable");
  const probe = await invoke("host_dashboard");
  if (probe.connection === "healthy") {
    renderDashboard(probe.dashboard);
    return;
  }
  elements.taskCount.textContent = "Host 离线";
  elements.providerDot.className = "provider-dot offline";
  elements.providerState.textContent = "不可达";
}

async function refresh() {
  elements.refresh.disabled = true;
  elements.refresh.classList.add("spinning");
  try {
    const invoke = window.__TAURI__?.core?.invoke;
    if (!invoke) throw new Error("tauri_unavailable");
    const [probe] = await Promise.all([
      invoke("host_health"),
      refreshDashboard(),
    ]);
    renderHealth(presentProbe(probe));
  } catch {
    renderHealth(
      presentProbe({ connection: "offline", reason: "invoke_failed" }),
    );
  } finally {
    elements.refresh.disabled = false;
    elements.refresh.classList.remove("spinning");
  }
}

elements.refresh.addEventListener("click", refresh);
void refresh();
window.setInterval(refresh, 3000);
