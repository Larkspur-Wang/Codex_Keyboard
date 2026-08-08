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
