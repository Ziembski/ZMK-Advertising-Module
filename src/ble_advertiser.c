/*
 * src/ble_advertiser.c
 *
 * Manages a non-connectable BLE advertising set that broadcasts the
 * keyboard status payload built by payload_builder.c.
 *
 * ── Why lazy init? ───────────────────────────────────────────────────
 * ZMK calls bt_enable(zmk_ble_bt_ready) asynchronously in its own
 * SYS_INIT handler (APPLICATION, priority 0). By the time our init
 * fires (priority 1), bt_enable has been called but the controller is
 * not ready yet — bt_le_ext_adv_create returns -EAGAIN or -EIO and
 * advertising silently never starts.
 *
 * Fix: SYS_INIT only registers event listeners. The first ZMK event
 * that fires after BT is ready triggers adv_set creation via a work
 * item. If it still fails (race), the work item reschedules itself
 * with backoff until it succeeds.
 *
 * ── Separate MAC address ─────────────────────────────────────────────
 * When CONFIG_BLE_ADVERTISER_SEPARATE_ADDR=y (default), the status
 * advertising set is given a deterministic random-static address
 * derived from CONFIG_ZMK_KEYBOARD_NAME via FNV-1a hash.
 *
 * Benefits:
 *   - The address is always the same after reflash.
 *   - It is different from ZMK's HID identity address, so the scanner
 *     locks onto the status advertiser without seeing HID traffic.
 *   - Put this address in CONFIG_BLE_TARGET_ADDR on the scanner.
 *   - The address is logged at boot: look for "Status MAC:" in RTT/USB
 *     serial output.
 *
 * A BLE random-static address has bits[47:46] = 0b11 (top two bits of
 * the most-significant byte set). The remaining 46 bits come from hash.
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

LOG_MODULE_REGISTER(ble_advertiser, LOG_LEVEL_INF);

/* ── Advertising set ─────────────────────────────────────── */

static struct bt_le_ext_adv *adv_set;

#define MFR_DATA_LEN (2u + PAYLOAD_LEN)
static uint8_t mfr_data[MFR_DATA_LEN] = {
    (uint8_t)(CONFIG_BLE_ADVERTISER_COMPANY_ID & 0xFF),
    (uint8_t)(CONFIG_BLE_ADVERTISER_COMPANY_ID >> 8),
};

static struct bt_data ad[] = {
    BT_DATA(BT_DATA_MANUFACTURER_DATA, mfr_data, MFR_DATA_LEN),
};

/* ── Deterministic random-static address ─────────────────── */

#if IS_ENABLED(CONFIG_BLE_ADVERTISER_SEPARATE_ADDR)

/**
 * Build a BLE random-static address from the keyboard name.
 *
 * FNV-1a over the name gives 32 bits; a second pass with a different
 * seed gives another 32 bits — together 64 bits, of which we use 46
 * (the address field minus the two mandatory top bits).
 *
 * Random-static address format (Bluetooth Core Spec Vol 6, Part B, §1.3):
 *   bits[47:46] must be 0b11  (set via | 0xC0 on the MSB)
 *   bits[45:0]  random
 */
static void build_status_addr(bt_addr_t *addr)
{
    const char *name = CONFIG_ZMK_KEYBOARD_NAME;
    uint32_t h1, h2;

    /* FNV-1a, two independent passes */
    h1 = 2166136261u;
    for (const char *p = name; *p; p++) {
        h1 ^= (uint8_t)*p;
        h1 *= 16777619u;
    }
    h2 = h1 ^ 0xDEADBEEFu;
    for (const char *p = name; *p; p++) {
        h2 ^= (uint8_t)*p;
        h2 *= 16777619u;
    }

    /*
     * Pack into 6 bytes (little-endian as Zephyr stores BT addresses).
     * val[5] is the most-significant byte in the BT address on air.
     */
    addr->val[0] = (uint8_t)(h1);
    addr->val[1] = (uint8_t)(h1 >>  8);
    addr->val[2] = (uint8_t)(h1 >> 16);
    addr->val[3] = (uint8_t)(h2);
    addr->val[4] = (uint8_t)(h2 >>  8);
    addr->val[5] = (uint8_t)(h2 >> 16) | 0xC0u; /* set random-static bits */
}

#endif /* CONFIG_BLE_ADVERTISER_SEPARATE_ADDR */

/* ── Advertising interval helper ─────────────────────────── */

/* Zephyr advertising interval unit = 0.625 ms */
#define MS_TO_ADV_UNITS(ms) ((ms) * 8u / 5u)

/* ── Lazy advertising set creation ──────────────────────────
 *
 * Called from a work item so it runs in the system workqueue context,
 * after the BT controller has had time to finish initialisation.
 * Reschedules itself with backoff on failure.
 */
