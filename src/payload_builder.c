/*
 * src/payload_builder.c
 *
 * Gathers state from ZMK subsystems and packs it into the 19-byte
 * manufacturer payload defined in payload_builder.h.
 *
 * All ZMK API calls are wrapped in IS_ENABLED() guards so the module
 * compiles correctly regardless of which ZMK features are active.
 */

#include "payload_builder.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/keymap.h>
#include <zmk/battery.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/wpm.h>
#endif

LOG_MODULE_DECLARE(ble_advertiser, LOG_LEVEL_DBG);

/* ── Keyboard ID — FNV-1a hash of keyboard name ─────────── */

/* Pre-computed at init time; immutable after that. */
static uint8_t keyboard_id[6];

/**
 * FNV-1a 32-bit hash over a NUL-terminated string.
 * Running two independent passes gives us 6 bytes without extra libs.
 */
static uint32_t fnv1a32(const char *str, uint32_t seed)
{
    uint32_t h = seed;

    for (; *str; str++) {
        h ^= (uint8_t)*str;
        h *= 16777619u; /* FNV prime */
    }
    return h;
}

void payload_builder_init(void)
{
    const char *name = CONFIG_ZMK_KEYBOARD_NAME;
    uint32_t h1 = fnv1a32(name, 2166136261u);           /* FNV offset basis */
    uint32_t h2 = fnv1a32(name, h1 ^ 0xA5A5A5A5u);     /* second pass */

    keyboard_id[0] = (uint8_t)(h1 >> 24);
    keyboard_id[1] = (uint8_t)(h1 >> 16);
    keyboard_id[2] = (uint8_t)(h1 >>  8);
    keyboard_id[3] = (uint8_t)(h1);
    keyboard_id[4] = (uint8_t)(h2 >>  8);
    keyboard_id[5] = (uint8_t)(h2);

    LOG_INF("Keyboard ID: %02X%02X%02X%02X%02X%02X",
            keyboard_id[0], keyboard_id[1], keyboard_id[2],
            keyboard_id[3], keyboard_id[4], keyboard_id[5]);
}

/* ── Peripheral battery (set from event, read from build) ── */

/* 0xFF = no peripheral connected. Updated by ble_advertiser.c. */
static uint8_t periph_batt = 0xFFu;

void payload_set_periph_batt(uint8_t level)
{
    periph_batt = level;
}

/* ── Main payload builder ────────────────────────────────── */

void payload_build(uint8_t out[PAYLOAD_LEN])
{
    memset(out, 0, PAYLOAD_LEN);

    /* Bytes 0-5: keyboard identity hash */
    memcpy(&out[0], keyboard_id, 6u);

    /* Byte 6: central battery (0-100 %) */
    out[6] = (uint8_t)zmk_battery_state_of_charge();

    /* Byte 7: active BLE profile slot */
#if IS_ENABLED(CONFIG_ZMK_BLE)
    out[7] = (uint8_t)zmk_ble_active_profile_index();
#else
    out[7] = 0u;
#endif

    /* Byte 8: charging (proxy: USB power present) */
#if IS_ENABLED(CONFIG_ZMK_USB)
    out[8] = zmk_usb_is_powered() ? 1u : 0u;
#else
    out[8] = 0u;
#endif

    /* Byte 9: USB bus connected */
#if IS_ENABLED(CONFIG_ZMK_USB)
    out[9] = (zmk_usb_get_conn_state() == ZMK_USB_CONN_HID) ? 1u : 0u;
#else
    out[9] = 0u;
#endif

    /* Byte 10: BLE profile connected */
#if IS_ENABLED(CONFIG_ZMK_BLE)
    out[10] = zmk_ble_active_profile_is_connected() ? 1u : 0u;
#else
    out[10] = 0u;
#endif

    /* Byte 11: peripheral battery (0xFF = no peripheral) */
    out[11] = periph_batt;

    /* Bytes 12-17: active layer name (ASCII, zero-padded) */
    uint8_t active_layer = zmk_keymap_highest_layer_active();
    const char *label    = zmk_keymap_layer_name(active_layer);

    if (label != NULL && label[0] != '\0') {
        size_t len = strlen(label);

        if (len > 6u) {
            len = 6u;
        }
        memcpy(&out[12], label, len);
    } else {
        /* No label — encode layer index as decimal string ("L3", etc.) */
        out[12] = 'L';
        out[13] = '0' + (active_layer % 10u);
    }

    /* Byte 18: words per minute */
#if IS_ENABLED(CONFIG_ZMK_WPM)
    out[18] = (uint8_t)zmk_wpm_get_state();
#else
    out[18] = 0u;
#endif
}
