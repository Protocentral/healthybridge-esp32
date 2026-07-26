/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge link façade — wires the transport HAL to the frame codec and
 * dispatches decoded frames to the consumers (data store / BLE / control).
 *
 * The public API (hb_link_init / hb_link_send / hb_link_get_stats) is unchanged;
 * the UART/parser internals moved to hb_transport_uart.c + hb_codec.c so the same
 * codec+dispatch serve any transport (UART today, SPI for HealthyPi 6).
 */
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "healthybridge.h"
#include "hb_link.h"
#include "hb_codec.h"
#include "hb_transport.h"
#include "data_store.h"
#include "control.h"
#include "ble_gatt.h"

#if defined(CONFIG_HB_PROFILE_HP6)
#include "healthybridge_hp6.h"
#endif

#if defined(CONFIG_HB_ENABLE_OPENVIEW)
#include "openview_protocol.h"
#include "wifi_server.h"
#endif

static const char *TAG = "hb_link";

static const struct hb_transport_if *s_tp;
static uint32_t s_rx_biosig, s_rx_vitals;
/* HP6 only — HealthyPi 5 carries PPG inside its BIOSIG batch and never sets this. */
static uint32_t s_rx_ppg;
#if defined(CONFIG_HB_PROFILE_HP6)
static uint32_t s_rx_resp, s_rx_hrv, s_rx_status_req;
#endif
/* Frames of a type this build has no case for. Both profiles. */
static uint32_t s_rx_unknown;

void hb_link_get_stats(uint32_t *bytes, uint32_t *biosig, uint32_t *vitals, uint32_t *crc_err)
{
    if (bytes)  { *bytes = 0; if (s_tp && s_tp->get_rx_bytes) { s_tp->get_rx_bytes(bytes); } }
    if (biosig) { *biosig = s_rx_biosig; }
    if (vitals) { *vitals = s_rx_vitals; }
    if (crc_err){ *crc_err = hb_codec_crc_errors(); }
}

uint32_t hb_link_ppg_frames(void)
{
    return s_rx_ppg;
}

void hb_link_get_type_counts(uint32_t *resp, uint32_t *hrv,
                             uint32_t *status_req, uint32_t *unknown)
{
#if defined(CONFIG_HB_PROFILE_HP6)
    if (resp)       { *resp       = s_rx_resp; }
    if (hrv)        { *hrv        = s_rx_hrv; }
    if (status_req) { *status_req = s_rx_status_req; }
#else
    if (resp)       { *resp       = 0; }
    if (hrv)        { *hrv        = 0; }
    if (status_req) { *status_req = 0; }
#endif
    if (unknown)    { *unknown    = s_rx_unknown; }
}

int hb_link_send(uint8_t type, uint8_t flags, const uint8_t *payload, uint16_t len)
{
    static uint16_t tx_seq;
    uint8_t frame[HB_HEADER_SIZE + HB_CODEC_MAX_PAYLOAD + HB_CRC_SIZE];
    int total = hb_codec_encode(type, flags, tx_seq++, payload, len, frame, sizeof(frame));
    if (total < 0 || s_tp == NULL) {
        return -1;
    }
    return s_tp->send(frame, (size_t)total);
}

#if defined(CONFIG_HB_PROFILE_HP6)
/*
 * HP6 profile: type 0x40 (VITALS) and 0x20 (BIOSIG) carry different payloads
 * than HP5. Decode the HP6 structs, then adapt them into the HP5-shaped structs
 * the consumers (data_store / BLE / dashboard / MQTT) already take, so the L2
 * consumer surface stays product-agnostic. The frozen HP5 path is untouched.
 */

/* Widest HP6 biosig batch that fits the codec cap, for the scratch conversion
 * buffer: (max payload - 8-byte HP6 header) / 32-byte HP6 sample. */
#define HB_BIOSIG_HP6_MAX_SAMPLES \
    ((HB_CODEC_MAX_PAYLOAD - sizeof(struct hb_biosig_payload_hp6)) / \
     sizeof(struct hb_biosig_sample_hp6))

