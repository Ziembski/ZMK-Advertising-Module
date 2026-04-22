/*
 * src/payload_builder.h
 *
 * Wire layout (23 bytes, after the 2-byte company-ID prefix):
 *
 *   [0..3]   pairing_code  (CONFIG_BLE_ADVERTISER_PAIRING_CODE)
 *   [4..9]   keyboard_id   (FNV-1a of CONFIG_ZMK_KEYBOARD_NAME)
 *   [10]     battery_pct   (central / right half)
 *   [11]     profile_slot
 *   [12]     charging
 *   [13]     usb_connected
 *   [14]     ble_connected
 *   [15]     periph_batt   (peripheral / left half; 0xFF = absent)
 *   [16..21] layer_name    (ASCII, zero-padded)
 *   [22]     wpm
 */

#ifndef BLE_ADVERTISER_PAYLOAD_BUILDER_H
#define BLE_ADVERTISER_PAYLOAD_BUILDER_H

#include <stdint.h>

#define PAYLOAD_LEN 23u

void payload_builder_init(void);
void payload_build(uint8_t out[PAYLOAD_LEN]);
void payload_set_periph_batt(uint8_t level);

#endif /* BLE_ADVERTISER_PAYLOAD_BUILDER_H */
