# Peahen Calling

A ZMK module that continuously broadcasts keyboard status as non-connectable BLE
manufacturer data. Any BLE scanner that knows the filter signature can read
battery level, active layer, WPM, modifier state, USB/BLE output selection, and
more — without pairing or connecting.

Only the **central half** of a split keyboard (or a standalone keyboard)
advertises. Peripheral halves are completely silent so the split BLE link is
not disturbed.

---

## Installation

### 1. Add the module to `config/west.yml`

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: your-github-username          # replace with your GitHub username
      url-base: https://github.com/your-github-username
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: peahen-calling                # replace with your actual repo name
      remote: your-github-username
      revision: main
      path: modules/peahen-calling
  self:
    path: config
```

### 2. Enable the module in `config/<keyboard>.conf`

Add the following to the **central or standalone** keyboard's `.conf` file.
Do **not** add these to a peripheral half's conf — the module ignores peripherals
automatically, but enabling it there wastes flash.

```conf
# ── Core ──────────────────────────────────────────────────────────────────────
CONFIG_ZMK_BLE_ADV=y

# ── Caps Lock reporting (bit 3 of status_flags) ───────────────────────────────
# Requires ZMK to track HID indicator state. Without this, bit 3 is always 0.
CONFIG_ZMK_HID_INDICATORS=y

# ── Preferred output reporting (bit 5 of status_flags) ────────────────────────
# Already enabled on most keyboards that have a USB port. If your keyboard is
# BLE-only, omit this — bit 5 will always read 0 (Bluetooth).
CONFIG_ZMK_USB=y

# ── Named layer support (layer_name field in payload) ─────────────────────────
# Requires a ZMK build from 2024+ that exposes zmk_keymap_layer_name().
# Set to n on older trees; the layer name field will fall back to "L<n>".
CONFIG_ZMK_BLE_ADV_LAYER_NAMES=y

# ── Optional tuning (shown with their defaults) ───────────────────────────────
# CONFIG_ZMK_BLE_ADV_INTERVAL_MS=1000         # active advertisement interval
# CONFIG_ZMK_BLE_ADV_EVENT_THROTTLE_MS=200    # min gap between event updates
# CONFIG_ZMK_BLE_ADV_IDLE_TIMEOUT_MS=10000    # ms of inactivity before idle
# CONFIG_ZMK_BLE_ADV_IDLE_INTERVAL_MS=30000   # advertisement interval in idle
# CONFIG_ZMK_BLE_ADV_WPM_WINDOW_SECONDS=30    # rolling window for WPM
```

### 3. Build and flash

Build exactly as you normally would:

```
west build -s zmk/app -b <board> -- -DZMK_CONFIG=/path/to/config
west flash
```

---

## Receiver-side filtering

Peahen Calling and ZMK's own connectable advertising both transmit from the same
device address. To tell them apart, filter on **AD type `0xFF`
(Manufacturer Specific Data)** and require the first 4 bytes to match exactly:

| Byte offset in mfr data | Hex value | ASCII |
|---|---|---|
| 0 | `0x50` | `P` |
| 1 | `0x45` | `E` |
| 2 | `0x41` | `A` |
| 3 | `0x21` | `!` |

Any advertisement packet that does not begin with `50 45 41 21` in its
manufacturer data is from ZMK's own stack and should be discarded.

These constants are also exported from the header for C/C++ receivers:

```c
#include <zmk/ble_adv.h>

