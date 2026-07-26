/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge transport backend — SPI slave (HealthyPi 6, ESP32-C6 <-> STM32 M7).
 *
 * Transport-only: brings up the FSPI (SPI2) slave with DMA, and on each completed
 * transaction pushes the received bytes into the codec sink. Framing/CRC/sync-hunt
 * live in hb_codec — this backend does NOT slice frames or dispatch (that was the
 * entanglement in the HP6 app's hl_spi.c; it is deliberately left out here).
 *
 * STATUS: RX verified on C6 hardware against the M7 master (crc_err=0 sustained).
 * The default (HP5/UART) build never compiles this file's body.
 */
#include "sdkconfig.h"

#if defined(CONFIG_HB_TRANSPORT_SPI)

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_slave.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"

#include "hb_transport.h"
#include "hb_codec.h"
#include "board_pins_hp6.h"

static const char *TAG = "hb_spi";

#define HB_SPI_HOST_ID    SPI2_HOST
/* Conservative DMA-safe transfer size; must cover the largest HP6 frame
 * (16 x 32-byte samples + header + CRC). */
#define HB_SPI_XFER_SIZE  640
#define HB_SPI_QUEUE_SIZE 10

/* Pending slave->master responses. Replies are small (a control response or a
 * status frame, tens of bytes); the depth only has to cover the replies that can
 * pile up between two transactions — a command response racing the 1 Hz status
 * frame from the main loop. */
#define HB_SPI_TX_FRAME_MAX 64
#define HB_SPI_TX_SLOTS     4

static hb_rx_sink_fn s_sink;
static void         *s_sink_user;
static volatile uint32_t s_rx_bytes;
static bool s_inited;

/* TX ring, written by any task via spi_send() and drained by hb_spi_task one
 * frame per transaction. s_tx_lock guards all four fields. */
static SemaphoreHandle_t s_tx_lock;
static uint8_t  s_tx_slot[HB_SPI_TX_SLOTS][HB_SPI_TX_FRAME_MAX];
static size_t   s_tx_len[HB_SPI_TX_SLOTS];
static uint8_t  s_tx_head, s_tx_tail;
static uint32_t s_tx_frames, s_tx_drops;

static void spi_set_rx_sink(hb_rx_sink_fn cb, void *user)
{
    s_sink = cb;
    s_sink_user = user;
}

static void spi_get_rx_bytes(uint32_t *out)
{
    if (out) { *out = s_rx_bytes; }
}

/*
 * Slave -> master transmit — deferred.
 *
 * NEVER call spi_slave_transmit() from here: the slave has a single transaction
 * queue, already owned by hb_spi_task (queue_trans / get_trans_result), and a
 * second submitter trips assert(ret_trans == trans_desc) in spi_slave.c. The
 * response is instead buffered here and loaded by hb_spi_task into the TX buffer
 * of a transaction it re-queues itself.
 *
 * Delivery is therefore deferred by however many transactions are already queued
 * ahead of it (up to HB_SPI_QUEUE_SIZE). That fits the M7's protocol unchanged:
 * healthybridge_spi_send_cmd() transceives the command, then polls with
 * STATUS_REQ frames every 5 ms for up to its timeout, so the reply is picked up
 * by one of the polls. The M7 holds its spi_lock across that whole sequence, so
 * no data-push transaction can consume the reply in between.
 */
static int spi_send(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || s_tx_lock == NULL) {
        return -1;
    }
    if (len > HB_SPI_TX_FRAME_MAX) {
        ESP_LOGW(TAG, "tx frame too large (%u > %d)", (unsigned)len, HB_SPI_TX_FRAME_MAX);
        return -1;
    }

    int rc = -1;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uint8_t next = (uint8_t)((s_tx_head + 1) % HB_SPI_TX_SLOTS);
    if (next == s_tx_tail) {
        s_tx_drops++;                       /* ring full — the master isn't clocking */
    } else {
        memcpy(s_tx_slot[s_tx_head], buf, len);
        s_tx_len[s_tx_head] = len;
        s_tx_head = next;
        rc = 0;
    }
    xSemaphoreGive(s_tx_lock);
    return rc;
}

/* Load the next pending response into a transaction's TX buffer, or clear the
 * buffer so a reply already on the wire is not repeated when this buffer cycles
 * round again. The frame starts at offset 0: the M7 casts its RX buffer straight
 * to the frame header and does not sync-hunt, so a response must not be padded. */
