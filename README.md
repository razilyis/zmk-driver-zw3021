# zmk-driver-zw3021

ZMK external module driver for the HLK-ZW3021 fingerprint sensor.

## Status

Phase 1 (auto-identify), Phase 3 (enroll/delete/clear), Phase 2
(per-fingerprint-ID keystroke output on match), and Phase 4 (Web Serial /
Web Bluetooth browser UI, `docs/index.html`) are implemented:

```text
finger placed → INT rising edge → VCC-D on → boot confirmed → PS_HandShake
→ PS_AutoIdentify (1:N) → result logged → matched ID's stored string (if
any) typed out → VCC-D off → wait for INT low → re-arm
```

```text
&zw3021_enroll <id> / &zw3021_delete <id> / &zw3021_clear (keymap behaviors)
→ queued to the same worker thread → VCC-D on → PS_HandShake
→ PS_AutoEnroll / PS_DeleteChar / PS_Empty → result logged → VCC-D off → re-arm
```

**Not implemented yet**: battery-life optimization, multiple sensors,
uppercase/symbol output characters. See "Roadmap" below.

## Hardware

The ZW3021 is used as a separate 6-pin fingerprint module board (not on the
main keyboard PCB), with its own load switch for `VCC-D`:

```text
MCU 3V3
  ├─ ZW3021 VCC-S (always on)
  └─ load switch IN
          └─ OUT → ZW3021 VCC-D (on only while authenticating)
```

- `VCC-S`: sensor power, must be supplied continuously (3.15–3.6V).
- `VCC-D`: DSP power, gated by a load switch driven from `power-en-gpios`
  (active high). The sensor itself cannot control its own `VCC-D` — the
  load switch is on the host side.
- `INT`: idle low, goes high while a finger rests on the sensor (rising
  edge = finger placed). Treated as a GPIO interrupt on `int-gpios`.
- UART: **57600 baud, 8 data bits, 2 stop bits, no parity (8N2)**. This is
  enforced at runtime via `uart_configure()`; it does not rely on the
  board's default UART settings.

## Wiring (moNa2 Left, `zw3021` branch of `zmk-config-moNa2-v2`)

