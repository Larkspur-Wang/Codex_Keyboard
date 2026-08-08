/*
 * SPDX-FileCopyrightText: 2017-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modified by EasyInput contributors in 2026; see ../UPSTREAM.md.
 */

#include <stdbool.h>
#include <errno.h>
#include <string.h>

#include "ble_hidd.h"
#include "easy_input_esp_hid_owner.h"
#include "esp_private/esp_hidd_private.h"
#include "esp_log.h"
#include "freertos/semphr.h"

#include "nimble/nimble_opt.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_hci.h"
#include "host/ble_att.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/bas/ble_svc_bas.h"
#include "services/hid/ble_svc_hid.h"
#include "services/dis/ble_svc_dis.h"
#include "services/sps/ble_svc_sps.h"

#if CONFIG_BT_NIMBLE_HID_SERVICE

static const char *TAG = "NIMBLE_HIDD";
#define BLE_SVC_BAS_UUID16                    0x180F

typedef struct esp_ble_hidd_dev_s esp_ble_hidd_dev_t;
// there can be only one BLE HID device
static esp_ble_hidd_dev_t *s_dev = NULL;
static SemaphoreHandle_t s_hidd_mutex = NULL;
/*
 * Serializes exact HID-owner lifetime changes with INPUT submission.  Keep it
 * separate from s_hidd_mutex so synchronous non-owning NimBLE callbacks can
 * still inspect ordinary device state while a report is being submitted.
 *
 * Lock order whenever both are needed: s_owner_gate, then s_hidd_mutex.
 */
static SemaphoreHandle_t s_owner_gate = NULL;
/*
 * Event callbacks must publish outside s_hidd_mutex: the consumer is allowed
 * to call back into esp_hidd_dev_* and would otherwise deadlock behind a full
 * event queue. A separate lifetime gate keeps the captured event loop alive
 * until every bounded post call has returned.
 */
static SemaphoreHandle_t s_event_post_gate = NULL;
static SemaphoreHandle_t s_event_posts_drained = NULL;
static size_t s_event_posts_in_flight = 0;
static bool s_event_posts_closing = true;
static bool s_gap_listener_registered = false;
static void (*s_prev_reset_cb)(int reason) = NULL;
static void (*s_prev_sync_cb)(void) = NULL;
static struct ble_gap_event_listener nimble_gap_event_listener;
static void nimble_host_synced(void);
void nimble_host_reset(int reason);
static void nimble_report_write_cb(uint16_t attr_handle, uint8_t report_type, uint8_t report_id,
                                   const uint8_t *data, uint16_t len);
static void nimble_char_write_cb(uint16_t attr_handle, uint16_t char_uuid16, uint8_t value);
static int nimble_hidd_dev_input_set(void *devp, size_t index, size_t id,
                                    uint8_t *data, size_t length);

static inline void lock_hidd(void)
{
    if (s_hidd_mutex) {
        xSemaphoreTake(s_hidd_mutex, portMAX_DELAY);
    }
}

static inline void unlock_hidd(void)
{
    if (s_hidd_mutex) {
        xSemaphoreGive(s_hidd_mutex);
    }
}

static inline void lock_owner_gate(void)
{
    if (s_owner_gate) {
        xSemaphoreTake(s_owner_gate, portMAX_DELAY);
    }
}

static inline void unlock_owner_gate(void)
{
    if (s_owner_gate) {
        xSemaphoreGive(s_owner_gate);
    }
}

static inline void unlock_hidd_then_owner_gate(bool owner_gate_held)
{
    unlock_hidd();
    if (owner_gate_held) {
        unlock_owner_gate();
    }
}

typedef struct {
    esp_event_loop_handle_t loop;
    bool acquired;
} hidd_event_post_ref_t;

/* Caller must hold s_hidd_mutex. */
static bool acquire_event_post_ref_locked(esp_event_loop_handle_t loop,
                                          hidd_event_post_ref_t *ref)
{
    if (ref == NULL) {
        return false;
    }
    ref->loop = NULL;
    ref->acquired = false;
    if (s_event_post_gate == NULL) {
        return false;
    }

    xSemaphoreTake(s_event_post_gate, portMAX_DELAY);
    if (!s_event_posts_closing && loop != NULL) {
        s_event_posts_in_flight++;
        ref->loop = loop;
        ref->acquired = true;
    }
    xSemaphoreGive(s_event_post_gate);
    return ref->acquired;
}

static void release_event_post_ref(hidd_event_post_ref_t *ref)
{
    if (ref == NULL || !ref->acquired || s_event_post_gate == NULL) {
        return;
    }
    xSemaphoreTake(s_event_post_gate, portMAX_DELAY);
    if (s_event_posts_in_flight > 0) {
        s_event_posts_in_flight--;
    }
    if (s_event_posts_closing && s_event_posts_in_flight == 0 &&
            s_event_posts_drained != NULL) {
        xSemaphoreGive(s_event_posts_drained);
    }
    xSemaphoreGive(s_event_post_gate);
    ref->loop = NULL;
    ref->acquired = false;
}

static esp_err_t post_hidd_event_bounded(hidd_event_post_ref_t *ref,
                                         esp_hidd_event_t event,
                                         const void *data,
                                         size_t data_size)
{
    if (ref == NULL || !ref->acquired || ref->loop == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = esp_event_post_to(ref->loop, ESP_HIDD_EVENTS, event,
                                            data, data_size, 0);
    release_event_post_ref(ref);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Dropping HIDD event=%d: %s", event,
                 esp_err_to_name(err));
    }
    return err;
}

static void activate_event_posts(esp_event_loop_handle_t loop)
{
    if (s_event_post_gate == NULL) {
        return;
    }
    xSemaphoreTake(s_event_post_gate, portMAX_DELAY);
    s_event_posts_in_flight = 0;
    s_event_posts_closing = loop == NULL;
    if (s_event_posts_drained != NULL) {
        while (xSemaphoreTake(s_event_posts_drained, 0) == pdTRUE) {
        }
    }
    xSemaphoreGive(s_event_post_gate);
}

static bool begin_event_post_shutdown(void)
{
    if (s_event_post_gate == NULL) {
        return false;
    }
    xSemaphoreTake(s_event_post_gate, portMAX_DELAY);
    s_event_posts_closing = true;
    if (s_event_posts_drained != NULL) {
        while (xSemaphoreTake(s_event_posts_drained, 0) == pdTRUE) {
        }
    }
    const bool must_wait = s_event_posts_in_flight != 0;
    xSemaphoreGive(s_event_post_gate);
    return must_wait;
}

static void wait_for_event_posts(bool must_wait)
{
    if (must_wait && s_event_posts_drained != NULL) {
        xSemaphoreTake(s_event_posts_drained, portMAX_DELAY);
    }
}
/** service index is used to identify the hid service instance
    of the registered characteristic.
    Assuming the first instance of the hid service is registered first.
    Increment service index as the hid services get registered */
static int service_index = -1;

typedef hidd_report_item_t hidd_le_report_item_t;

typedef struct {
    esp_hid_raw_report_map_t    reports_map;
    uint8_t                     reports_len;
    hidd_le_report_item_t      *reports;
    uint16_t                    hid_svc;
    uint16_t                    hid_control_handle;
    uint16_t                    hid_protocol_handle;
} hidd_dev_map_t;

struct esp_ble_hidd_dev_s {
    esp_hidd_dev_t             *dev;
    esp_event_loop_handle_t     event_loop_handle;
    esp_hid_device_config_t     config;
    uint16_t                    appearance;

    bool                        connected;
    uint16_t                    conn_id;
    uint32_t                    owner_generation;
    uint32_t                    host_generation;
    bool                        host_synced;

    uint8_t                     control;    // 0x00 suspend, 0x01 suspend off
    uint8_t                     protocol;   // 0x00 boot, 0x01 report

