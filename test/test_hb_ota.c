/*
 * SPDX-License-Identifier: MIT
 * Host test for the HealthyPi 6 firmware-update receiver (frame types
 * 0x80/0x81): the transfer state machine in main/hb_ota.c, run against a fake
 * flash slot, a reference SHA-256 and a fake clock.
 *
 * The golden frames are the contract the M7 relay is written against
 * (docs/PROTOCOL.md, "Firmware update"). A change that moves a byte must fail
 * here first.
 *
 *   cc -std=c11 -I main -DHB_CODEC_MAX_PAYLOAD=1024 test/test_hb_ota.c \
 *      main/hb_ota.c main/hb_codec.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hb_codec.h"
#include "hb_ota.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); fails++; } } while (0)

/* ---- reference SHA-256 (FIPS 180-4), for the fake platform ---------------- */

struct sha256 { uint32_t h[8]; uint8_t blk[64]; uint64_t bits; uint32_t n; };

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(struct sha256 *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(struct sha256 *s)
{
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->bits = 0; s->n = 0;
}

static void sha_update(struct sha256 *s, const uint8_t *p, size_t len)
{
    s->bits += (uint64_t)len * 8;
    while (len--) {
        s->blk[s->n++] = *p++;
        if (s->n == 64) { sha_block(s, s->blk); s->n = 0; }
    }
}

static void sha_final(struct sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->bits;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56) { sha_update(s, &pad, 1); }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) { len[i] = (uint8_t)(bits >> (56 - 8 * i)); }
    sha_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4*i] = (uint8_t)(s->h[i] >> 24); out[4*i+1] = (uint8_t)(s->h[i] >> 16);
        out[4*i+2] = (uint8_t)(s->h[i] >> 8); out[4*i+3] = (uint8_t)s->h[i];
    }
}

static void sha256(const uint8_t *p, size_t n, uint8_t out[32])
{
    struct sha256 s;
    sha_init(&s); sha_update(&s, p, n); sha_final(&s, out);
}

/* ---- fake platform -------------------------------------------------------- */

#define SLOT_MAX (256 * 1024)

static struct {
    uint8_t  slot[SLOT_MAX];
    uint8_t  written[SLOT_MAX];   /* times each byte was written since begin */
    uint32_t slot_size;
    bool     pending;             /* running image PENDING_VERIFY */
    bool     open;                /* handle open */
    uint32_t wptr;
    int      begins, aborts, ends, writes;
    int      boot_sets;           /* successful set_boot calls */
    int      begin_rc, end_rc, boot_rc;
    int      fail_write_at;       /* 1-based write call that fails; 0 = never */
    uint32_t fail_read_at;        /* fail a read covering this offset; UINT32_MAX = never */
    uint32_t now;
    struct sha256 sha;
    int      handle_errors;       /* double open / close-without-open */
    struct hb_ota_snapshot pub;
    int      publishes;
} F;

static uint32_t f_slot_size(void *u) { (void)u; return F.slot_size; }
static bool f_unconfirmed(void *u) { (void)u; return F.pending; }

static int f_begin(void *u)
{
    (void)u;
    if (F.open) { F.handle_errors++; }
    if (F.begin_rc) { return F.begin_rc; }
    F.open = true; F.begins++; F.wptr = 0;
    memset(F.written, 0, sizeof(F.written));
    return 0;
}

static int f_write(void *u, const uint8_t *d, uint32_t n)
{
    (void)u;
    if (!F.open) { F.handle_errors++; return HB_OTA_IO_FAIL; }
    F.writes++;
    if (F.fail_write_at && F.writes == F.fail_write_at) { return HB_OTA_IO_FAIL; }
    if (F.wptr == 0 && d[0] != 0xE9) { return HB_OTA_IO_INVALID; }   /* as esp_ota_write */
    if (F.wptr + n > F.slot_size) { return HB_OTA_IO_FAIL; }
    memcpy(&F.slot[F.wptr], d, n);
    for (uint32_t i = 0; i < n; i++) { F.written[F.wptr + i]++; }
    F.wptr += n;
    return HB_OTA_IO_OK;
}

static void f_abort(void *u) { (void)u; if (!F.open) { F.handle_errors++; } F.open = false; F.aborts++; }

static int f_read(void *u, uint32_t off, uint8_t *buf, uint32_t n)
{
    (void)u;
    if (F.fail_read_at >= off && F.fail_read_at < off + n) { return -1; }
    memcpy(buf, &F.slot[off], n);
    return 0;
}

static void f_sha_start(void *u) { (void)u; sha_init(&F.sha); }
static void f_sha_update(void *u, const uint8_t *d, uint32_t n) { (void)u; sha_update(&F.sha, d, n); }
static void f_sha_finish(void *u, uint8_t out[32]) { (void)u; sha_final(&F.sha, out); }

static int f_end(void *u)
{
    (void)u;
    if (!F.open) { F.handle_errors++; }
    F.open = false; F.ends++;
    return F.end_rc;
}

static int f_set_boot(void *u)
{
    (void)u;
    if (F.boot_rc) { return F.boot_rc; }
    F.boot_sets++;
    return 0;
}