Verified against the moNa2 Left-side devicetree — see
`documents/codex_zw3021_driver_spec.md` section 2.2 for how this was derived
(the encoder's native pins alone aren't enough; one line is wired directly
to the XIAO nRF52840 module rather than through the board's D-connector).

| ZW3021 pin | XIAO pin | nRF GPIO | Signal | Path |
|---|---|---|---|---|
| UART-RX | D5 | `P0.05` | `HOST_TX` | Peripheral connector (former encoder A) |
| UART-TX | D0 | `P0.02` | `HOST_RX` | Peripheral connector (former encoder B) |
| — (load switch EN) | D4 | `P0.04` | `FP_EN` | Peripheral connector (unused on Left) |
| INT | — | `P0.16` | `FP_INT` | Direct wire to the XIAO nRF52840 module |
| VCC-S | 3V3 | — | | Always-on 3.3V |
| VCC-D | — | — | | Load switch OUT |
| GND | GND | — | | Common ground |

This requires removing the Left-side rotary encoder (`&left_encoder`); doing
so is safe on its own (ZMK's sensor code uses `DEVICE_DT_GET_OR_NULL()`, so a
disabled sensor node is simply skipped — no other devicetree/keymap changes
are required).

## Devicetree usage

```dts
&pinctrl {
    uart1_default: uart1_default {
        group1 {
            psels = <NRF_PSEL(UART_TX, 0, 5)>,   /* D5 */
                    <NRF_PSEL(UART_RX, 0, 2)>;    /* D0 */
        };
    };
    uart1_sleep: uart1_sleep {
        group1 {
            psels = <NRF_PSEL(UART_TX, 0, 5)>,
                    <NRF_PSEL(UART_RX, 0, 2)>;
            low-power-enable;
        };
    };
};

&uart1 {
    status = "okay";
    compatible = "nordic,nrf-uarte";
    current-speed = <57600>;
    pinctrl-0 = <&uart1_default>;
    pinctrl-1 = <&uart1_sleep>;
    pinctrl-names = "default", "sleep";

    zw3021: zw3021 {
        compatible = "razilyis,zw3021";
        status = "okay";

        int-gpios = <&gpio0 16 (GPIO_PULL_DOWN | GPIO_ACTIVE_HIGH)>;
        power-en-gpios = <&gpio0 4 GPIO_ACTIVE_HIGH>;

        power-on-delay-ms = <200>;
        startup-timeout-ms = <500>;
        identify-timeout-ms = <12000>;
        score-level = <3>;
    };
};

&left_encoder {
    status = "disabled";
};
```

If using `CONFIG_ZW3021_STORAGE`, also carve out a dedicated flash
partition (needed once per board, not per sensor instance):

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        /* Shrunk to make room for zw3021_partition below. Adjust the
         * original size/offset to match your board's flash layout. */
        code_partition: partition@27000 {
            reg = <0x00027000 0x000bd000>;
        };

        zw3021_partition: partition@e4000 {
            label = "zw3021_storage";
            reg = <0x000e4000 0x00008000>;
        };

        storage_partition: partition@ec000 {
            label = "storage";
            reg = <0x000ec000 0x00008000>;
        };
    };
};
```

### Devicetree properties

| Property | Required | Default | Description |
|---|---|---:|---|
| `int-gpios` | yes | — | INT pin, rising edge = finger placed |
| `power-en-gpios` | yes | — | VCC-D load switch enable, active high |
| `power-on-delay-ms` | no | 200 | Delay after VCC-D on before waiting for boot |
| `startup-timeout-ms` | no | 500 | Max wait for the sensor's `0x55` boot byte |
| `identify-timeout-ms` | no | 12000 | Host-side upper bound for PS_AutoIdentify |
| `score-level` | no | 3 | PS_AutoIdentify match score level (1–5) |
| `enroll-times` | no | 3 | Number of finger captures PS_AutoEnroll requires per ID |
| `enroll-timeout-ms` | no | 60000 | Host-side upper bound for a full PS_AutoEnroll run |

## Enroll / delete / clear behaviors

Three `BEHAVIOR_LOCALITY_GLOBAL` keymap behaviors are provided so they can be
bound anywhere in a shared keymap on a split build; on the side(s) without
`CONFIG_ZW3021` (e.g. the BLE central) they're no-ops.

**Keep the devicetree node names to 8 characters or less.** The BLE split
transport's "run behavior" characteristic packs the device name into
`char behavior_dev[ZMK_SPLIT_RUN_BEHAVIOR_DEV_LEN]` where
`ZMK_SPLIT_RUN_BEHAVIOR_DEV_LEN` is 9
(`zmk/app/include/zmk/split/bluetooth/service.h`), and `DEVICE_DT_NAME()`
falls back to the node's own name when there's no separate `label`
property. A longer name gets silently truncated (logged as `Truncated
behavior label ... before invoking peripheral behavior`) and the peripheral
can never resolve it, so the request just never arrives. The phandle
labels (used in the keymap as `&zw3021_enroll` etc.) can stay descriptive
since only the node name itself is transmitted:

```dts
zw3021_enroll: zwenroll {
    compatible = "razilyis,zw3021-enroll";
    #binding-cells = <1>;
};
zw3021_delete: zwdelete {
    compatible = "razilyis,zw3021-delete";
    #binding-cells = <1>;
};
zw3021_clear: zwclear {
    compatible = "razilyis,zw3021-clear";
    #binding-cells = <0>;
};
```

Keymap usage: `&zw3021_enroll 1` (enroll under ID 1), `&zw3021_delete 1`
(delete ID 1), `&zw3021_clear` (erase the whole database). Each queues a
request to the sensor's worker thread and returns immediately; a request
made while one is already running is dropped with a logged warning
(`-EBUSY`) rather than queued.

## Per-fingerprint-ID keystroke output

On a successful match, the driver looks up a stored output string for that
fingerprint ID and types it out -- **without the string ever touching git
or the compiled firmware image**. Two pieces make this work:

### 1. Virtual output keyboard (`CONFIG_ZW3021_STORAGE`)

Typing has to happen on the BLE **central** (only it sends HID reports),
but the sensor lives on the peripheral. Symmetric to the enroll/delete/
clear behaviors (which flow central → peripheral), this direction
(peripheral → central) reuses another existing, unmodified ZMK mechanism:
raising a `zmk_position_state_changed` event locally on the peripheral is
automatically forwarded to the central by ZMK's own
`split_peripheral_listener` (`zmk/app/src/split/peripheral.c`) -- the same
path real key matrix presses already use
(`zmk/app/src/physical_layouts.c`).

This requires 37 **virtual key positions** reserved beyond the keyboard's
real key count (one per `0-9`/`a-z` character, plus one for `ENTER`),
bound on `default_layer` to the matching `&kp` keycode and to `&trans` on
every other layer -- see `boards/shields/mona2/mona2.dtsi`'s `RC(4,N)`
transform entries (row 4 is never producible by real kscan hardware,
which only has 4 physical rows) and `mona2.keymap`'s per-layer bindings in
`zmk-config-moNa2-v2`. Case and symbols aren't supported yet: uppercase
letters fold to the same position as lowercase (see
`zw3021_char_to_offset()` in `src/zw3021.c`). The 37th (`ENTER`) position
is only pressed after the string if that fingerprint ID has the "send
enter" flag enabled (`zw3021_storage_get_enter()`, set per-ID over the
serial RPC's `set_finger_enter` command below) -- useful for something
like a password manager's master password field where you also want the
form submitted.

The string itself is stored in NVS on a dedicated flash partition
(`zw3021_partition`, see the devicetree section above), keyed by
fingerprint ID, via `src/storage.c` -- the same pattern as
`zmk-module-Fingerprint/src/storage.c`. The "send enter" flag is stored in
the same NVS instance under a disjoint key range so it can never collide
with a fingerprint ID.

### 2. RPC command core (`src/rpc_commands.c`) + two transports

The only way to get a string into that NVS partition is over a JSON RPC
protocol -- the command dispatch itself (`src/rpc_commands.c`) is
transport-agnostic and shared by two independent transports, either or
both of which can be enabled:

- **`CONFIG_ZW3021_SERIAL_RPC`** (`src/serial_rpc.c`): a line-based
  console on the same USB CDC-ACM stream already used for logging
  (requires the `zmk-usb-logging` snippet) -- modeled on
  `zmk-module-Fingerprint/src/serial_rpc.c`.
- **`CONFIG_ZW3021_BLE_RPC`** (`src/ble_rpc.c`): a standalone BLE GATT
  service, letting a browser configure the sensor with no USB cable and
  no dependency on ZMK Studio or the split link to the central half --
  see "Standalone BLE RPC" below for how this coexists with the split
  connection.

Both use the same flat (unnested) JSON shape so a browser UI can talk to
either one without a backend rewrite:

```text
Request:  {"cmd":"<name>","req_id":<int>,"finger_id":<int>,"value":"<str>","enter":<bool>}
Response: {"ok":true,"req_id":<int>,"data":{...}}
       or: {"ok":false,"req_id":<int>,"message":"..."}