    uint16_t                    bat_svc_handle;
    uint16_t                    info_svc_handle;
    struct ble_gatt_svc         hid_incl_svc;

    uint16_t                    bat_level_handle;
    uint8_t                     pnp[7]; /* something related to device info service */
    hidd_dev_map_t             *devices;
    uint8_t                     devices_len;
};

/*
 * Caller must hold s_owner_gate and s_hidd_mutex, in that order.
 */
static void advance_owner_generation_locked(esp_ble_hidd_dev_t *dev)
{
    if (dev == NULL) {
        return;
    }
    dev->owner_generation++;
    if (dev->owner_generation == 0) {
        /* Zero is reserved for application state that has not observed an
         * owner yet. */
        dev->owner_generation = 1;
    }
}

/* Caller must hold s_hidd_mutex. */
static void advance_host_generation_locked(esp_ble_hidd_dev_t *dev)
{
    if (dev == NULL) {
        return;
    }
    dev->host_generation++;
    if (dev->host_generation == 0) {
        dev->host_generation = 1;
    }
}

static esp_err_t nimble_error_to_esp(int rc)
{
    switch (rc) {
    case 0:
        return ESP_OK;
    case BLE_HS_ENOMEM:
    case BLE_HS_ENOMEM_EVT:
        return ESP_ERR_NO_MEM;
    case BLE_HS_EAGAIN:
    case BLE_HS_EBUSY:
        return ESP_ERR_NOT_FINISHED;
    case BLE_HS_ENOTCONN:
        return ESP_ERR_INVALID_STATE;
    case BLE_HS_EINVAL:
        return ESP_ERR_INVALID_ARG;
    case BLE_HS_EMSGSIZE:
        return ESP_ERR_INVALID_SIZE;
    case BLE_HS_ENOENT:
        return ESP_ERR_NOT_FOUND;
    case BLE_HS_ENOTSUP:
        return ESP_ERR_NOT_SUPPORTED;
    default:
        return ESP_FAIL;
    }
}

// HID Information characteristic value
static const uint8_t hidInfo[4] = {
    0x11, 0x01,     // bcdHID (USB HID version)
    0x00,           // bCountryCode
    ESP_HID_FLAGS_REMOTE_WAKE | ESP_HID_FLAGS_NORMALLY_CONNECTABLE   // Flags
};

static int create_hid_db(int device_index)
{
    int rc = 0;
    struct ble_svc_hid_params hparams = {0};
    int report_mode_rpts = 0;

    /* fill hid info */
    memcpy(&hparams.hid_info, hidInfo, sizeof hparams.hid_info);

    if (s_dev->devices[device_index].reports_map.len > REPORT_MAP_SIZE) {
        ESP_LOGE(TAG, "Report map too large (%d > %d); aborting", s_dev->devices[device_index].reports_map.len, REPORT_MAP_SIZE);
        return ESP_FAIL;
    }
    memcpy(&hparams.report_map, (uint8_t *)s_dev->devices[device_index].reports_map.data, s_dev->devices[device_index].reports_map.len);
    hparams.report_map_len = s_dev->devices[device_index].reports_map.len;
    hparams.external_rpt_ref = BLE_SVC_BAS_UUID16;

    /* fill protocol mode */
    hparams.proto_mode_present = 1;
    hparams.proto_mode = s_dev->protocol;

    for (uint8_t i = 0; i < s_dev->devices[device_index].reports_len; i++) {
        hidd_le_report_item_t *report = &s_dev->devices[device_index].reports[i];
        if (report->protocol_mode == ESP_HID_PROTOCOL_MODE_REPORT) {
            if (report_mode_rpts >= MAX_REPORTS) {
                ESP_LOGE(TAG, "Too many report-mode reports (%d >= MAX_REPORTS); truncating", report_mode_rpts);
                break;
            }
            /* only consider report mode reports, all boot mode reports will be registered by default */
            if (report->report_type == ESP_HID_REPORT_TYPE_INPUT) {
                /* Input Report */
                hparams.rpts[report_mode_rpts].type = ESP_HID_REPORT_TYPE_INPUT;
            } else if (report->report_type == ESP_HID_REPORT_TYPE_OUTPUT) {
                /* Output Report */
                hparams.rpts[report_mode_rpts].type = ESP_HID_REPORT_TYPE_OUTPUT;
            } else {
                /* Feature Report */
                hparams.rpts[report_mode_rpts].type = ESP_HID_REPORT_TYPE_FEATURE;
            }
            hparams.rpts[report_mode_rpts].id = report->report_id;
            report_mode_rpts++;
        } else {
            if (report->report_type == ESP_HID_REPORT_TYPE_INPUT) {
                /* Boot mode reports */
                if (report->usage == ESP_HID_USAGE_KEYBOARD) { //Boot Keyboard Input
                    hparams.kbd_inp_present = 1;
                } else { //Boot Mouse Input
                    hparams.mouse_inp_present = 1;
                }
            } else { //Boot Keyboard Output
                hparams.kbd_out_present = 1;
            }
        }
    }
    hparams.rpts_len = report_mode_rpts;
    /* Add service */
    rc = ble_svc_hid_add(hparams);
    if (rc != 0) {
        return rc;
    }
    return rc;
}

static int ble_hid_create_info_db(void)
{
    int rc;

    rc = 0;
    ble_svc_dis_init();
    uint8_t pnp_val[7] = {
        0x02, //0x1=BT, 0x2=USB
        s_dev->config.vendor_id & 0xFF, (s_dev->config.vendor_id >> 8) & 0xFF, //VID
                     s_dev->config.product_id & 0xFF, (s_dev->config.product_id >> 8) & 0xFF, //PID
                     s_dev->config.version & 0xFF, (s_dev->config.version >> 8) & 0xFF  //VERSION
    };
    memcpy(s_dev->pnp, pnp_val, 7);
    ble_svc_dis_pnp_id_set((char *)s_dev->pnp);
    if (s_dev->config.manufacturer_name && s_dev->config.manufacturer_name[0]) {
        rc = ble_svc_dis_manufacturer_name_set(s_dev->config.manufacturer_name);
    }
    if (s_dev->config.serial_number && s_dev->config.serial_number[0]) {
        rc = ble_svc_dis_serial_number_set(s_dev->config.serial_number);
    }
    return rc;
}

static int nimble_hid_start_gatts(void)
{
    int rc = ESP_OK;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_sps_init(0, 0); // initialize with 0
    ble_svc_bas_init();
    ble_hid_create_info_db();

    for (uint8_t d = 0; d < s_dev->devices_len; d++) {
        rc = create_hid_db(d);
        if (rc != 0) {
            return rc;
        }
    }
    /* init the hid svc */
    ble_svc_hid_init();

    return rc;
}

static int nimble_hid_stop_gatts(esp_ble_hidd_dev_t *dev)
{
    int rc = ESP_OK;

    if (s_gap_listener_registered) {
        ble_gap_event_listener_unregister(&nimble_gap_event_listener);
        s_gap_listener_registered = false;
    }

    if (dev && dev->connected) {
        ble_gap_terminate(dev->conn_id, BLE_ERR_REM_USER_CONN_TERM);
    }

    /* stop gatt database */
    ble_gatts_stop();

    ble_svc_hid_deinit();
    ble_svc_hid_reset();
    ble_svc_dis_deinit();
    ble_svc_bas_deinit();
    ble_svc_sps_deinit();
    ble_svc_gatt_deinit();
    ble_svc_gap_deinit();

    return rc;
}