// ZMK_BLE_ADV_COMPANY_ID_0   0x50  ('P')
// ZMK_BLE_ADV_COMPANY_ID_1   0x45  ('E')
// ZMK_BLE_ADV_PROTOCOL_ID_0  0x41  ('A')
// ZMK_BLE_ADV_PROTOCOL_ID_1  0x21  ('!')
```

---

## Payload format

The advertisement carries a **20-byte manufacturer data** block (3-byte Flags AD +
23-byte Manufacturer AD = **26 bytes**, within the 31-byte legacy limit).
The keyboard name appears in the **scan response**.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0–1 | 2 | `company_id` | `0x50 0x45` — `"PE"` (filter signature) |
| 2–3 | 2 | `protocol_id` | `0x41 0x21` — `"A!"` (filter signature) |
| 4 | 1 | `battery_main` | Central / standalone battery 0–100 % |
| 5 | 1 | `battery_periph` | First peripheral battery 0–100 %; `0` = not available |
| 6 | 1 | `bt_profile_layer` | `(active_layer × 15) + bt_profile` |
| 7 | 1 | `status_flags` | See table below |
| 8–17 | 10 | `layer_name` | Active layer name, ASCII, null-padded, max 10 chars |
| 18 | 1 | `modifiers` | HID modifier byte — see table below |
| 19 | 1 | `wpm` | Words per minute, 0–255 |

### `bt_profile_layer` encoding

```
value = (active_layer × 15) + bt_profile
```

Decode on the receiver: `layer = value / 15`, `profile = value % 15`.

Example: layer 1, profile 4 → `(1 × 15) + 4 = 19`.
Maximum: layer 15, profile 4 → `(15 × 15) + 4 = 229` (fits in one byte).

### `status_flags`

| Bit | Meaning | Required config |
|-----|---------|----------------|
| 0 | USB connected | `CONFIG_ZMK_USB=y` |
| 1 | Charging (USB powered + battery < 100 %) | `CONFIG_ZMK_USB=y` |
| 2 | BLE profile active and connected | always available |
| 3 | Caps Lock on | `CONFIG_ZMK_HID_INDICATORS=y` — always `0` without it |
| 4 | USB logging enabled (compile-time constant) | `1` when built with `zmk-usb-logging` snippet |
| 5 | Preferred output: `0` = Bluetooth, `1` = USB | `CONFIG_ZMK_USB=y` — always `0` without it |
| 6–7 | Reserved | — |

### `modifiers`

Standard USB HID modifier byte.

| Bit | Modifier |
|-----|----------|
| 0 | Left Ctrl |
| 1 | Left Shift |
| 2 | Left Alt |
| 3 | Left GUI (Win / Cmd) |
| 4 | Right Ctrl |
| 5 | Right Shift |
| 6 | Right Alt |
| 7 | Right GUI |

---

## Advertisement scheduling

| Condition | Interval |
|-----------|----------|
| Active (keypress within `IDLE_TIMEOUT`) | `ZMK_BLE_ADV_INTERVAL_MS` (default 1 s) |
| Idle (no keypress for `IDLE_TIMEOUT`) | `ZMK_BLE_ADV_IDLE_INTERVAL_MS` (default 30 s) |
| Event (keypress / layer change / profile change / output change) | Immediate, throttled to no faster than `ZMK_BLE_ADV_EVENT_THROTTLE_MS` (default 200 ms) |

---

## Peripheral battery

The module subscribes to `zmk_peripheral_battery_state_changed` events emitted by
ZMK's split BLE stack. Only `source == 0` (the first connected peripheral) is
stored in `battery_periph`. If no peripheral has connected since boot, the byte
is `0`.

---

## Kconfig reference

| Symbol | Default | Description |
|--------|---------|-------------|
| `ZMK_BLE_ADV` | `n` | Master switch — enable the module |
| `ZMK_BLE_ADV_INTERVAL_MS` | 1000 | Periodic advertisement interval in active mode (ms) |
| `ZMK_BLE_ADV_EVENT_THROTTLE_MS` | 200 | Minimum gap between event-driven advertisement updates (ms) |
| `ZMK_BLE_ADV_IDLE_TIMEOUT_MS` | 10000 | Milliseconds of inactivity before entering idle mode |
| `ZMK_BLE_ADV_IDLE_INTERVAL_MS` | 30000 | Advertisement interval in idle mode (ms) |
| `ZMK_BLE_ADV_WPM_WINDOW_SECONDS` | 30 | Rolling window length for WPM calculation (seconds) |
| `ZMK_BLE_ADV_LAYER_NAMES` | `y` | Use `zmk_keymap_layer_name()` for the `layer_name` field; set to `n` on ZMK trees older than 2024 to fall back to `"L<n>"` |

The build system automatically selects `CONFIG_BT_EXT_ADV=y` and sets
`CONFIG_BT_EXT_ADV_MAX_ADV_SET` to at least `2` via Kconfig — no manual changes
to these symbols are needed.

---

## Requirements

- ZMK on the `main` branch (2024 or later recommended).
- `CONFIG_ZMK_BLE=y` — enforced as a Kconfig dependency; the module will not
  compile without it.
- A BLE 5.0-capable SoC (nRF52840, nRF52833, etc.) for extended advertising
  support.
- `CONFIG_ZMK_BLE_ADV_LAYER_NAMES=n` if your ZMK tree predates named-layer
  support.