```

| Command | Params | Notes |
|---|---|---|
| `ping` | — | |
| `get_status` | — | `data.busy` |
| `get_fingers` | — | `data.ids`: array of IDs with a stored string |
| `get_finger` | `finger_id` | `data.has_value`, `data.enter` -- **never returns the string itself** |
| `update_finger` | `finger_id`, `value` | writes/overwrites the string (write-only) |
| `delete_finger` | `finger_id` | also clears the "send enter" flag |
| `set_finger_enter` | `finger_id`, `enter` | toggles the per-ID "send enter" flag independently of the string |
| `enroll_start` | `finger_id` | wraps `zw3021_request_enroll()` |
| `enroll_status` | — | `data.busy` |

**`get_finger` is deliberately write-only for the actual string.** This
console has no authentication -- anyone with physical USB access can send
RPC commands -- so echoing the stored value back would let them read out
every fingerprint's output string without ever touching the sensor,
defeating the entire point of gating it behind a match. `get_finger` only
reports whether a value exists and the enter flag; `update_finger` can
still overwrite it blind, and `docs/index.html` is built around this (its
"edit" flow always starts from an empty field).

Over the serial transport, logs and RPC responses share one stream, so
expect them interleaved. Test by typing a line directly into the same
serial terminal used for logs, e.g.:

```text
{"cmd":"update_finger","req_id":1,"finger_id":1,"value":"hunter2"}
{"ok":true,"req_id":1,"data":{}}
{"cmd":"set_finger_enter","req_id":2,"finger_id":1,"enter":true}
{"ok":true,"req_id":2,"data":{}}
{"cmd":"get_finger","req_id":3,"finger_id":1}
{"ok":true,"req_id":3,"data":{"finger_id":1,"has_value":true,"enter":true}}
```

### 3. Standalone BLE RPC (`CONFIG_ZW3021_BLE_RPC`)

The fingerprint sensor and its NVS storage only exist on the split
peripheral half (Left), which has no direct BLE connection to a host --
only ZMK's own split link to the central half (Right). Rather than
relaying requests through the central (which would need new, unproven
split-transport infrastructure -- see `documents/codex_zw3021_driver_spec.md`
section 13 for the investigation that ruled this out, including why ZMK
Studio integration was considered and abandoned), `src/ble_rpc.c` runs a
**second, independent BLE connection directly on the peripheral half**:
its own GATT service (own randomly-generated UUIDs, unrelated to ZMK
Studio's), its own extended-advertising set, running alongside --
completely separately from -- ZMK's own split-link advertising and
connection. `zmk/app/src/split/bluetooth/peripheral.c` is not modified.

This needs headroom for a second simultaneous BLE connection and a
second advertising set on the peripheral half:

```conf
CONFIG_BT_MAX_CONN=2               # existing split link + this connection
CONFIG_BT_EXT_ADV_MAX_ADV_SET=2    # existing split advertising + this one
CONFIG_BT_MAX_PAIRED=2             # existing bond with the central + this one
CONFIG_BT_USER_DATA_LEN_UPDATE=y   # larger notification payloads
CONFIG_BT_CONN_TX_MAX=64           # see below
```

`CONFIG_BT_MAX_PAIRED` is easy to miss: the peripheral half has no
`ZMK_SPLIT_ROLE_CENTRAL`-specific override for it (that only applies to
the central), so it silently falls back to Zephyr's stock default of
**1** -- already consumed by the bond with the central. Without bumping
it, pairing with a browser appears to succeed, but the encrypted CCC
write required to enable notifications fails with a generic "GATT
operation failed for unknown reason" (confirmed on real hardware).

`CONFIG_BT_CONN_TX_MAX` is also easy to miss: the default TX buffer pool
is sized for occasional traffic, not a notify-heavy RPC console.
Confirmed on real hardware: polling `get_status` every couple of seconds
exhausted it (`bt_att: Ran out of TX buffers or contexts`), and handling
that appears to have starved the ZW3021's own UART handshake of timing,
breaking enrollment while a browser stayed connected. ZMK Studio's own
BLE RPC transport hits the identical problem and bumps this exact
Kconfig option to the same value for the same reason. `docs/index.html`
also pauses its background `get_status` poll for the duration of an
enrollment attempt, to keep BLE traffic minimal exactly when the
sensor's UART timing matters most.

Both GATT characteristics require an encrypted (paired/bonded)
connection (`BT_GATT_PERM_*_ENCRYPT`) -- consistent with the RPC's
write-only design, the goal is that nobody can read or write a
fingerprint's output string without first pairing with the physical
device. Expect an OS-level BLE pairing prompt on first connection.

## Web Serial / Web Bluetooth UI (`docs/index.html`)

A self-contained, build-free HTML/JS page ([docs/index.html](docs/index.html))
implements a browser client for the RPC protocol above, connecting over
either transport:
[Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API)
(USB) or
[Web Bluetooth API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Bluetooth_API)
(no cable). Either way it does the same thing: see which of 5 fixed
finger slots have an output string configured (never the string itself,
per the write-only design above), overwrite/delete them, toggle the
per-finger "send enter" flag, and trigger enrollment with an animated
modal -- all from a Chrome or Edge tab, no software install required.

Requirements:
- Chrome or Edge 89+ (neither API is implemented in Firefox/Safari).
- A [secure context](https://developer.mozilla.org/en-US/docs/Web/Security/Secure_Contexts):
  HTTPS, `localhost`, or opening the file directly (`file://`) all work in
  Chromium. Serving `docs/` via GitHub Pages (repo Settings → Pages →
  deploy from branch, folder `/docs`) is the simplest way to get a stable
  HTTPS URL to share, but is not required -- opening the file locally
  works too.
