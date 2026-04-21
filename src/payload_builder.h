/*
 * src/payload_builder.h
 *
 * Builds the 19-byte keyboard status payload from ZMK subsystem APIs.
 *
 * Wire layout — must match adv_parser.h in the BLE Scanner firmware:
 *
 *   Offset  Len  Field
 *   ──────  ───  ────────────────────────────────────────────────
 *    0       6   keyboard_id   — FNV-1a hash of CONFIG_ZMK_KEYBOARD_NAME
 *    6       1   battery_pct   — central battery, 0-100 %
 *    7       1   profile_slot  — active BLE profile index, 0-based
 *    8       1   charging      — 1 = USB power present (proxy for charging)
 *    9       1   usb_connected — 1 = USB bus active
 *   10       1   ble_connected — 1 = active BLE profile is connected
 *   11       1   periph_batt   — peripheral battery, 0-100 % or 0xFF
 *   12       6   layer_name    — ASCII label, zero-padded (not NUL on wire)
 *   18       1   wpm           — words per minute (0 if not enabled)
 */

#ifndef BLE_ADVERTISER_PAYLOAD_BUILDER_H
#define BLE_ADVERTISER_PAYLOAD_BUILDER_H

#include <stdint.h>

/** Total payload length — must match KBD_ADV_PAYLOAD_MIN_LEN in scanner. */
#define PAYLOAD_LEN 19u

/**
 * @brief One-time init: compute the keyboard ID hash.
 *        Call once before any payload_build() calls.
 */
void payload_builder_init(void);

/**
 * @brief Build a fresh 19-byte status payload from current ZMK state.
 * @param out  Output buffer, exactly PAYLOAD_LEN bytes.
 */
void payload_build(uint8_t out[PAYLOAD_LEN]);

/**
 * @brief Update the stored peripheral battery level.
 *        Called by ble_advertiser.c from the split battery event handler.
 * @param level  0-100 % or 0xFF (no peripheral).
 */
void payload_set_periph_batt(uint8_t level);

#endif /* BLE_ADVERTISER_PAYLOAD_BUILDER_H */
