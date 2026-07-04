# zmk-driver-zw3021

ZMK external module driver for the HLK-ZW3021 fingerprint sensor.

## Status

Phase 1 (auto-identify) and Phase 3 (enroll/delete/clear) are implemented:

```text
finger placed → INT rising edge → VCC-D on → boot confirmed → PS_HandShake
→ PS_AutoIdentify (1:N) → result logged → VCC-D off → wait for INT low → re-arm
```

```text
&zw3021_enroll <id> / &zw3021_delete <id> / &zw3021_clear (keymap behaviors)
→ queued to the same worker thread → VCC-D on → PS_HandShake
→ PS_AutoEnroll / PS_DeleteChar / PS_Empty → result logged → VCC-D off → re-arm
```

**Not implemented yet**: keystroke/macro output on match, password storage,
Web Serial UI, battery-life optimization, multiple sensors. See "Roadmap"
below.

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

## Kconfig

```conf
CONFIG_ZW3021=y                 # the side with the physical sensor only
CONFIG_ZW3021_ENROLL_BEHAVIOR=y # every side (see below)
CONFIG_ZW3021_DELETE_BEHAVIOR=y
CONFIG_ZW3021_CLEAR_BEHAVIOR=y
```

`CONFIG_ZW3021_ENROLL_BEHAVIOR` / `_DELETE_BEHAVIOR` / `_CLEAR_BEHAVIOR` must
be set to `y` in **every** side's `.conf` on a split build (including the
BLE central, which doesn't have `CONFIG_ZW3021` at all) -- ZMK only forwards
a `BEHAVIOR_LOCALITY_GLOBAL` behavior to other split sides if it can first
resolve the behavior device locally; a side missing it logs `No behavior
assigned to <position> on layer <N>` and never forwards anything at all.

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
- No keystroke/macro output on a successful match — authentication and
  enrollment results only reach the log.
- Only one enroll/delete/clear request is queued at a time; a request made
  while another is running is dropped (logged, not queued).
- `PS_AutoEnroll`'s per-capture reporting is only partially documented
  (see the comment above `zw3021_auto_enroll()`); the receive loop is
  written to work whether the sensor sends one final packet or several,
  but hasn't been exercised against every failure path on real hardware.

## Roadmap

```text
Phase 2: &zw3021_touchpass-style manual auth trigger, keystroke/macro output on match
Phase 4: Web Serial config, ZMK settings/NVS storage (see zmk-module-Fingerprint for prior art)
Phase 5: Split keyboard peripheral integration, central event forwarding
```

See `documents/codex_zw3021_driver_spec.md` (local, not tracked in git) for
the full protocol reference and implementation plan this driver was built
from.
