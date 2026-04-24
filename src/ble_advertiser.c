/*
 * Non-connectable BLE status advertiser for ZMK keyboards.
 * Fixed 200 ms interval. Payload updated on ZMK events, throttled to
 * one HCI call per 200 ms to avoid flooding the command queue.
 * Advertising set creation is deferred 2 s after boot so the BT stack
 * has time to fully initialise before we call bt_le_ext_adv_create().
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

/* Advertising unit = 0.625 ms */
#define MS_TO_ADV_UNITS(ms)   ((uint32_t)(ms) * 8u / 5u)
#define ADV_INTERVAL_MS       200u
#define ADV_UNITS             MS_TO_ADV_UNITS(ADV_INTERVAL_MS)
#define UPDATE_THROTTLE_MS    200u

static struct bt_le_ext_adv *adv_set;

#define MFR_DATA_LEN (2u + PAYLOAD_LEN)
static uint8_t mfr_data[MFR_DATA_LEN] = {
    (uint8_t)(CONFIG_BLE_ADVERTISER_COMPANY_ID & 0xFFu),
    (uint8_t)(CONFIG_BLE_ADVERTISER_COMPANY_ID >> 8u),
};

static struct bt_data ad[] = {
    BT_DATA(BT_DATA_MANUFACTURER_DATA, mfr_data, MFR_DATA_LEN),
};

static void do_update(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(update_work, do_update);

static void do_update(struct k_work *work)
{
    ARG_UNUSED(work);
    if (adv_set == NULL) {
        return;
    }
    payload_build(&mfr_data[2]);
    int err = bt_le_ext_adv_set_data(adv_set, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err && err != -EAGAIN) {
        LOG_WRN("adv_set_data: %d", err);
    }
}

static void schedule_update(void)
{
    k_work_reschedule(&update_work, K_MSEC(UPDATE_THROTTLE_MS));
}

static void adv_create_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(adv_create_work, adv_create_fn);

static void adv_create_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    if (adv_set != NULL) {
        return;
    }

    static const struct bt_le_adv_param adv_param = {
        .options      = BT_LE_ADV_OPT_NONE,
        .interval_min = ADV_UNITS,
        .interval_max = ADV_UNITS,
        .peer         = NULL,
    };

    int err = bt_le_ext_adv_create(&adv_param, NULL, &adv_set);
    if (err) {
        LOG_DBG("ext_adv_create failed (%d), retry in 500 ms", err);
        k_work_reschedule(&adv_create_work, K_MSEC(500));
        return;
    }

    payload_build(&mfr_data[2]);
    err = bt_le_ext_adv_set_data(adv_set, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("ext_adv_set_data failed (%d), retrying", err);
        bt_le_ext_adv_delete(adv_set);
        adv_set = NULL;
        k_work_reschedule(&adv_create_work, K_MSEC(500));
        return;
    }

    err = bt_le_ext_adv_start(adv_set, BT_LE_EXT_ADV_START_DEFAULT);
    if (err) {
        LOG_ERR("ext_adv_start failed (%d), retrying", err);
        bt_le_ext_adv_delete(adv_set);
        adv_set = NULL;
        k_work_reschedule(&adv_create_work, K_MSEC(500));
        return;
    }

    LOG_INF("BLE advertiser started (code \"%.4s\", %u ms)",
            CONFIG_BLE_ADVERTISER_PAIRING_CODE, ADV_INTERVAL_MS);
}

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

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int on_periph_status(const zmk_event_t *eh)
{
    const struct zmk_split_peripheral_status_changed *ev =
        as_zmk_split_peripheral_status_changed(eh);
    if (ev != NULL) {
        payload_set_periph_batt(ev->connected ? 0u : 0xFFu);
        schedule_update();
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ble_adv_periph, on_periph_status);
ZMK_SUBSCRIPTION(ble_adv_periph, zmk_split_peripheral_status_changed);
#endif

static int ble_advertiser_init(void)
{
    payload_builder_init();
    k_work_schedule(&adv_create_work, K_MSEC(2000));
    return 0;
}

SYS_INIT(ble_advertiser_init, APPLICATION, CONFIG_BLE_ADVERTISER_INIT_PRIORITY);