static uint32_t f_now(void *u) { (void)u; return F.now; }
static void f_publish(void *u, const struct hb_ota_snapshot *s) { (void)u; F.pub = *s; F.publishes++; }

static const struct hb_ota_ops OPS = {
    .slot_size = f_slot_size, .running_unconfirmed = f_unconfirmed,
    .begin = f_begin, .write = f_write, .abort = f_abort, .read = f_read,
    .sha256_start = f_sha_start, .sha256_update = f_sha_update, .sha256_finish = f_sha_finish,
    .end = f_end, .set_boot = f_set_boot, .now_ms = f_now, .publish = f_publish,
};

static struct hb_ota O;

static void reset_platform(uint32_t slot_size)
{
    memset(&F, 0, sizeof(F));
    F.slot_size = slot_size;
    F.fail_read_at = UINT32_MAX;
    F.now = 1000;
    hb_ota_init(&O, &OPS, 30000);
}

/* ---- request builders and reply parsing ------------------------------------ */

static uint8_t RSP[HB_OTA_RSP_MAX];
static bool    REBOOT;

struct ack { uint8_t op, status; uint16_t data_len; uint32_t req_off, next, size; };

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static struct ack exchange(const uint8_t *req, uint16_t len)
{
    size_t n = hb_ota_handle(&O, req, len, RSP, sizeof(RSP), &REBOOT);
    struct ack a = {0};
    CHECK(n == 16);
    a.op = RSP[0]; a.status = RSP[1]; a.data_len = (uint16_t)(RSP[2] | RSP[3] << 8);
    a.req_off = le32(&RSP[4]); a.next = le32(&RSP[8]); a.size = le32(&RSP[12]);
    CHECK(a.data_len == 12);
    return a;
}

static struct ack req_begin(uint32_t size, const uint8_t digest[32])
{
    uint8_t r[40] = { HB_OTA_OP_BEGIN, 0, 36, 0,
                      (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16), (uint8_t)(size >> 24) };
    memcpy(&r[8], digest, 32);
    return exchange(r, sizeof(r));
}

static struct ack req_data(uint32_t off, const uint8_t *bytes, uint16_t n)
{
    static uint8_t r[8 + 2048];
    uint16_t plen = (uint16_t)(4 + n);
    r[0] = HB_OTA_OP_DATA; r[1] = 0; r[2] = (uint8_t)plen; r[3] = (uint8_t)(plen >> 8);
    r[4] = (uint8_t)off; r[5] = (uint8_t)(off >> 8); r[6] = (uint8_t)(off >> 16); r[7] = (uint8_t)(off >> 24);
    memcpy(&r[8], bytes, n);
    return exchange(r, (uint16_t)(8 + n));
}

static struct ack req_simple(uint8_t op)
{
    uint8_t r[4] = { op, 0, 0, 0 };
    return exchange(r, sizeof(r));
}

/* A plausible image: 0xE9 magic, then pseudo-random bytes. */
static uint8_t IMG[200000];
static uint8_t IMG_SHA[32];

static void make_image(uint32_t n)
{
    uint32_t x = 0x12345678;
    for (uint32_t i = 0; i < n; i++) { x = x * 1103515245u + 12345u; IMG[i] = (uint8_t)(x >> 16); }
    IMG[0] = 0xE9;
    sha256(IMG, n, IMG_SHA);
}

/* Send IMG[0..n) in `chunk`-byte DATA frames; stops at the first non-OK. */
static struct ack send_image(uint32_t from, uint32_t n, uint16_t chunk)
{
    struct ack a = {0};
    for (uint32_t off = from; off < n; off += chunk) {
        uint16_t c = (uint16_t)((n - off) < chunk ? (n - off) : chunk);
        a = req_data(off, &IMG[off], c);
        if (a.status != HB_OTA_OK) { break; }
    }
    return a;
}

static bool slot_matches_image(uint32_t n)
{
    if (F.wptr != n || memcmp(F.slot, IMG, n) != 0) { return false; }
    for (uint32_t i = 0; i < n; i++) { if (F.written[i] != 1) { return false; } }
    return true;
}

/* ---- tests ------------------------------------------------------------------ */

static void test_sha_reference(void)
{
    static const uint8_t abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
    uint8_t d[32];
    sha256((const uint8_t *)"abc", 3, d);
    CHECK(memcmp(d, abc, 32) == 0);
}

