/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BLE manufacturer data payload (21 bytes total inside the AD structure).
 *
 * AD packet budget (31 bytes):
 *   Flags AD        : 3 bytes  [0x02 0x01 0x06]
 *   Manufacturer AD : 1(len) + 1(type=0xFF) + 20(payload struct) = 22 bytes
 *   Total           : 25 bytes  (6 bytes spare)
 *
 * company_id[2] are the first two bytes of the payload struct; the BT stack
 * treats the first two bytes of manufacturer data as the company ID.
 *
 * Scan response carries the keyboard name (CONFIG_ZMK_KEYBOARD_NAME).
 *
 * Payload byte map:
 *   [0-1]   company_id       : 0xFF 0xFF  (BT SIG reserved / test)
 *   [2-3]   protocol_id      : 0x00 0x01
 *   [4]     battery_main     : 0-100 %
 *   [5]     battery_periph   : 0-100 %, 0 = not available
 *   [6]     bt_profile_layer : (active_layer * 15) + bt_profile
 *                              decode: layer = value / 15, profile = value % 15
 *   [7]     status_flags     : see ZMK_BLE_ADV_FLAG_* below
 *   [8-17]  layer_name       : up to 10 ASCII chars, null-padded
 *   [18]    modifiers        : HID modifier byte (see ZMK_BLE_ADV_MOD_* below)
 *   [19]    wpm              : 0-255
 */
struct zmk_ble_adv_payload {
    uint8_t company_id[2];
    uint8_t protocol_id[2];
    uint8_t battery_main;
    uint8_t battery_periph;
    uint8_t bt_profile_layer;
    uint8_t status_flags;
    char    layer_name[10];
    uint8_t modifiers;
    uint8_t wpm;
} __packed;

/* status_flags bit definitions */
#define ZMK_BLE_ADV_FLAG_USB_CONNECTED  BIT(0)
#define ZMK_BLE_ADV_FLAG_CHARGING       BIT(1)
#define ZMK_BLE_ADV_FLAG_BLE_ACTIVE     BIT(2)
#define ZMK_BLE_ADV_FLAG_CAPS_LOCK      BIT(3)
/* bits 4-7 reserved */

/*
 * modifiers bit definitions (identical to USB HID modifier byte).
 *
 *  Bit 7 6 5 4 3 2 1 0
 *      │ │ │ │ │ │ │ └─ Left Ctrl
 *      │ │ │ │ │ │ └─── Left Shift
 *      │ │ │ │ │ └───── Left Alt
 *      │ │ │ │ └─────── Left GUI
 *      │ │ │ └───────── Right Ctrl
 *      │ │ └─────────── Right Shift
 *      │ └───────────── Right Alt
 *      └─────────────── Right GUI
 */
#define ZMK_BLE_ADV_MOD_LCTL  BIT(0)
#define ZMK_BLE_ADV_MOD_LSFT  BIT(1)
#define ZMK_BLE_ADV_MOD_LALT  BIT(2)
#define ZMK_BLE_ADV_MOD_LGUI  BIT(3)
#define ZMK_BLE_ADV_MOD_RCTL  BIT(4)
#define ZMK_BLE_ADV_MOD_RSFT  BIT(5)
#define ZMK_BLE_ADV_MOD_RALT  BIT(6)
#define ZMK_BLE_ADV_MOD_RGUI  BIT(7)

#ifdef __cplusplus
}
#endif
