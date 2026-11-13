/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — HealthyPi 6 firmware-update receiver: transfer state machine.
 * Pure C; see hb_ota.h for the contract and test/test_hb_ota.c for the tests.
 */
#include <string.h>

#include "hb_ota.h"

/* All multi-byte wire fields are little-endian; read and write them byte by
 * byte so this is independent of host byte order and alignment. */
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

void hb_ota_snapshot(const struct hb_ota *o, struct hb_ota_snapshot *s)
{
    s->state          = o->state;
    s->last_error     = o->last_error;
    s->bytes_received = o->next_offset;
    s->image_size     = o->image_size;
    s->slot_size      = o->slot_size;
}

static void publish(struct hb_ota *o)
{
    if (o->ops->publish) {
        struct hb_ota_snapshot s;
        hb_ota_snapshot(o, &s);
        o->ops->publish(o->ops->user, &s);
    }
}

void hb_ota_init(struct hb_ota *o, const struct hb_ota_ops *ops, uint32_t timeout_ms)
{
    memset(o, 0, sizeof(*o));
    o->ops = ops;
    o->timeout_ms = timeout_ms;
    o->state = HB_OTA_STATE_IDLE;
    o->slot_size = ops->slot_size(ops->user);
    publish(o);
}

/* End the transfer without committing. The slot keeps whatever was written;
 * boot selection is untouched. */
static void fail_transfer(struct hb_ota *o, uint8_t err)
{
    if (o->handle_open) {
        o->ops->abort(o->ops->user);
        o->handle_open = false;
    }
    o->state = HB_OTA_STATE_ERROR;
    o->last_error = err;
}

static bool transfer_active(const struct hb_ota *o)
{
    return o->state == HB_OTA_STATE_RECEIVING || o->state == HB_OTA_STATE_VERIFYING ||
           o->state == HB_OTA_STATE_COMMITTED;
}

/* Reply header + the 12-byte ack every op but STATUS carries. */
static size_t ack(struct hb_ota *o, uint8_t op, uint8_t status, uint32_t req_offset,
                  uint8_t *out, size_t cap)
{
    const size_t n = sizeof(struct hb_ota_rsp_hp6) + sizeof(struct hb_ota_ack_hp6);
    if (cap < n) {
        return 0;
    }
    if (status != HB_OTA_OK) {
        o->last_error = status;
    }
    bool active = transfer_active(o);
    out[0] = op;
    out[1] = status;
    put_u16(&out[2], (uint16_t)sizeof(struct hb_ota_ack_hp6));
    put_u32(&out[4],  req_offset);
    put_u32(&out[8],  active ? o->next_offset : 0);
    put_u32(&out[12], active ? o->image_size : 0);
    return n;
}