#if defined(CONFIG_HB_ENABLE_OPENVIEW)
/*
 * OpenView streaming: each HP6 biosig sample becomes a 50-byte OpenView v2 packet
 * carrying the latest cached vitals, batched per frame and broadcast to TCP
 * clients. Runs in the dispatch worker task (single-threaded — statics are safe).
 * Mirrors the reference app_esp32c6_esp_idf/hl_spi.c dispatch mapping.
 */
static uint16_t s_ov_hr;
static uint8_t  s_ov_spo2, s_ov_rr;
static uint16_t s_ov_temp;     /* centidegrees C (OpenView temperature units) */
static uint32_t s_ov_seq;
/* Latest PPG, cached from the separate 0x10 frames. The M7 zeroes ppg_red/ppg_ir
 * inside the biosig batch ("PPG goes on its own channel") because ECG and PPG
 * run at different rates, so an OpenView packet must take PPG from here rather
 * than from the biosig sample it is built out of. */
static int32_t  s_ov_ppg_red, s_ov_ppg_ir;

static void openview_cache_vitals(const struct hb_vitals_payload_hp6 *h)
{
    s_ov_hr   = h->heart_rate_bpm;
    s_ov_spo2 = h->spo2_percent;
    s_ov_rr   = h->resp_rate_bpm;
    s_ov_temp = (uint16_t)(h->temp_celsius_x10 * 10);   /* x10 -> centidegrees */
}

static void openview_push_biosig(const struct hb_biosig_sample_hp6 *s, uint16_t n)
{
    if (n == 0 || !wifi_server_is_running()) {
        return;
    }
    static uint8_t batch[HB_BIOSIG_HP6_MAX_SAMPLES * OPENVIEW_PACKET_LENGTH];
    size_t off = 0;
    for (uint16_t i = 0; i < n; i++) {
        openview_data_t ov = {
            .sequence_number  = s_ov_seq++,
            .ecg1             = s[i].ch1,       /* ECG Lead I   */
            .ecg2             = s[i].ch2,       /* ECG Lead II  */
            .ecg3             = s[i].ch3,       /* ECG Lead III */
            .respiration      = s[i].ch0,
            .ppg_red          = s_ov_ppg_red,   /* from the 0x10 PPG frames */
            .ppg_ir           = s_ov_ppg_ir,    /* (zero inside the biosig batch) */
            .ppg_valid        = 0xFF,
            .heart_rate       = s_ov_hr,
            .spo2             = s_ov_spo2,
            .respiration_rate = s_ov_rr,
            .temperature      = s_ov_temp,
            .adc_ch1          = s[i].adc_ch1,
            .adc_ch2          = s[i].adc_ch2,
        };
        if (openview_pack_packet(&ov, &batch[off], OPENVIEW_PACKET_LENGTH) == 0) {
            off += OPENVIEW_PACKET_LENGTH;
        }
    }
    if (off) {
        wifi_server_send_packet(batch, off);
    }
}
#endif /* CONFIG_HB_ENABLE_OPENVIEW */

static void dispatch_vitals_hp6(const uint8_t *payload, uint16_t len)
{
    if (len < sizeof(struct hb_vitals_payload_hp6)) {
        return;
    }
    const struct hb_vitals_payload_hp6 *h = (const struct hb_vitals_payload_hp6 *)payload;
    struct hb_vitals_payload v = {
        .hr          = h->heart_rate_bpm,
        .spo2        = h->spo2_percent,
        .rr          = h->resp_rate_bpm,
        .temp_c_x100 = (int16_t)(h->temp_celsius_x10 * 10),  /* x10 -> x100 */
        .flags       = 0,
    };
    if (v.hr)   { v.flags |= HB_VITAL_HR_VALID; }
    if (v.spo2) { v.flags |= HB_VITAL_SPO2_VALID; }
    if (v.rr)   { v.flags |= HB_VITAL_RR_VALID; }
    /* HP6 status_flags lead-off bit layout isn't pinned in the M7 contract yet;
     * the lead-off flags stay clear until it is (L2 step 4). */
    data_store_set_vitals(&v);
    ble_gatt_on_vitals(&v);
#if defined(CONFIG_HB_ENABLE_OPENVIEW)
    openview_cache_vitals(h);
#endif
}

