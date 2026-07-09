/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — SoftAP captive-portal provisioning.
 *
 * Brings up a SoftAP ("HealthyPi-XXXX"), a captive DNS responder (every A
 * query → the AP gateway, so the phone pops the portal), and an HTTP server
 * serving a settings form. On submit the form writes Wi-Fi creds + telemetry
 * toggles to NVS (cfg) and reboots into STA mode.
 *
 * Started by wifi_start_provisioning() (which owns the SoftAP/netif side);
 * this module owns the DNS + HTTP servers only.
 */
#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <stdbool.h>

/* Start the captive DNS + HTTP servers. Idempotent. The caller must already
 * have the SoftAP up (esp_wifi in AP/APSTA mode, started). */
void provisioning_start(void);

/* Stop both servers. Idempotent. */
void provisioning_stop(void);

#endif /* PROVISIONING_H */