- Over serial, the RPC responses share the same USB CDC-ACM stream as
  ZMK's log output (see above); the page ignores any line that isn't
  valid JSON, so log lines show up in its debug panel but don't interfere
  with the request/response protocol. This doesn't apply to the
  Bluetooth path, which is its own dedicated GATT service.
- Over Bluetooth, the first connection will likely prompt an OS-level
  pairing dialog (see "Standalone BLE RPC" above).

The page never sends anything anywhere except over the local serial or
Bluetooth connection -- there is no server component and no other network
requests, so stored strings never leave the device and the browser tab.

## Kconfig

```conf
CONFIG_ZW3021=y                 # the side with the physical sensor only
CONFIG_ZW3021_ENROLL_BEHAVIOR=y # every side (see below)
CONFIG_ZW3021_DELETE_BEHAVIOR=y
CONFIG_ZW3021_CLEAR_BEHAVIOR=y
CONFIG_ZW3021_STORAGE=y         # the side with the sensor only
CONFIG_ZW3021_SERIAL_RPC=y      # the side with the sensor only
CONFIG_ZW3021_BLE_RPC=y         # the side with the sensor only (optional, see below)

# Only needed if CONFIG_ZW3021_BLE_RPC=y -- see "Standalone BLE RPC" above
CONFIG_BT_MAX_CONN=2
CONFIG_BT_EXT_ADV_MAX_ADV_SET=2
CONFIG_BT_MAX_PAIRED=2
CONFIG_BT_USER_DATA_LEN_UPDATE=y
CONFIG_BT_CONN_TX_MAX=64
```