/*
 * Which channels the HP6 BIOSIG batch actually owns.
 *
 * Never PPG: the M7 zeroes ppg_red/ppg_ir there and sends PPG on type 0x10, so a
 * biosig push claiming DS_CH_PPG writes a zero between every real sample and the
 * PPG characteristic notifies nothing but zeros.
 *
 * RESP is decided at runtime. The M7 carries respiration in biosig ch0 today and
 * does not send type 0x30 — but it may, and nothing in the contract says it stops
 * filling ch0 when it does. So the dedicated frame takes ownership on arrival:
 * the first valid 0x30 latches s_resp_dedicated and biosig drops the channel from
 * then on. Either source alone works; they never both write.
 */
static bool s_resp_dedicated;

static inline uint8_t biosig_hp6_channels(void)
{
    return (uint8_t)(DS_CH_BIT(DS_CH_ECG) |
                     (s_resp_dedicated ? 0u : DS_CH_BIT(DS_CH_RESP)));
}

static void dispatch_biosig_hp6(const uint8_t *payload, uint16_t len)
{
    if (len < sizeof(struct hb_biosig_payload_hp6)) {
        return;
    }
    const struct hb_biosig_payload_hp6 *h = (const struct hb_biosig_payload_hp6 *)payload;
    uint16_t n = h->sample_count;
    uint16_t avail = (uint16_t)((len - sizeof(*h)) / sizeof(struct hb_biosig_sample_hp6));
    if (n > avail) { n = avail; }                                 /* trust len over count */
    if (n > HB_BIOSIG_HP6_MAX_SAMPLES) { n = HB_BIOSIG_HP6_MAX_SAMPLES; }

    uint8_t buf[sizeof(struct hb_biosig_payload) +
                HB_BIOSIG_HP6_MAX_SAMPLES * sizeof(struct hb_biosig_sample)];
    struct hb_biosig_payload *b = (struct hb_biosig_payload *)buf;
    b->timestamp_ms   = h->timestamp_ms;
    b->sample_count   = n;
    b->sample_rate_hz = h->sample_rate_hz;
    for (uint16_t i = 0; i < n; i++) {
        b->samples[i].ecg     = h->samples[i].ch2;      /* ECG Lead II -> ECG   */
        b->samples[i].bioz    = h->samples[i].ch0;      /* Respiration -> RESP  */
        /* ppg_* left unset — this batch does not own PPG (see biosig_hp6_channels). */
        b->samples[i].ppg_red = 0;
        b->samples[i].ppg_ir  = 0;
    }
    uint8_t ch = biosig_hp6_channels();
    data_store_push_biosig_ch(b, ch);
    ble_gatt_on_biosig_ch(b, ch);
#if defined(CONFIG_HB_ENABLE_OPENVIEW)
    openview_push_biosig(h->samples, n);   /* full HP6 samples (Lead I/III + ADC) */
#endif
}

/*
 * HP6 sends PPG as its own frame type (0x10) rather than inside the biosig batch
 * — ECG (500 Hz) and PPG (250 Hz) come off different sample-bus channels, so
 * they cannot share one sample struct without resampling, and the M7 explicitly
 * zeroes ppg_red/ppg_ir in the biosig frame. Without this handler every PPG
 * sample the M7 sends is dropped on the default: arm.
 */