static void adv_create_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(adv_create_work, adv_create_work_fn);

static void adv_create_work_fn(struct k_work *work)
{
    if (adv_set != NULL) {
        return; /* already created */
    }

    static const struct bt_le_adv_param adv_param = {
#if IS_ENABLED(CONFIG_BLE_ADVERTISER_SEPARATE_ADDR)
        /* No USE_IDENTITY — we will set our own random-static addr. */
        .options      = BT_LE_ADV_OPT_NONE,
#else
        /* Share the identity MAC with ZMK's HID advertising. */
        .options      = BT_LE_ADV_OPT_USE_IDENTITY,
#endif
        .interval_min = MS_TO_ADV_UNITS(CONFIG_BLE_ADVERTISER_INTERVAL_MIN_MS),
        .interval_max = MS_TO_ADV_UNITS(CONFIG_BLE_ADVERTISER_INTERVAL_MAX_MS),
        .peer         = NULL,
    };

    int err = bt_le_ext_adv_create(&adv_param, NULL, &adv_set);
    if (err) {
        LOG_DBG("bt_le_ext_adv_create failed (%d), retrying...", err);
        /* Retry in 500 ms — BT controller may still be initialising. */
        k_work_reschedule(&adv_create_work, K_MSEC(500));
        return;
    }

#if IS_ENABLED(CONFIG_BLE_ADVERTISER_SEPARATE_ADDR)
    /* Assign the deterministic random-static address to this set. */
    bt_addr_t status_addr;
    build_status_addr(&status_addr);

    err = bt_le_ext_adv_set_addr(adv_set, &status_addr);
    if (err) {
        LOG_WRN("bt_le_ext_adv_set_addr failed (%d) — using identity", err);
    } else {
        /*
         * Log the MAC in big-endian display order (val[5]..val[0])
         * so the user can paste it directly into CONFIG_BLE_TARGET_ADDR.
         */
        LOG_INF("Status advertiser MAC (use as CONFIG_BLE_TARGET_ADDR):");
        LOG_INF("  %02X:%02X:%02X:%02X:%02X:%02X",
                status_addr.val[5], status_addr.val[4],
                status_addr.val[3], status_addr.val[2],
                status_addr.val[1], status_addr.val[0]);
    }
#endif

    /* Set initial payload. */
    payload_build(&mfr_data[2]);
    err = bt_le_ext_adv_set_data(adv_set, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("bt_le_ext_adv_set_data failed: %d", err);
        return;
    }

    err = bt_le_ext_adv_start(adv_set, BT_LE_EXT_ADV_START_DEFAULT);
    if (err) {
        LOG_ERR("bt_le_ext_adv_start failed: %d", err);
        return;
    }

    LOG_INF("BLE status advertising started (company 0x%04X, %d-%d ms)",
            CONFIG_BLE_ADVERTISER_COMPANY_ID,
            CONFIG_BLE_ADVERTISER_INTERVAL_MIN_MS,
            CONFIG_BLE_ADVERTISER_INTERVAL_MAX_MS);
}

/* ── Payload update ──────────────────────────────────────── */

static void do_update(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(update_work, do_update);

#define UPDATE_THROTTLE_MS 50

static void do_update(struct k_work *work)
{
    if (adv_set == NULL) {
        return; /* still initialising */
    }

    payload_build(&mfr_data[2]);

    int err = bt_le_ext_adv_set_data(adv_set, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err && err != -EAGAIN) {
        LOG_WRN("adv_set_data failed: %d", err);
    }

#if IS_ENABLED(CONFIG_ZMK_WPM)
    k_work_reschedule(&update_work, K_MSEC(500));
#endif
}

static void schedule_update(void)
{
    /*
     * Kick off adv_set creation if it hasn't happened yet.
     * This is a no-op if the work item is already scheduled or done.
     */
    if (adv_set == NULL) {
        k_work_schedule(&adv_create_work, K_NO_WAIT);
    }

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

/* ── Module init ─────────────────────────────────────────── */

static int ble_advertiser_init(void)
{
    payload_builder_init();

    /*
     * Do NOT call bt_le_ext_adv_create here. At APPLICATION priority 1,
     * ZMK has called bt_enable() but its callback has not yet fired —
     * the BT controller is not ready. adv_set creation is deferred to
     * adv_create_work, which is triggered by the first ZMK event.
     *
     * Schedule an initial attempt with a short delay as a safety net
     * for keyboards where no event fires quickly (e.g. idle at boot).
     */
    k_work_schedule(&adv_create_work, K_MSEC(2000));

    return 0;
}

SYS_INIT(ble_advertiser_init, APPLICATION, CONFIG_BLE_ADVERTISER_INIT_PRIORITY);
