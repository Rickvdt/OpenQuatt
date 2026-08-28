# Intuis Edel Eau 270/3 — PV mode and tank temperature

Fork-specific add-on. Lets OpenQuatt read the DHW tank temperature of an Intuis
Edel Eau 270/3 heat pump boiler and drive its two volt-free PV inputs, so a Home
Assistant automation can park surplus solar production in the tank.

Implemented by [`openquatt/oq_intuis_dhw.yaml`](../openquatt/oq_intuis_dhw.yaml),
included from the Heatpump Controller Q-edition hardware profile.

> [!WARNING]
> The boiler's PV connectors 1 and 2 are **volt-free (dry contact) only** and must
> never be connected to 230 V. Switch the boiler off at the breaker before you
> remove its cover or touch the PCB.

## What the boiler does with the contacts

| Contact | Closed means | Boiler behaviour |
|---|---|---|
| Connector 1 | low PV yield — **PV ECO** | heat pump alone heats to `T°PV ECO` (normal setpoint…55 °C, factory 55 °C) |
| Connector 2 | high PV yield — **PV MAX** | heat pump **plus electric back-up** heats to `T°PV MAX` (`T°PV ECO`…65 °C, factory 65 °C) |

The manual does not define the behaviour when both contacts are closed at once,
so the firmware never asserts both: each mode writes one exclusive pattern.

## Wiring

Tank temperature reuses the existing `T` connector; the PV contacts use an
external 2-channel opto-isolated relay module.

The board (V1.1) carries two 4-pin expansion headers next to the ESP32-S3:

- `+5V` / `CLK` / `SDO` / `SDI` — the `CLK`/`SDO`/`SDI` pins are a breakout of the
  PT1000 SPI bus and are **not** free. Only `+5V` is used here.
- `+3.3V` / `GPIO43` / `GPIO44` / `GND` — the two free GPIOs, plus ground.

| Board | Relay module | Boiler |
|---|---|---|
| `+5V` (SPI header) | `VCC` | — |
| `GND` (GPIO header) | `GND` | — |
| `GPIO43` (GPIO header) | `IN1` | ch1 `COM` + `NO` → PV connector **1** (PV ECO) |
| `GPIO44` (GPIO header) | `IN2` | ch2 `COM` + `NO` → PV connector **2** (PV MAX) |
| `T`: `+3.3V` / `GND` / `DATA` | — | DS18B20 strapped to the tank |

`GPIO43` and `GPIO44` are the only free GPIOs brought out to a header. Other
ESP32-S3 pins are electrically unused but not accessible on this board. Neither
is a strapping pin, and the console runs over `USB_SERIAL_JTAG` (ESPHome's
default for this build) rather than UART0, so nothing else drives them at
runtime.

## Relay module polarity

The module in use is a TinyTronics 5 V 2-channel relay board (SKU 003089) whose
trigger polarity is jumper-selectable. It is wired **high-active** — jumper
bridging `COM`–`HIGH` on both channels — so `oq_intuis_relay_inverted` is
`"false"`: the firmware drives the pin HIGH to close a contact and LOW to
release it.

> [!CAUTION]
> Do **not** switch this module to low-active. The vendor states that in that
> mode the signal pin sits at about 5 V, which exceeds the ESP32-S3's 3.3 V GPIO
> rating and can damage `GPIO43`/`GPIO44`. High-active is the only safe mode
> here; the module accepts 3.3 V logic on its inputs.

If both relays energise and stay on, `oq_intuis_relay_inverted` does not match
the jumper setting. Check the jumpers first, then the substitution.

### Boot behaviour

`GPIO43` is `U0TXD`. The ESP32-S3 ROM bootloader drives it HIGH and prints a boot
message on it at every reset, before the firmware runs. That is hardware
behaviour and no firmware setting suppresses it. In high-active mode this means
**PV ECO briefly closes for well under a second at each reset** — long enough for
the coil to pull in, far too short for the boiler to act on given it expects
activation times measured in hours.

`GPIO44` is only weakly pulled up internally (about 45 kΩ), so it can be held
down externally. Fit a **10 kΩ resistor from `IN2` to `GND`** and PV MAX stays
released through the entire boot, which is the channel worth protecting since it
enables the electric back-up. The same resistor on `IN1` will not overcome the
ROM actively driving `GPIO43`.

If you want both channels fully silent at boot, the options are an inverting
transistor stage per channel (then set `oq_intuis_relay_inverted` back to
`"true"`) or moving to an on-board relay. Neither is needed for normal use.

Each energised relay draws about 70 mA from the board's 5 V rail. Only one is
ever energised by design, so the steady-state load stays modest.

## Boiler-side settings

The contacts do nothing until PV mode is enabled on the boiler itself:

1. `Menu → INST. MENU → PV mode` → **ja** (factory default is `nee`).
2. `Menu → INST. MENU → PV MODE → PRIORITY` — `ja` lets the PV signals override
   eco/frost modes and the programmed time slots; `nee` gives those priority.
3. Set `T °PV ECO` and `T °PV MAX` to the temperatures you want each mode to reach.

## Entities

| Entity | Type | Purpose |
|---|---|---|
| `Intuis PV Mode` | select | The control: `Off` / `PV ECO` / `PV MAX` |
| `Intuis Tank Temp` | sensor | DS18B20 tank temperature, 30 s interval |
| `Intuis PV ECO Minimum Hold` | number (config) | Minimum minutes PV ECO stays on before it can be released; default 60. Does not apply to PV MAX. |
| `Intuis PV ECO Contact` | binary sensor (diagnostic) | Actual state of relay channel 1 |
| `Intuis PV MAX Contact` | binary sensor (diagnostic) | Actual state of relay channel 2 |
| `Intuis PV Status` | text sensor (diagnostic) | Active mode, pending hold, probe faults |

All of them reach Home Assistant through the standard ESPHome native API; no
extra configuration is needed on the Home Assistant side.

## Behaviour

- **Escalation is immediate.** `Off → PV ECO` or `PV ECO → PV MAX` applies on the
  next loop pass, so the boiler can react to rising production without delay.
- **PV ECO is rate-limited on the way down.** Leaving PV ECO waits until it has
  been active for `Intuis PV ECO Minimum Hold` minutes. The boiler manual
  recommends at least one hour of PV activation, and this keeps passing clouds
  from short-cycling the compressor. The pending request is remembered and
  applied as soon as the hold expires; `Intuis PV Status` shows the countdown.
- **PV MAX is released immediately.** No firmware hold applies to it, so Home
  Assistant owns boost duration (15 min, 30 min, until-target, whatever you
  automate) and can cancel at any moment. This is deliberate: electric back-up
  heating should never be locked on by the firmware.
- One consequence worth knowing: because escalation is free and PV MAX has no
  hold, `PV ECO → PV MAX → Off` bypasses a running ECO hold. That is fine for a
  deliberate boost-then-stop sequence, but avoid using PV MAX as a way to shortcut
  the ECO hold.
- **PV mode does not survive a reboot.** The selector deliberately does not
  restore, so a power cut can never resume electric back-up heating unattended.
  Both relays restore OFF and stay OFF until the control loop runs.

## Interaction with the local supply-temperature path — read this

The DS18B20 is repurposed for the tank, but OpenQuatt's own
[`oq_local_sensors.yaml`](../openquatt/oq_local_sensors.yaml) still reads
`index: 0` of the same 1-Wire bus as `Water Supply Temp (DS18B20)`. With a single
probe on the bus, **that entity now reports tank temperature under a supply-water
name.** It is `disabled_by_default`, so it stays out of Home Assistant.

> [!CAUTION]
> Leave **Local Water Supply Temp Source** on `PT1000`. Selecting `DS18B20` would
> feed tank temperature into the Quatt's supply-water control path.

If you ever fit a second DS18B20 for supply water, pin both sensors by explicit
`address:` instead of `index:` — bus index is ordered by ROM address, not by
wiring order, so adding a probe can silently swap which one is read.

## Commissioning checklist

1. Flash and boot; confirm `Intuis Tank Temp` reports a plausible value. If it
   shows unavailable, check that the probe was connected **before** boot — a
   DS18B20 added later is only detected after a restart.
2. With the boiler still isolated from the relay outputs, step the selector
   through `PV ECO` and `PV MAX` and confirm the two diagnostic contact sensors
   follow, and that only ever one is on.
3. Verify relay polarity: no channel should be engaged while the mode is `Off`.
   Reset the board and confirm nothing latches — a brief click on channel 1
   during boot is the ROM output on `GPIO43` and is expected; channel 2 should
   stay silent if the 10 kΩ pull-down is fitted.
4. Connect the relay outputs to the boiler's PV connectors 1 and 2.
5. Select `PV MAX` and confirm the boiler's display shows photovoltaic mode
   active and raises its target temperature.
6. Confirm the timing asymmetry: from `PV ECO`, selecting `Off` should show a
   countdown in `Intuis PV Status` and keep contact 1 closed; from `PV MAX`,
   selecting `Off` should open both contacts on the next loop pass.

## Building firmware

Compilation runs in CI on this fork: **Actions → Dev Build → Run workflow →
branch `personal`**, then download
`openquatt-heatpump-controller-q-single-wifi.firmware.factory.bin` from the
resulting `dev-latest` release.

Validate locally before pushing — `scripts/dev.py` does not run on Windows, but
these do:

```bash
esphome config configs/heatpump_controller_q/single_wifi.yaml
python scripts/check_style_consistency.py
python scripts/check_docs_consistency.py
```
