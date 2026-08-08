import { describe, expect, it } from "vitest";
import { presentProbe } from "../ui/view-model.js";

describe("desktop Host health view", () => {
  it("renders the authoritative healthy snapshot", () => {
    expect(
      presentProbe({
        connection: "healthy",
        health: {
          v: 1,
          status: "ready",
          host_version: "0.1.0",
          pid: 321,
          started_at_unix_ms: 1,
          socket: "run/host.sock",
          database_schema: 1,
          recovered_jobs_on_start: 2,
        },
      }),
    ).toEqual({
      tone: "ready",
      title: "Host 正常运行",
      detail: "启动时恢复了 2 个任务",
      version: "0.1.0",
      pid: "321",
      socket: "run/host.sock · 0600",
      schema: "v1",
    });
  });

  it("keeps offline distinct from invalid protocol", () => {
    expect(
      presentProbe({ connection: "offline", reason: "unreachable" }).tone,
    ).toBe("offline");
    expect(
      presentProbe({ connection: "protocol_error", reason: "invalid" }).tone,
    ).toBe("error");
  });
});
