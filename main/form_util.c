/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — form parsing / HTML escaping helpers.
 */
#include <stdio.h>
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

void html_escape(char *dst, size_t dstlen, const char *src)
{
    if (dstlen == 0) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; src[i]; i++) {
        const char *ent;
        switch (src[i]) {
        case '&':  ent = "&amp;";  break;
        case '<':  ent = "&lt;";   break;
        case '>':  ent = "&gt;";   break;
        case '"':  ent = "&quot;"; break;
        case '\'': ent = "&#39;";  break;
        default:   ent = NULL;     break;
        }
        if (ent) {
            size_t elen = strlen(ent);
            if (o + elen >= dstlen) break;   /* don't split an entity */
            memcpy(dst + o, ent, elen);
            o += elen;
        } else {
            if (o + 1 >= dstlen) break;
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

void json_escape(char *dst, size_t dstlen, const char *src)
{
    if (dstlen == 0) {
        return;
    }
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        /* Longest expansion is \uXXXX = 6 bytes; bail before it would not fit
         * so the result is never truncated part-way through an escape. */
        const char *esc = NULL;
        char ubuf[7];
        size_t need;

        switch (*p) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (*p < 0x20) {
                snprintf(ubuf, sizeof(ubuf), "\\u%04X", *p);
                esc = ubuf;
            }
            break;
        }
        need = esc ? strlen(esc) : 1;
        if (o + need >= dstlen) {
            break;
        }
        if (esc) {
            memcpy(dst + o, esc, need);
        } else {
            dst[o] = (char)*p;
        }
        o += need;
    }
    dst[o] = '\0';
}