static void test_happy_path(void)
{
    reset_platform(SLOT_MAX);
    const uint32_t n = 150001;     /* not a multiple of anything */
    make_image(n);
    CHECK(O.state == HB_OTA_STATE_IDLE);

    struct ack a = req_begin(n, IMG_SHA);
    CHECK(a.op == HB_OTA_OP_BEGIN && a.status == HB_OTA_OK);
    CHECK(a.next == 0 && a.size == n && a.req_off == 0);
    CHECK(O.state == HB_OTA_STATE_RECEIVING && F.begins == 1);
    CHECK(F.pub.state == HB_OTA_STATE_RECEIVING && F.pub.image_size == n);

    a = send_image(0, n, HB_OTA_MAX_CHUNK);
    CHECK(a.status == HB_OTA_OK && a.next == n);
    CHECK(slot_matches_image(n));
    CHECK(F.boot_sets == 0);

    a = req_simple(HB_OTA_OP_COMMIT);
    CHECK(a.op == HB_OTA_OP_COMMIT && a.status == HB_OTA_OK);
    CHECK(REBOOT);
    CHECK(F.ends == 1 && F.boot_sets == 1 && F.aborts == 0);
    CHECK(O.state == HB_OTA_STATE_COMMITTED);
    CHECK(F.handle_errors == 0 && !F.open);

    /* A retransmitted COMMIT that races the restart gets the same answer. */
    a = req_simple(HB_OTA_OP_COMMIT);
    CHECK(a.status == HB_OTA_OK && REBOOT && F.boot_sets == 1);
    /* ...and nothing can start or abort over a committed image. */
    CHECK(req_begin(n, IMG_SHA).status == HB_OTA_ERR_BUSY);
    CHECK(req_simple(HB_OTA_OP_ABORT).status == HB_OTA_ERR_BUSY);
    CHECK(F.begins == 1);
}

static void test_begin_refusals(void)
{
    make_image(5000);

    /* size 0 */
    reset_platform(SLOT_MAX);
    CHECK(req_begin(0, IMG_SHA).status == HB_OTA_ERR_BAD_SIZE);
    CHECK(O.state == HB_OTA_STATE_IDLE && F.begins == 0 && O.last_error == HB_OTA_ERR_BAD_SIZE);

    /* larger than the slot; exactly the slot is fine */
    CHECK(req_begin(SLOT_MAX + 1, IMG_SHA).status == HB_OTA_ERR_BAD_SIZE);
    CHECK(F.begins == 0);
    CHECK(req_begin(SLOT_MAX, IMG_SHA).status == HB_OTA_OK);

    /* no inactive slot */
    reset_platform(0);
    CHECK(req_begin(5000, IMG_SHA).status == HB_OTA_ERR_NO_SLOT);
    CHECK(F.begins == 0);

    /* running image not yet confirmed */
    reset_platform(SLOT_MAX);
    F.pending = true;
    struct ack a = req_begin(5000, IMG_SHA);
    CHECK(a.status == HB_OTA_ERR_RUNNING_UNCONFIRMED && a.next == 0 && a.size == 0);
    CHECK(F.begins == 0 && O.state == HB_OTA_STATE_IDLE);

    /* esp_ota_begin itself fails */
    reset_platform(SLOT_MAX);
    F.begin_rc = -1;
    CHECK(req_begin(5000, IMG_SHA).status == HB_OTA_ERR_INTERNAL);
    CHECK(O.state == HB_OTA_STATE_ERROR && !F.open);

    /* malformed: params short, param_len disagreeing with the frame */
    reset_platform(SLOT_MAX);
    uint8_t shortb[8] = { HB_OTA_OP_BEGIN, 0, 4, 0, 1, 0, 0, 0 };
    CHECK(exchange(shortb, sizeof(shortb)).status == HB_OTA_ERR_BAD_REQUEST);
    uint8_t lie[40] = { HB_OTA_OP_BEGIN, 0, 37, 0 };
    CHECK(exchange(lie, sizeof(lie)).status == HB_OTA_ERR_BAD_REQUEST);
    CHECK(F.begins == 0);
}

static void test_begin_during_transfer(void)
{
    reset_platform(SLOT_MAX);
    make_image(20000);
    CHECK(req_begin(20000, IMG_SHA).status == HB_OTA_OK);
    CHECK(send_image(0, 8000, 1000).next == 8000);

    /* A different image: refused, the transfer carries on untouched. */
    uint8_t other[32];
    memcpy(other, IMG_SHA, 32); other[31] ^= 1;
    struct ack a = req_begin(20000, other);
    CHECK(a.status == HB_OTA_ERR_BUSY && a.next == 8000 && a.size == 20000);
    CHECK(req_begin(19999, IMG_SHA).status == HB_OTA_ERR_BUSY);
    CHECK(O.state == HB_OTA_STATE_RECEIVING && O.next_offset == 8000 && F.aborts == 0);
    CHECK(req_data(8000, &IMG[8000], 1000).status == HB_OTA_OK);

    /* Refusals of an identical BEGIN must not kill the transfer either. */
    F.pending = true;
    CHECK(req_begin(20000, IMG_SHA).status == HB_OTA_ERR_RUNNING_UNCONFIRMED);
    F.pending = false;
    CHECK(O.state == HB_OTA_STATE_RECEIVING && O.next_offset == 9000 && F.aborts == 0);

    /* The identical BEGIN restarts from 0: old handle closed, new one opened. */
    a = req_begin(20000, IMG_SHA);
    CHECK(a.status == HB_OTA_OK && a.next == 0 && a.size == 20000);
    CHECK(F.aborts == 1 && F.begins == 2 && F.open);
    CHECK(req_data(9000, &IMG[9000], 1000).status == HB_OTA_ERR_BAD_OFFSET);
    CHECK(send_image(0, 20000, 777).next == 20000);
    CHECK(slot_matches_image(20000));
    CHECK(req_simple(HB_OTA_OP_COMMIT).status == HB_OTA_OK);
    CHECK(F.boot_sets == 1 && F.handle_errors == 0);
}