static size_t do_begin(struct hb_ota *o, const uint8_t *p, uint16_t plen,
                       uint8_t *out, size_t cap)
{
    if (plen != sizeof(struct hb_ota_begin_hp6)) {
        return ack(o, HB_OTA_OP_BEGIN, HB_OTA_ERR_BAD_REQUEST, 0, out, cap);
    }
    uint32_t size = get_u32(p);
    const uint8_t *digest = p + 4;

    /*
     * An identical BEGIN (same size, same digest) during a transfer RESTARTS
     * it from offset 0. That makes a retransmitted BEGIN — its reply lost —
     * harmless (no DATA can have been sent yet, so 0 is where the host is), and
     * lets a host that lost its place start the same image over without an
     * ABORT. A BEGIN for anything else is refused: one transfer at a time.
     */
    bool restart = false;
    if (o->state == HB_OTA_STATE_RECEIVING) {
        if (size != o->image_size || memcmp(digest, o->sha256, 32) != 0) {
            return ack(o, HB_OTA_OP_BEGIN, HB_OTA_ERR_BUSY, 0, out, cap);
        }
        restart = true;
    } else if (o->state != HB_OTA_STATE_IDLE && o->state != HB_OTA_STATE_ERROR) {
        return ack(o, HB_OTA_OP_BEGIN, HB_OTA_ERR_BUSY, 0, out, cap);
    }

    /* Every refusal below leaves an active transfer (restart case) as it was:
     * nothing is aborted until all checks have passed. */
    if (o->ops->running_unconfirmed(o->ops->user)) {
        return ack(o, HB_OTA_OP_BEGIN, HB_OTA_ERR_RUNNING_UNCONFIRMED, 0, out, cap);
    }
    o->slot_size = o->ops->slot_size(o->ops->user);
    if (o->slot_size == 0) {
        return ack(o, HB_OTA_OP_BEGIN, HB_OTA_ERR_NO_SLOT, 0, out, cap);
    }
    if (size == 0 || size > o->slot_size) {
        return ack(o, HB_OTA_OP_BEGIN, HB_OTA_ERR_BAD_SIZE, 0, out, cap);
    }

    if (restart && o->handle_open) {
        o->ops->abort(o->ops->user);
        o->handle_open = false;
    }
    o->state = HB_OTA_STATE_IDLE;
    o->next_offset = 0;
    o->last_len = 0;
    o->last_offset = 0;
    o->image_size = size;
    memcpy(o->sha256, digest, 32);

    if (o->ops->begin(o->ops->user) != 0) {
        o->state = HB_OTA_STATE_ERROR;
        return ack(o, HB_OTA_OP_BEGIN, HB_OTA_ERR_INTERNAL, 0, out, cap);
    }
    o->handle_open = true;
    o->state = HB_OTA_STATE_RECEIVING;
    o->last_error = HB_OTA_OK;
    o->last_activity_ms = o->ops->now_ms(o->ops->user);
    return ack(o, HB_OTA_OP_BEGIN, HB_OTA_OK, 0, out, cap);
}

static size_t do_data(struct hb_ota *o, const uint8_t *p, uint16_t plen,
                      uint8_t *out, size_t cap)
{
    if (plen < sizeof(struct hb_ota_data_hp6)) {
        return ack(o, HB_OTA_OP_DATA, HB_OTA_ERR_BAD_REQUEST, 0, out, cap);
    }
    uint32_t off = get_u32(p);
    uint32_t n   = (uint32_t)plen - sizeof(struct hb_ota_data_hp6);
    const uint8_t *bytes = p + sizeof(struct hb_ota_data_hp6);

    if (n == 0 || n > HB_OTA_MAX_CHUNK) {
        return ack(o, HB_OTA_OP_DATA, HB_OTA_ERR_BAD_REQUEST, off, out, cap);
    }
    if (o->state != HB_OTA_STATE_RECEIVING) {
        return ack(o, HB_OTA_OP_DATA, HB_OTA_ERR_NOT_IN_TRANSFER, off, out, cap);
    }
    o->last_activity_ms = o->ops->now_ms(o->ops->user);

    if (off != o->next_offset) {
        /*
         * The chunk this receiver accepted last, sent again (its ack was lost
         * or late): acknowledge it exactly as before and write nothing. Any
         * other offset is a gap or a rewind; the ack's next_offset tells the
         * host where to resume. Neither changes the transfer.
         */
        if (o->last_len != 0 && off == o->last_offset && n == o->last_len &&
            o->last_offset + o->last_len == o->next_offset) {
            o->retransmits++;
            return ack(o, HB_OTA_OP_DATA, HB_OTA_OK, off, out, cap);
        }
        return ack(o, HB_OTA_OP_DATA, HB_OTA_ERR_BAD_OFFSET, off, out, cap);
    }
    if (n > o->image_size - o->next_offset) {
        return ack(o, HB_OTA_OP_DATA, HB_OTA_ERR_BAD_SIZE, off, out, cap);
    }

    int rc = o->ops->write(o->ops->user, bytes, n);
    if (rc != HB_OTA_IO_OK) {
        fail_transfer(o, rc == HB_OTA_IO_INVALID ? HB_OTA_ERR_IMAGE_INVALID
                                                 : HB_OTA_ERR_FLASH_WRITE);
        return ack(o, HB_OTA_OP_DATA, o->last_error, off, out, cap);
    }
    o->last_offset = off;
    o->last_len = n;
    o->next_offset += n;
    return ack(o, HB_OTA_OP_DATA, HB_OTA_OK, off, out, cap);
}