/* Identify the reports using the report map */
static int ble_hid_init_config(esp_ble_hidd_dev_t *dev, const esp_hid_device_config_t *config)
{
    memset((uint8_t *)(&dev->config), 0, sizeof(esp_hid_device_config_t));
    dev->config.vendor_id = config->vendor_id;
    dev->config.product_id = config->product_id;
    dev->config.version = config->version;
    if (config->device_name != NULL) {
        dev->config.device_name = strdup(config->device_name);
    }
    if (config->manufacturer_name != NULL) {
        dev->config.manufacturer_name = strdup(config->manufacturer_name);
    }
    if (config->serial_number != NULL) {
        dev->config.serial_number = strdup(config->serial_number);
    }
    dev->appearance = ESP_HID_APPEARANCE_GENERIC;

    if (config->report_maps_len) {
        dev->devices = (hidd_dev_map_t *)malloc(config->report_maps_len * sizeof(hidd_dev_map_t));
        if (dev->devices == NULL) {
            ESP_LOGE(TAG, "devices malloc(%d) failed", config->report_maps_len);
            return ESP_FAIL;
        }
        memset(dev->devices, 0, config->report_maps_len * sizeof(hidd_dev_map_t));
        dev->devices_len = config->report_maps_len;
        for (uint8_t d = 0; d < dev->devices_len; d++) {

            //raw report map
            uint8_t *map = (uint8_t *)malloc(config->report_maps[d].len);
            if (map == NULL) {
                ESP_LOGE(TAG, "report map malloc(%d) failed", config->report_maps[d].len);
                return ESP_FAIL;
            }
            memcpy(map, config->report_maps[d].data, config->report_maps[d].len);

            dev->devices[d].reports_map.data = (const uint8_t *)map;
            dev->devices[d].reports_map.len = config->report_maps[d].len;

            esp_hid_report_map_t *rmap = esp_hid_parse_report_map(config->report_maps[d].data, config->report_maps[d].len);
            if (rmap == NULL) {
                ESP_LOGE(TAG, "hid_parse_report_map[%d](%d) failed", d, config->report_maps[d].len);
                return ESP_FAIL;
            }
            dev->appearance = rmap->appearance;
            dev->devices[d].reports_len = rmap->reports_len;
            dev->devices[d].reports = (hidd_le_report_item_t *)malloc(rmap->reports_len * sizeof(hidd_le_report_item_t));
            if (dev->devices[d].reports == NULL) {
                ESP_LOGE(TAG, "reports malloc(%d) failed", rmap->reports_len * sizeof(hidd_le_report_item_t));
                free(rmap->reports);
                free(rmap);
                return ESP_FAIL;
            }
            for (uint8_t r = 0; r < rmap->reports_len; r++) {
                dev->devices[d].reports[r].map_index = d;
                dev->devices[d].reports[r].report_id = rmap->reports[r].report_id;
                dev->devices[d].reports[r].protocol_mode = rmap->reports[r].protocol_mode;
                dev->devices[d].reports[r].report_type = rmap->reports[r].report_type;
                dev->devices[d].reports[r].usage = rmap->reports[r].usage;
                dev->devices[d].reports[r].value_len = rmap->reports[r].value_len;
            }
            free(rmap->reports);
            free(rmap);
        }
    }
    return ESP_OK;
}

static int ble_hid_free_config(esp_ble_hidd_dev_t *dev)
{
    for (uint8_t d = 0; d < dev->devices_len; d++) {
        free((void *)dev->devices[d].reports);
        free((void *)dev->devices[d].reports_map.data);
    }

    free((void *)dev->devices);
    free((void *)dev->config.device_name);
    free((void *)dev->config.manufacturer_name);
    free((void *)dev->config.serial_number);
    if (dev->event_loop_handle != NULL) {
        esp_event_loop_delete(dev->event_loop_handle);
    }
    return ESP_OK;
}

static int nimble_hidd_dev_deinit(void *devp)
{
    esp_ble_hidd_dev_t *dev = (esp_ble_hidd_dev_t *)devp;
    bool wait_event_posts = false;
    lock_owner_gate();
    lock_hidd();
    if (!s_dev) {
        ESP_LOGE(TAG, "HID device profile already uninitialized");
        unlock_hidd();
        unlock_owner_gate();
        return ESP_OK;
    }

    if (s_dev != dev) {
        ESP_LOGE(TAG, "Wrong HID device provided");
        unlock_hidd();
        unlock_owner_gate();
        return ESP_FAIL;
    }
    wait_event_posts = begin_event_post_shutdown();
    s_dev = NULL;
    service_index = -1;

    if (ble_hs_cfg.reset_cb == nimble_host_reset) {
        ble_hs_cfg.reset_cb = s_prev_reset_cb;
    }
    if (ble_hs_cfg.sync_cb == nimble_host_synced) {
        ble_hs_cfg.sync_cb = s_prev_sync_cb;
    }
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_svc_hid_register_report_write_cb(NULL);
    ble_svc_hid_register_char_write_cb(NULL);
    unlock_hidd();
    unlock_owner_gate();

    /* NimBLE stop/terminate can synchronously invoke callbacks. Global state
     * is already detached, so those callbacks observe a closed device without
     * re-entering locks held by this teardown path. */
    nimble_hid_stop_gatts(dev);
    wait_for_event_posts(wait_event_posts);

    /* STOP_EVENT may be discarded because ble_hid_free_config() deletes the event
     * loop right after this call.  This does not cause a crash or data corruption. */
    const esp_err_t stop_post_err = esp_event_post_to(
            dev->event_loop_handle, ESP_HIDD_EVENTS, ESP_HIDD_STOP_EVENT,
            NULL, 0, 0);
    if (stop_post_err != ESP_OK) {
        ESP_LOGW(TAG, "Dropping HIDD stop event: %s",
                 esp_err_to_name(stop_post_err));
    }
    ble_hid_free_config(dev);
    free(dev);
    /* Synchronization primitives are component-lifetime objects. Keeping
     * them allocated avoids deleting a mutex while a callback that was
     * already dispatched is still waiting to observe s_dev == NULL. */
    return ESP_OK;
}

static bool nimble_hidd_dev_connected(void *devp)
{
    esp_ble_hidd_dev_t *dev = (esp_ble_hidd_dev_t *)devp;
    bool connected;

    lock_hidd();
    connected = dev != NULL && s_dev == dev && dev->connected &&
                dev->conn_id != BLE_HS_CONN_HANDLE_NONE;
    unlock_hidd();
    return connected;
}

