# BLE Advertiser — ZMK Module

A ZMK module that broadcasts keyboard status as a non-connectable BLE
advertisement, readable by the companion [BLE Scanner](https://github.com/<your-user>/ble-scanner) firmware.

## What it advertises

A 23-byte manufacturer-specific payload (after a 2-byte company ID):

| Bytes | Field | Notes |
|-------|-------|-------|
| 0–3   | Pairing code | 4 ASCII chars — must match scanner |
| 4–9   | Keyboard ID | FNV-1a hash of `CONFIG_ZMK_KEYBOARD_NAME` |
| 10    | Battery % | Central (right half) battery, 0–100 |
| 11    | Profile slot | Active BLE profile, 0-based |
| 12    | Charging | 1 = USB power present |
| 13    | USB connected | 1 = USB HID active |
| 14    | BLE connected | 1 = active profile connected |
| 15    | Peripheral battery | 0–100 %, `0xFF` = no peripheral |
| 16–21 | Layer name | ASCII, zero-padded |
| 22    | WPM | Words per minute (0 if disabled) |

## How it works

A separate `bt_le_ext_adv` set runs **alongside** ZMK's existing HID advertising at a fixed 200 ms interval. The scanner locks onto the first device whose payload starts with the matching 4-byte pairing code — no MAC address configuration needed on either side.

Payload is rebuilt on every ZMK event: layer change, battery update, profile switch, USB state change, WPM update.

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
CONFIG_BLE_ADVERTISER=y

# Optional — fills in the WPM field
CONFIG_ZMK_WPM=y
```

`CONFIG_BT_EXT_ADV=y` and `CONFIG_BT_EXT_ADV_MAX_ADV_SET=2` are selected/defaulted automatically by the module.

For split keyboards `CONFIG_ZMK_SPLIT=y` and `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y` are already set by ZMK. Peripheral connection state is populated automatically.

### 3. Set the pairing code (must match scanner)

In your keyboard `.conf`:
```ini
CONFIG_BLE_ADVERTISER_PAIRING_CODE="ZMKK"
```

In the BLE Scanner `prj.conf`:
```ini
CONFIG_BLE_PAIRING_CODE="ZMKK"
```

Both sides must use the same 4-character code. Change it to anything unique if you have multiple keyboards nearby.

### 4. (Optional) Change the company ID

```ini
# Keyboard side
CONFIG_BLE_ADVERTISER_COMPANY_ID=0xFFFF

# Scanner side — must match
CONFIG_BLE_COMPANY_ID=0xFFFF
```

`0xFFFF` (default) is the Bluetooth SIG test/prototype ID. Fine for personal use.

---

## Kconfig reference

| Option | Default | Purpose |
|--------|---------|---------|
| `CONFIG_BLE_ADVERTISER` | n | Enable this module |
| `CONFIG_BLE_ADVERTISER_PAIRING_CODE` | `"ZMKK"` | 4-char lock code |
| `CONFIG_BLE_ADVERTISER_COMPANY_ID` | `0xFFFF` | Manufacturer data company ID |
| `CONFIG_BLE_ADVERTISER_INIT_PRIORITY` | `1` | SYS_INIT priority (must be > ZMK BLE) |
| `CONFIG_ZMK_WPM` | n | Enables WPM field in payload |

---

## Licence

MIT