static size_t do_commit(struct hb_ota *o, uint16_t plen, uint8_t *out, size_t cap,
                        bool *reboot)
{
    if (plen != 0) {
        return ack(o, HB_OTA_OP_COMMIT, HB_OTA_ERR_BAD_REQUEST, 0, out, cap);
    }
    if (o->state == HB_OTA_STATE_COMMITTED) {
        /* A retransmitted COMMIT that raced the reboot: same answer. */
        *reboot = true;
        return ack(o, HB_OTA_OP_COMMIT, HB_OTA_OK, 0, out, cap);
    }
    if (o->state != HB_OTA_STATE_RECEIVING) {
        return ack(o, HB_OTA_OP_COMMIT, HB_OTA_ERR_NOT_IN_TRANSFER, 0, out, cap);
    }
    o->last_activity_ms = o->ops->now_ms(o->ops->user);
    if (o->next_offset != o->image_size) {
        return ack(o, HB_OTA_OP_COMMIT, HB_OTA_ERR_INCOMPLETE, 0, out, cap);
    }

    o->state = HB_OTA_STATE_VERIFYING;
    publish(o);

    /* Integrity: hash what is actually in flash, not what was received. */
    const struct hb_ota_ops *ops = o->ops;
    uint8_t digest[32];
    ops->sha256_start(ops->user);
    for (uint32_t off = 0; off < o->image_size; ) {
        uint32_t n = o->image_size - off;
        if (n > sizeof(o->buf)) {
            n = sizeof(o->buf);
        }
        if (ops->read(ops->user, off, o->buf, n) != 0) {
            ops->sha256_finish(ops->user, digest);   /* release the context */
            fail_transfer(o, HB_OTA_ERR_FLASH_READ);
            return ack(o, HB_OTA_OP_COMMIT, HB_OTA_ERR_FLASH_READ, 0, out, cap);
        }
        ops->sha256_update(ops->user, o->buf, n);
        off += n;
    }
    ops->sha256_finish(ops->user, digest);
    if (memcmp(digest, o->sha256, sizeof(digest)) != 0) {
        fail_transfer(o, HB_OTA_ERR_DIGEST_MISMATCH);
        return ack(o, HB_OTA_OP_COMMIT, HB_OTA_ERR_DIGEST_MISMATCH, 0, out, cap);
    }

    /* Sanity: ops->end closes the handle whatever it returns. */
    o->handle_open = false;
    if (ops->end(ops->user) != 0) {
        fail_transfer(o, HB_OTA_ERR_IMAGE_INVALID);
        return ack(o, HB_OTA_OP_COMMIT, HB_OTA_ERR_IMAGE_INVALID, 0, out, cap);
    }
    if (ops->set_boot(ops->user) != 0) {
        fail_transfer(o, HB_OTA_ERR_BOOT_SET);
        return ack(o, HB_OTA_OP_COMMIT, HB_OTA_ERR_BOOT_SET, 0, out, cap);
    }

    o->state = HB_OTA_STATE_COMMITTED;
    *reboot = true;
    return ack(o, HB_OTA_OP_COMMIT, HB_OTA_OK, 0, out, cap);
}

static size_t do_abort(struct hb_ota *o, uint16_t plen, uint8_t *out, size_t cap)
{
    if (plen != 0) {
        return ack(o, HB_OTA_OP_ABORT, HB_OTA_ERR_BAD_REQUEST, 0, out, cap);
    }
    if (o->state == HB_OTA_STATE_COMMITTED) {
        /* Too late: the boot slot has already been switched. */
        return ack(o, HB_OTA_OP_ABORT, HB_OTA_ERR_BUSY, 0, out, cap);
    }
    if (o->state == HB_OTA_STATE_RECEIVING) {
        fail_transfer(o, HB_OTA_ERR_ABORTED);
    }
    o->state = HB_OTA_STATE_IDLE;
    return ack(o, HB_OTA_OP_ABORT, HB_OTA_OK, 0, out, cap);
}