esp_err_t easy_input_hidd_owner_snapshot_get(
    esp_hidd_dev_t *dev,
    easy_input_hidd_owner_snapshot_t *owner_snapshot)
{
    if (owner_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    owner_snapshot->conn_handle = EASY_INPUT_HIDD_OWNER_NONE;
    owner_snapshot->generation = 0;
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_hidd();
    if (s_dev == NULL || s_dev->dev != dev) {
        unlock_hidd();
        return ESP_ERR_INVALID_STATE;
    }
    owner_snapshot->generation = s_dev->owner_generation;
    if (!s_dev->connected || s_dev->conn_id == BLE_HS_CONN_HANDLE_NONE) {
        unlock_hidd();
        return ESP_ERR_INVALID_STATE;
    }
    owner_snapshot->conn_handle = s_dev->conn_id;
    unlock_hidd();
    return ESP_OK;
}

esp_err_t easy_input_hidd_lifecycle_snapshot_get(
    esp_hidd_dev_t *dev,
    easy_input_hidd_lifecycle_snapshot_t *lifecycle_snapshot)
{
    if (lifecycle_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lifecycle_snapshot->conn_handle = EASY_INPUT_HIDD_OWNER_NONE;
    lifecycle_snapshot->generation = 0;
    lifecycle_snapshot->host_generation = 0;
    lifecycle_snapshot->connected = false;
    lifecycle_snapshot->host_synced = false;
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_hidd();
    if (s_dev == NULL || s_dev->dev != dev) {
        unlock_hidd();
        return ESP_ERR_INVALID_STATE;
    }
    lifecycle_snapshot->generation = s_dev->owner_generation;
    lifecycle_snapshot->host_generation = s_dev->host_generation;
    lifecycle_snapshot->host_synced = s_dev->host_synced;
    lifecycle_snapshot->connected =
            s_dev->connected && s_dev->conn_id != BLE_HS_CONN_HANDLE_NONE;
    if (lifecycle_snapshot->connected) {
        lifecycle_snapshot->conn_handle = s_dev->conn_id;
    }
    unlock_hidd();
    return ESP_OK;
}

esp_err_t easy_input_hidd_owner_terminate(
    esp_hidd_dev_t *dev,
    const easy_input_hidd_owner_snapshot_t *expected_owner,
    uint8_t hci_reason)
{
    if (dev == NULL || expected_owner == NULL ||
            expected_owner->conn_handle == EASY_INPUT_HIDD_OWNER_NONE ||
            expected_owner->generation == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev->transport != ESP_HID_TRANSPORT_BLE ||
            dev->input_set != nimble_hidd_dev_input_set) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    lock_owner_gate();
    lock_hidd();
    if (s_dev == NULL || s_dev->dev != dev || !s_dev->connected ||
            s_dev->conn_id == BLE_HS_CONN_HANDLE_NONE ||
            s_dev->conn_id != expected_owner->conn_handle ||
            s_dev->owner_generation != expected_owner->generation) {
        unlock_hidd();
        unlock_owner_gate();
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t conn_handle = s_dev->conn_id;
    unlock_hidd();
    const int rc = ble_gap_terminate(conn_handle, hci_reason);
    unlock_owner_gate();
    if (rc == BLE_HS_EALREADY) {
        return ESP_OK;
    }
    return nimble_error_to_esp(rc);
}

esp_err_t easy_input_hidd_owner_conn_handle_get(
    esp_hidd_dev_t *dev,
    uint16_t *owner_conn_handle)
{
    easy_input_hidd_owner_snapshot_t owner_snapshot = {
        .conn_handle = EASY_INPUT_HIDD_OWNER_NONE,
        .generation = 0,
    };
    if (owner_conn_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err =
        easy_input_hidd_owner_snapshot_get(dev, &owner_snapshot);
    *owner_conn_handle = owner_snapshot.conn_handle;
    return err;
}

static int nimble_hidd_dev_battery_set(void *devp, uint8_t level)
{
    esp_ble_hidd_dev_t *dev = (esp_ble_hidd_dev_t *)devp;
    lock_owner_gate();
    lock_hidd();
    if (!dev || s_dev != dev) {
        unlock_hidd();
        unlock_owner_gate();
        return ESP_FAIL;
    }
    unlock_hidd();

    /*
     * ble_svc_bas_battery_level_set() enters the NimBLE host and may schedule a
     * local characteristic read. Never carry the HIDD state mutex across that
     * protocol-stack boundary: a synchronous callback is allowed to inspect
     * HIDD state and would otherwise wait forever on this same mutex.
     */
    const int rc = ble_svc_bas_battery_level_set(level);
    if (rc == 0) {
        unlock_owner_gate();
        return ESP_OK;
    }

    lock_hidd();
    const bool connected = s_dev == dev && s_dev->connected;
    unlock_hidd();
    unlock_owner_gate();
    if (connected) {
        ESP_LOGE(TAG, "ble_svc_bas_battery_level_set failed: %d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* if mode is NULL, find the first matching report */
static hidd_le_report_item_t* find_report(uint8_t id, uint8_t type, uint8_t *mode)
{
    hidd_le_report_item_t *rpt;
    if (!s_dev) {
        return NULL;
    }
    for (uint8_t d = 0; d < s_dev->devices_len; d++) {
        for (uint8_t i = 0; i < s_dev->devices[d].reports_len; i++) {
            rpt = &s_dev->devices[d].reports[i];
            if (rpt->report_id == id && rpt->report_type == type && (!mode || (mode && *mode == rpt->protocol_mode))) {
                return rpt;
            }
        }
    }
    return NULL;
}

static hidd_le_report_item_t* find_report_by_usage_and_type(uint8_t dev_index, uint8_t usage, uint8_t type, uint8_t *mode)
{
    hidd_le_report_item_t *rpt;
    if (!s_dev || dev_index >= s_dev->devices_len) {
        return NULL;
    }
    for (uint8_t i = 0; i < s_dev->devices[dev_index].reports_len; i++) {
        rpt = &s_dev->devices[dev_index].reports[i];
        if (rpt->usage == usage && rpt->report_type == type && (!mode || (mode && *mode == rpt->protocol_mode))) {
            return rpt;
        }
    }
    return NULL;
}

/*
 * Report Protocol identifies reports by Report ID, while Boot Protocol uses
 * the fixed keyboard/mouse characteristics without those IDs. Resolve the
 * standard usage that owns a report-mode ID, then select its boot equivalent.
 * Vendor-defined reports intentionally have no boot fallback.
 */
static hidd_le_report_item_t* find_boot_report_for_report_id(uint8_t id,
                                                             uint8_t type)
{
    uint8_t report_mode = ESP_HID_PROTOCOL_MODE_REPORT;
    hidd_le_report_item_t *report = find_report(id, type, &report_mode);
    if (report == NULL ||
            (report->usage != ESP_HID_USAGE_KEYBOARD &&
             report->usage != ESP_HID_USAGE_MOUSE)) {
        return NULL;
    }
    uint8_t boot_mode = ESP_HID_PROTOCOL_MODE_BOOT;
    return find_report_by_usage_and_type(report->map_index, report->usage,
                                         type, &boot_mode);
}

/*
 * Caller must hold s_hidd_mutex.
 *
 * A vendor-defined INPUT report can be subscribed by the EasyInput
 * configuration client without making that connection the operating-system
 * keyboard host.  Only the standard keyboard or mouse usages establish HID
 * transport ownership.  This also covers their boot-protocol reports because
 * the report-map parser assigns those reports the same standard usages.
 */
static bool is_hid_owner_input_report_handle(uint16_t attr_handle)
{
    if (s_dev == NULL) {
        return false;
    }
    for (uint8_t d = 0; d < s_dev->devices_len; d++) {
        for (uint8_t i = 0; i < s_dev->devices[d].reports_len; i++) {
            hidd_le_report_item_t *rpt = &s_dev->devices[d].reports[i];
            if (rpt->report_type == ESP_HID_REPORT_TYPE_INPUT &&
                    (rpt->usage == ESP_HID_USAGE_KEYBOARD ||
                     rpt->usage == ESP_HID_USAGE_MOUSE) &&
                    rpt->handle == attr_handle) {
                return true;
            }
        }
    }
    return false;
}

static void nimble_report_write_cb(uint16_t attr_handle, uint8_t report_type, uint8_t report_id,
                                   const uint8_t *data, uint16_t len)
{
    hidd_event_post_ref_t post_ref = {0};
    esp_hidd_event_t event = ESP_HIDD_MAX_EVENT;
    esp_hidd_dev_t *public_dev = NULL;
    uint8_t usage = 0;
    uint8_t map_index = 0;

    lock_hidd();
    if (s_dev == NULL || s_dev->event_loop_handle == NULL || data == NULL) {
        unlock_hidd();
        return;
    }

    hidd_le_report_item_t *match = NULL;
    for (uint8_t d = 0; d < s_dev->devices_len && match == NULL; d++) {
        for (uint8_t r = 0; r < s_dev->devices[d].reports_len; r++) {
            hidd_le_report_item_t *item = &s_dev->devices[d].reports[r];
            if (item->handle == attr_handle) {
                match = item;
                map_index = d;
                break;
            }
        }
    }

    if (match == NULL) {
        unlock_hidd();
        return;
    }

    if (report_type != ESP_HID_REPORT_TYPE_OUTPUT &&
            report_type != ESP_HID_REPORT_TYPE_FEATURE) {
        ESP_LOGD(TAG, "Ignoring host write for unsupported report type=%u, id=%u, handle=%u",
                 report_type, report_id, attr_handle);
        unlock_hidd();
        return;
    }

    public_dev = s_dev->dev;
    usage = match->usage;
    const bool can_post = acquire_event_post_ref_locked(
            s_dev->event_loop_handle, &post_ref);
    unlock_hidd();
    if (!can_post) {
        return;
    }

    size_t event_data_size = sizeof(esp_hidd_event_data_t);
    if (len > 0) {
        event_data_size += len;
    }
    esp_hidd_event_data_t *p_cb_param = (esp_hidd_event_data_t *)calloc(1, event_data_size);
    if (p_cb_param == NULL) {
        ESP_LOGE(TAG, "%s malloc event data failed!", __func__);
        release_event_post_ref(&post_ref);
        return;
    }

    if (len > 0) {
        memcpy(((uint8_t *)p_cb_param) + sizeof(esp_hidd_event_data_t), data, len);
    }

    if (report_type == ESP_HID_REPORT_TYPE_OUTPUT) {
        p_cb_param->output.dev = public_dev;
        p_cb_param->output.usage = usage;
        p_cb_param->output.report_id = report_id;
        p_cb_param->output.length = len;
        p_cb_param->output.data = (len > 0)
                ? ((uint8_t *)p_cb_param) + sizeof(esp_hidd_event_data_t)
                : NULL; /* fixed after event-loop deep copy */
        p_cb_param->output.map_index = map_index;
        event = ESP_HIDD_OUTPUT_EVENT;
    } else if (report_type == ESP_HID_REPORT_TYPE_FEATURE) {
        p_cb_param->feature.dev = public_dev;
        p_cb_param->feature.usage = usage;
        p_cb_param->feature.report_id = report_id;
        p_cb_param->feature.length = len;
        p_cb_param->feature.data = (len > 0)
                ? ((uint8_t *)p_cb_param) + sizeof(esp_hidd_event_data_t)
                : NULL; /* fixed after event-loop deep copy */
        p_cb_param->feature.map_index = map_index;
        event = ESP_HIDD_FEATURE_EVENT;
    }
    post_hidd_event_bounded(&post_ref, event, p_cb_param, event_data_size);
    free(p_cb_param);
}

static void nimble_char_write_cb(uint16_t attr_handle, uint16_t char_uuid16, uint8_t value)
{
    hidd_event_post_ref_t post_ref = {0};
    esp_hidd_event_t event = ESP_HIDD_MAX_EVENT;
    lock_hidd();
    if (s_dev == NULL || s_dev->event_loop_handle == NULL) {
        unlock_hidd();
        return;
    }

    uint8_t map_index = 0;
    bool found = false;

    for (uint8_t d = 0; d < s_dev->devices_len; d++) {
        if (char_uuid16 == BLE_SVC_HID_CHR_UUID16_PROTOCOL_MODE &&
                s_dev->devices[d].hid_protocol_handle == attr_handle) {
            found = true;
            map_index = d;
            break;
        }
        if (char_uuid16 == BLE_SVC_HID_CHR_UUID16_HID_CTRL_PT &&
                s_dev->devices[d].hid_control_handle == attr_handle) {
            found = true;
            map_index = d;
            break;
        }
    }

    if (!found) {
        unlock_hidd();
        return;
    }

    esp_hidd_event_data_t cb_param = {0};
    if (char_uuid16 == BLE_SVC_HID_CHR_UUID16_PROTOCOL_MODE) {
        s_dev->protocol = value;
        cb_param.protocol_mode.dev = s_dev->dev;
        cb_param.protocol_mode.protocol_mode = value;
        cb_param.protocol_mode.map_index = map_index;
        event = ESP_HIDD_PROTOCOL_MODE_EVENT;
    } else if (char_uuid16 == BLE_SVC_HID_CHR_UUID16_HID_CTRL_PT) {
        s_dev->control = value;
        cb_param.control.dev = s_dev->dev;
        cb_param.control.control = value;
        cb_param.control.map_index = map_index;
        event = ESP_HIDD_CONTROL_EVENT;
    }
    const bool can_post = acquire_event_post_ref_locked(
            s_dev->event_loop_handle, &post_ref);
    unlock_hidd();
    if (can_post) {
        post_hidd_event_bounded(&post_ref, event, &cb_param,
                                sizeof(esp_hidd_event_data_t));
    }
}

static int nimble_hidd_dev_input_set_for_owner(
    void *devp,
    const easy_input_hidd_owner_snapshot_t *expected_owner,
    size_t index,
    size_t id,
    uint8_t *data,
    size_t length)
{
    esp_ble_hidd_dev_t *dev = (esp_ble_hidd_dev_t *)devp;
    uint16_t report_handle;
    uint16_t owner_conn_id;
    uint8_t protocol;
    uint8_t boot_keyboard_data[8] = {0};
    const uint8_t *notification_data = data;
    size_t notification_length = length;
    int rc;
    struct os_mbuf *om = NULL;

    if (data == NULL && length != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (expected_owner != NULL &&
            (expected_owner->conn_handle == EASY_INPUT_HIDD_OWNER_NONE ||
             expected_owner->generation == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_owner_gate();
    lock_hidd();
    if (!dev || s_dev != dev) {
        unlock_hidd();
        unlock_owner_gate();
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s_dev->devices_len) {
        unlock_hidd();
        unlock_owner_gate();
        return ESP_ERR_INVALID_ARG;
    }
    if (!dev->connected || dev->conn_id == BLE_HS_CONN_HANDLE_NONE) {
        unlock_hidd();
        unlock_owner_gate();
        return ESP_ERR_INVALID_STATE;
    }
    if (expected_owner != NULL &&
            (dev->conn_id != expected_owner->conn_handle ||
             dev->owner_generation != expected_owner->generation)) {
        unlock_hidd();
        unlock_owner_gate();
        return ESP_ERR_INVALID_STATE;
    }
    owner_conn_id = dev->conn_id;
    protocol = dev->protocol;
    hidd_le_report_item_t *p_rpt =
        find_report(id, ESP_HID_REPORT_TYPE_INPUT, &protocol);
    if (p_rpt == NULL && protocol == ESP_HID_PROTOCOL_MODE_BOOT) {
        p_rpt = find_boot_report_for_report_id(
                id, ESP_HID_REPORT_TYPE_INPUT);
    }
    if (p_rpt == NULL) {
        unlock_hidd();
        unlock_owner_gate();
        return ESP_ERR_NOT_FOUND;
    }
    report_handle = p_rpt->handle;
    if (report_handle == 0) {
        unlock_hidd();
        unlock_owner_gate();
        return ESP_ERR_INVALID_STATE;
    }

    if (protocol == ESP_HID_PROTOCOL_MODE_BOOT) {
        if (p_rpt->usage == ESP_HID_USAGE_KEYBOARD) {
            if (length != sizeof(boot_keyboard_data) ||
                    p_rpt->value_len != sizeof(boot_keyboard_data)) {
                unlock_hidd();
                unlock_owner_gate();
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(boot_keyboard_data, data, sizeof(boot_keyboard_data));
            /* EasyInput's report-protocol byte 1 carries Apple Fn. The same
             * byte is reserved in the boot keyboard packet and must be zero. */
            boot_keyboard_data[1] = 0;
            notification_data = boot_keyboard_data;
            notification_length = sizeof(boot_keyboard_data);
        } else if (p_rpt->usage == ESP_HID_USAGE_MOUSE) {
            if (p_rpt->value_len > length) {
                unlock_hidd();
                unlock_owner_gate();
                return ESP_ERR_INVALID_SIZE;
            }
            for (size_t i = p_rpt->value_len; i < length; i++) {
                if (data[i] != 0) {
                    /* Wheel and horizontal pan have no Boot Mouse encoding. */
                    unlock_hidd();
                    unlock_owner_gate();
                    return ESP_ERR_NOT_SUPPORTED;
                }
            }
            notification_length = p_rpt->value_len;
        }
    }

    om = ble_hs_mbuf_from_flat(
            (void *)notification_data, notification_length);
    if (om == NULL) {
        unlock_hidd();
        unlock_owner_gate();
        return ESP_ERR_NO_MEM;
    }
    unlock_hidd();

    /*
     * ble_gatts_notify_custom consumes om regardless of its result.  Do not
     * release the owner gate between the exact owner-generation check and
     * notification submission: a numeric connection handle can be reused
     * after disconnect.  ble_gatts_notify_custom synchronously emits
     * NOTIFY_TX, so this component's GAP listener explicitly bypasses the
     * owner gate for that non-owning event.
     */
    rc = ble_gatts_notify_custom(owner_conn_id, report_handle, om);
    unlock_owner_gate();
    if (rc != 0) {
        ESP_LOGD(TAG, "Write Input Report deferred/failed: %d", rc);
        return nimble_error_to_esp(rc);
    }
    return ESP_OK;
}

static int nimble_hidd_dev_input_set(
    void *devp,
    size_t index,
    size_t id,
    uint8_t *data,
    size_t length)
{
    return nimble_hidd_dev_input_set_for_owner(
        devp, NULL, index, id, data, length);
}

esp_err_t easy_input_hidd_dev_input_set_for_owner(
    esp_hidd_dev_t *dev,
    const easy_input_hidd_owner_snapshot_t *expected_owner,
    size_t map_index,
    size_t report_id,
    uint8_t *data,
    size_t length)
{
    if (dev == NULL || expected_owner == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev->transport != ESP_HID_TRANSPORT_BLE ||
            dev->input_set != nimble_hidd_dev_input_set) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const easy_input_hidd_owner_snapshot_t expected_owner_copy =
        *expected_owner;
    return nimble_hidd_dev_input_set_for_owner(
        dev->dev,
        &expected_owner_copy,
        map_index,
        report_id,
        data,
        length);
}

static int nimble_hidd_dev_feature_set(void *devp, size_t index, size_t id, uint8_t *data, size_t length)
{
    esp_ble_hidd_dev_t *dev = (esp_ble_hidd_dev_t *)devp;
    uint16_t report_handle;
    uint8_t protocol;
    int rc;
    struct os_mbuf *om = NULL;

    if (data == NULL && length != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_hidd();
    if (!dev || s_dev != dev) {
        unlock_hidd();
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s_dev->devices_len) {
        unlock_hidd();
        return ESP_ERR_INVALID_ARG;
    }
    if (!dev->connected || dev->conn_id == BLE_HS_CONN_HANDLE_NONE) {
        unlock_hidd();
        return ESP_ERR_INVALID_STATE;
    }
    protocol = dev->protocol;
    hidd_le_report_item_t *p_rpt =
        find_report(id, ESP_HID_REPORT_TYPE_FEATURE, &protocol);
    if (p_rpt == NULL) {
        unlock_hidd();
        return ESP_ERR_NOT_FOUND;
    }
    report_handle = p_rpt->handle;
    unlock_hidd();
    if (report_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    om = ble_hs_mbuf_from_flat((void *)data, length);
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* ble_att_svr_write_local consumes om regardless of its result. */
    rc = ble_att_svr_write_local(report_handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "Set Feature Report deferred/failed: %d", rc);
        return nimble_error_to_esp(rc);
    }
    return ESP_OK;
}

static int nimble_hidd_dev_event_handler_register(void *devp, esp_event_handler_t callback, esp_hidd_event_t event)
{
    esp_ble_hidd_dev_t *dev = (esp_ble_hidd_dev_t *)devp;
    if (!dev || s_dev != dev) {
        return ESP_FAIL;
    }
    return esp_event_handler_register_with(dev->event_loop_handle, ESP_HIDD_EVENTS, event, callback, dev->dev);
}

static int esp_ble_hidd_dev_event_handler_unregister(void *devp, esp_event_handler_t callback, esp_hidd_event_t event)
{
    esp_ble_hidd_dev_t *dev = (esp_ble_hidd_dev_t *)devp;
    if (!dev || s_dev != dev) {
        return ESP_FAIL;
    }
    return esp_event_handler_unregister_with(dev->event_loop_handle, ESP_HIDD_EVENTS, event, callback);
}

static void ble_hidd_dev_free(void)
{
    const bool wait_event_posts = begin_event_post_shutdown();
    wait_for_event_posts(wait_event_posts);
    if (s_dev) {
        ble_hid_free_config(s_dev);
        free(s_dev);
        s_dev = NULL;
    }
}

static int nimble_hid_gap_event(struct ble_gap_event *event, void *arg)
{
    uint8_t data;
    int rc;
    esp_ble_hidd_dev_t *dev;

    (void)arg;
    /*
     * ble_gatts_notify_custom() reports its submission result synchronously
     * through NOTIFY_TX.  It is deliberately outside the owner-change gate:
     * this event neither selects nor invalidates an owner, and taking the gate
     * here would recursively deadlock an in-flight INPUT submission.
     */
    if (event == NULL) {
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_NOTIFY_TX) {
        return 0;
    }

    const bool owner_gate_held =
        event->type == BLE_GAP_EVENT_SUBSCRIBE ||
        event->type == BLE_GAP_EVENT_DISCONNECT;
    if (owner_gate_held) {
        lock_owner_gate();
    }
    lock_hidd();
    dev = s_dev;
    if (dev == NULL) {
        unlock_hidd_then_owner_gate(owner_gate_held);
        return 0;
    }

    /* HOGP encryption is not enforced at the GATT level in this component.
     * Applications must configure encryption themselves, either by setting
     * BLE_GATT_CHR_F_READ_ENC/WRITE_ENC in ble_svc_hid.c or by configuring
     * ble_hs_cfg.sm_* security parameters. */
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed. */
        ESP_LOGD(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);
        if (event->connect.status != 0) {
            unlock_hidd_then_owner_gate(owner_gate_held);
            return 0;
        }

        /*
         * A generic configuration connection must not become the HID owner.
         * Reset the shared protocol default only while there is no operational
         * HID owner; the INPUT subscription below performs owner selection.
         */
        if (!dev->connected) {
            data = ESP_HID_PROTOCOL_MODE_REPORT;
            /*
             * ble_att_svr_write_local() does not invoke
             * nimble_char_write_cb(), so keep the cached hot-path value in
             * sync explicitly. Remote writes update this cache in the normal
             * characteristic callback.
             */
            dev->protocol = data;
            for (int i = 0; i < dev->devices_len; i++) {
                struct os_mbuf *om = ble_hs_mbuf_from_flat(&data, 1);
                if (om == NULL) {
                    ESP_LOGD(TAG, "No memory to allocate protocol-mode mbuf");
                    break;
                }
                rc = ble_att_svr_write_local(dev->devices[i].hid_protocol_handle, om);
                if (rc != 0) {
                    ESP_LOGD(TAG, "Write on Protocol Mode Failed: %d", rc);
                }
            }
        }
        unlock_hidd_then_owner_gate(owner_gate_held);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        /*
         * NimBLE emits SUBSCRIBE both for a new CCCD write and for a bonded
         * subscription restored during reconnect.  A standard keyboard/mouse
         * INPUT notification subscription is the first unambiguous evidence
         * that this connection is the OS HID host rather than an auxiliary
         * App/config client.  Vendor-defined INPUT reports intentionally do
         * not establish ownership.
         */
        if (event->subscribe.cur_notify &&
                is_hid_owner_input_report_handle(event->subscribe.attr_handle)) {
            if (!dev->connected) {
                advance_owner_generation_locked(dev);
                dev->connected = true;
                dev->conn_id = event->subscribe.conn_handle;
                esp_hidd_event_data_t cb_param = {
                    .connect.dev = dev->dev,
                    .connect.status = ESP_OK
                };
                hidd_event_post_ref_t post_ref = {0};
                const bool can_post = acquire_event_post_ref_locked(
                        dev->event_loop_handle, &post_ref);
                unlock_hidd_then_owner_gate(owner_gate_held);
                if (can_post) {
                    post_hidd_event_bounded(
                            &post_ref, ESP_HIDD_CONNECT_EVENT, &cb_param,
                            sizeof(esp_hidd_event_data_t));
                }
                return 0;
            }
            if (dev->conn_id != event->subscribe.conn_handle) {
                ESP_LOGD(TAG,
                         "Ignoring HID subscriber conn=%u; owner=%u",
                         event->subscribe.conn_handle, dev->conn_id);
            }
        }
        /*
         * Do not relinquish ownership on an unsubscribe transition. Hosts can
         * change protocol/report subscriptions during a live connection; the
         * exact owner DISCONNECT below is the authoritative lifetime boundary.
         */
        unlock_hidd_then_owner_gate(owner_gate_held);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGD(TAG, "disconnect; reason=%d", event->disconnect.reason);

        if (dev->connected &&
                dev->conn_id == event->disconnect.conn.conn_handle) {
            advance_owner_generation_locked(dev);
            dev->connected = false;
            dev->conn_id = BLE_HS_CONN_HANDLE_NONE;
            esp_hidd_event_data_t cb_param = {0};
            cb_param.disconnect.dev = dev->dev;
            cb_param.disconnect.reason = event->disconnect.reason;
            hidd_event_post_ref_t post_ref = {0};
            const bool can_post = acquire_event_post_ref_locked(
                    dev->event_loop_handle, &post_ref);
            unlock_hidd_then_owner_gate(owner_gate_held);
            if (can_post) {
                post_hidd_event_bounded(
                        &post_ref, ESP_HIDD_DISCONNECT_EVENT, &cb_param,
                        sizeof(esp_hidd_event_data_t));
            }
        } else {
            unlock_hidd_then_owner_gate(owner_gate_held);
        }
        return 0;
    }
    unlock_hidd_then_owner_gate(owner_gate_held);
    return 0;
}

static void nimble_gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];
    hidd_le_report_item_t *rpt = NULL;
    struct os_mbuf *om = NULL;
    uint16_t uuid16;
    uint16_t report_info;
    uint8_t report_type, report_id;
    uint16_t report_handle;
    uint8_t protocol_mode;
    int rc;
    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGD(TAG, "registered service %s with handle=%d",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                 ctxt->svc.handle);
        uuid16 = ble_uuid_u16(ctxt->svc.svc_def->uuid);
        if (uuid16 == BLE_SVC_HID_UUID16) {
            if (service_index + 1 >= (int)s_dev->devices_len) {
                ESP_LOGE(TAG, "Too many HID services registered (%d >= devices_len %d); ignoring",
                         service_index + 1, s_dev->devices_len);
            } else {
                ++service_index;
                s_dev->devices[service_index].hid_svc = ctxt->svc.handle;
            }
        }

        break;

    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGD(TAG, "registering characteristic %s with "
                 "def_handle=%d val_handle=%d\n",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                 ctxt->chr.def_handle,
                 ctxt->chr.val_handle);

        if (service_index < 0 || (uint8_t)service_index >= s_dev->devices_len) {
            break;
        }
        uuid16 = ble_uuid_u16(ctxt->chr.chr_def->uuid);
        if (uuid16 == BLE_SVC_HID_CHR_UUID16_HID_CTRL_PT) {
            /* assuming this characteristic is from the last registered hid service */
            s_dev->devices[service_index].hid_control_handle = ctxt->chr.val_handle;
        }
        if (uuid16 == BLE_SVC_HID_CHR_UUID16_PROTOCOL_MODE) {
            /* assuming this characteristic is from the last registered hid service */
            s_dev->devices[service_index].hid_protocol_handle = ctxt->chr.val_handle;
        }
        if (uuid16 == BLE_SVC_HID_CHR_UUID16_BOOT_KBD_INP) {
            protocol_mode = ESP_HID_PROTOCOL_MODE_BOOT;
            rpt = find_report_by_usage_and_type(service_index, ESP_HID_USAGE_KEYBOARD, ESP_HID_REPORT_TYPE_INPUT, &protocol_mode);
            if (rpt == NULL) {
                ESP_LOGE(TAG, "Unknown boot kbd input report registration");
                return;
            }
            rpt->handle = ctxt->chr.val_handle;
        }
        if (uuid16 == BLE_SVC_HID_CHR_UUID16_BOOT_KBD_OUT) {
            protocol_mode = ESP_HID_PROTOCOL_MODE_BOOT;
            rpt = find_report_by_usage_and_type(service_index, ESP_HID_USAGE_KEYBOARD, ESP_HID_REPORT_TYPE_OUTPUT, &protocol_mode);
            if (rpt == NULL) {
                ESP_LOGE(TAG, "Unknown boot kbd output report registration");
                return;
            }
            rpt->handle = ctxt->chr.val_handle;
        }
        if (uuid16 == BLE_SVC_HID_CHR_UUID16_BOOT_MOUSE_INP) {
            protocol_mode = ESP_HID_PROTOCOL_MODE_BOOT;
            rpt = find_report_by_usage_and_type(service_index, ESP_HID_USAGE_MOUSE, ESP_HID_REPORT_TYPE_INPUT, &protocol_mode);
            if (rpt == NULL) {
                ESP_LOGE(TAG, "Unknown boot mouse input report registration");
                return;
            }
            rpt->handle = ctxt->chr.val_handle;
        }
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGD(TAG, "registering descriptor %s with handle=%d",
                 ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                 ctxt->dsc.handle);
        uuid16 = ble_uuid_u16(ctxt->dsc.dsc_def->uuid);
        if (uuid16 == BLE_SVC_HID_DSC_UUID16_RPT_REF) {
            rc = ble_att_svr_read_local(ctxt->dsc.handle, &om);
            if (rc != 0 || om == NULL) {
                ESP_LOGE(TAG,
                         "Failed to read Report Reference descriptor handle=%u, rc=%d",
                         ctxt->dsc.handle,
                         rc);
                if (om != NULL) {
                    os_mbuf_free_chain(om);
                }
                break;
            }

            rc = ble_hs_mbuf_to_flat(om, &report_info, sizeof report_info, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG,
                         "Failed to decode Report Reference descriptor handle=%u, rc=%d",
                         ctxt->dsc.handle,
                         rc);
                os_mbuf_free_chain(om);
                break;
            }
            report_type = (uint8_t)((report_info & 0xFF00) >> 8);
            report_id = report_info & 0x00FF;
            if (ctxt->dsc.dsc_def->arg == NULL) {
                ESP_LOGE(TAG, "Report Reference descriptor has NULL arg; skipping handle assignment");
                os_mbuf_free_chain(om);
                break;
            }
            report_handle = (*(uint16_t*)(ctxt->dsc.dsc_def->arg));
            protocol_mode = ESP_HID_PROTOCOL_MODE_REPORT;
            rpt = find_report(report_id, report_type, &protocol_mode);
            if (rpt == NULL) {
                ESP_LOGE(TAG,
                         "Unknown report reference id=%u type=%u; skipping handle assignment",
                         report_id,
                         report_type);
                os_mbuf_free_chain(om);
                break;
            }
            rpt->handle = report_handle;
            /* free the mbuf */
            os_mbuf_free_chain(om);
            om = NULL;
        }
        break;

    default:
        ESP_LOGE(TAG, "Unknown GATT registration operation: %d", ctxt->op);
        break;
    }
}

static void nimble_host_synced(void)
{
    void (*previous_sync_cb)(void) = NULL;
    hidd_event_post_ref_t post_ref = {0};

    lock_hidd();
    if (s_prev_sync_cb && s_prev_sync_cb != nimble_host_synced) {
        previous_sync_cb = s_prev_sync_cb;
    }
    unlock_hidd();

    /* An inherited host callback is arbitrary application code and may call
     * back into this component or even deinitialize it. Never invoke it while
     * holding the non-recursive HIDD state lock. */
    if (previous_sync_cb != NULL) {
        previous_sync_cb();
    }

    lock_hidd();
    if (s_dev != NULL) {
        s_dev->host_synced = true;
    }
    const bool can_post = s_dev != NULL &&
            acquire_event_post_ref_locked(s_dev->event_loop_handle, &post_ref);
    unlock_hidd();
    if (can_post) {
        post_hidd_event_bounded(&post_ref, ESP_HIDD_START_EVENT, NULL, 0);
    }
}

void nimble_host_reset(int reason)
{
    void (*previous_reset_cb)(int) = NULL;
    /*
     * A host reset can tear down every ACL link without delivering the normal
     * per-connection DISCONNECT callback. Clear the exact HID owner here so a
     * restored subscription after sync can claim ownership and so producers
     * stop submitting into a stale connection.
     */
    lock_owner_gate();
    lock_hidd();
    if (s_dev != NULL) {
        advance_host_generation_locked(s_dev);
        s_dev->host_synced = false;
        if (s_dev->connected ||
                s_dev->conn_id != BLE_HS_CONN_HANDLE_NONE) {
            advance_owner_generation_locked(s_dev);
        }
        s_dev->connected = false;
        s_dev->conn_id = BLE_HS_CONN_HANDLE_NONE;
        s_dev->protocol = ESP_HID_PROTOCOL_MODE_REPORT;
    }
    if (s_prev_reset_cb && s_prev_reset_cb != nimble_host_reset) {
        previous_reset_cb = s_prev_reset_cb;
    }
    unlock_hidd();
    unlock_owner_gate();
    if (previous_reset_cb != NULL) {
        previous_reset_cb(reason);
    }
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

esp_err_t esp_ble_hidd_dev_init(esp_hidd_dev_t *dev_p, const esp_hid_device_config_t *config, esp_event_handler_t callback)
{
    int rc;

    if (s_dev) {
        ESP_LOGE(TAG, "HID device profile already initialized");
        return ESP_FAIL;
    }

    s_dev = (esp_ble_hidd_dev_t *)calloc(1, sizeof(esp_ble_hidd_dev_t));
    if (s_dev == NULL) {
        ESP_LOGE(TAG, "HID device could not be allocated");
        return ESP_FAIL;
    }
    if (s_hidd_mutex == NULL) {
        s_hidd_mutex = xSemaphoreCreateMutex();
        if (s_hidd_mutex == NULL) {
            free(s_dev);
            s_dev = NULL;
            ESP_LOGE(TAG, "HID device mutex allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_owner_gate == NULL) {
        s_owner_gate = xSemaphoreCreateMutex();
        if (s_owner_gate == NULL) {
            vSemaphoreDelete(s_hidd_mutex);
            s_hidd_mutex = NULL;
            free(s_dev);
            s_dev = NULL;
            ESP_LOGE(TAG, "HID owner gate allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_event_post_gate == NULL) {
        s_event_post_gate = xSemaphoreCreateMutex();
        if (s_event_post_gate == NULL) {
            free(s_dev);
            s_dev = NULL;
            ESP_LOGE(TAG, "HID event-post gate allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_event_posts_drained == NULL) {
        s_event_posts_drained = xSemaphoreCreateBinary();
        if (s_event_posts_drained == NULL) {
            free(s_dev);
            s_dev = NULL;
            ESP_LOGE(TAG, "HID event-post drain allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    // Reset the hid device target environment
    s_dev->control = ESP_HID_CONTROL_EXIT_SUSPEND;
    s_dev->protocol = ESP_HID_PROTOCOL_MODE_REPORT;
    s_dev->conn_id = BLE_HS_CONN_HANDLE_NONE;
    s_dev->host_generation = 1;
    s_dev->host_synced = false;
    s_dev->event_loop_handle = NULL;
    s_dev->dev = dev_p;

    esp_event_loop_args_t event_task_args = {
        .queue_size = 16,
        .task_name = "ble_hidd_events",
        .task_priority = uxTaskPriorityGet(NULL),
        .task_stack_size = 4096,
        .task_core_id = tskNO_AFFINITY
    };
    rc = esp_event_loop_create(&event_task_args, &s_dev->event_loop_handle);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "HID device event loop could not be created");
        ble_hidd_dev_free();
        return rc;
    }
    activate_event_posts(s_dev->event_loop_handle);

    rc = ble_hid_init_config(s_dev, config);
    if (rc != ESP_OK) {
        ble_hidd_dev_free();
        return rc;
    }

    dev_p->dev = s_dev;
    dev_p->connected = nimble_hidd_dev_connected;
    dev_p->deinit = nimble_hidd_dev_deinit;
    dev_p->battery_set = nimble_hidd_dev_battery_set;
    dev_p->input_set = nimble_hidd_dev_input_set;
    dev_p->feature_set = nimble_hidd_dev_feature_set;
    dev_p->event_handler_register = nimble_hidd_dev_event_handler_register;
    dev_p->event_handler_unregister = esp_ble_hidd_dev_event_handler_unregister;

    rc = nimble_hidd_dev_event_handler_register(s_dev, esp_hidd_process_event_data_handler, ESP_EVENT_ANY_ID);
    if (rc != ESP_OK) {
        ble_hidd_dev_free();
        return rc;
    }

    if (callback != NULL) {
        rc = nimble_hidd_dev_event_handler_register(s_dev, callback, ESP_EVENT_ANY_ID);
        if (rc != ESP_OK) {
            ble_hidd_dev_free();
            return rc;
        }
    }

    s_prev_reset_cb = ble_hs_cfg.reset_cb;
    s_prev_sync_cb = ble_hs_cfg.sync_cb;
    ble_hs_cfg.reset_cb = nimble_host_reset;
    ble_hs_cfg.sync_cb = nimble_host_synced;
    ble_hs_cfg.gatts_register_cb = nimble_gatt_svr_register_cb;
    ble_svc_hid_register_report_write_cb(nimble_report_write_cb);
    ble_svc_hid_register_char_write_cb(nimble_char_write_cb);
    rc = nimble_hid_start_gatts();
    if (rc != ESP_OK) {
        if (ble_hs_cfg.reset_cb == nimble_host_reset) {
            ble_hs_cfg.reset_cb = s_prev_reset_cb;
        }
        if (ble_hs_cfg.sync_cb == nimble_host_synced) {
            ble_hs_cfg.sync_cb = s_prev_sync_cb;
        }
        ble_hs_cfg.gatts_register_cb = NULL;
        ble_svc_hid_register_report_write_cb(NULL);
        ble_svc_hid_register_char_write_cb(NULL);
        ble_hidd_dev_free();
        return rc;
    }
    ble_gap_event_listener_register(&nimble_gap_event_listener,
                                    nimble_hid_gap_event, NULL);
    s_gap_listener_registered = true;

    return rc;
}
#endif // CONFIG_BT_NIMBLE_HID_SERVICE
