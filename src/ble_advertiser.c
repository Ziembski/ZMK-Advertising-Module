/*
 * src/ble_advertiser.c
 *
 * Non-connectable BLE status advertiser for ZMK keyboards.
 *
 * Key changes vs previous version:
 *
 *   REMOVED: bt_le_ext_adv_update_param() and adaptive interval logic.
 *   WHY: bt_le_ext_adv_update_param() races with ZMK's own BLE stack
 *   operations (profile switching, split connection management) and
 *   causes assertion failures / hard faults in the nRF BLE controller
 *   driver. The workqueue flooding (40 ms reschedule during typing =
 *   25 Hz) also overflowed the system workqueue stack.
 *
 *   REPLACED WITH: fixed 200 ms interval. This is fast enough that
 *   the scanner sees every state change within 200 ms, and power
 *   consumption is negligible vs the keyboard's normal BLE activity.
 *
 *   THROTTLE: update work fires at most once per 200 ms. ZMK events
 *   still trigger immediate reschedules, but the actual HCI call is
 *   capped. No self-rescheduling during "typing".
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

/* ── Fixed advertising interval ──────────────────────────── */

/* Zephyr advertising unit = 0.625 ms  →  ms * 8 / 5 */
#define MS_TO_ADV_UNITS(ms)  ((uint32_t)(ms) * 8u / 5u)

#define ADV_INTERVAL_MS  200u
#define ADV_UNITS        MS_TO_ADV_UNITS(ADV_INTERVAL_MS)

/* Minimum ms between consecutive bt_le_ext_adv_set_data() calls.
 * Caps the update rate so the HCI command queue is never flooded. */
#define UPDATE_THROTTLE_MS  200u

/* ── Static advertising state ────────────────────────────── */

static struct bt_le_ext_adv *adv_set;  /* NULL until created */

#define MFR_DATA_LEN (2u + PAYLOAD_LEN)
static uint8_t mfr_data[MFR_DATA_LEN] = {
    (uint8_t)(CONFIG_BLE_ADVERTISER_COMPANY_ID & 0xFFu),
    (uint8_t)(CONFIG_BLE_ADVERTISER_COMPANY_ID >> 8u),
};

static struct bt_data ad[] = {
    BT_DATA(BT_DATA_MANUFACTURER_DATA, mfr_data, MFR_DATA_LEN),
};

/* ── Payload update work ─────────────────────────────────── */

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
    /*
     * k_work_reschedule() cancels any pending delay and restarts from
     * now + UPDATE_THROTTLE_MS. So even if 10 events fire in quick
     * succession, only one HCI call happens after the throttle window.
     */
    k_work_reschedule(&update_work, K_MSEC(UPDATE_THROTTLE_MS));
}

/* ── Advertising set creation (deferred, with retry) ────── */

static void adv_create_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(adv_create_work, adv_create_fn);

static void adv_create_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    if (adv_set != NULL) {
        return;
    }

    /*
     * Fixed-interval, non-connectable, non-scannable, legacy PDU.
     * No BT_LE_ADV_OPT_USE_IDENTITY: let the controller assign an
     * address. No BT_LE_ADV_OPT_EXT_ADV: passive scanners can receive
     * legacy PDUs without needing to send scan requests.
     *
     * interval_min == interval_max: fixed interval, no jitter.
     */
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

    LOG_INF("BLE advertiser started (code \"%.4s\", company 0x%04X, %u ms)",
            CONFIG_BLE_ADVERTISER_PAIRING_CODE,
            CONFIG_BLE_ADVERTISER_COMPANY_ID,
            ADV_INTERVAL_MS);
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

/* ── Module init ─────────────────────────────────────────── */

static int ble_advertiser_init(void)
{
    payload_builder_init();
    k_work_schedule(&adv_create_work, K_MSEC(2000));
    return 0;
}

SYS_INIT(ble_advertiser_init, APPLICATION,
         CONFIG_BLE_ADVERTISER_INIT_PRIORITY);
