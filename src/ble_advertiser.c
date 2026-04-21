/*
 * src/ble_advertiser.c
 *
 * Manages a non-connectable BLE advertising set that broadcasts the
 * keyboard status payload built by payload_builder.c.
 *
 * Design:
 *   • Uses bt_le_ext_adv with a LEGACY PDU (no BT_LE_ADV_OPT_EXT_ADV),
 *     so passive scanners (including the companion BLE Scanner firmware)
 *     receive it without needing active scanning.
 *   • BT_LE_ADV_OPT_USE_IDENTITY ensures the same MAC address as ZMK's
 *     own HID advertising — the scanner sees one device, not two.
 *   • Runs alongside ZMK's existing advertising set with no interference.
 *   • ZMK events trigger an immediate rebuild + update.
 *   • A periodic work item refreshes WPM (which changes continuously).
 */

#include "payload_builder.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/events/layer_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/event_manager.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/events/ble_active_profile_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/events/usb_conn_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/events/wpm_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/events/split_peripheral_status_changed.h>
#endif

LOG_MODULE_REGISTER(ble_advertiser, LOG_LEVEL_INF);

/* ── Advertising set ─────────────────────────────────────── */

static struct bt_le_ext_adv *adv_set;

/*
 * Manufacturer Specific Data layout:
 *   [0..1]  company ID (little-endian)
 *   [2..20] 19-byte keyboard payload
 */
#define MFR_DATA_LEN (2u + PAYLOAD_LEN)
static uint8_t mfr_data[MFR_DATA_LEN] = {
    (uint8_t)(CONFIG_BLE_ADVERTISER_COMPANY_ID & 0xFF),
    (uint8_t)(CONFIG_BLE_ADVERTISER_COMPANY_ID >> 8),
    /* payload bytes follow — updated before advertising starts */
};

static struct bt_data ad[] = {
    BT_DATA(BT_DATA_MANUFACTURER_DATA, mfr_data, MFR_DATA_LEN),
};

/* ── Advertising update ──────────────────────────────────── */

static void do_update(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(update_work, do_update);

/* Minimum ms between two consecutive advertising data updates. */
#define UPDATE_THROTTLE_MS 50

static void do_update(struct k_work *work)
{
    if (adv_set == NULL) {
        return;
    }

    /* Rebuild payload into the manufacturer data buffer (bytes 2+). */
    payload_build(&mfr_data[2]);

    int err = bt_le_ext_adv_set_data(adv_set, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err && err != -EAGAIN) {
        LOG_WRN("adv_set_data failed: %d", err);
    }

#if IS_ENABLED(CONFIG_ZMK_WPM)
    /* Re-schedule for continuous WPM refresh. */
    k_work_reschedule(&update_work, K_MSEC(500));
#endif
}

static void schedule_update(void)
{
    k_work_reschedule(&update_work, K_MSEC(UPDATE_THROTTLE_MS));
}

/* ── ZMK event listeners ─────────────────────────────────── */

static int on_layer_changed(const zmk_event_t *eh)
{
    schedule_update();
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_battery_changed(const zmk_event_t *eh)
{
    schedule_update();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ble_adv_layer,   on_layer_changed);
ZMK_LISTENER(ble_adv_battery, on_battery_changed);

ZMK_SUBSCRIPTION(ble_adv_layer,   zmk_layer_state_changed);
ZMK_SUBSCRIPTION(ble_adv_battery, zmk_battery_state_changed);

#if IS_ENABLED(CONFIG_ZMK_BLE)
static int on_profile_changed(const zmk_event_t *eh)
{
    schedule_update();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ble_adv_profile, on_profile_changed);
ZMK_SUBSCRIPTION(ble_adv_profile, zmk_ble_active_profile_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
static int on_usb_changed(const zmk_event_t *eh)
{
    schedule_update();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ble_adv_usb, on_usb_changed);
ZMK_SUBSCRIPTION(ble_adv_usb, zmk_usb_conn_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
static int on_wpm_changed(const zmk_event_t *eh)
{
    schedule_update();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ble_adv_wpm, on_wpm_changed);
ZMK_SUBSCRIPTION(ble_adv_wpm, zmk_wpm_state_changed);
#endif

/*
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int on_periph_battery(const zmk_event_t *eh)
{
    const struct zmk_split_peripheral_status_changed *ev =
        as_zmk_split_peripheral_status_changed(eh);

    if (ev != NULL) {
        payload_set_periph_batt(ev->connected ? ev->battery : 0xFFu);
        schedule_update();
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ble_adv_periph, on_periph_battery);
ZMK_SUBSCRIPTION(ble_adv_periph, zmk_split_peripheral_status_changed);
#endif
*/

/* ── Module init ─────────────────────────────────────────── */

/*
 * Advertising interval: Zephyr units are 0.625 ms per unit.
 * ms_to_units(x) = x * 1000 / 625 = x * 8 / 5
 */
#define MS_TO_ADV_UNITS(ms) ((ms) * 8u / 5u)

static int ble_advertiser_init(void)
{
    payload_builder_init();

    /* Build initial payload before advertising starts. */
    payload_build(&mfr_data[2]);

    /*
     * Non-connectable, non-scannable legacy PDU.
     * BT_LE_ADV_OPT_USE_IDENTITY: same MAC as ZMK's HID advertising,
     *   so the scanner sees one device with one address.
     * No BT_LE_ADV_OPT_EXT_ADV: forces legacy PDU (31-byte limit),
     *   received by passive scanners without scan requests.
     */
    static const struct bt_le_adv_param adv_param = {
        .options     = BT_LE_ADV_OPT_USE_IDENTITY,
        .interval_min = MS_TO_ADV_UNITS(CONFIG_BLE_ADVERTISER_INTERVAL_MIN_MS),
        .interval_max = MS_TO_ADV_UNITS(CONFIG_BLE_ADVERTISER_INTERVAL_MAX_MS),
        .peer        = NULL,
    };

    int err = bt_le_ext_adv_create(&adv_param, NULL, &adv_set);
    if (err) {
        LOG_ERR("Failed to create advertising set: %d", err);
        return err;
    }

    err = bt_le_ext_adv_set_data(adv_set, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("Failed to set advertising data: %d", err);
        return err;
    }

    err = bt_le_ext_adv_start(adv_set, BT_LE_EXT_ADV_START_DEFAULT);
    if (err) {
        LOG_ERR("Failed to start advertising: %d", err);
        return err;
    }

    LOG_INF("BLE status advertising started (company 0x%04X, interval %d-%d ms)",
            CONFIG_BLE_ADVERTISER_COMPANY_ID,
            CONFIG_BLE_ADVERTISER_INTERVAL_MIN_MS,
            CONFIG_BLE_ADVERTISER_INTERVAL_MAX_MS);

#if IS_ENABLED(CONFIG_ZMK_WPM)
    /* Kick off periodic WPM refresh. */
    k_work_reschedule(&update_work, K_MSEC(500));
#endif

    return 0;
}

/*
 * Priority must be > CONFIG_ZMK_BLE_INIT_PRIORITY (default 0) so that
 * the Bluetooth stack is fully ready before we call bt_le_ext_adv_create.
 */
SYS_INIT(ble_advertiser_init, APPLICATION, CONFIG_BLE_ADVERTISER_INIT_PRIORITY);