static void test_retransmit_and_gaps(void)
{
    reset_platform(SLOT_MAX);
    make_image(10000);
    CHECK(req_begin(10000, IMG_SHA).status == HB_OTA_OK);
    CHECK(req_data(0, IMG, 1000).status == HB_OTA_OK);
    CHECK(req_data(1000, &IMG[1000], 1000).next == 2000);
    int writes = F.writes;

    /* The last accepted chunk again: OK, same next_offset, nothing written. */
    struct ack a = req_data(1000, &IMG[1000], 1000);
    CHECK(a.status == HB_OTA_OK && a.req_off == 1000 && a.next == 2000);
    a = req_data(1000, &IMG[1000], 1000);
    CHECK(a.status == HB_OTA_OK && a.next == 2000);
    CHECK(F.writes == writes && O.retransmits == 2);

    /* Same offset, different length: not a retransmit. */
    a = req_data(1000, &IMG[1000], 999);
    CHECK(a.status == HB_OTA_ERR_BAD_OFFSET && a.req_off == 1000 && a.next == 2000);
    /* An older chunk: rewind refused. */
    CHECK(req_data(0, IMG, 1000).status == HB_OTA_ERR_BAD_OFFSET);
    /* A gap: refused, carrying the offset to resume from. */
    a = req_data(3000, &IMG[3000], 1000);
    CHECK(a.status == HB_OTA_ERR_BAD_OFFSET && a.req_off == 3000 && a.next == 2000);
    CHECK(F.writes == writes && O.state == HB_OTA_STATE_RECEIVING);
    CHECK(O.last_error == HB_OTA_ERR_BAD_OFFSET);

    /* Resync and finish: every byte written once. */
    CHECK(send_image(2000, 10000, 1000).next == 10000);
    CHECK(slot_matches_image(10000));
    /* A retransmit of the final chunk, after it was accepted. */
    CHECK(req_data(9000, &IMG[9000], 1000).status == HB_OTA_OK);
    CHECK(req_simple(HB_OTA_OP_COMMIT).status == HB_OTA_OK);
    CHECK(F.boot_sets == 1);
}

static void test_data_refusals(void)
{
    reset_platform(SLOT_MAX);
    make_image(1500);

    /* No transfer. */
    struct ack a = req_data(0, IMG, 100);
    CHECK(a.status == HB_OTA_ERR_NOT_IN_TRANSFER && a.req_off == 0 && a.next == 0);
    CHECK(F.writes == 0);

    CHECK(req_begin(1500, IMG_SHA).status == HB_OTA_OK);
    /* Empty chunk, chunk over the maximum, header-only params. */
    CHECK(req_data(0, IMG, 0).status == HB_OTA_ERR_BAD_REQUEST);
    static uint8_t big[HB_OTA_MAX_CHUNK + 1];
    big[0] = 0xE9;
    CHECK(req_data(0, big, sizeof(big)).status == HB_OTA_ERR_BAD_REQUEST);
    uint8_t hdr_only[6] = { HB_OTA_OP_DATA, 0, 2, 0, 0, 0 };
    CHECK(exchange(hdr_only, sizeof(hdr_only)).status == HB_OTA_ERR_BAD_REQUEST);
    CHECK(F.writes == 0);
    /* Exactly HB_OTA_MAX_CHUNK is accepted. */
    CHECK(req_data(0, IMG, HB_OTA_MAX_CHUNK).status == HB_OTA_OK);
    /* Running past image_size. */
    a = req_data(HB_OTA_MAX_CHUNK, &IMG[HB_OTA_MAX_CHUNK], 1500 - HB_OTA_MAX_CHUNK + 1);
    CHECK(a.status == HB_OTA_ERR_BAD_SIZE && a.next == HB_OTA_MAX_CHUNK);
    CHECK(O.state == HB_OTA_STATE_RECEIVING);

    /* Unknown op and an empty payload are answered, not ignored. */
    a = req_simple(0x7E);
    CHECK(a.op == 0x7E && a.status == HB_OTA_ERR_UNKNOWN_OP);
    size_t n = hb_ota_handle(&O, NULL, 0, RSP, sizeof(RSP), &REBOOT);
    CHECK(n == 16 && RSP[0] == 0 && RSP[1] == HB_OTA_ERR_BAD_REQUEST);
    CHECK(O.state == HB_OTA_STATE_RECEIVING);
}