`CONFIG_ZW3021_ENROLL_BEHAVIOR` / `_DELETE_BEHAVIOR` / `_CLEAR_BEHAVIOR` must
be set to `y` in **every** side's `.conf` on a split build (including the
BLE central, which doesn't have `CONFIG_ZW3021` at all) -- ZMK only forwards
a `BEHAVIOR_LOCALITY_GLOBAL` behavior to other split sides if it can first
resolve the behavior device locally; a side missing it logs `No behavior
assigned to <position> on layer <N>` and never forwards anything at all.

**Harmless log noise on every peripheral invocation, confirmed working
despite it:** the peripheral will log `Unhandled command type 1` followed
by `Failed to invoke behavior <name>: -134` even when the invocation
succeeded. This is an upstream ZMK bug -- the `INVOKE_BEHAVIOR` case in
`zmk/app/src/split/peripheral.c`'s `zmk_split_transport_peripheral_command_handler`
switch statement is missing a `break;`, so it always falls through to
`default:` (logging the warning and returning `-ENOTSUP`) regardless of
whether the behavior itself ran successfully. Look for a real error
(e.g. `-22`/`EINVAL`, meaning the behavior device wasn't found) to tell an
actual failure apart from this cosmetic one.

## Build

Add this repository to the consuming config's `west.yml` as a project, then
enable `CONFIG_ZW3021=y` in the target shield's `.conf` file (the side with
the sensor only). On a split build, instantiate the three behavior nodes
above in a devicetree file shared by every side (not just the one with the
sensor), and set the three `CONFIG_ZW3021_*_BEHAVIOR=y` flags in every
side's `.conf` per the Kconfig section above.

