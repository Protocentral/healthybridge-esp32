/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — HealthyPi 6 firmware-update receiver: the transfer state
 * machine, in pure C.
 *
 * The M7 relays a new C6 image over the HealthyBridge link (frame types
 * 0x80/0x81, see healthybridge_hp6.h and docs/PROTOCOL.md). This module decides
 * what each request does and what its reply says. Everything that touches the
 * chip — esp_ota_*, the partition, SHA-256, the clock — is behind
 * struct hb_ota_ops, so the whole protocol runs on a host against a fake flash
 * (test/test_hb_ota.c). hb_ota_esp.c supplies the real ops, the task and the
 * routing.
 *
 * Invariants this module guarantees, whatever the ops return:
 *   - boot selection (ops->set_boot) is touched only by a COMMIT whose
 *     read-back digest matched and whose image validated;
 *   - nothing is written outside [0, image_size) of the inactive slot, and
 *     every byte is written exactly once, in order;
 *   - a handle opened by ops->begin is closed exactly once (ops->abort, or
 *     ops->end which closes it on success and failure alike).
 *
 * Not thread-safe: one task calls hb_ota_handle()/hb_ota_tick(). The STATUS
 * reply is built from a snapshot (hb_ota_snapshot / ops->publish) so another
 * task can answer it while this one is busy hashing.
 *
 * HealthyPi 6 only. The HealthyPi 5 build does not compile this file.
 */
#ifndef HB_OTA_H
#define HB_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "healthybridge_hp6.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Results of hb_ota_ops.write. */
#define HB_OTA_IO_OK       0
#define HB_OTA_IO_FAIL     (-1)   /* flash erase/write error                   */
#define HB_OTA_IO_INVALID  (-2)   /* rejected as not an image (bad magic byte) */

/* Bytes read back per ops->read call while hashing at COMMIT. */
#define HB_OTA_READ_CHUNK  4096

/* Reply payload capacity hb_ota_handle() / hb_ota_status_reply() need. */
#define HB_OTA_RSP_MAX     (sizeof(struct hb_ota_rsp_hp6) + sizeof(struct hb_ota_status_hp6))

/* What the receiver says about itself in a STATUS reply. */
struct hb_ota_snapshot {
    uint8_t  state;           /* HB_OTA_STATE_* */
    uint8_t  last_error;      /* HB_OTA_ERR_* */
    uint32_t bytes_received;
    uint32_t image_size;
    uint32_t slot_size;
};

/* What the platform says about the running image in a STATUS reply. */
struct hb_ota_running {
    uint8_t slot;             /* 0 / 1 / 0xFF */
    uint8_t img_state;        /* HB_OTA_IMG_* */
    uint8_t other_img_state;  /* HB_OTA_IMG_* */
    char    version[32];
};

struct hb_ota_ops {
    void *user;
    /* Capacity of the inactive OTA slot; 0 when there is none. */
    uint32_t (*slot_size)(void *user);
    /* True while the running image is PENDING_VERIFY. */
    bool (*running_unconfirmed)(void *user);
    /* Open the inactive slot for sequential writing. 0 on success. Must not
     * change which slot boots. */
    int  (*begin)(void *user);
    /* Append n bytes to the open slot (erasing ahead as needed): HB_OTA_IO_*. */
    int  (*write)(void *user, const uint8_t *data, uint32_t n);
    /* Close the open slot without validating. Never fails. */
    void (*abort)(void *user);
    /* Read n bytes at `off` of the inactive slot. 0 on success. */
    int  (*read)(void *user, uint32_t off, uint8_t *buf, uint32_t n);
    void (*sha256_start)(void *user);
    void (*sha256_update)(void *user, const uint8_t *data, uint32_t n);
    void (*sha256_finish)(void *user, uint8_t out[32]);
    /* Close the slot and validate the image in it (esp_ota_end, chip, project).
     * 0 when valid. Closes the handle whatever it returns. */
    int  (*end)(void *user);
    /* Make the inactive slot the boot slot. 0 on success; on failure the boot
     * selection must be as it was. */
    int  (*set_boot)(void *user);
    /* Monotonic milliseconds. */
    uint32_t (*now_ms)(void *user);
    /* Optional: called with the new snapshot whenever it changes, including on
     * entering VERIFYING before the (slow) digest. */
    void (*publish)(void *user, const struct hb_ota_snapshot *s);
};

struct hb_ota {
    const struct hb_ota_ops *ops;
    uint32_t timeout_ms;        /* inactivity limit; 0 = never */

    uint8_t  state;
    uint8_t  last_error;
    bool     handle_open;
    uint32_t image_size;
    uint8_t  sha256[32];
    uint32_t next_offset;
    uint32_t last_offset;       /* the last accepted chunk, for idempotent retransmit */
    uint32_t last_len;
    uint32_t last_activity_ms;
    uint32_t slot_size;

    /* Diagnostics. */
    uint32_t retransmits;       /* duplicate DATA acknowledged without a write */

    uint8_t  buf[HB_OTA_READ_CHUNK];
};

/* Initialise to IDLE. `timeout_ms` = 0 disables the inactivity abort. */
void hb_ota_init(struct hb_ota *o, const struct hb_ota_ops *ops, uint32_t timeout_ms);

/* Handle one OTA_REQ payload (anything but STATUS, which hb_ota_status_reply
 * answers; a STATUS passed here is answered from the live state). Writes the
 * OTA_RSP payload to `out` and returns its length (0 only if cap is too small).
 * `*reboot` is set when the reply must be flushed and the chip restarted. */
size_t hb_ota_handle(struct hb_ota *o, const uint8_t *req, uint16_t len,
                     uint8_t *out, size_t cap, bool *reboot);

/* Advance the inactivity timer. Aborts an abandoned transfer: state ERROR,
 * last_error HB_OTA_ERR_TIMEOUT. Returns true if it did. */
bool hb_ota_tick(struct hb_ota *o);

/* The current snapshot. */
void hb_ota_snapshot(const struct hb_ota *o, struct hb_ota_snapshot *s);

/* Build the reply to a STATUS request from a snapshot and the running-image
 * facts. A malformed request is answered with HB_OTA_ERR_BAD_REQUEST and the
 * same 52 bytes of data. Returns the payload length (0 if cap is too small). */
size_t hb_ota_status_reply(const struct hb_ota_snapshot *s, const struct hb_ota_running *r,
                           const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);

/* The op byte of a request, or 0 for an empty payload. */
static inline uint8_t hb_ota_req_op(const uint8_t *req, uint16_t len)
{
    return (req != NULL && len > 0) ? req[0] : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* HB_OTA_H */
