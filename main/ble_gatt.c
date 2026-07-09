/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — BLE GATT (NimBLE peripheral).
 *
 * Recreates the HealthyPi 5 BLE services/characteristics from the legacy
 * app/src/ble_module.c with the SAME UUIDs and notify byte formats so an
 * existing phone app works against the new ESP-hosted stack:
 *
 *   Heart Rate      0x180D / 0x2A37   notify [flags=0, hr:u8]
 *   Battery         0x180F / 0x2A19   notify [level:u8]
 *   Pulse Oximeter  0x1822 / 0x2A5E   notify [0x00, spo2:u8]
 *   Health Therm.   0x1809 / 0x2A6E   notify [temp:i16 LE  (degC*100)]
 *   ECG/Resp (128)  svc 0000112 2..   ECG  char 00001424.. notify [ecg:i32 LE * N]
 *                                     Resp char babe4a4c.. notify [bioz:i32 LE * N]
 *   PPG (128)       svc cd5c7491..     PPG  char cd5c1525.. notify [ppg:i16 LE]
 *                                     RR   char cd5ca86f.. notify [rr:u16 LE]
 *
 * Data is pushed by the hb_link RX path: ble_gatt_on_vitals() (1 Hz) and
 * ble_gatt_on_biosig() (per BIOSIG batch, ~16 Hz). A unified char table caches
 * the last value (for reads) and tracks CCC subscription (for notify gating).
 */
#include <string.h>
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "healthybridge.h"
#include "ble_gatt.h"
#include "hb_link.h"

static const char *TAG = "ble_gatt";

/* ---- 128-bit custom UUIDs (little-endian byte order for NimBLE) ---- */
static const ble_uuid128_t uuid_ecg_resp_svc = BLE_UUID128_INIT(
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00,0x22,0x11,0x00,0x00);
static const ble_uuid128_t uuid_ecg_chr = BLE_UUID128_INIT(
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00,0x24,0x14,0x00,0x00);
static const ble_uuid128_t uuid_resp_chr = BLE_UUID128_INIT(
    0x02,0x00,0x12,0xac,0x42,0x02,0xeb,0xa1,0xed,0x11,0x89,0x77,0x4c,0x4a,0xbe,0xba);
static const ble_uuid128_t uuid_ppg_svc = BLE_UUID128_INIT(
    0xd0,0x36,0xba,0x8c,0xda,0xd1,0x4c,0xae,0xb8,0x7d,0x48,0x44,0x91,0x74,0x5c,0xcd);
static const ble_uuid128_t uuid_ppg_chr = BLE_UUID128_INIT(
    0xd0,0x36,0xba,0x8c,0xda,0xd1,0x4c,0xae,0xb8,0x7d,0x48,0x44,0x25,0x15,0x5c,0xcd);
static const ble_uuid128_t uuid_rr_chr = BLE_UUID128_INIT(
    0xd0,0x36,0xba,0x8c,0xda,0xd1,0x4c,0xae,0xb8,0x7d,0x48,0x44,0x6f,0xa8,0x5c,0xcd);
/* Command service: phone writes TX, device notifies RX. */
static const ble_uuid128_t uuid_cmd_svc = BLE_UUID128_INIT(
    0xdc,0xad,0x7f,0xc4,0x23,0x90,0x4d,0xd4,0x96,0x8d,0x0f,0x97,0x92,0x74,0xbf,0x01);
static const ble_uuid128_t uuid_cmd_tx = BLE_UUID128_INIT(
    0xdc,0xad,0x7f,0xc4,0x23,0x90,0x4d,0xd4,0x96,0x8d,0x0f,0x97,0x28,0x15,0xbf,0x01);
static const ble_uuid128_t uuid_cmd_rx = BLE_UUID128_INIT(
    0xdc,0xad,0x7f,0xc4,0x23,0x90,0x4d,0xd4,0x96,0x8d,0x0f,0x97,0x27,0x15,0xbf,0x01);

/* ---- Unified characteristic table ---- */
enum { CH_HR, CH_BAT, CH_SPO2, CH_TEMP, CH_ECG, CH_RESP, CH_PPG, CH_RR, CH_CMD_RX, CH_N };