static void test_bad_image_and_flash_errors(void)
{
    /* First byte is not the 0xE9 image magic: esp_ota_write refuses. */
    reset_platform(SLOT_MAX);
    make_image(4000);
    IMG[0] = 0x00;
    sha256(IMG, 4000, IMG_SHA);
    CHECK(req_begin(4000, IMG_SHA).status == HB_OTA_OK);
    struct ack a = req_data(0, IMG, 1000);
    CHECK(a.status == HB_OTA_ERR_IMAGE_INVALID && a.next == 0 && a.size == 0);
    CHECK(O.state == HB_OTA_STATE_ERROR && !F.open && F.aborts == 1);
    CHECK(req_data(0, IMG, 1000).status == HB_OTA_ERR_NOT_IN_TRANSFER);

    /* A flash write failure is terminal; a new BEGIN starts over. */
    reset_platform(SLOT_MAX);
    make_image(4000);
    F.fail_write_at = 2;
    CHECK(req_begin(4000, IMG_SHA).status == HB_OTA_OK);
    CHECK(req_data(0, IMG, 1000).status == HB_OTA_OK);
    CHECK(req_data(1000, &IMG[1000], 1000).status == HB_OTA_ERR_FLASH_WRITE);
    CHECK(O.state == HB_OTA_STATE_ERROR && O.last_error == HB_OTA_ERR_FLASH_WRITE && !F.open);
    CHECK(req_simple(HB_OTA_OP_COMMIT).status == HB_OTA_ERR_NOT_IN_TRANSFER);
    F.fail_write_at = 0;
    CHECK(req_begin(4000, IMG_SHA).status == HB_OTA_OK && O.last_error == HB_OTA_OK);
    CHECK(send_image(0, 4000, 1000).next == 4000);
    CHECK(req_simple(HB_OTA_OP_COMMIT).status == HB_OTA_OK);
    CHECK(F.boot_sets == 1 && F.handle_errors == 0);
}

static void test_commit_failures(void)
{
    /* Commit with no transfer, and before the last byte. */
    reset_platform(SLOT_MAX);
    make_image(6000);
    CHECK(req_simple(HB_OTA_OP_COMMIT).status == HB_OTA_ERR_NOT_IN_TRANSFER);
    CHECK(req_begin(6000, IMG_SHA).status == HB_OTA_OK);
    CHECK(send_image(0, 5000, 1000).next == 5000);
    struct ack a = req_simple(HB_OTA_OP_COMMIT);
    CHECK(a.status == HB_OTA_ERR_INCOMPLETE && a.next == 5000 && !REBOOT);
    CHECK(O.state == HB_OTA_STATE_RECEIVING && F.ends == 0);
    uint8_t c_params[5] = { HB_OTA_OP_COMMIT, 0, 1, 0, 0 };
    CHECK(exchange(c_params, sizeof(c_params)).status == HB_OTA_ERR_BAD_REQUEST);
    CHECK(send_image(5000, 6000, 1000).next == 6000);

    /* Digest mismatch: the bytes in flash differ from what BEGIN announced.
     * Corrupt the slot after the write — the receiver must hash flash, not
     * the bytes it was sent. */
    F.slot[4321] ^= 0x10;
    a = req_simple(HB_OTA_OP_COMMIT);
    CHECK(a.status == HB_OTA_ERR_DIGEST_MISMATCH && !REBOOT);
    CHECK(O.state == HB_OTA_STATE_ERROR && O.last_error == HB_OTA_ERR_DIGEST_MISMATCH);
    CHECK(F.boot_sets == 0 && F.ends == 0 && F.aborts == 1 && !F.open);
    CHECK(F.pub.state == HB_OTA_STATE_ERROR);

    /* The digest announced at BEGIN is wrong. */
    reset_platform(SLOT_MAX);
    make_image(6000);
    uint8_t wrong[32];
    memcpy(wrong, IMG_SHA, 32); wrong[0] ^= 0xFF;
    CHECK(req_begin(6000, wrong).status == HB_OTA_OK);
    CHECK(send_image(0, 6000, 1016).next == 6000);
    CHECK(req_simple(HB_OTA_OP_COMMIT).status == HB_OTA_ERR_DIGEST_MISMATCH);
    CHECK(F.boot_sets == 0);

    /* Read-back fails. */
    reset_platform(SLOT_MAX);
    make_image(9000);
    CHECK(req_begin(9000, IMG_SHA).status == HB_OTA_OK);
    CHECK(send_image(0, 9000, 1000).next == 9000);
    F.fail_read_at = 8500;
    CHECK(req_simple(HB_OTA_OP_COMMIT).status == HB_OTA_ERR_FLASH_READ);
    CHECK(F.boot_sets == 0 && !F.open && O.state == HB_OTA_STATE_ERROR);

    /* esp_ota_end / chip / project validation fails: handle closed by end, not
     * aborted a second time. */
    reset_platform(SLOT_MAX);
    make_image(9000);
    F.end_rc = -1;
    CHECK(req_begin(9000, IMG_SHA).status == HB_OTA_OK);
    CHECK(send_image(0, 9000, 1000).next == 9000);
    CHECK(req_simple(HB_OTA_OP_COMMIT).status == HB_OTA_ERR_IMAGE_INVALID);
    CHECK(F.ends == 1 && F.aborts == 0 && F.boot_sets == 0 && F.handle_errors == 0);

    /* Setting the boot partition fails. */
    reset_platform(SLOT_MAX);
    make_image(9000);
    F.boot_rc = -1;
    CHECK(req_begin(9000, IMG_SHA).status == HB_OTA_OK);
    CHECK(send_image(0, 9000, 1000).next == 9000);
    a = req_simple(HB_OTA_OP_COMMIT);
    CHECK(a.status == HB_OTA_ERR_BOOT_SET && !REBOOT);
    CHECK(O.state == HB_OTA_STATE_ERROR && F.handle_errors == 0);
}

