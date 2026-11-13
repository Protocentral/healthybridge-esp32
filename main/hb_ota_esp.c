/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — HealthyPi 6 firmware-update receiver: the ESP-IDF half.
 *
 * hb_ota.c decides; this file does. It runs the transfer state machine in its
 * own task and gives it the real esp_ota / partition / SHA-256 operations.
 *
 * Why a task of its own
 * ---------------------
 * A DATA frame can cost a 4 KB sector erase (tens of ms, a few hundred worst
 * case) and COMMIT reads the whole image back two to three times. Done in the
 * link's dispatch worker, that would stall every waveform consumer and, since
 * the dispatch queue drops when full, could lose the next update frame behind a
 * burst of biosignal frames. So OTA_REQ frames bypass that queue: the RX path
 * copies them straight into this task's queue.
 *
 * Why no frame is lost while flash is busy
 * ----------------------------------------
 * During an erase or write the cache is off and every non-IRAM interrupt —
 * including this UART's (CONFIG_UART_ISR_IN_IRAM is off) — waits. Bytes still
 * land in the UART's 128-byte hardware FIFO, and RTS is driven by the
 * PERIPHERAL from the FIFO level (rx_flow_ctrl_thresh = 100), not by software:
 * at 100 bytes it de-asserts and the M7's USART stops at the next character
 * boundary (hardware CTS). The 28 bytes of headroom are ~140 us at 2 Mbaud
 * against a stop latency of one character, so the FIFO cannot overrun. When the
 * flash operation ends, the ISR drains the FIFO and RTS re-asserts.
 *
 * What the M7 can see instead is its own TX deadline expiring on a frame stuck
 * behind RTS (it aborts the frame mid-way). The codec here resyncs on the next
 * sync word and counts a CRC error. If that frame was an OTA request, or this
 * task's queue was full, the request simply gets no reply: the host's reply
 * timeout and the idempotent-retransmit rule in hb_ota.c recover it.
 *
 * Radios
 * ------
 * An update is accepted with Wi-Fi/BLE on or off. ESP-IDF supports flash
 * writes with both radios running — it is how esp_https_ota works, and the
 * Wi-Fi/BLE stacks keep their flash-critical paths in IRAM — so there is no
 * correctness reason to refuse. The costs of radios-on are timing (CPU shared
 * with Wi-Fi/OpenView, BLE events delayed by an erase) and power, neither of
 * which affects the result: the link is flow-controlled and every request is
 * acknowledged. The host is advised to update with radios off (the boot
 * default) for the shortest transfer.
 */
#include "sdkconfig.h"

#if defined(CONFIG_HB_OTA_RECEIVER)

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "psa/crypto.h"

#include "healthybridge_hp6.h"
#include "hb_codec.h"
#include "hb_link.h"
#include "hb_transport.h"
#include "hb_ota.h"
#include "hb_ota_esp.h"

static const char *TAG = "hb_ota";

_Static_assert(sizeof(struct hb_ota_req_hp6) + sizeof(struct hb_ota_data_hp6) +
               HB_OTA_MAX_CHUNK == HB_CODEC_MAX_PAYLOAD,
               "HB_OTA_MAX_CHUNK must fill exactly one HP6 frame payload");

/* Stop-and-wait means one request in the queue; the rest is slack for a host
 * retransmitting while a slow request (erase, COMMIT) is still in progress. */
#define HB_OTA_QUEUE_DEPTH 4

struct ota_item {
    uint16_t len;
    uint8_t  payload[HB_CODEC_MAX_PAYLOAD];
};

static QueueHandle_t s_q;
static uint32_t      s_rx_drops;

static struct hb_ota s_ota;                 /* owned by the update task */
static const esp_partition_t *s_target;     /* the inactive slot */
static esp_ota_handle_t s_handle;
static psa_hash_operation_t s_sha;

/* Shared with the RX task, which answers STATUS. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct hb_ota_snapshot s_snap;
static struct hb_ota_running  s_run;

/* ---- hb_ota_ops --------------------------------------------------------- */

static uint32_t op_slot_size(void *u)
{
    (void)u;
    s_target = esp_ota_get_next_update_partition(NULL);
    return s_target ? (uint32_t)s_target->size : 0;
}

