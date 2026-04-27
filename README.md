# zmk-ble-adv

A ZMK module that broadcasts keyboard status as non-connectable BLE manufacturer data.
Only the **central half** of a split keyboard (or a standalone keyboard) advertises.
Peripheral halves are completely silent so the split BLE link is not disturbed.

---

## Payload format

The advertisement uses a **21-byte manufacturer data** block inside a standard
BLE AD structure (3-byte Flags + 23-byte Manufacturer = **26 bytes**, within the
31-byte legacy advertising limit). The keyboard name appears in the **scan response**.

| Offset | Size (bytes) | Field | Description |
|--------|-------------|-------|-------------|
| 0–1 | 2 | `company_id` | `0xFF 0xFF` (BT SIG reserved / test) |
| 2–3 | 2 | `protocol_id` | `0x00 0x01` |
| 4 | 1 | `battery_main` | Central / standalone battery 0–100 % |
| 5 | 1 | `battery_periph` | First peripheral battery 0–100 %, `0` = not available |
| 6 | 1 | `bt_profile_layer` | `(active_layer × 15) + bt_profile` |
| 7 | 1 | `active_layer` | 0–15 |
| 8 | 1 | `status_flags` | See table below |
| 9–18 | 10 | `layer_name` | ASCII, null-padded, trimmed at 10 chars |
| 19 | 1 | `modifiers` | HID modifier byte – see table below |
| 20 | 1 | `wpm` | Words per minute 0–255 |

### `bt_profile_layer` encoding

```
value = (active_layer × 15) + bt_profile
```

Example: layer 1, profile 4 → `(1 × 15) + 4 = 19`.
Maximum: layer 15, profile 4 → `(15 × 15) + 4 = 229` (fits in one byte).

### `status_flags`

| Bit | Meaning |
|-----|---------|
| 0 | USB Connected |
| 1 | Charging (USB powered + battery < 100 %) |
| 2 | BLE profile active and connected |
| 3 | Caps Lock on (requires `CONFIG_ZMK_HID_INDICATORS=y`) |
| 4–7 | Reserved |

### `modifiers`

Identical to the USB HID modifier byte.

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

## Peripheral battery

The module listens to `zmk_peripheral_battery_state_changed` events emitted by
ZMK's split BLE stack. **Only `source == 0`** (the first connected peripheral) is
stored in `battery_periph`. If no peripheral is connected the byte is `0`.
Peripheral halves never call any advertising code.

---

## Scheduling

| Condition | Interval |
|-----------|----------|
| Active (keypress within `IDLE_TIMEOUT`) | `ZMK_BLE_ADV_INTERVAL_MS` (default 1 s) |
| Idle (no keypress for `IDLE_TIMEOUT`) | `ZMK_BLE_ADV_IDLE_INTERVAL_MS` (default 30 s) |
| Event (keypress / layer change / profile change) | immediate, but **not** more often than `ZMK_BLE_ADV_EVENT_THROTTLE_MS` (default 200 ms) |

---

## Adding to your ZMK config

### 1. `config/west.yml`

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: your-github-username
      url-base: https://github.com/your-github-username
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-ble-adv
      remote: your-github-username
      revision: main
      path: modules/zmk-ble-adv
  self:
    path: config
```

### 2. `config/<keyboard>.conf` (central or standalone only)

```conf
CONFIG_ZMK_BLE_ADV=y

# Optional overrides (all have defaults):
# CONFIG_ZMK_BLE_ADV_INTERVAL_MS=1000
# CONFIG_ZMK_BLE_ADV_EVENT_THROTTLE_MS=200
# CONFIG_ZMK_BLE_ADV_IDLE_TIMEOUT_MS=10000
# CONFIG_ZMK_BLE_ADV_IDLE_INTERVAL_MS=30000
# CONFIG_ZMK_BLE_ADV_WPM_WINDOW_SECONDS=30
# CONFIG_ZMK_BLE_ADV_LAYER_NAMES=y
```

Do **not** add `CONFIG_ZMK_BLE_ADV=y` to the peripheral half's conf file.

---

## Kconfig reference

| Symbol | Default | Description |
|--------|---------|-------------|
| `ZMK_BLE_ADV` | `n` | Enable the module |
| `ZMK_BLE_ADV_INTERVAL_MS` | 1000 | Periodic advertisement interval (ms) |
| `ZMK_BLE_ADV_EVENT_THROTTLE_MS` | 200 | Minimum gap between event-driven updates (ms) |
| `ZMK_BLE_ADV_IDLE_TIMEOUT_MS` | 10000 | Inactivity before idle mode (ms) |
| `ZMK_BLE_ADV_IDLE_INTERVAL_MS` | 30000 | Advertisement interval in idle mode (ms) |
| `ZMK_BLE_ADV_WPM_WINDOW_SECONDS` | 30 | Rolling window for WPM calculation |
| `ZMK_BLE_ADV_LAYER_NAMES` | `y` | Use `zmk_keymap_layer_name()` for layer names |

### `ZMK_BLE_ADV_LAYER_NAMES`

Requires a ZMK version that exposes `zmk_keymap_layer_name()` (added when named
layer support was introduced). Set to `n` on older trees; the layer name field
will fall back to `"L<n>"`.

---

## Caps Lock detection

Set `CONFIG_ZMK_HID_INDICATORS=y` in your keyboard conf to enable Caps Lock
reporting in the status flags. Without it the Caps Lock bit is always `0`.

---

## Requirements

- ZMK with `zmk_keymap_layer_name()` available (main branch, 2024+) unless
  `CONFIG_ZMK_BLE_ADV_LAYER_NAMES=n` is set.
- `CONFIG_ZMK_BLE=y` (dependency enforced by Kconfig).
- The build system will automatically set `CONFIG_BT_EXT_ADV=y` and
  `CONFIG_BT_EXT_ADV_MAX_ADV_SET=2` via Kconfig selects/defaults.