static void dispatch_ppg_hp6(const uint8_t *payload, uint16_t len)
{
    if (len < sizeof(struct hb_ppg_payload_hp6)) {
        return;
    }
    const struct hb_ppg_payload_hp6 *h = (const struct hb_ppg_payload_hp6 *)payload;
    uint16_t n = h->sample_count;
    uint16_t avail = (uint16_t)((len - sizeof(*h)) / sizeof(struct hb_ppg_sample_hp6));
    if (n > avail) { n = avail; }                 /* trust len over count */
    if (n == 0) {
        return;
    }

    /* hb_ppg_sample_hp6 is exactly {int32 red; int32 ir;}, so the sample array is
     * already the interleaved red/ir layout both consumers expect. This is the
     * sole writer of the PPG channel on HP6 — the biosig batch never claims it. */
    data_store_push_ppg((const int32_t *)h->samples, n);
    ble_gatt_on_ppg((const int32_t *)h->samples, n);

#if defined(CONFIG_HB_ENABLE_OPENVIEW)
    s_ov_ppg_red = h->samples[n - 1].red;         /* latest wins */
    s_ov_ppg_ir  = h->samples[n - 1].ir;
#endif
}

/*
 * Respiration as its own frame type (0x30).
 *
 * Arrival takes ownership of DS_CH_RESP away from the biosig batch — see
 * biosig_hp6_channels(). The M7 does not send these today (it carries respiration
 * in biosig ch0), so in practice this is dormant; the latch is what makes it safe
 * for the M7 to start sending them without a coordinated change on this side.
 */
static void dispatch_resp_hp6(const uint8_t *payload, uint16_t len)
{
    if (len < sizeof(struct hb_wave_payload_hp6)) {
        return;
    }
    const struct hb_wave_payload_hp6 *h = (const struct hb_wave_payload_hp6 *)payload;
    uint16_t avail = (uint16_t)((len - sizeof(*h)) / sizeof(int32_t));
    uint16_t n = (h->sample_count > avail) ? avail : h->sample_count;
    if (n == 0) {
        return;
    }

    if (!s_resp_dedicated) {
        s_resp_dedicated = true;
        ESP_LOGI(TAG, "RESP (0x30) frames arriving: %u samples @ %u Hz — respiration "
                      "now sourced from 0x30, biosig ch0 dropped", n, h->sample_rate_hz);
    }

    /* samples[] lives inside a packed struct, so the array is not guaranteed to be
     * 4-byte aligned; copy through an aligned buffer rather than hand the consumers
     * a misaligned int32 pointer. Chunked at 8 to match the BLE notify batch. */
    const uint8_t *src = payload + sizeof(*h);
    while (n) {
        int32_t chunk[8];
        uint16_t c = (n > 8) ? 8 : n;
        memcpy(chunk, src, c * sizeof(int32_t));
        data_store_push_resp(chunk, c);
        ble_gatt_on_resp(chunk, c);
        src += c * sizeof(int32_t);
        n   -= c;
    }
}

/*
 * HRV metrics (0x42).
 *
 * Decoded and counted only. There is no HRV consumer to route it to: HP5 has no
 * HRV type, BLE exposes no HRV characteristic, and the OpenView packet has no
 * field for it — adding one is a feature decision, not a dispatch fix. What this
 * case buys is that the frames stop vanishing on the default arm, that the length
 * is validated, and that there is a place for a consumer to attach.
 */
static void dispatch_hrv_hp6(const uint8_t *payload, uint16_t len)
{
    if (len < sizeof(struct hb_hrv_payload_hp6)) {
        return;
    }
    const struct hb_hrv_payload_hp6 *h = (const struct hb_hrv_payload_hp6 *)payload;

    static bool logged;
    if (!logged) {
        logged = true;
        ESP_LOGI(TAG, "HRV (0x42) frames arriving: hr=%u rr=%ums sdnn=%u rmssd=%u "
                      "valid=%u — decoded, no consumer wired",
                 h->heart_rate_bpm, h->rr_interval_ms, h->hrv_sdnn_ms,
                 h->hrv_rmssd_ms, h->hrv_valid);
    }
}
#endif /* CONFIG_HB_PROFILE_HP6 */

/* Route one decoded frame to its consumer. Runs in the dispatch worker task on
 * HP6, or inline in the RX task on HP5 (see hb_dispatch below). */
