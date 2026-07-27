/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — form parsing / HTML escaping helpers.
 * Shared by the captive portal (provisioning) and the web dashboard.
 */
#ifndef FORM_UTIL_H
#define FORM_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/* Extract `key`'s value from a form-urlencoded body into out (URL-decoded,
 * NUL-terminated). Returns true if the key was present (checkbox semantics:
 * a present key with no value still returns true). */
bool form_field(const char *body, const char *key, char *out, size_t outlen);

/* Escape src for interpolation into HTML text or a quoted attribute value,
 * writing a NUL-terminated result to dst. Escapes & < > " ' — enough for both
 * element content and single/double-quoted attributes. Output is truncated
 * (never split mid-entity) rather than overflowing dst. */
void html_escape(char *dst, size_t dstlen, const char *src);

/* Escape src for use inside a double-quoted JSON string, writing a
 * NUL-terminated result to dst. Escapes \ and " and control characters below
 * 0x20; bytes >= 0x80 pass through, so a UTF-8 SSID survives intact. Output is
 * truncated (never split mid-escape) rather than overflowing dst. */
void json_escape(char *dst, size_t dstlen, const char *src);

#endif /* FORM_UTIL_H */
