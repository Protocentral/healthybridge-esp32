/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge Lite ESP32-C3 — local web dashboard (E4d).
 *
 * Optional, toggled by cfg->dashboard_enabled. When enabled and the STA link is
 * up, serves a live vitals page + settings form on the device's STA IP (port
 * 80). Mutually exclusive with the SoftAP captive portal (provisioning), which
 * only runs in AP mode. Driven from the 1 Hz main loop via dashboard_tick().
 *
 * First cut is vitals-only (JSON poll); waveform streaming is deferred pending
 * data_store ring buffers.
 */
#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <stdbool.h>

void dashboard_init(void);

/* Call once per second. Starts the HTTP server when the dashboard is enabled
 * and Wi-Fi is connected, stops it otherwise. No-op when disabled. */
void dashboard_tick(void);

bool dashboard_is_running(void);

#endif /* DASHBOARD_H */
