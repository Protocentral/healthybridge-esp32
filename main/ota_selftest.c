/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
 *
 * HealthyBridge — post-update self-test for bootloader app rollback.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, an image installed by OTA boots
 * in ESP_OTA_IMG_PENDING_VERIFY. If it resets in that state for any reason
 * (panic, watchdog, power loss, the host pulling the reset line), the
 * bootloader marks it ABORTED and boots the previous slot instead. The image
 * keeps its slot only by calling esp_ota_mark_app_valid_cancel_rollback().
 *
 * What counts as working
 * ----------------------
 * On HealthyPi 6 this C6 is updated through the M7 over the HealthyBridge UART
 * link, so an image that cannot hold that link cannot be updated again. The
 * test is therefore the link, and only the link: Wi-Fi being up proves nothing
 * about the update path, and on HealthyPi 6 the radios are off at boot anyway.
 *
 * "Link proven" = CONFIG_HB_OTA_SELFTEST_FRAMES CRC-valid frames received from
 * the host, of the types the M7 actually sends (vitals, waveforms, control
 * commands, status polls, firmware-update requests). Unknown types do not
 * count. What the HealthyPi 6 M7 actually puts on the wire:
 *
 *  - Vitals frames, each time the M7 publishes vitals, even with the radios
 *    off. They exist only while the M4 (the source of vitals) is producing.
 *    Waveform frames are sent only while a radio is on.
 *  - Firmware-update STATUS requests (0x80), about 1 Hz while the M7's update
 *    service is watching a newly installed image boot and verify, and about
 *    every 2 s while it sees a PENDING_VERIFY image outside an update session.
 *  - A keepalive GET_STATUS about every 5 s when the link is up and the M7 has
 *    sent nothing else (healthypi-6-fw PR #23, CONN_KEEPALIVE_IDLE_MS).
 *
 * The M7's 2 s connectivity status poll does NOT reliably reach the wire: it
 * is mostly answered from the M7 driver's cache of this C6's own 1 Hz
 * unsolicited status push (0x61). Do not count on it. With the radios off and
 * the M4 silent, the keepalive is what proves the link (~11 frames in 60 s);
 * an M7 without it may send nothing at all.
 *
 * What happens when the link never comes up
 * -----------------------------------------
 *  - Image is PENDING_VERIFY and another slot holds a valid image:
 *    after CONFIG_HB_OTA_SELFTEST_TIMEOUT_S the image marks itself invalid and
 *    reboots into the previous slot. That restores a firmware that held the
 *    link, i.e. the update path. (A reset before the timeout has the same
 *    effect via the bootloader.) A timeout of 0 disables the active rollback;
 *    the image then rolls back only on its next reset.
 *  - Image is PENDING_VERIFY but no other slot is valid: rolling back would
 *    leave the bootloader nothing to boot, so the image marks itself valid and
 *    keeps running, with an error in the log. It never rolls back into nothing.
 *  - Image was flashed over USB (esptool / idf.py / flash_hp6.sh): it is never
 *    PENDING_VERIFY. Flashing writes a blank otadata; with no factory partition
 *    the bootloader then boots ota_0 and itself records it as VALID
 *    (set_actual_ota_seq()). There is nothing to roll back to and nothing to
 *    do: the test only logs, and a C6 running standalone on the bench (M7 not
 *    talking, or no M7 at all) keeps running.
 *  - The M7 holds the C6 in reset: the image never runs, so nothing here
 *    executes. If that happens while an update is PENDING_VERIFY, the next
 *    release from reset rolls it back — the updater must keep the C6 running
 *    until the image reports itself valid.
 */
#include "ota_selftest.h"

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "hb_link.h"

static const char *TAG = "ota_test";

static bool     s_pending;      /* running image is PENDING_VERIFY and undecided */
static uint32_t s_frames_at_start;

static const char *state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
    default:                         return "?";
    }
}

void ota_selftest_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    esp_err_t err = esp_ota_get_state_partition(run, &st);

    if (err != ESP_OK) {
        /* No otadata entry for this slot. Not expected after the bootloader has
         * run (it records a USB-flashed ota_0 as VALID), but harmless: there is
         * no pending verification to resolve. */
        ESP_LOGI(TAG, "running %s @ 0x%lx: no OTA state (%s) — nothing to verify",
                 run->label, (unsigned long)run->address, esp_err_to_name(err));
        return;
    }

    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "running %s @ 0x%lx: %s — no rollback pending",
                 run->label, (unsigned long)run->address, state_name(st));
        return;
    }

    s_pending = true;
    s_frames_at_start = hb_link_host_frames();
    ESP_LOGW(TAG, "running %s @ 0x%lx: new image, PENDING_VERIFY — waiting for "
                  "%d valid frames from the host (timeout %d s)",
             run->label, (unsigned long)run->address,
             CONFIG_HB_OTA_SELFTEST_FRAMES, CONFIG_HB_OTA_SELFTEST_TIMEOUT_S);
}

void ota_selftest_tick(void)
{
    if (!s_pending) {
        return;
    }

    uint32_t frames = hb_link_host_frames() - s_frames_at_start;
    if (frames >= (uint32_t)CONFIG_HB_OTA_SELFTEST_FRAMES) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HealthyBridge link proven (%lu frames): image marked "
                          "VALID, rollback cancelled", (unsigned long)frames);
        } else {
            ESP_LOGE(TAG, "mark valid failed: %s — the next reset will roll back",
                     esp_err_to_name(err));
        }
        s_pending = false;
        return;
    }

#if CONFIG_HB_OTA_SELFTEST_TIMEOUT_S > 0
    int64_t up_s = esp_timer_get_time() / 1000000;
    if (up_s < CONFIG_HB_OTA_SELFTEST_TIMEOUT_S) {
        return;
    }

    s_pending = false;
    if (!esp_ota_check_rollback_is_possible()) {
        /* Nothing valid in the other slot: rolling back would leave the
         * bootloader without a bootable image. Keep this one. */
        ESP_LOGE(TAG, "no host frames after %d s, but no other valid slot to roll "
                      "back to — keeping this image (marking it VALID)",
                 CONFIG_HB_OTA_SELFTEST_TIMEOUT_S);
        esp_ota_mark_app_valid_cancel_rollback();
        return;
    }

    ESP_LOGE(TAG, "no host frames after %d s (got %lu): marking image INVALID and "
                  "rolling back", CONFIG_HB_OTA_SELFTEST_TIMEOUT_S, (unsigned long)frames);
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Only returns on failure. */
    ESP_LOGE(TAG, "rollback failed: %s — keeping this image", esp_err_to_name(err));
#endif
}

#endif /* CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE */
