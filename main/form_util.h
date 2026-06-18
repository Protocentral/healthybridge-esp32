/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge Lite ESP32-C3 — x-www-form-urlencoded parsing helpers.
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

#endif /* FORM_UTIL_H */
