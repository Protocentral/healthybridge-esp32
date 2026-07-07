/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — Wi-Fi (STA) management.
 */
#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stddef.h>

/* Init netif/event/wifi (coexists with NimBLE) and, if creds are present,
 * connect in STA mode. Otherwise stays idle until provisioned. */
void wifi_init(void);

/* (Re)connect STA using the current cfg credentials. */
void wifi_start_sta(void);

/* Bring up the SoftAP captive portal (E4b) for on-device provisioning. */
void wifi_start_provisioning(void);

/* Service deferred Wi-Fi work; call once per second from the main loop. Opens
 * the SoftAP portal if STA failed to connect WIFI_STA_MAX_FAIL times in a row. */
void wifi_tick(void);

/* Disconnect/stop STA. */
void wifi_stop(void);

bool wifi_is_connected(void);
bool wifi_is_ap_mode(void);
void wifi_get_ip(char *buf, size_t n);   /* "0.0.0.0" if not connected */

#endif /* WIFI_H */
