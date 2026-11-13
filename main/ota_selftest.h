/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — post-update self-test for bootloader app rollback (HealthyPi 6).
 *
 * After an OTA update the bootloader starts the new image in PENDING_VERIFY.
 * The image keeps its slot only if it proves the HealthyBridge link to the host
 * works, because that link is the path every later update arrives on. See
 * ota_selftest.c for the policy and its edge cases.
 *
 * Compiled to no-ops unless CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is set, so a
 * build without rollback (HealthyPi 5) carries none of it.
 */
#ifndef OTA_SELFTEST_H
#define OTA_SELFTEST_H

#include "sdkconfig.h"

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)

/* Read the running slot's OTA state and log it. Call once, after hb_link_init(). */
void ota_selftest_init(void);

/* Advance the self-test; call from the 1 Hz service loop. Marks the image valid
 * once the link is proven, or rolls back after the timeout. */
void ota_selftest_tick(void);

#else

static inline void ota_selftest_init(void) {}
static inline void ota_selftest_tick(void) {}

#endif

#endif /* OTA_SELFTEST_H */
