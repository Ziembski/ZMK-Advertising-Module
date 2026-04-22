/*
 * src/ble_advertiser.c
 *
 * Non-connectable BLE status advertiser.
 *   - Lazy init: adv set created after BT stack is ready
 *   - Adaptive interval: 100 ms while typing, 5 s after 2 s idle
 *   - Zero heap allocation
 *
 * Bug fixes applied:
 *   #2: On partial init failure (set_data or start fails after create),
 *       the adv_set is now deleted and reset to NULL so the retry loop
 *       can start clean. Previously adv_set was left non-NULL pointing
 *       to a created-but-not-started set, silently killing advertising.
 *
 *   #3: last_typing_uptime_ms = 0 caused is_typing() to return true for
 *       the first 2 s after boot. Fixed with a separate `typing_started`
 *       flag — is_typing() returns false until the first key event.
 *
 * Memory audit:
 *   adv_set — kernel-managed object, app lifetime. Not a leak.
 *   mfr_data[], ad[] — static BSS. Not a leak.
 *   K_WORK_DELAYABLE_DEFINE — static storage. Not a leak.
 *   No k_malloc / malloc / realloc anywhere.
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

/* ── Interval constants ──────────────────────────────────── */

/* Zephyr advertising unit = 0.625 ms  →  ms * 8 / 5 */
#define MS_TO_ADV_UNITS(ms)  ((uint32_t)(ms) * 8u / 5u)

#define UNITS_TYPING  MS_TO_ADV_UNITS(CONFIG_BLE_ADVERTISER_TYPING_INTERVAL_MS)
#define UNITS_IDLE    MS_TO_ADV_UNITS(CONFIG_BLE_ADVERTISER_IDLE_INTERVAL_MS)
#define IDLE_TIMEOUT_MS \
    ((int64_t)(CONFIG_BLE_ADVERTISER_IDLE_TIMEOUT_S) * 1000)

/* ── Static advertising state ────────────────────────────── */

static struct bt_le_ext_adv *adv_set;   /* NULL until created */

#define MFR_DATA_LEN (2u + PAYLOAD_LEN)
static uint8_t mfr_data[MFR_DATA_LEN] = {
    (uint8_t)(CONFIG_BLE_ADVERTISER_COMPANY_ID & 0xFFu),
    (uint8_t)(CONFIG_BLE_ADVERTISER_COMPANY_ID >> 8u),
};

static struct bt_data ad[] = {
    BT_DATA(BT_DATA_MANUFACTURER_DATA, mfr_data, MFR_DATA_LEN),
};

/* ── Typing / idle detection ─────────────────────────────── */

/*
 * Bug #3 fix: separate flag for "at least one key event received".
 * Without this, last_typing_uptime_ms = 0 made is_typing() return
 * true for the first IDLE_TIMEOUT_MS after boot.
 */
static int64_t last_typing_uptime_ms;
static bool    typing_started;
static bool    currently_idle = true;

static bool is_typing(void)
{
    if (!typing_started) {
        return false; /* no key event yet — conserve power */
    }
    return (k_uptime_get() - last_typing_uptime_ms) < IDLE_TIMEOUT_MS;
}

/* ── Interval update ─────────────────────────────────────── */

static void apply_interval(bool typing)
{
    if (adv_set == NULL) {
        return;
    }
    /* Skip if already in the correct mode. */
    if (typing == !currently_idle) {
        return;
    }

    uint32_t units = typing ? UNITS_TYPING : UNITS_IDLE;

    struct bt_le_adv_param param = {
        .options      = BT_LE_ADV_OPT_NONE,
        .interval_min = units,
        .interval_max = units + MS_TO_ADV_UNITS(50),
        .peer         = NULL,
    };

    int err = bt_le_ext_adv_update_param(adv_set, &param);
    if (err && err != -EALREADY) {
        LOG_WRN("update_param failed: %d", err);
        return;
    }

    currently_idle = !typing;
    LOG_DBG("Interval -> %s (%u ms)",
            typing ? "typing" : "idle",
            typing ? CONFIG_BLE_ADVERTISER_TYPING_INTERVAL_MS
                   : CONFIG_BLE_ADVERTISER_IDLE_INTERVAL_MS);
}