static void test_abort(void)
{
    reset_platform(SLOT_MAX);
    make_image(9000);
    /* Abort when idle: always safe. */
    struct ack a = req_simple(HB_OTA_OP_ABORT);
    CHECK(a.status == HB_OTA_OK && O.state == HB_OTA_STATE_IDLE && F.aborts == 0);

    /* Mid-transfer: handle freed, nothing committed, state idle. */
    CHECK(req_begin(9000, IMG_SHA).status == HB_OTA_OK);
    CHECK(send_image(0, 4000, 1000).next == 4000);
    a = req_simple(HB_OTA_OP_ABORT);
    CHECK(a.status == HB_OTA_OK && a.next == 0 && a.size == 0);
    CHECK(O.state == HB_OTA_STATE_IDLE && O.last_error == HB_OTA_ERR_ABORTED);
    CHECK(F.aborts == 1 && !F.open && F.boot_sets == 0 && F.ends == 0);
    CHECK(req_data(4000, &IMG[4000], 1000).status == HB_OTA_ERR_NOT_IN_TRANSFER);
    /* Twice is fine. */
    CHECK(req_simple(HB_OTA_OP_ABORT).status == HB_OTA_OK && F.aborts == 1);

    /* Abort clears ERROR back to IDLE. */
    reset_platform(SLOT_MAX);
    F.begin_rc = -1;
    CHECK(req_begin(9000, IMG_SHA).status == HB_OTA_ERR_INTERNAL);
    CHECK(O.state == HB_OTA_STATE_ERROR);
    CHECK(req_simple(HB_OTA_OP_ABORT).status == HB_OTA_OK && O.state == HB_OTA_STATE_IDLE);
    uint8_t ap[5] = { HB_OTA_OP_ABORT, 0, 1, 0, 0 };
    CHECK(exchange(ap, sizeof(ap)).status == HB_OTA_ERR_BAD_REQUEST);
}

static void test_timeout(void)
{
    reset_platform(SLOT_MAX);
    make_image(9000);
    CHECK(!hb_ota_tick(&O));                       /* idle: nothing to time out */
    CHECK(req_begin(9000, IMG_SHA).status == HB_OTA_OK);
    CHECK(send_image(0, 3000, 1000).next == 3000);

    F.now += 29999;
    CHECK(!hb_ota_tick(&O) && O.state == HB_OTA_STATE_RECEIVING);
    /* Any DATA (even a refused one) is activity. */
    CHECK(req_data(5000, &IMG[5000], 1000).status == HB_OTA_ERR_BAD_OFFSET);
    F.now += 29999;
    CHECK(!hb_ota_tick(&O));
    F.now += 1;
    CHECK(hb_ota_tick(&O));
    CHECK(O.state == HB_OTA_STATE_ERROR && O.last_error == HB_OTA_ERR_TIMEOUT);
    CHECK(!F.open && F.aborts == 1 && F.boot_sets == 0);
    CHECK(F.pub.state == HB_OTA_STATE_ERROR && F.pub.last_error == HB_OTA_ERR_TIMEOUT);
    CHECK(!hb_ota_tick(&O) && F.aborts == 1);
    CHECK(req_data(3000, &IMG[3000], 1000).status == HB_OTA_ERR_NOT_IN_TRANSFER);

    /* The clock wrapping does not fire it early. */
    reset_platform(SLOT_MAX);
    F.now = 0xFFFFF000u;
    CHECK(req_begin(9000, IMG_SHA).status == HB_OTA_OK);
    F.now += 0x2000;                               /* wrapped; 8 s later */
    CHECK(!hb_ota_tick(&O) && O.state == HB_OTA_STATE_RECEIVING);

    /* Timeout 0 disables it. */
    memset(&F, 0, sizeof(F)); F.slot_size = SLOT_MAX; F.fail_read_at = UINT32_MAX;
    hb_ota_init(&O, &OPS, 0);
    CHECK(req_begin(9000, IMG_SHA).status == HB_OTA_OK);
    F.now += 3600000;
    CHECK(!hb_ota_tick(&O) && O.state == HB_OTA_STATE_RECEIVING);
}

/* ---- golden frames ---------------------------------------------------------- */

static void check_frame(const char *what, uint8_t type, const uint8_t *payload, size_t n,
                        const uint8_t *golden, size_t golden_len)
{
    uint8_t f[HB_HEADER_SIZE + HB_CODEC_MAX_PAYLOAD + HB_CRC_SIZE];
    int len = hb_codec_encode(type, 0, 0, payload, (uint16_t)n, f, sizeof(f));
    if (len != (int)golden_len || memcmp(f, golden, golden_len) != 0) {
        printf("FAIL: golden frame %s:\n  got ", what);
        for (int i = 0; i < len; i++) { printf("%02X ", f[i]); }
        printf("\n");
        fails++;
    }
}