static void hb_dispatch_frame(uint8_t type, uint8_t flags, uint16_t seq,
                              const uint8_t *payload, uint16_t len)
{
    (void)flags; (void)seq;
    switch (type) {
    case HB_TYPE_VITALS:
        s_rx_vitals++;
#if defined(CONFIG_HB_PROFILE_HP6)
        dispatch_vitals_hp6(payload, len);
#else
        if (len >= sizeof(struct hb_vitals_payload)) {
            const struct hb_vitals_payload *v = (const struct hb_vitals_payload *)payload;
            data_store_set_vitals(v);
            ble_gatt_on_vitals(v);
        }
#endif
        break;
    case HB_TYPE_BIOSIG:
        s_rx_biosig++;
#if defined(CONFIG_HB_PROFILE_HP6)
        dispatch_biosig_hp6(payload, len);
#else
        if (len >= sizeof(struct hb_biosig_payload)) {
            const struct hb_biosig_payload *b = (const struct hb_biosig_payload *)payload;
            data_store_push_biosig(b);
            ble_gatt_on_biosig(b);
        }
#endif
        break;
#if defined(CONFIG_HB_PROFILE_HP6)
    /* HP6-only: PPG arrives as its own frame type. HealthyPi 5 does not define
     * or emit 0x10 — its PPG rides inside HB_TYPE_BIOSIG — so this case, like
     * the header that declares HB_TYPE_PPG, is compiled out of the HP5 build. */
    case HB_TYPE_PPG:
        s_rx_ppg++;
        dispatch_ppg_hp6(payload, len);
        break;
    case HB_TYPE_RESP:
        s_rx_resp++;
        dispatch_resp_hp6(payload, len);
        break;
    case HB_TYPE_HRV:
        s_rx_hrv++;
        dispatch_hrv_hp6(payload, len);
        break;
    /*
     * The M7's poll frame. healthybridge_spi_send_cmd() emits these every 5 ms
     * purely to clock the bus while it waits for a CTRL_RESP — its poll loop
     * accepts nothing else, so answering a poll would be useless traffic, and at
     * one reply per 5 ms it would swamp the 4-deep TX ring and evict the very
     * CTRL_RESP the M7 is waiting for. Count it and move on.
     */
    case HB_TYPE_STATUS_REQ:
        s_rx_status_req++;
        break;
#endif
    case HB_TYPE_BATTERY:
        if (len >= sizeof(struct hb_battery_payload)) {
            const struct hb_battery_payload *bp = (const struct hb_battery_payload *)payload;
            data_store_set_battery(bp->soc, bp->flags & HB_BATT_CHARGING, bp->millivolts);
            ble_gatt_on_battery(bp->soc);
        }
        break;
    case HB_TYPE_CTRL_CMD:
        if (len >= 1) {
            control_handle_cmd(payload, len);
        }
        break;
    case HB_TYPE_HOST_RESP:
        ble_gatt_on_host_resp(payload, len);
        break;
    default:
        /* Counted, not just logged: an unhandled type produces one ESP_LOGD and
         * is invisible at INFO, which is how PPG was lost for an entire bring-up
         * while biosig and vitals counters looked healthy. The counter on the
         * status line makes the next one obvious. */
        s_rx_unknown++;
        ESP_LOGD(TAG, "unhandled type 0x%02x len=%u", type, len);
        break;
    }
}

#if defined(CONFIG_HB_PROFILE_HP6)
/*
 * HP6: decouple heavy consumer dispatch from the RX task.
 *
 * The codec callback runs in the transport's RX task. Doing data_store / BLE /
 * OpenView work there blocks that task, and HP6's frame rate is ~8.4x HP5's with
 * markedly heavier consumers (OpenView TCP, dashboard SSE, BLE notify). So the
 * callback only copies each frame into a queue, and a lower-priority worker does
 * the dispatch.
 *
 * Gated on the PROFILE, not the transport — the queue exists for HP6's load, and
 * both transports need it. What it costs when omitted differs, though:
 *
 *   SPI  — inline dispatch delays re-queueing the DMA transactions, and a master
 *          that clocks into an unarmed slave loses the frame silently.
 *   UART — inline dispatch lets the RX ring fill, RTS de-asserts and the host
 *          halts mid-byte. Nothing is lost, which is the point of that link, but
 *          the stream is needlessly rate-limited by the slowest consumer.
 *
 * HP5 keeps inline dispatch: one host, light consumers, released behaviour.
 */