static void spi_load_tx(uint8_t *tx)
{
    if (tx == NULL) {
        return;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if (s_tx_tail != s_tx_head) {
        size_t len = s_tx_len[s_tx_tail];
        memcpy(tx, s_tx_slot[s_tx_tail], len);
        memset(tx + len, 0, HB_SPI_XFER_SIZE - len);
        s_tx_tail = (uint8_t)((s_tx_tail + 1) % HB_SPI_TX_SLOTS);
        s_tx_frames++;
    } else {
        memset(tx, 0, HB_SPI_XFER_SIZE);
    }
    xSemaphoreGive(s_tx_lock);
}

void hb_transport_spi_tx_stats(uint32_t *sent, uint32_t *drops)
{
    if (sent)  { *sent  = s_tx_frames; }
    if (drops) { *drops = s_tx_drops; }
}

/*
 * Bring-up diagnostic: what is actually landing in the RX buffer?
 *
 * `rx=` climbing while every frame counter AND crc_err stay 0 is a real observed
 * failure mode (2026-07-26), and the status line cannot tell its causes apart:
 * a master clocking zeros looks identical to a stream whose framing never
 * matches. crc_err does not disambiguate either — a false sync reads a random
 * 16-bit length, which busts HB_CODEC_MAX_PAYLOAD ~98% of the time and is then
 * dropped at S_HDR with no CRC error recorded.
 *
 * So report the bytes themselves:
 *   - while every byte of every transaction is zero, say so periodically — that
 *     is "the master is clocking us but the data line is silent";
 *   - on the first transaction carrying any non-zero byte, dump its head once.
 *     A leading 8-byte zero preamble then 55 AA is a healthy M7 frame; anything
 *     else is a framing or contract mismatch, not a wiring fault.
 *
 * Self-silencing: once data arrives, this stops logging. SPI-only file, so HP5
 * cannot be affected.
 */
static uint32_t s_diag_trans, s_diag_data_trans;
static bool     s_diag_dumped;

static void spi_diag_rx(const uint8_t *rx, size_t n)
{
    size_t nz = 0;
    for (size_t i = 0; i < n; i++) {
        if (rx[i] != 0) { nz++; }
    }
    s_diag_trans++;
    if (nz > 0) { s_diag_data_trans++; }

    if (nz > 0 && !s_diag_dumped) {
        s_diag_dumped = true;
        ESP_LOGW(TAG, "first RX transaction with data: %u/%u bytes non-zero "
                      "(expect an 8-byte zero preamble then 55 AA)",
                 (unsigned)nz, (unsigned)n);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, rx, (n < 48) ? n : 48, ESP_LOG_WARN);
    }
    /* ~5 s at the HP6 frame rate. Only while nothing has ever carried data. */
    if (s_diag_data_trans == 0 && (s_diag_trans % 256) == 0) {
        ESP_LOGW(TAG, "rx diag: %lu transactions clocked, EVERY byte zero — "
                      "master is clocking but the data line is silent",
                 (unsigned long)s_diag_trans);
    }
}

