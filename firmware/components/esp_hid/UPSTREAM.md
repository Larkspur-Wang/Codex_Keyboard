# EasyInput ESP HID safety adapter

This project component intentionally overrides ESP-IDF's built-in `esp_hid`
component. ESP-IDF gives project components precedence over components with the
same name under `IDF_PATH/components`.

## Audited upstream

- ESP-IDF release: `v5.5.5`
- ESP-IDF commit: `b774170ff46c393eeb5e495ea37936038d3f4f4f`
- Upstream component: `components/esp_hid`
- Upstream `src/nimble_hidd.c` SHA-256:
  `b117a0236c2086d07b74fb6ca356be62d5e94335b020abc18f53a2ec9dec7428`
- License: Apache-2.0

The upstream source retains its Espressif copyright and SPDX headers.

## Local delta

Only `src/nimble_hidd.c` is copied and modified locally. `Kconfig` is kept as
a byte-identical copy of the audited upstream file so ESP-IDF can discover the
same component options from the project override. The override
`CMakeLists.txt` obtains the original component path through
`COMPONENT_OVERRIDEN_DIR` and compiles every other source and header directly
from the pinned ESP-IDF installation.

The local backend:

1. treats the connection that subscribes to a standard keyboard or mouse HID
   INPUT notification as the HID owner, including bonded subscription
   restoration, while vendor-defined INPUT reports cannot claim ownership;
2. prevents auxiliary App/configuration connections from replacing or
   disconnecting the HID owner;
3. replaces runtime report/mbuf assertions with recoverable `esp_err_t`
   results;
4. sends HID reports from the cached protocol mode so the hot path allocates
   only the notification mbuf, while keeping that cache synchronized with
   connection resets and host protocol writes;
5. clears stale HID ownership on a NimBLE host reset even when no ordinary GAP
   disconnect event is delivered;
6. exposes read-only owner and lifecycle snapshots. The lifecycle level also
   includes `host_generation + host_synced`; reset/sync update those fields
   under the HIDD mutex, so advertising can recover even if the best-effort
   START event is dropped. The handle-only helper remains for source
   compatibility;
7. exposes an expected-owner INPUT API that serializes exact
   `{connection handle, generation}` validation with NimBLE notification
   submission. Owner-changing subscription, disconnect, and host-reset paths
   use the same gate, while synchronous non-owning `NOTIFY_TX` callbacks bypass
   it to avoid mutex re-entry. The ordinary `esp_hidd_dev_input_set()` API
   remains source compatible and targets the owner current at submission time;
8. never calls the component event loop or an inherited host callback while
   holding the non-recursive HIDD state mutex. Event publication uses a
   bounded, non-blocking post plus an in-flight lifetime reference so teardown
   cannot delete the event loop underneath a publisher; the local event queue
   is increased from 5 to 16 entries, posts use zero timeout, saturation drops
   best-effort control events instead of blocking the NimBLE host task, and
   teardown closes new references then drains in-flight publishers before
   deleting the loop;
9. maps standard report-protocol keyboard/mouse IDs to their Boot Protocol
   characteristics when a host switches protocol. Keyboard packets zero the
   report-only Apple Fn byte; unrepresentable wheel/pan and vendor reports
   return explicit unsupported/not-found errors so the application can drop
   only stateless optional traffic while treating stateful failures as an
   owner-lifetime fault;
10. maps NimBLE errors without collapsing their meaning:
    `ENOMEM/ENOMEM_EVT -> ESP_ERR_NO_MEM`, `EAGAIN/EBUSY ->
    ESP_ERR_NOT_FINISHED`, `ENOTCONN -> ESP_ERR_INVALID_STATE`, and
    `ENOENT/ENOTSUP -> ESP_ERR_NOT_FOUND/ESP_ERR_NOT_SUPPORTED`; and
11. exposes an authoritative lifecycle snapshot plus exact-owner terminate.
    The application keeps a permanently failed owner quarantined until the
    snapshot generation changes; terminate failure or timeout escalates to a
    bounded NimBLE host reset, so recovery does not depend on best-effort
    START/DISCONNECT component events.

The component fails configuration when either `IDF_VER` or the audited
upstream source hash differs. Any ESP-IDF update requires re-auditing and
refreshing this adapter rather than silently compiling against a new backend.