## Expected log output

```text
[INF] zw3021: initialized
[INF] zw3021: finger detected
[INF] zw3021: VCC-D enabled
[INF] zw3021: boot handshake byte received
[INF] zw3021: PS_HandShake OK
[INF] zw3021: identify started
[INF] zw3021: match id=3 score=87
[INF] zw3021: VCC-D disabled
```

Enrolling (success):

```text
[INF] zw3021: VCC-D enabled
[INF] zw3021: boot handshake byte received
[INF] zw3021: PS_HandShake OK
[INF] zw3021: enroll started, id=1 times=3
[INF] zw3021: enroll stored id=1
[INF] zw3021: VCC-D disabled
```

On errors:

```text
[WRN] zw3021: startup timeout, trying handshake
[ERR] zw3021: handshake timeout/error: -116
[WRN] zw3021: no matching fingerprint
[WRN] zw3021: identify timeout
[ERR] zw3021: invalid checksum
[WRN] zw3021: enroll: feature generation failed, retrying capture
[WRN] zw3021: enroll: id already has a template
[WRN] zw3021: enroll failed: -5
[WRN] zw3021: busy, dropping request (type=0)
```

No raw fingerprint image, template, or stored-string data is ever logged.

## Known limitations

- Single sensor instance only (no split left+right dual-sensor support).
- No power-consumption tuning; `VCC-D` gating is the only power-saving
  measure implemented.
- `INT` is wired directly to the XIAO nRF52840 module rather than through
  the board's standard D0–D10 header; verify this wiring against your own
  hardware before flashing.
- Only one enroll/delete/clear request is queued at a time; a request made
  while another is running is dropped (logged, not queued).
- `PS_AutoEnroll`'s per-capture reporting is only partially documented
  (see the comment above `zw3021_auto_enroll()`). Confirmed working on
  real hardware for the success path (enroll → identify → match); the
  various failure confirm codes (duplicate ID, full database, etc.) are
  implemented per the protocol manual but not individually exercised.
- Output strings are lowercase-alphanumeric only (no uppercase, symbols,
  or spaces); see `zw3021_char_to_offset()` in `src/zw3021.c`.
- `get_fingers` finds stored IDs by scanning 1..100 and checking each with
  `nvs_read` -- fine at this scale, but not how you'd enumerate a much
  larger ID space.
- The standalone BLE RPC (`CONFIG_ZW3021_BLE_RPC`) needs a few seconds
  after connecting before sensor operations (enrollment) are reliable:
  the driver requests a much longer BLE connection interval on connect
  (see "Standalone BLE RPC" above) to stop it from disrupting the
  sensor's UART timing, and that renegotiation takes a few seconds to
  land. `docs/index.html` disables the enroll button and shows a
  countdown for the first 6 seconds after a Bluetooth connection to
  cover this; USB has no such warm-up period.
- `enroll_status` only reports whether the driver is busy, not
  success/failure detail for the enrollment attempt in progress -- the
  Web UI can only tell you when it's done, not whether it succeeded; check
  the device log or re-check `get_fingers`/`get_finger` afterwards.

## Roadmap

```text
Phase 5: Split keyboard peripheral integration, central event forwarding
```

See `documents/codex_zw3021_driver_spec.md` (local, not tracked in git) for
the full protocol reference and implementation plan this driver was built
from.