/* Requests (host -> ESP), seq 0. BEGIN: image_size 1,448,640 (0x00161AC0),
 * sha256 = 00 01 .. 1F. DATA: offset 0, bytes E9 06 02 20. */
static const uint8_t G_BEGIN[] = {
    0x55,0xAA,0x80,0x00,0x28,0x00,0x00,0x00, 0x01,0x00,0x24,0x00, 0xC0,0x1A,0x16,0x00,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
    0xCE,0xA8 };
static const uint8_t G_DATA[] = {
    0x55,0xAA,0x80,0x00,0x0C,0x00,0x00,0x00, 0x02,0x00,0x08,0x00, 0x00,0x00,0x00,0x00,
    0xE9,0x06,0x02,0x20, 0x1B,0xC3 };
static const uint8_t G_COMMIT[] = { 0x55,0xAA,0x80,0x00,0x04,0x00,0x00,0x00, 0x03,0x00,0x00,0x00, 0x62,0x0D };
static const uint8_t G_ABORT[]  = { 0x55,0xAA,0x80,0x00,0x04,0x00,0x00,0x00, 0x04,0x00,0x00,0x00, 0x43,0x5A };
static const uint8_t G_STATUS[] = { 0x55,0xAA,0x80,0x00,0x04,0x00,0x00,0x00, 0x05,0x00,0x00,0x00, 0xF8,0x46 };

/* Replies (ESP -> host), seq 0, for that BEGIN and DATA. */
static const uint8_t G_BEGIN_OK[] = {
    0x55,0xAA,0x81,0x00,0x10,0x00,0x00,0x00, 0x01,0x00,0x0C,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC0,0x1A,0x16,0x00, 0x12,0x9C };
static const uint8_t G_DATA_OK[] = {
    0x55,0xAA,0x81,0x00,0x10,0x00,0x00,0x00, 0x02,0x00,0x0C,0x00,
    0x00,0x00,0x00,0x00, 0x04,0x00,0x00,0x00, 0xC0,0x1A,0x16,0x00, 0xED,0x10 };
/* DATA at offset 0x1000 when 4 is expected. */
static const uint8_t G_DATA_BAD_OFFSET[] = {
    0x55,0xAA,0x81,0x00,0x10,0x00,0x00,0x00, 0x02,0x03,0x0C,0x00,
    0x00,0x10,0x00,0x00, 0x04,0x00,0x00,0x00, 0xC0,0x1A,0x16,0x00, 0xD6,0xB9 };
/* COMMIT while 4 of 0x161AC0 bytes have arrived. */
static const uint8_t G_COMMIT_INCOMPLETE[] = {
    0x55,0xAA,0x81,0x00,0x10,0x00,0x00,0x00, 0x03,0x09,0x0C,0x00,
    0x00,0x00,0x00,0x00, 0x04,0x00,0x00,0x00, 0xC0,0x1A,0x16,0x00, 0xA5,0x87 };
static const uint8_t G_ABORT_OK[] = {
    0x55,0xAA,0x81,0x00,0x10,0x00,0x00,0x00, 0x04,0x00,0x0C,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x17,0x36 };
/* COMMIT accepted, for an image of 9000 (0x2328) bytes. */
static const uint8_t G_COMMIT_OK[] = {
    0x55,0xAA,0x81,0x00,0x10,0x00,0x00,0x00, 0x03,0x00,0x0C,0x00,
    0x00,0x00,0x00,0x00, 0x28,0x23,0x00,0x00, 0x28,0x23,0x00,0x00, 0x21,0x52 };
/* STATUS: receiving, 4096 of 1,448,640 B, slot 2,031,616 B (0x1F0000); running
 * ota_0 VALID, ota_1 UNKNOWN (its otadata entry was cleared by BEGIN), "1.1.0". */
static const uint8_t G_STATUS_RSP[] = {
    0x55,0xAA,0x81,0x00,0x38,0x00,0x00,0x00, 0x05,0x00,0x34,0x00,
    0x01,0x01,0x00,0x00,0x00,0xFF,0xF8,0x03, 0x00,0x10,0x00,0x00, 0xC0,0x1A,0x16,0x00,
    0x00,0x00,0x1F,0x00, '1','.','1','.','0',0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0x52,0x84 };