static void hb_spi_task(void *arg)
{
    (void)arg;
    uint8_t *rx_bufs[HB_SPI_QUEUE_SIZE];
    uint8_t *tx_bufs[HB_SPI_QUEUE_SIZE];
    spi_slave_transaction_t trans[HB_SPI_QUEUE_SIZE];

    for (int i = 0; i < HB_SPI_QUEUE_SIZE; i++) {
        rx_bufs[i] = heap_caps_malloc(HB_SPI_XFER_SIZE, MALLOC_CAP_DMA);
        tx_bufs[i] = heap_caps_malloc(HB_SPI_XFER_SIZE, MALLOC_CAP_DMA);
        if (!rx_bufs[i] || !tx_bufs[i]) {
            ESP_LOGE(TAG, "DMA buffer alloc failed (%d)", i);
            vTaskDelete(NULL);
            return;
        }
        memset(tx_bufs[i], 0, HB_SPI_XFER_SIZE);
        memset(&trans[i], 0, sizeof(trans[i]));
        trans[i].rx_buffer = rx_bufs[i];
        trans[i].tx_buffer = tx_bufs[i];
        trans[i].length = (size_t)HB_SPI_XFER_SIZE * 8;
        ESP_ERROR_CHECK(spi_slave_queue_trans(HB_SPI_HOST_ID, &trans[i], portMAX_DELAY));
    }
    ESP_LOGI(TAG, "SPI slave ready (%d x %d-byte transactions queued)",
             HB_SPI_QUEUE_SIZE, HB_SPI_XFER_SIZE);

    for (;;) {
        spi_slave_transaction_t *ret = NULL;
        esp_err_t r = spi_slave_get_trans_result(HB_SPI_HOST_ID, &ret, pdMS_TO_TICKS(2000));
        if (r != ESP_OK || ret == NULL) {
            ESP_LOGD(TAG, "SPI idle (rc=%d, rx_bytes=%lu)", r, (unsigned long)s_rx_bytes);
            continue;
        }
        size_t nbytes = ret->trans_len / 8;
        if (nbytes > HB_SPI_XFER_SIZE) {   /* clamp a bogus trans_len to the DMA buffer */
            nbytes = HB_SPI_XFER_SIZE;
        }
        /* A completed transaction can carry a NULL rx_buffer or zero length during
         * bring-up (idle/short clocking); never hand that to the codec. */
        if (nbytes > 0 && ret->rx_buffer != NULL) {
            s_rx_bytes += (uint32_t)nbytes;
            spi_diag_rx((const uint8_t *)ret->rx_buffer, nbytes);
            if (s_sink) {
                /* SPI is message-framed: each completed transaction is one CS-delimited
                 * frame (<= one full frame + preamble, always < the transfer size).
                 * Reset the parser per transaction so it parses each buffer standalone
                 * — a corrupted/short frame cannot desync the stream and cascade CRC
                 * failures into the following frames. This mirrors the proven
                 * per-transaction parsing in the reference app_esp32c6_esp_idf/hl_spi.c;
                 * the codec's cross-call streaming state is only appropriate for the
                 * byte-stream UART transport. */
                hb_codec_reset();
                s_sink((const uint8_t *)ret->rx_buffer, nbytes, s_sink_user);
            }
        }
        spi_load_tx((uint8_t *)ret->tx_buffer);
        ret->length = (size_t)HB_SPI_XFER_SIZE * 8;
        spi_slave_queue_trans(HB_SPI_HOST_ID, ret, portMAX_DELAY);
    }
}

static int spi_init(void)
{
    if (s_inited) { return 0; }

    /* Created before the RX task, and before hb_link can call spi_send(): a NULL
     * lock is spi_send()'s "transport not up yet" guard. */
    s_tx_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL) {
        ESP_LOGE(TAG, "tx mutex alloc failed");
        return -1;
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = HB_SPI_PIN_MOSI,
        .miso_io_num = HB_SPI_PIN_MISO,
        .sclk_io_num = HB_SPI_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = HB_SPI_XFER_SIZE,
    };
    spi_slave_interface_config_t slvcfg = {
        .spics_io_num = HB_SPI_PIN_CS,
        .flags = 0,
        .queue_size = HB_SPI_QUEUE_SIZE,
        .mode = 0,
    };
    esp_err_t err = spi_slave_initialize(HB_SPI_HOST_ID, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_initialize failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* The SPI peripheral owns the CS pin (spics_io_num); do NOT reconfigure it as a
     * GPIO or hang an ISR on it — an interrupt at CS assertion delays the slave's
     * readiness for bit 0 and corrupts the first byte of every transaction. */

    if (xTaskCreate(hb_spi_task, "hb_spi", 6 * 1024, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create hb_spi task");
        return -1;
    }
    s_inited = true;
    ESP_LOGI(TAG, "SPI slave up (SCLK=%d MOSI=%d MISO=%d CS=%d)",
             HB_SPI_PIN_SCLK, HB_SPI_PIN_MOSI, HB_SPI_PIN_MISO, HB_SPI_PIN_CS);
    return 0;
}

static const struct hb_transport_if s_spi_if = {
    .name         = "spi",
    .init         = spi_init,
    .send         = spi_send,
    .set_rx_sink  = spi_set_rx_sink,
    .get_rx_bytes = spi_get_rx_bytes,
};

const struct hb_transport_if *hb_transport_get(void)
{
    return &s_spi_if;
}

#endif /* CONFIG_HB_TRANSPORT_SPI */
