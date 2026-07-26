/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — product identity strings, selected by the build profile.
 *
 * The user-visible name of the host product: BLE advertised name, mDNS instance
 * name, dashboard title/header, provisioning portal heading, MQTT topic root.
 * One definition so a new target cannot half-rename itself — before this, an
 * HP6 unit advertised and served as "HealthyPi 5".
 *
 * Three forms because the call sites are not interchangeable:
 *   HB_PRODUCT_NAME       plain text
 *   HB_PRODUCT_NAME_HTML  same, with a non-breaking space (dashboard header)
 *   HB_PRODUCT_SLUG       lowercase, no space (MQTT topic root)
 *
 * The HP5 values are frozen: they are what the released firmware advertises,
 * what the phone app scans for, and what existing MQTT subscribers match on.
 *
 * NOT product-scoped, deliberately:
 *   - the mDNS hostname stays "healthypi" (-> healthypi.local) on both targets;
 *     the feature freeze lists it for both and client tools key off it.
 *   - the SoftAP SSID prefix stays "HealthyPi-" for both; it already carries a
 *     MAC suffix, so two devices are still distinguishable.
 */
#ifndef HB_PRODUCT_H
#define HB_PRODUCT_H

#include "sdkconfig.h"

#if defined(CONFIG_HB_PROFILE_HP6)

#define HB_PRODUCT_NAME       "HealthyPi 6"
#define HB_PRODUCT_NAME_HTML  "HealthyPi&nbsp;6"
#define HB_PRODUCT_SLUG       "healthypi6"

#else  /* HealthyPi 5 — released; these strings are frozen. */

#define HB_PRODUCT_NAME       "HealthyPi 5"
#define HB_PRODUCT_NAME_HTML  "HealthyPi&nbsp;5"
#define HB_PRODUCT_SLUG       "healthypi5"

#endif

#endif /* HB_PRODUCT_H */