#define HB_FRAME_QUEUE_DEPTH 16

struct hb_frame_item {
    uint16_t len;
    uint16_t seq;
    uint8_t  type;
    uint8_t  flags;
    uint8_t  payload[HB_CODEC_MAX_PAYLOAD];
};

static QueueHandle_t s_frame_q;
static uint32_t      s_frame_drops;

static void hb_dispatch_task(void *arg)
{
    (void)arg;
    static struct hb_frame_item it;   /* ~1 KB — keep it off the task stack */
    for (;;) {
        if (xQueueReceive(s_frame_q, &it, portMAX_DELAY) == pdTRUE) {
            hb_dispatch_frame(it.type, it.flags, it.seq,
                              it.len ? it.payload : NULL, it.len);
        }
    }
}

/* Codec callback (transport RX task): copy + enqueue; drop if the worker is
 * behind. A drop here is counted by hb_link_frame_drops() and shown as `drop=`
 * on the status line — on UART it is the only place a frame can be lost, since
 * the link itself back-pressures rather than dropping. */
static void hb_dispatch(uint8_t type, uint8_t flags, uint16_t seq,
                        const uint8_t *payload, uint16_t len, void *user)
{
    (void)user;
    static struct hb_frame_item it;   /* filled, then copied into the queue */
    if (s_frame_q == NULL) {
        return;
    }
    it.type  = type;
    it.flags = flags;
    it.seq   = seq;
    it.len   = (len > HB_CODEC_MAX_PAYLOAD) ? HB_CODEC_MAX_PAYLOAD : len;
    if (it.len && payload) {
        memcpy(it.payload, payload, it.len);
    }
    if (xQueueSend(s_frame_q, &it, 0) != pdTRUE) {
        s_frame_drops++;
    }
}
#else
/* HP5: dispatch inline in the RX task — unchanged runtime behavior. */
static void hb_dispatch(uint8_t type, uint8_t flags, uint16_t seq,
                        const uint8_t *payload, uint16_t len, void *user)
{
    (void)user;
    hb_dispatch_frame(type, flags, seq, payload, len);
}
#endif

uint32_t hb_link_frame_drops(void)
{
#if defined(CONFIG_HB_PROFILE_HP6)
    return s_frame_drops;
#else
    return 0;   /* HP5 dispatches inline — there is no queue to overflow. */
#endif
}

/* Adapter: transport delivers raw bytes -> codec. */
static void hb_codec_sink(const uint8_t *bytes, size_t n, void *user)
{
    (void)user;
    hb_codec_feed(bytes, n);
}

void hb_link_init(void)
{
    s_tp = hb_transport_get();
    hb_codec_reset();
    hb_codec_set_frame_cb(hb_dispatch, NULL);
#if defined(CONFIG_HB_ENABLE_OPENVIEW)
    wifi_server_init();   /* server task is started later, once Wi-Fi is up (main loop) */
#endif
#if defined(CONFIG_HB_PROFILE_HP6)
    /* Worker drains the frame queue at a priority BELOW the transport RX task
     * (SPI 5, UART 10), so the RX task always preempts it: on SPI that keeps DMA
     * transactions re-armed, on UART it keeps the RX ring drained so RTS stays
     * asserted while the host still has room to send. */
    s_frame_q = xQueueCreate(HB_FRAME_QUEUE_DEPTH, sizeof(struct hb_frame_item));
    if (s_frame_q == NULL ||
        xTaskCreate(hb_dispatch_task, "hb_disp", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "dispatch queue/task create failed");
        return;
    }
#endif
    s_tp->set_rx_sink(hb_codec_sink, NULL);
    if (s_tp->init() != 0) {
        ESP_LOGE(TAG, "transport '%s' init failed", s_tp->name);
        return;
    }
    ESP_LOGI(TAG, "HealthyBridge link up (transport=%s)", s_tp->name);
}
