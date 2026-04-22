/*
 * src/payload_builder.c
 *
 * Builds the 23-byte keyboard status payload.
 * Memory: all static. Zero heap allocation. Zero leaks.
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

/* ── Keyboard ID (FNV-1a hash of name, computed once) ─────── */

static uint8_t keyboard_id[6];

static uint32_t fnv1a32(const char *str, uint32_t seed)
{
    uint32_t h = seed;
    for (; *str; str++) {
        h ^= (uint8_t)*str;
        h *= 16777619u;
    }
    return h;
}

void payload_builder_init(void)
{
    const char *name = CONFIG_ZMK_KEYBOARD_NAME;
    uint32_t h1 = fnv1a32(name, 2166136261u);
    uint32_t h2 = fnv1a32(name, h1 ^ 0xA5A5A5A5u);

    keyboard_id[0] = (uint8_t)(h1 >> 24);
    keyboard_id[1] = (uint8_t)(h1 >> 16);
    keyboard_id[2] = (uint8_t)(h1 >>  8);
    keyboard_id[3] = (uint8_t)(h1);
    keyboard_id[4] = (uint8_t)(h2 >>  8);
    keyboard_id[5] = (uint8_t)(h2);

    LOG_INF("Keyboard ID: %02X%02X%02X%02X%02X%02X",
            keyboard_id[0], keyboard_id[1], keyboard_id[2],
            keyboard_id[3], keyboard_id[4], keyboard_id[5]);
    LOG_INF("Pairing code: \"%.4s\"", CONFIG_BLE_ADVERTISER_PAIRING_CODE);
}

/* ── Peripheral battery ───────────────────────────────────── */

static uint8_t periph_batt = 0xFFu;  /* 0xFF = no peripheral */

void payload_set_periph_batt(uint8_t level)
{
    periph_batt = level;
}

/* ── Main builder ─────────────────────────────────────────── */

void payload_build(uint8_t out[PAYLOAD_LEN])
{
    memset(out, 0, PAYLOAD_LEN);

    /* Bytes 0-3: pairing code (4 ASCII bytes, NOT NUL-terminated on wire) */
    const char *code = CONFIG_BLE_ADVERTISER_PAIRING_CODE;
    for (uint8_t i = 0u; i < 4u; i++) {
        out[i] = (code[i] != '\0') ? (uint8_t)code[i] : ' ';
    }

    /* Bytes 4-9: keyboard identity hash */
    memcpy(&out[4], keyboard_id, 6u);

    /* Byte 10: central (right half) battery */
    out[10] = (uint8_t)zmk_battery_state_of_charge();

    /* Byte 11: active BLE profile slot */
#if IS_ENABLED(CONFIG_ZMK_BLE)
    out[11] = (uint8_t)zmk_ble_active_profile_index();
#endif

    /* Byte 12: USB power present (proxy for charging) */
#if IS_ENABLED(CONFIG_ZMK_USB)
    out[12] = zmk_usb_is_powered() ? 1u : 0u;
#endif

    /* Byte 13: USB HID connected */
#if IS_ENABLED(CONFIG_ZMK_USB)
    out[13] = (zmk_usb_get_conn_state() == ZMK_USB_CONN_HID) ? 1u : 0u;
#endif

    /* Byte 14: active BLE profile connected */
#if IS_ENABLED(CONFIG_ZMK_BLE)
    out[14] = zmk_ble_active_profile_is_connected() ? 1u : 0u;
#endif

    /* Byte 15: peripheral (left half) battery */
    out[15] = periph_batt;

    /* Bytes 16-21: layer name (ASCII, zero-padded, not NUL on wire) */
    uint8_t     active = zmk_keymap_highest_layer_active();
    const char *label  = zmk_keymap_layer_name(active);

    if (label != NULL && label[0] != '\0') {
        size_t len = strlen(label);
        if (len > 6u) {
            len = 6u;
        }
        memcpy(&out[16], label, len);
    } else {
        out[16] = 'L';
        out[17] = '0' + (active % 10u);
    }

    /* Byte 22: WPM */
#if IS_ENABLED(CONFIG_ZMK_WPM)
    out[22] = (uint8_t)zmk_wpm_get_state();
#endif
}
