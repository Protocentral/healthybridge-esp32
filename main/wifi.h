/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — Wi-Fi (STA) management.
 */
#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Init netif/event/wifi (coexists with NimBLE) and, if creds are present,
 * connect in STA mode. Otherwise stays idle until provisioned. */
void wifi_init(void);

/* (Re)connect STA using the current cfg credentials. Re-arms the retry budgets. */
void wifi_start_sta(void);

/* Bring up the SoftAP captive portal for on-device provisioning, at a human's
 * (or the host MCU's) explicit request. Such a portal is *sticky* — it stays up
 * until provisioning completes, unlike one opened automatically after failures. */
void wifi_start_provisioning(void);

/* Wi-Fi scheduler; call once per second from the main loop. Performs the backoff
 * reconnect, opens a deferred portal, and times an unused automatic portal out
 * back to STA. All esp_wifi mode changes happen here, never in the event loop. */
void wifi_tick(void);

/* Disconnect/stop STA. */
void wifi_stop(void);

bool wifi_is_connected(void);
bool wifi_is_ap_mode(void);
void wifi_get_ip(char *buf, size_t n);   /* "0.0.0.0" if not connected */

/* Link details for a host status report (HP6 GET_STATUS). Each yields the
 * "unknown" value when it does not apply, never a stale one. */
bool   wifi_is_sta_active(void);         /* STA up and auto-reconnect wanted */
int8_t wifi_get_rssi(void);              /* dBm; 0 when not connected */
void   wifi_get_ip4(uint8_t out[4]);     /* a.b.c.d; 0.0.0.0 when no lease */
void   wifi_get_ssid(char *buf, size_t n); /* "" when not connected */
void   wifi_get_ap_name(char *buf, size_t n); /* SoftAP SSID; "" before the AP is up */

#endif /* WIFI_H */
