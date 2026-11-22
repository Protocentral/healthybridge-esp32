/*
 * SPDX-License-Identifier: MIT
 * Host test for form_flag(): the on/off reading of a posted form.
 *
 * The bug this exists to prevent: the captive portal's switches post hidden
 * inputs that are ALWAYS present, and the save handler read "present" as "on",
 * so MQTT and the dashboard saved as enabled no matter what the user chose.
 */
#include <stdio.h>
#include "form_util.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void)
{
    /* Portal: hidden inputs, "1" / "0" (and "" from a page cached before this fix). */
    CHECK( form_flag("ssid=x&mqtt_en=1&dash_en=0", "mqtt_en"));
    CHECK(!form_flag("ssid=x&mqtt_en=1&dash_en=0", "dash_en"));
    CHECK(!form_flag("ssid=x&mqtt_en=&dash_en=", "mqtt_en"));
    CHECK(!form_flag("ssid=x&mqtt_en=&dash_en=", "dash_en"));

    /* Dashboard: a checkbox, absent when off and "on" when ticked. */
    CHECK( form_flag("mqtt_en=on&mqtt_uri=&dash_en=on", "mqtt_en"));
    CHECK(!form_flag("mqtt_uri=&dash_en=on", "mqtt_en"));
    CHECK( form_flag("mqtt_uri=&dash_en=on", "dash_en"));

    /* A key that is a prefix of another is not that key. */
    CHECK(!form_flag("mqtt_enx=1", "mqtt_en"));
    CHECK(!form_flag("", "mqtt_en"));

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("form_util: all passed\n");
    return 0;
}
