/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — MQTT vitals publisher (E4c).
 *
 * Optional, toggled by cfg->mqtt_enabled. When enabled and the STA link is up,
 * connects to cfg->mqtt_uri and publishes vitals/status JSON. All lifecycle is
 * driven from the 1 Hz main loop via mqtt_pub_tick(); no work happens when the
 * feature is disabled.
 */
#ifndef MQTT_PUB_H
#define MQTT_PUB_H

#include <stdbool.h>
#include "healthybridge.h"

void mqtt_pub_init(void);

/* Call once per second. Lazily (re)starts the client when mqtt is enabled and
 * Wi-Fi is connected, stops it when either condition drops, and publishes the
 * latest vitals when connected. No-op when mqtt is disabled. */
void mqtt_pub_tick(const struct hb_vitals_payload *v);

bool mqtt_pub_is_connected(void);

#endif /* MQTT_PUB_H */
