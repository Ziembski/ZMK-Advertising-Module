# BLE Advertiser — ZMK Module

A ZMK module that broadcasts keyboard status as a non-connectable BLE
advertisement, readable by the companion [BLE Scanner](https://github.com/<your-user>/ble-scanner) firmware.

## What it advertises

A 19-byte manufacturer-specific payload (after a 2-byte company ID):

| Bytes | Field | Notes |
|-------|-------|-------|
| 0–5   | Keyboard ID | FNV-1a hash of `CONFIG_ZMK_KEYBOARD_NAME` |
| 6     | Battery % | Central battery, 0–100 |
| 7     | Profile slot | Active BLE profile, 0-based |
| 8     | Charging | 1 = USB power present |
| 9     | USB connected | 1 = USB HID active |
| 10    | BLE connected | 1 = active profile connected |
| 11    | Peripheral battery | 0–100 %, `0xFF` = no peripheral |
| 12–17 | Layer name | ASCII, zero-padded |
| 18    | WPM | Words per minute (0 if disabled) |

## How it works

A separate `bt_le_ext_adv` set runs **alongside** ZMK's existing HID
advertising with no interference:

- **Legacy PDU** → received by passive scanners (no active scanning needed)
- **`BT_LE_ADV_OPT_USE_IDENTITY`** → same MAC address as ZMK's HID
  advertising, so the scanner sees one device with one address
- Payload is rebuilt on every relevant ZMK event (layer change, battery
  update, profile switch, USB state, WPM change)

---

## Integration into your keyboard

### 1. Add to `config/west.yml`

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: your-user
      url-base: https://github.com/<your-username>

  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: ble-advertiser
      remote: your-user
      revision: main
      path: modules/ble-advertiser
```

### 2. Add to your keyboard `.conf`

```ini
# ── BLE Advertiser (required) ──────────────────────────────
CONFIG_BLE_ADVERTISER=y

# BT_EXT_ADV is selected automatically, but you need a second adv set:
CONFIG_BT_EXT_ADV_MAX_ADV_SET=2

# ── Optional features that fill in more payload fields ──────
CONFIG_ZMK_WPM=y               # byte 18: WPM
```

> For split keyboards, `CONFIG_ZMK_SPLIT=y` and
> `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y` are already set by ZMK.
> Peripheral battery (byte 11) is populated automatically when enabled.

### 3. (Optional) Customise in `.conf`

```ini
# Match this in BLE Scanner's KBD_ADV_COMPANY_ID:
CONFIG_BLE_ADVERTISER_COMPANY_ID=0xFFFF

# Advertising interval (ms). ≤200 ms keeps scanner display responsive.
CONFIG_BLE_ADVERTISER_INTERVAL_MIN_MS=200
CONFIG_BLE_ADVERTISER_INTERVAL_MAX_MS=500
```

### 4. Find your MAC and configure the scanner

Flash your keyboard. Use **nRF Connect** (mobile/desktop) or
`bluetoothctl` to find the device. Its MAC will appear as both ZMK's
HID device and the status advertiser (same address).

In the BLE Scanner `prj.conf`:
```ini
CONFIG_BLE_TARGET_ADDR="AA:BB:CC:DD:EE:FF"
```

---

## ZMK Kconfig options required on keyboard

| Option | Required | Purpose |
|--------|----------|---------|
| `CONFIG_BLE_ADVERTISER=y` | ✅ | Enables this module |
| `CONFIG_BT_EXT_ADV_MAX_ADV_SET=2` | ✅ | Second adv set alongside ZMK |
| `CONFIG_ZMK_WPM=y` | Optional | Populates WPM field (byte 18) |
| `CONFIG_ZMK_SPLIT=y` | If split | Enables split keyboard support |
| `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y` | If split | Enables peripheral battery |

`CONFIG_BT_EXT_ADV=y` is automatically selected by `CONFIG_BLE_ADVERTISER`.

---

## Licence

MIT