/* ── Payload update work ─────────────────────────────────── */

static void do_update(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(update_work, do_update);

#define UPDATE_THROTTLE_MS 40u

static void do_update(struct k_work *work)
{
    ARG_UNUSED(work);

    if (adv_set == NULL) {
        return;
    }

    bool typing = is_typing();
    apply_interval(typing);

    payload_build(&mfr_data[2]);

    int err = bt_le_ext_adv_set_data(adv_set, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err && err != -EAGAIN) {
        LOG_WRN("adv_set_data: %d", err);
    }

    /* While typing, keep refreshing to track WPM. */
    if (typing) {
        k_work_reschedule(&update_work, K_MSEC(UPDATE_THROTTLE_MS));
    }
}

static void schedule_update(bool is_key_event)
{
    if (is_key_event) {
        last_typing_uptime_ms = k_uptime_get();
        typing_started        = true;
    }
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

    static const struct bt_le_adv_param adv_param = {
        .options      = BT_LE_ADV_OPT_NONE,
        .interval_min = UNITS_IDLE,
        .interval_max = UNITS_IDLE + MS_TO_ADV_UNITS(50),
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
        /*
         * Bug #2 fix: delete the orphaned adv_set so the retry starts
         * clean. Previously adv_set was left non-NULL after a partial
         * failure, causing the system to silently never advertise.
         */
        LOG_ERR("ext_adv_set_data failed (%d) — deleting and retrying", err);
        bt_le_ext_adv_delete(adv_set);
        adv_set = NULL;
        k_work_reschedule(&adv_create_work, K_MSEC(500));
        return;
    }

    err = bt_le_ext_adv_start(adv_set, BT_LE_EXT_ADV_START_DEFAULT);
    if (err) {
        /* Same fix: clean up before retry. */
        LOG_ERR("ext_adv_start failed (%d) — deleting and retrying", err);
        bt_le_ext_adv_delete(adv_set);
        adv_set = NULL;
        k_work_reschedule(&adv_create_work, K_MSEC(500));
        return;
    }

    LOG_INF("BLE advertiser started (code \"%.4s\", company 0x%04X)",
            CONFIG_BLE_ADVERTISER_PAIRING_CODE,
            CONFIG_BLE_ADVERTISER_COMPANY_ID);
    LOG_INF("Interval: typing=%d ms, idle=%d ms (after %d s idle)",
            CONFIG_BLE_ADVERTISER_TYPING_INTERVAL_MS,
            CONFIG_BLE_ADVERTISER_IDLE_INTERVAL_MS,
            CONFIG_BLE_ADVERTISER_IDLE_TIMEOUT_S);
}

/* ── ZMK event listeners ─────────────────────────────────── */

static int on_layer_changed(const zmk_event_t *eh)
{
    schedule_update(true);
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_battery_changed(const zmk_event_t *eh)
{
    schedule_update(false);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ble_adv_layer,   on_layer_changed);
ZMK_LISTENER(ble_adv_battery, on_battery_changed);
ZMK_SUBSCRIPTION(ble_adv_layer,   zmk_layer_state_changed);
ZMK_SUBSCRIPTION(ble_adv_battery, zmk_battery_state_changed);

#if IS_ENABLED(CONFIG_ZMK_BLE)
static int on_profile_changed(const zmk_event_t *eh)
{
    schedule_update(false);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ble_adv_profile, on_profile_changed);
ZMK_SUBSCRIPTION(ble_adv_profile, zmk_ble_active_profile_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
static int on_usb_changed(const zmk_event_t *eh)
{
    schedule_update(false);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ble_adv_usb, on_usb_changed);
ZMK_SUBSCRIPTION(ble_adv_usb, zmk_usb_conn_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
/*
 * WPM events fire on every keypress — most reliable typing signal.
 */
static int on_wpm_changed(const zmk_event_t *eh)
{
    schedule_update(true);
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
        payload_set_periph_batt(ev->connected ? ev->battery : 0xFFu);
        schedule_update(false);
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