static void test_golden(void)
{
    /* Requests: build them the way an M7 would, and check the frame bytes. */
    uint8_t begin[40] = { HB_OTA_OP_BEGIN, 0, 36, 0, 0xC0, 0x1A, 0x16, 0x00 };
    for (int i = 0; i < 32; i++) { begin[8 + i] = (uint8_t)i; }
    check_frame("BEGIN", HB_TYPE_OTA_REQ, begin, sizeof(begin), G_BEGIN, sizeof(G_BEGIN));
    uint8_t data[12] = { HB_OTA_OP_DATA, 0, 8, 0, 0, 0, 0, 0, 0xE9, 0x06, 0x02, 0x20 };
    check_frame("DATA", HB_TYPE_OTA_REQ, data, sizeof(data), G_DATA, sizeof(G_DATA));
    uint8_t c[4] = { HB_OTA_OP_COMMIT, 0, 0, 0 }, a[4] = { HB_OTA_OP_ABORT, 0, 0, 0 },
            s[4] = { HB_OTA_OP_STATUS, 0, 0, 0 };
    check_frame("COMMIT", HB_TYPE_OTA_REQ, c, 4, G_COMMIT, sizeof(G_COMMIT));
    check_frame("ABORT", HB_TYPE_OTA_REQ, a, 4, G_ABORT, sizeof(G_ABORT));
    check_frame("STATUS", HB_TYPE_OTA_REQ, s, 4, G_STATUS, sizeof(G_STATUS));

    /* Replies: produced by the receiver from those requests. */
    reset_platform(SLOT_MAX);
    F.slot_size = 0x1F0000;          /* the real slot size; only 4 bytes get written */
    size_t n = hb_ota_handle(&O, begin, sizeof(begin), RSP, sizeof(RSP), &REBOOT);
    check_frame("BEGIN reply", HB_TYPE_OTA_RSP, RSP, n, G_BEGIN_OK, sizeof(G_BEGIN_OK));
    n = hb_ota_handle(&O, data, sizeof(data), RSP, sizeof(RSP), &REBOOT);
    check_frame("DATA reply", HB_TYPE_OTA_RSP, RSP, n, G_DATA_OK, sizeof(G_DATA_OK));
    uint8_t gap[12] = { HB_OTA_OP_DATA, 0, 8, 0, 0x00, 0x10, 0, 0, 1, 2, 3, 4 };
    n = hb_ota_handle(&O, gap, sizeof(gap), RSP, sizeof(RSP), &REBOOT);
    check_frame("DATA bad offset", HB_TYPE_OTA_RSP, RSP, n, G_DATA_BAD_OFFSET, sizeof(G_DATA_BAD_OFFSET));
    n = hb_ota_handle(&O, c, 4, RSP, sizeof(RSP), &REBOOT);
    check_frame("COMMIT incomplete", HB_TYPE_OTA_RSP, RSP, n, G_COMMIT_INCOMPLETE, sizeof(G_COMMIT_INCOMPLETE));

    struct hb_ota_snapshot snap = { .state = HB_OTA_STATE_RECEIVING, .last_error = 0,
                                    .bytes_received = 4096, .image_size = 0x161AC0,
                                    .slot_size = 0x1F0000 };
    struct hb_ota_running run = { .slot = 0, .img_state = HB_OTA_IMG_VALID,
                                  .other_img_state = HB_OTA_IMG_UNKNOWN, .version = "1.1.0" };
    n = hb_ota_status_reply(&snap, &run, s, 4, RSP, sizeof(RSP));
    CHECK(n == 56);
    check_frame("STATUS reply", HB_TYPE_OTA_RSP, RSP, n, G_STATUS_RSP, sizeof(G_STATUS_RSP));
    /* A malformed STATUS still carries the 52 bytes, flagged BAD_REQUEST. */
    uint8_t s_bad[5] = { HB_OTA_OP_STATUS, 0, 1, 0, 0 };
    n = hb_ota_status_reply(&snap, &run, s_bad, 5, RSP, sizeof(RSP));
    CHECK(n == 56 && RSP[1] == HB_OTA_ERR_BAD_REQUEST && RSP[4] == HB_OTA_PROTO_VERSION);
    /* A version string of 32+ characters is truncated to 31 and NUL-terminated. */
    memset(run.version, 'x', sizeof(run.version));
    n = hb_ota_status_reply(&snap, &run, s, 4, RSP, sizeof(RSP));
    CHECK(RSP[4 + 20 + 30] == 'x' && RSP[4 + 20 + 31] == 0);

    n = hb_ota_handle(&O, a, 4, RSP, sizeof(RSP), &REBOOT);
    check_frame("ABORT reply", HB_TYPE_OTA_RSP, RSP, n, G_ABORT_OK, sizeof(G_ABORT_OK));

    reset_platform(SLOT_MAX);
    make_image(9000);
    CHECK(req_begin(9000, IMG_SHA).status == HB_OTA_OK);
    CHECK(send_image(0, 9000, 1000).next == 9000);
    n = hb_ota_handle(&O, c, 4, RSP, sizeof(RSP), &REBOOT);
    check_frame("COMMIT reply", HB_TYPE_OTA_RSP, RSP, n, G_COMMIT_OK, sizeof(G_COMMIT_OK));

    /* The reply never outgrows the buffer the platform gives it. */
    CHECK(hb_ota_handle(&O, c, 4, RSP, 15, &REBOOT) == 0);
}

int main(void)
{
    test_sha_reference();
    test_happy_path();
    test_begin_refusals();
    test_begin_during_transfer();
    test_retransmit_and_gaps();
    test_data_refusals();
    test_bad_image_and_flash_errors();
    test_commit_failures();
    test_abort();
    test_timeout();
    test_golden();

    if (fails) {
        printf("test_hb_ota: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_hb_ota: all passed\n");
    return 0;
}