size_t hb_ota_status_reply(const struct hb_ota_snapshot *s, const struct hb_ota_running *r,
                           const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
    const size_t n = sizeof(struct hb_ota_rsp_hp6) + sizeof(struct hb_ota_status_hp6);
    if (cap < n) {
        return 0;
    }
    uint8_t status = HB_OTA_OK;
    if (len < sizeof(struct hb_ota_req_hp6) || get_u16(&req[2]) != 0 ||
        len != sizeof(struct hb_ota_req_hp6)) {
        status = HB_OTA_ERR_BAD_REQUEST;
    }
    memset(out, 0, n);
    out[0] = HB_OTA_OP_STATUS;
    out[1] = status;
    put_u16(&out[2], (uint16_t)sizeof(struct hb_ota_status_hp6));
    uint8_t *d = &out[4];
    d[0] = HB_OTA_PROTO_VERSION;
    d[1] = s->state;
    d[2] = s->last_error;
    d[3] = r->slot;
    d[4] = r->img_state;
    d[5] = r->other_img_state;
    put_u16(&d[6], HB_OTA_MAX_CHUNK);
    put_u32(&d[8],  s->bytes_received);
    put_u32(&d[12], s->image_size);
    put_u32(&d[16], s->slot_size);
    for (size_t i = 0; i < sizeof(r->version) - 1 && r->version[i] != '\0'; i++) {
        d[20 + i] = (uint8_t)r->version[i];   /* rest stays zero: NUL-terminated, padded */
    }
    return n;
}

size_t hb_ota_handle(struct hb_ota *o, const uint8_t *req, uint16_t len,
                     uint8_t *out, size_t cap, bool *reboot)
{
    *reboot = false;
    uint8_t op = hb_ota_req_op(req, len);
    size_t n;

    if (len < sizeof(struct hb_ota_req_hp6) ||
        get_u16(&req[2]) != (uint16_t)(len - sizeof(struct hb_ota_req_hp6))) {
        n = ack(o, op, HB_OTA_ERR_BAD_REQUEST, 0, out, cap);
        publish(o);
        return n;
    }
    const uint8_t *p = req + sizeof(struct hb_ota_req_hp6);
    uint16_t plen = (uint16_t)(len - sizeof(struct hb_ota_req_hp6));

    switch (op) {
    case HB_OTA_OP_BEGIN:  n = do_begin(o, p, plen, out, cap);          break;
    case HB_OTA_OP_DATA:   n = do_data(o, p, plen, out, cap);           break;
    case HB_OTA_OP_COMMIT: n = do_commit(o, plen, out, cap, reboot);    break;
    case HB_OTA_OP_ABORT:  n = do_abort(o, plen, out, cap);             break;
    case HB_OTA_OP_STATUS: {
        /* Normally answered by the RX path from the published snapshot; here
         * for completeness, with the running-image fields unknown. */
        struct hb_ota_snapshot s;
        struct hb_ota_running r = { .slot = 0xFF, .img_state = HB_OTA_IMG_UNKNOWN,
                                    .other_img_state = HB_OTA_IMG_UNKNOWN };
        hb_ota_snapshot(o, &s);
        return hb_ota_status_reply(&s, &r, req, len, out, cap);
    }
    default:
        n = ack(o, op, HB_OTA_ERR_UNKNOWN_OP, 0, out, cap);
        break;
    }
    publish(o);
    return n;
}

bool hb_ota_tick(struct hb_ota *o)
{
    if (o->state != HB_OTA_STATE_RECEIVING || o->timeout_ms == 0) {
        return false;
    }
    uint32_t idle = o->ops->now_ms(o->ops->user) - o->last_activity_ms;
    if (idle < o->timeout_ms) {
        return false;
    }
    fail_transfer(o, HB_OTA_ERR_TIMEOUT);
    publish(o);
    return true;
}
