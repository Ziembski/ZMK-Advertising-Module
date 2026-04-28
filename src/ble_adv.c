/*
 * SPDX-License-Identifier: MIT
 *
 * ZMK BLE Status Advertisement module.
 *
 * Advertising runs only on the central half or a standalone keyboard.
 * Peripheral halves do nothing so that the split BLE link is undisturbed.
 *
 * Scheduling:
 *   - Keypresses, layer changes and BT profile changes all request an
 *     immediate update, gated by ZMK_BLE_ADV_EVENT_THROTTLE_MS.
 *   - The work item reschedules itself at ZMK_BLE_ADV_INTERVAL_MS after
 *     each execution, even on error, so a transient BT stack failure is
 *     automatically retried on the next cycle.
 *   - Once no keypress has been seen for ZMK_BLE_ADV_IDLE_TIMEOUT_MS the
 *     scheduling switches to ZMK_BLE_ADV_IDLE_INTERVAL_MS and WPM resets.
 *   - Any keypress restores active scheduling immediately.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/hid.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/ble_adv.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#include <zmk/endpoints.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/keymap.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* -------------------------------------------------------------------------
 * Peripheral-side guard: compile nothing for peripheral halves.
 * ------------------------------------------------------------------------- */
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* Peripheral side: module is inert. */

#else  /* central or standalone */

/* -------------------------------------------------------------------------
 * Static assertions
 * ------------------------------------------------------------------------- */
BUILD_ASSERT(sizeof(struct zmk_ble_adv_payload) == 20,
             "zmk_ble_adv_payload must be exactly 20 bytes");

/* -------------------------------------------------------------------------
 * WPM state
 * ------------------------------------------------------------------------- */
#define WPM_HISTORY_SLOTS CONFIG_ZMK_BLE_ADV_WPM_WINDOW_SECONDS

static uint8_t  wpm_history[WPM_HISTORY_SLOTS];
static uint32_t wpm_slot_idx;
static uint32_t wpm_last_second;
static uint8_t  wpm_slot_keys;
static uint8_t  wpm_value;

static void wpm_on_keypress(uint32_t now_ms) {
    uint32_t current_second = now_ms / 1000U;

    if (wpm_last_second == 0U) {
        wpm_last_second = current_second;
    }

    if (current_second != wpm_last_second) {
        uint32_t delta = current_second - wpm_last_second;
        for (uint32_t i = 0; i < delta && i < WPM_HISTORY_SLOTS; i++) {
            wpm_slot_idx = (wpm_slot_idx + 1U) % WPM_HISTORY_SLOTS;
            wpm_history[wpm_slot_idx] = (i == 0U) ? wpm_slot_keys : 0U;
        }
        wpm_last_second = current_second;
        wpm_slot_keys   = 0U;
    }

    wpm_slot_keys++;
}

static void wpm_update(uint32_t now_ms) {
    uint32_t current_second = now_ms / 1000U;

    if (wpm_last_second > 0U && current_second != wpm_last_second) {
        uint32_t delta = current_second - wpm_last_second;
        for (uint32_t i = 0; i < delta && i < WPM_HISTORY_SLOTS; i++) {
            wpm_slot_idx = (wpm_slot_idx + 1U) % WPM_HISTORY_SLOTS;
            wpm_history[wpm_slot_idx] = (i == 0U) ? wpm_slot_keys : 0U;
        }
        wpm_last_second = current_second;
        wpm_slot_keys   = 0U;
    }

    /* WPM = (keys / chars_per_word) / (window_seconds / 60)
     *     = keys * 60 / (5 * window)
     *     = keys * 12 / window                               */
    uint32_t total_keys = wpm_slot_keys;
    for (uint32_t i = 1U; i < WPM_HISTORY_SLOTS; i++) {
        uint32_t idx = (wpm_slot_idx + WPM_HISTORY_SLOTS - i) % WPM_HISTORY_SLOTS;
        total_keys += wpm_history[idx];
    }

    uint32_t computed = (total_keys * 12U) / WPM_HISTORY_SLOTS;
    wpm_value = (uint8_t)MIN(computed, 255U);
}

static void wpm_reset_to_idle(void) {
    memset(wpm_history, 0, sizeof(wpm_history));
    wpm_slot_idx    = 0U;
    wpm_last_second = 0U;
    wpm_slot_keys   = 0U;
    wpm_value       = 0U;
}

/* -------------------------------------------------------------------------
 * Advertisement state
 * ------------------------------------------------------------------------- */
static struct k_work_delayable adv_work;
static struct bt_le_ext_adv   *adv_set;
static bool                    adv_running;
static bool                    in_idle;

static uint32_t last_keypress_ms;
static uint32_t last_adv_ms;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static uint8_t peripheral_battery;
#endif

/* -------------------------------------------------------------------------
 * Payload and advertising data
 *
 * adv_ad[1] holds a pointer to the static `payload` struct. BT_DATA captures
 * the pointer at declaration time; bt_le_ext_adv_set_data() dereferences it
 * when called, so updating `payload` in place before each set_data call is
 * correct and no re-initialisation of adv_ad is needed between cycles.
 * ------------------------------------------------------------------------- */
