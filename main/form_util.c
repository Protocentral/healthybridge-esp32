/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge Lite ESP32-C3 — form-urlencoded parsing helpers.
 */
#include <string.h>
#include "form_util.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL-decode src into dst (NUL-terminated). '+' -> space, %XX -> byte. */
static void urldecode(char *dst, size_t dstlen, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dstlen; i++) {
        if (src[i] == '+') {
            dst[o++] = ' ';
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            int hi = hexval(src[i + 1]), lo = hexval(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[o++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[o++] = src[i];
            }
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

bool form_field(const char *body, const char *key, char *out, size_t outlen)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
        if (seglen >= klen && p[klen] == '=' && strncmp(p, key, klen) == 0) {
            char raw[160];
            size_t vlen = seglen - klen - 1;
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, p + klen + 1, vlen);
            raw[vlen] = '\0';
            urldecode(out, outlen, raw);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    out[0] = '\0';
    return false;
}