static struct hpi_char {
    uint16_t vh;            /* value handle, filled by NimBLE */
    bool     notify;        /* CCC notify enabled */
    uint16_t cache_len;
    uint8_t  cache[64];     /* last value (for reads); ECG batch = 8*4 = 32 */
} s_chars[CH_N];

static uint8_t  s_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool     s_advertising;
static char     s_dev_name[32] = "HealthyPi 5";

static int gap_event_cb(struct ble_gap_event *event, void *arg);

bool ble_gatt_is_connected(void)  { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
bool ble_gatt_is_advertising(void){ return s_advertising; }

/* Cache + (if subscribed) notify one characteristic. */
static void hpi_notify(int idx, const void *data, uint16_t len)
{
    if (len > sizeof(s_chars[idx].cache)) {
        len = sizeof(s_chars[idx].cache);
    }
    memcpy(s_chars[idx].cache, data, len);
    s_chars[idx].cache_len = len;

    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_chars[idx].notify) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) {
        ble_gatts_notify_custom(s_conn, s_chars[idx].vh, om);
    }
}

/* Generic read: returns the last cached value. arg = char index. */
static int chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int idx = (int)(intptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, s_chars[idx].cache, s_chars[idx].cache_len) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* Command TX char write: forward the phone's raw bytes to the RP2040 as a
 * HOST_CMD frame. The RP2040 feeds them to its bounded command parser. */
static int cmd_tx_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[HB_MAX_PAYLOAD];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(buf)) {
            len = sizeof(buf);
        }
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL) == 0) {
            hb_link_send(HB_TYPE_HOST_CMD, 0, buf, len);
            ESP_LOGI(TAG, "host cmd %u bytes -> RP2040", len);
        }
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;   /* empty read */
    }
    return BLE_ATT_ERR_UNLIKELY;
}