static struct zmk_ble_adv_payload payload;

/* Populated once at init from the compile-time constant CONFIG_ZMK_KEYBOARD_NAME. */
static uint8_t scan_rsp_name_len;
static char    scan_rsp_name[32];

static struct bt_data adv_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, (const uint8_t *)&payload, sizeof(payload)),
};

static struct bt_data adv_sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, scan_rsp_name, 0),
};

static void build_payload(void) {
    /* Zero the whole struct first so layer_name is null-padded and reserved
     * bits remain clear. The strncpy below is safe: the field is already
     * zeroed so no separate null terminator write is required.            */
    memset(&payload, 0, sizeof(payload));

    payload.company_id[0]  = 0xFF;
    payload.company_id[1]  = 0xFF;
    payload.protocol_id[0] = 0x00;
    payload.protocol_id[1] = 0x01;

    uint8_t bat_main = zmk_battery_state_of_charge();
    if (bat_main > 100U) {
        bat_main = 100U;
    }
    payload.battery_main = bat_main;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    payload.battery_periph = peripheral_battery;
#else
    payload.battery_periph = 0U;
#endif

    uint8_t layer = 0U;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    layer = (uint8_t)zmk_keymap_highest_layer_active();
    /* Clamp to 15: the payload spec encodes layer in 4 effective bits via
     * the bt_profile_layer combined field.                                */
    if (layer > 15U) {
        layer = 15U;
    }
#endif
    uint8_t profile = 0U;
#if IS_ENABLED(CONFIG_ZMK_BLE) && \
    (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
    profile = (uint8_t)zmk_ble_active_profile_index();
    if (profile > 4U) {
        profile = 4U;
    }
#endif
    /* Encoding: (layer * 15) + profile.  Max value: (15*15)+4 = 229 <= 255.
     * Receiver recovers layer = value / 15, profile = value % 15.           */
    payload.bt_profile_layer = (uint8_t)((layer * 15U) + profile);

    uint8_t flags = 0U;
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        flags |= ZMK_BLE_ADV_FLAG_USB_CONNECTED;
        if (bat_main < 100U) {
            flags |= ZMK_BLE_ADV_FLAG_CHARGING;
        }
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE) && \
    (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
    if (zmk_ble_active_profile_is_connected()) {
        flags |= ZMK_BLE_ADV_FLAG_BLE_ACTIVE;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    /* Indicators are tracked per endpoint; query the currently selected one. */
    if (zmk_hid_indicators_get_profile(zmk_endpoint_get_selected()) & BIT(1)) {
        flags |= ZMK_BLE_ADV_FLAG_CAPS_LOCK;
    }
#endif
    payload.status_flags = flags;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
#if IS_ENABLED(CONFIG_ZMK_BLE_ADV_LAYER_NAMES)
    const char *name = zmk_keymap_layer_name(layer);
    if (name != NULL && name[0] != '\0') {
        strncpy(payload.layer_name, name, sizeof(payload.layer_name));
    } else {
        snprintf(payload.layer_name, sizeof(payload.layer_name), "L%u", layer);
    }
#else
    snprintf(payload.layer_name, sizeof(payload.layer_name), "L%u", layer);
#endif
#endif

    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    if (report != NULL) {
        payload.modifiers = report->body.modifiers;
    }

    uint32_t now_ms = k_uptime_get_32();
    wpm_update(now_ms);
    payload.wpm = wpm_value;
}

/* -------------------------------------------------------------------------
 * Advertising set management
 * ------------------------------------------------------------------------- */
static const struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
    BT_LE_ADV_OPT_SCANNABLE | BT_LE_ADV_OPT_USE_IDENTITY,
    BT_GAP_ADV_FAST_INT_MIN_2,
    BT_GAP_ADV_FAST_INT_MAX_2,
    NULL);

static int ensure_adv_set(void) {
    if (adv_set != NULL) {
        return 0;
    }
    int err = bt_le_ext_adv_create(&adv_param, NULL, &adv_set);
    if (err != 0) {
        LOG_ERR("Failed to create advertising set: %d", err);
    }
    return err;
}

static void do_advertise(void) {
    if (ensure_adv_set() != 0) {
        return;
    }

    build_payload();

    adv_sd[0].data_len = scan_rsp_name_len;

    int err = bt_le_ext_adv_set_data(adv_set, adv_ad, ARRAY_SIZE(adv_ad),
                                     adv_sd, ARRAY_SIZE(adv_sd));
    if (err != 0) {
        LOG_WRN("Failed to set advertising data: %d -- will recreate set next cycle", err);
        adv_set = NULL;
        return;
    }

    err = bt_le_ext_adv_start(adv_set, BT_LE_EXT_ADV_START_DEFAULT);
    if (err != 0 && err != -EALREADY) {
        LOG_WRN("Advertising start failed: %d", err);
    }
}

/* -------------------------------------------------------------------------
 * Work handler and scheduling helpers
 * ------------------------------------------------------------------------- */
static uint32_t next_interval_ms(uint32_t now_ms) {
    uint32_t idle_for = now_ms - last_keypress_ms;
    if (idle_for >= (uint32_t)CONFIG_ZMK_BLE_ADV_IDLE_TIMEOUT_MS) {
        return (uint32_t)CONFIG_ZMK_BLE_ADV_IDLE_INTERVAL_MS;
    }
    return (uint32_t)CONFIG_ZMK_BLE_ADV_INTERVAL_MS;
}

static void adv_work_handler(struct k_work *work) {
    uint32_t now_ms   = k_uptime_get_32();
    uint32_t idle_for = now_ms - last_keypress_ms;

    if (!in_idle && idle_for >= (uint32_t)CONFIG_ZMK_BLE_ADV_IDLE_TIMEOUT_MS) {
        in_idle = true;
        wpm_reset_to_idle();
        LOG_DBG("zmk-ble-adv: entering idle");
    }

    last_adv_ms = now_ms;
    do_advertise();

    /* Always reschedule even if do_advertise() failed so transient errors
     * are retried automatically on the next cycle.                        */
    k_work_reschedule(&adv_work, K_MSEC(next_interval_ms(now_ms)));
}

/*
 * Request an event-driven update. Fires immediately if the last advertisement
 * was at least ZMK_BLE_ADV_EVENT_THROTTLE_MS ago, otherwise delayed to stay
 * within the throttle window.
 */
static void request_event_update(void) {
    if (!adv_running) {
        return;
    }
    uint32_t now_ms  = k_uptime_get_32();
    uint32_t elapsed = now_ms - last_adv_ms;
    uint32_t delay_ms;

    if (elapsed >= (uint32_t)CONFIG_ZMK_BLE_ADV_EVENT_THROTTLE_MS) {
        delay_ms = 0U;
    } else {
        delay_ms = (uint32_t)CONFIG_ZMK_BLE_ADV_EVENT_THROTTLE_MS - elapsed;
    }

    k_work_reschedule(&adv_work, K_MSEC(delay_ms));
}

/* -------------------------------------------------------------------------
 * Event listeners
 * ------------------------------------------------------------------------- */

static int on_position_state_changed(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev != NULL && ev->state) {
        uint32_t now_ms  = k_uptime_get_32();
        last_keypress_ms = now_ms;
        in_idle          = false;
        wpm_on_keypress(now_ms);
        request_event_update();
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ble_adv_position, on_position_state_changed);
ZMK_SUBSCRIPTION(ble_adv_position, zmk_position_state_changed);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
static int on_layer_state_changed(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev != NULL) {
        request_event_update();
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ble_adv_layer, on_layer_state_changed);
ZMK_SUBSCRIPTION(ble_adv_layer, zmk_layer_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE) && \
    (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
static int on_ble_profile_changed(const zmk_event_t *eh) {
    request_event_update();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ble_adv_profile, on_ble_profile_changed);
ZMK_SUBSCRIPTION(ble_adv_profile, zmk_ble_active_profile_changed);
#endif

/*
 * Peripheral battery: ZMK fires zmk_peripheral_battery_state_changed on the
 * central whenever the peripheral half reports its charge level over the split
 * BLE link. Only source index 0 (first peripheral) is stored; the payload has
 * a single byte for peripheral battery.
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int on_peripheral_battery_changed(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (ev != NULL && ev->source == 0U) {
        peripheral_battery = ev->state_of_charge;
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ble_adv_periph_bat, on_peripheral_battery_changed);
ZMK_SUBSCRIPTION(ble_adv_periph_bat, zmk_peripheral_battery_state_changed);
#endif

/* -------------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------------- */
static int ble_adv_init(void) {
    /* Capture keyboard name once; CONFIG_ZMK_KEYBOARD_NAME is a compile-time
     * constant so this only needs to happen here, not on each advertisement. */
    size_t name_len = strlen(CONFIG_ZMK_KEYBOARD_NAME);
    if (name_len >= sizeof(scan_rsp_name)) {
        name_len = sizeof(scan_rsp_name) - 1U;
    }
    memcpy(scan_rsp_name, CONFIG_ZMK_KEYBOARD_NAME, name_len);
    scan_rsp_name[name_len] = '\0';
    scan_rsp_name_len = (uint8_t)name_len;

    k_work_init_delayable(&adv_work, adv_work_handler);

    last_keypress_ms = k_uptime_get_32();
    last_adv_ms      = 0U;
    in_idle          = false;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_INF("zmk-ble-adv: central half, advertising enabled");
#else
    LOG_INF("zmk-ble-adv: standalone keyboard, advertising enabled");
#endif

    adv_running = true;
    k_work_schedule(&adv_work, K_SECONDS(1));
    return 0;
}

SYS_INIT(ble_adv_init, APPLICATION, 95);

#endif /* central or standalone */