static bool op_running_unconfirmed(void *u)
{
    (void)u;
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

/*
 * OTA_WITH_SEQUENTIAL_WRITES: erase each 4 KB sector just before it is first
 * written, instead of the whole image up front. That bounds every reply by one
 * sector erase rather than making BEGIN take seconds.
 *
 * esp_ota_begin() also erases the INACTIVE otadata entry — the one describing
 * the image this transfer is about to overwrite. The running image's entry,
 * and so which slot boots, is untouched.
 */
static int op_begin(void *u)
{
    (void)u;
    s_target = esp_ota_get_next_update_partition(NULL);
    if (s_target == NULL) {
        return -1;
    }
    esp_err_t err = esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin(%s): %s", s_target->label, esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static int op_write(void *u, const uint8_t *data, uint32_t n)
{
    (void)u;
    esp_err_t err = esp_ota_write(s_handle, data, n);
    if (err == ESP_OK) {
        return HB_OTA_IO_OK;
    }
    ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
    return (err == ESP_ERR_OTA_VALIDATE_FAILED) ? HB_OTA_IO_INVALID : HB_OTA_IO_FAIL;
}

static void op_abort(void *u)
{
    (void)u;
    esp_ota_abort(s_handle);
}

static int op_read(void *u, uint32_t off, uint8_t *buf, uint32_t n)
{
    (void)u;
    return esp_partition_read(s_target, off, buf, n) == ESP_OK ? 0 : -1;
}

static void op_sha_start(void *u)
{
    (void)u;
    s_sha = psa_hash_operation_init();
    psa_hash_setup(&s_sha, PSA_ALG_SHA_256);
}

static void op_sha_update(void *u, const uint8_t *data, uint32_t n)
{
    (void)u;
    psa_hash_update(&s_sha, data, n);
}

static void op_sha_finish(void *u, uint8_t out[32])
{
    (void)u;
    size_t olen = 0;
    if (psa_hash_finish(&s_sha, out, 32, &olen) != PSA_SUCCESS || olen != 32) {
        /* A failed hash must not compare equal to anything. */
        psa_hash_abort(&s_sha);
        memset(out, 0, 32);
        out[0] = 0xA5;
    }
}

/*
 * esp_ota_end() runs esp_image_verify(): headers, segments, checksum, the
 * appended SHA-256 and the chip ID. It does not look at what the image IS, so
 * the project name is checked here too: an image built from another project
 * (say, a different C6 application) is valid to IDF but not to us.
 */
static int op_end(void *u)
{
    (void)u;
    esp_err_t err = esp_ota_end(s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return -1;
    }
    esp_app_desc_t d;
    err = esp_ota_get_partition_description(s_target, &d);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new image has no app description: %s", esp_err_to_name(err));
        return -1;
    }
    const esp_app_desc_t *me = esp_app_get_description();
    if (strncmp(d.project_name, me->project_name, sizeof(d.project_name)) != 0) {
        ESP_LOGE(TAG, "new image is project \"%.32s\", this is \"%.32s\" — refused",
                 d.project_name, me->project_name);
        return -1;
    }
    ESP_LOGI(TAG, "new image valid: %.32s %.32s (IDF %.32s)",
             d.project_name, d.version, d.idf_ver);
    return 0;
}

static int op_set_boot(void *u)
{
    (void)u;
    esp_err_t err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s): %s", s_target->label,
                 esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static uint32_t op_now_ms(void *u)
{
    (void)u;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void op_publish(void *u, const struct hb_ota_snapshot *s)
{
    (void)u;
    portENTER_CRITICAL(&s_mux);
    s_snap = *s;
    portEXIT_CRITICAL(&s_mux);
}

static const struct hb_ota_ops s_ops = {
    .slot_size           = op_slot_size,
    .running_unconfirmed = op_running_unconfirmed,
    .begin               = op_begin,
    .write               = op_write,
    .abort               = op_abort,
    .read                = op_read,
    .sha256_start        = op_sha_start,
    .sha256_update       = op_sha_update,
    .sha256_finish       = op_sha_finish,
    .end                 = op_end,
    .set_boot            = op_set_boot,
    .now_ms              = op_now_ms,
    .publish             = op_publish,
};

/* ---- running-image facts for STATUS -------------------------------------- */

static uint8_t img_state(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    if (p == NULL || esp_ota_get_state_partition(p, &st) != ESP_OK) {
        return HB_OTA_IMG_UNKNOWN;
    }
    switch (st) {
    case ESP_OTA_IMG_VALID:          return HB_OTA_IMG_VALID;
    case ESP_OTA_IMG_PENDING_VERIFY: return HB_OTA_IMG_PENDING_VERIFY;
    case ESP_OTA_IMG_NEW:            return HB_OTA_IMG_NEW;
    case ESP_OTA_IMG_INVALID:        return HB_OTA_IMG_INVALID;
    case ESP_OTA_IMG_ABORTED:        return HB_OTA_IMG_ABORTED;
    case ESP_OTA_IMG_UNDEFINED:      return HB_OTA_IMG_UNDEFINED;
    default:                         return HB_OTA_IMG_UNKNOWN;
    }
}

/* Re-read from otadata: the self-test marks the image VALID from another task,
 * and the host waits for exactly that. Called at 1 Hz from the update task. */
static void refresh_running(void)
{
    struct hb_ota_running r;
    memset(&r, 0, sizeof(r));
    const esp_partition_t *run = esp_ota_get_running_partition();
    r.slot = 0xFF;
    if (run && run->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
        run->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 1) {
        r.slot = (uint8_t)(run->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
    }
    r.img_state = img_state(run);
    r.other_img_state = img_state(esp_ota_get_next_update_partition(NULL));
    strncpy(r.version, esp_app_get_description()->version, sizeof(r.version) - 1);

    portENTER_CRITICAL(&s_mux);
    s_run = r;
    portEXIT_CRITICAL(&s_mux);
}

/* ---- RX path (transport RX task) ----------------------------------------- */

void hb_ota_esp_rx(const uint8_t *payload, uint16_t len)
{
    if (s_q == NULL) {
        return;
    }
    if (hb_ota_req_op(payload, len) == HB_OTA_OP_STATUS) {
        struct hb_ota_snapshot s;
        struct hb_ota_running r;
        uint8_t rsp[HB_OTA_RSP_MAX];
        portENTER_CRITICAL(&s_mux);
        s = s_snap;
        r = s_run;
        portEXIT_CRITICAL(&s_mux);
        size_t n = hb_ota_status_reply(&s, &r, payload, len, rsp, sizeof(rsp));
        if (n) {
            hb_link_send(HB_TYPE_OTA_RSP, 0, rsp, (uint16_t)n);
        }
        return;
    }

    static struct ota_item it;   /* RX task only; ~1 KB, kept off its stack */
    it.len = (len > HB_CODEC_MAX_PAYLOAD) ? HB_CODEC_MAX_PAYLOAD : len;
    if (it.len) {
        memcpy(it.payload, payload, it.len);
    }
    if (xQueueSend(s_q, &it, 0) != pdTRUE) {
        s_rx_drops++;
    }
}

uint32_t hb_ota_esp_rx_drops(void)
{
    return s_rx_drops;
}

/* ---- update task ---------------------------------------------------------- */

static const char *state_name(uint8_t st)
{
    switch (st) {
    case HB_OTA_STATE_IDLE:      return "idle";
    case HB_OTA_STATE_RECEIVING: return "receiving";
    case HB_OTA_STATE_VERIFYING: return "verifying";
    case HB_OTA_STATE_COMMITTED: return "committed";
    case HB_OTA_STATE_ERROR:     return "error";
    default:                     return "?";
    }
}

static void hb_ota_task(void *arg)
{
    (void)arg;
    static struct ota_item it;
    uint8_t rsp[HB_OTA_RSP_MAX];
    int64_t last_refresh = 0;

    for (;;) {
        if (xQueueReceive(s_q, &it, pdMS_TO_TICKS(1000)) == pdTRUE) {
            uint8_t before = s_ota.state;
            bool reboot = false;
            size_t n = hb_ota_handle(&s_ota, it.len ? it.payload : NULL, it.len,
                                     rsp, sizeof(rsp), &reboot);
            if (n) {
                hb_link_send(HB_TYPE_OTA_RSP, 0, rsp, (uint16_t)n);
            }
            if (s_ota.state != before || rsp[1] != HB_OTA_OK) {
                ESP_LOGI(TAG, "op 0x%02x -> status 0x%02x, %s -> %s, %lu/%lu B",
                         hb_ota_req_op(it.payload, it.len), rsp[1],
                         state_name(before), state_name(s_ota.state),
                         (unsigned long)s_ota.next_offset, (unsigned long)s_ota.image_size);
            }
            if (reboot) {
                /* The reply must be on the wire before the chip goes: wait for
                 * the TX FIFO to empty (bounded — a host holding CTS must not
                 * keep the old image running forever), then restart into the
                 * new slot. It boots PENDING_VERIFY; ota_selftest.c decides. */
                ESP_LOGW(TAG, "update committed to %s — restarting", s_target->label);
                hb_transport_uart_flush(200);
                esp_restart();
            }
        }

        if (hb_ota_tick(&s_ota)) {
            ESP_LOGW(TAG, "no request for %d s — transfer aborted (%lu/%lu B)",
                     CONFIG_HB_OTA_RX_TIMEOUT_S, (unsigned long)s_ota.next_offset,
                     (unsigned long)s_ota.image_size);
        }

        int64_t now = esp_timer_get_time();
        if (now - last_refresh >= 1000000) {
            refresh_running();
            last_refresh = now;
        }
    }
}

void hb_ota_esp_init(void)
{
    refresh_running();
    hb_ota_init(&s_ota, &s_ops, (uint32_t)CONFIG_HB_OTA_RX_TIMEOUT_S * 1000u);

    /* Priority 5: above the dispatch worker (4), so a reply is never queued
     * behind waveform consumers; below the transport RX task (10), which must
     * always be able to drain the UART. */
    s_q = xQueueCreate(HB_OTA_QUEUE_DEPTH, sizeof(struct ota_item));
    if (s_q == NULL || xTaskCreate(hb_ota_task, "hb_ota", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "update task/queue create failed — firmware update unavailable");
        if (s_q) {
            vQueueDelete(s_q);
            s_q = NULL;
        }
        return;
    }
    ESP_LOGI(TAG, "update receiver ready: inactive slot %s (%lu B), timeout %d s",
             s_target ? s_target->label : "none", (unsigned long)s_ota.slot_size,
             CONFIG_HB_OTA_RX_TIMEOUT_S);
}

#endif /* CONFIG_HB_OTA_RECEIVER */