#define CHR_RN (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY)

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {   /* Heart Rate */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180D),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A37), .access_cb = chr_access_cb,
              .arg = (void *)CH_HR, .flags = CHR_RN, .val_handle = &s_chars[CH_HR].vh },
            { 0 },
        },
    },
    {   /* Battery */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = chr_access_cb,
              .arg = (void *)CH_BAT, .flags = CHR_RN, .val_handle = &s_chars[CH_BAT].vh },
            { 0 },
        },
    },
    {   /* Pulse Oximeter (SpO2) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1822),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A5E), .access_cb = chr_access_cb,
              .arg = (void *)CH_SPO2, .flags = CHR_RN, .val_handle = &s_chars[CH_SPO2].vh },
            { 0 },
        },
    },
    {   /* Health Thermometer */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1809),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A6E), .access_cb = chr_access_cb,
              .arg = (void *)CH_TEMP, .flags = CHR_RN, .val_handle = &s_chars[CH_TEMP].vh },
            { 0 },
        },
    },
    {   /* ECG + Resp (custom 128-bit) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_ecg_resp_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &uuid_ecg_chr.u, .access_cb = chr_access_cb,
              .arg = (void *)CH_ECG, .flags = CHR_RN, .val_handle = &s_chars[CH_ECG].vh },
            { .uuid = &uuid_resp_chr.u, .access_cb = chr_access_cb,
              .arg = (void *)CH_RESP, .flags = CHR_RN, .val_handle = &s_chars[CH_RESP].vh },
            { 0 },
        },
    },
    {   /* PPG + Resp-rate (custom 128-bit) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_ppg_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &uuid_ppg_chr.u, .access_cb = chr_access_cb,
              .arg = (void *)CH_PPG, .flags = CHR_RN, .val_handle = &s_chars[CH_PPG].vh },
            { .uuid = &uuid_rr_chr.u, .access_cb = chr_access_cb,
              .arg = (void *)CH_RR, .flags = CHR_RN, .val_handle = &s_chars[CH_RR].vh },
            { 0 },
        },
    },
    {   /* Command service (phone <-> device control) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_cmd_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &uuid_cmd_tx.u, .access_cb = cmd_tx_write_cb,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_READ },
            { .uuid = &uuid_cmd_rx.u, .access_cb = chr_access_cb,
              .arg = (void *)CH_CMD_RX, .flags = CHR_RN, .val_handle = &s_chars[CH_CMD_RX].vh },
            { 0 },
        },
    },
    { 0 },
};

/* Device -> phone command response (RP2040 HOST_RESP -> notify CMD_RX char). */
void ble_gatt_on_host_resp(const uint8_t *data, uint16_t len)
{
    hpi_notify(CH_CMD_RX, data, len);
}

/* ---- Push paths from the HealthyBridge RX ---- */
void ble_gatt_on_vitals(const struct hb_vitals_payload *v)
{
    uint8_t hr[2]   = { 0x00, (uint8_t)v->hr };
    hpi_notify(CH_HR, hr, sizeof(hr));

    uint8_t spo2[2] = { 0x00, v->spo2 };
    hpi_notify(CH_SPO2, spo2, sizeof(spo2));

    uint16_t rr = v->rr;
    hpi_notify(CH_RR, &rr, sizeof(rr));

    int16_t temp = v->temp_c_x100;
    hpi_notify(CH_TEMP, &temp, sizeof(temp));
}

void ble_gatt_on_biosig(const struct hb_biosig_payload *b)
{
    uint16_t n = b->sample_count;
    if (n == 0) {
        return;
    }
    if (n > 8) {
        n = 8;   /* cache holds 8 i32 */
    }

    int32_t ecg[8], bioz[8];
    for (uint16_t i = 0; i < n; i++) {
        ecg[i]  = b->samples[i].ecg;
        bioz[i] = b->samples[i].bioz;
    }
    hpi_notify(CH_ECG,  ecg,  n * sizeof(int32_t));
    hpi_notify(CH_RESP, bioz, n * sizeof(int32_t));

    /* PPG notified per-sample as int16 (matches legacy ble_ppg_notify). */
    for (uint16_t i = 0; i < b->sample_count; i++) {
        int16_t ppg = (int16_t)b->samples[i].ppg_red;
        hpi_notify(CH_PPG, &ppg, sizeof(ppg));
    }
}

void ble_gatt_on_battery(uint8_t soc)
{
    hpi_notify(CH_BAT, &soc, sizeof(soc));
}

/* ---- Advertising ---- */
void ble_gatt_start_adv(void)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_dev_name;
    fields.name_len = strlen(s_dev_name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "adv_start rc=%d", rc); return; }
    s_advertising = true;
    ESP_LOGI(TAG, "advertising as \"%s\"", s_dev_name);
}

void ble_gatt_stop_adv(void) { ble_gap_adv_stop(); s_advertising = false; }

void ble_gatt_set_name(const char *name)
{
    if (!name) return;
    strncpy(s_dev_name, name, sizeof(s_dev_name) - 1);
    s_dev_name[sizeof(s_dev_name) - 1] = '\0';
    ble_svc_gap_device_name_set(s_dev_name);
    if (s_advertising) { ble_gatt_stop_adv(); ble_gatt_start_adv(); }
}

/* ---- GAP events ---- */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_advertising = false;
            ESP_LOGI(TAG, "connected");
        } else {
            ble_gatt_start_adv();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        for (int i = 0; i < CH_N; i++) { s_chars[i].notify = false; }
        ble_gatt_start_adv();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        for (int i = 0; i < CH_N; i++) {
            if (event->subscribe.attr_handle == s_chars[i].vh) {
                s_chars[i].notify = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "char[%d] notify %s", i, s_chars[i].notify ? "on" : "off");
            }
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_gatt_start_adv();
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)  { ble_hs_id_infer_auto(0, &s_addr_type); ble_gatt_start_adv(); }
static void on_reset(int reason) { ESP_LOGW(TAG, "BLE host reset, reason=%d", reason); }

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_gatt_init(void)
{
    /* NVS is initialised once in app_main (shared with cfg + Wi-Fi). */
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ble_svc_gap_device_name_set(s_dev_name);

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "NimBLE GATT ready (HR/Bat/SpO2/Temp/ECG/Resp/PPG/RR)");
}
