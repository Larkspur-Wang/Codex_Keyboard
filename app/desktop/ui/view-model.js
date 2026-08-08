/**
 * @typedef {{
 *   v: number,
 *   status: "ready",
 *   host_version: string,
 *   pid: number,
 *   started_at_unix_ms: number,
 *   socket: string,
 *   database_schema: number,
 *   recovered_jobs_on_start: number
 * }} HealthSnapshot
 *
 * @typedef {
 *   | {connection: "healthy", health: HealthSnapshot}
 *   | {connection: "offline", reason: string}
 *   | {connection: "protocol_error", reason: string}
 * } HostProbe
 *
 * @typedef {{
 *   tone: "ready" | "offline" | "error",
 *   title: string,
 *   detail: string,
 *   version: string,
 *   pid: string,
 *   socket: string,
 *   schema: string
 * }} HostView
 *
 * @typedef {{
 *   task_id: string,
 *   name: string,
 *   project: string,
 *   updated_at_ms: number,
 *   pinned: boolean
 * }} DashboardTask
 *
 * @typedef {{
 *   slot: number,
 *   task_id: string | null,
 *   task_name: string | null,
 *   project: string | null,
 *   binding_generation: number | null,
 *   pending_jobs: number,
 *   unread_generation: number | null,
 *   unread_coverage: number | null
 * }} DashboardSlot
 *
 * @typedef {{
 *   v: number,
 *   tasks: DashboardTask[],
 *   slots: DashboardSlot[],
 *   provider: {
 *     configured: boolean,
 *     region: string,
 *     asr_model: string,
 *     tts_model: string,
 *     voice: string
 *   }
 * }} DashboardSnapshot
 */

/** @param {HostProbe} probe @returns {HostView} */
export function presentProbe(probe) {
  if (probe.connection === "healthy") {
    const recovered = probe.health.recovered_jobs_on_start;
    return {
      tone: "ready",
      title: "Host 正常运行",
      detail:
        recovered > 0 ? `启动时恢复了 ${recovered} 个任务` : "后台监控已就绪",
      version: probe.health.host_version,
      pid: String(probe.health.pid),
      socket: `${probe.health.socket} · 0600`,
      schema: `v${probe.health.database_schema}`,
    };
  }
  if (probe.connection === "offline") {
    return {
      tone: "offline",
      title: "Host 未连接",
      detail: "常驻服务当前不可达",
      version: "--",
      pid: "--",
      socket: "run/host.sock · 离线",
      schema: "--",
    };
  }
  return {
    tone: "error",
    title: "Health 协议异常",
    detail: "Host 响应未通过本地协议校验",
    version: "--",
    pid: "--",
    socket: "run/host.sock · 拒绝",
    schema: "--",
  };
}

/** @param {DashboardSlot} slot @returns {string} */
export function presentSlotStatus(slot) {
  const facts = [];
  if (slot.pending_jobs > 0) {
    facts.push(`队列 ${slot.pending_jobs}`);
  }
  if (slot.unread_generation !== null) {
    const coverage = slot.unread_coverage ?? 1;
    facts.push(`待听总结 ${coverage} 次`);
  }
  if (facts.length > 0) {
    return facts.join(" · ");
  }
  return slot.task_id ? "已绑定" : "未绑定";
}

/** @param {DashboardTask[]} tasks @returns {DashboardTask[]} */
export function sortedTasks(tasks) {
  return [...tasks].sort(
    (left, right) =>
      Number(right.pinned) - Number(left.pinned) ||
      right.updated_at_ms - left.updated_at_ms ||
      left.name.localeCompare(right.name, "zh-CN"),
  );
}
