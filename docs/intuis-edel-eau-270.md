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
| `T`: `+3.3V` / `GND` / `DATA` | — | DS18B20 **inside** the tank, next to the boiler's own sensor |

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
| `Intuis PV ECO Contact` | binary sensor (diagnostic) | Actual state of relay channel 1 |
| `Intuis PV MAX Contact` | binary sensor (diagnostic) | Actual state of relay channel 2 |
| `Intuis PV Status` | text sensor (diagnostic) | Active mode and probe faults |

All of them reach Home Assistant through the standard ESPHome native API; no
extra configuration is needed on the Home Assistant side.

## Behaviour

- **Mode changes apply on the next loop pass, in both directions.** The firmware
  imposes no hold or rate limit of any kind, so Home Assistant can assert or
  withdraw a mode at any moment.
- **All timing lives in Home Assistant.** Scheduling, minimum run length,
  anti-cycling hysteresis and boost duration are decided there, not here. The
  firmware only guarantees that the requested mode is applied exclusively.
  Earlier revisions carried a configurable PV ECO minimum-hold timer; it was
  removed because it could defer a Home Assistant `Off` and overrun a planned
  window, and the scheduler now owns that concern.
- **PV mode does not survive a reboot.** The selector deliberately does not
  restore, so a power cut can never resume electric back-up heating unattended.
  Both relays restore OFF and stay OFF until the control loop runs.

## Reading the tank sensor

The probe sits inside the tank beside the boiler's own sensor, so there is no
strap-on lag — but it is near the condenser. **While the compressor runs it reads
roughly 3 °C high**, settling back over about 10–12 minutes after it stops
(measured: −3.6 °C and −3.1 °C on two stops). Stopping at 55 °C indicated
therefore stores about 52 °C.

It is also **mid-height**, so a single shower barely moves it: cold water enters
at the bottom and the stratification is stable. The reading holds steady and then
drops sharply once the thermocline passes the probe — observed once as 46 → 32 °C
within an hour. Consequence: stored energy is over-estimated straight after a
draw. See `intuis-ha-control-logic.md` for how the planner copes.

## Keeping the floor loop warm — the loop guard

The Edel Eau is **water-source**: it takes its heat from the floor-heating loop,
not from outside air. So every hour it runs, the loop gets colder.

That is fine in winter, when the Quatt is heating anyway and simply replaces the
heat. It is a problem in autumn: the house sits above the thermostat setpoint, so
the Quatt idles, nothing refills the loop, and the floor drifts down toward the
boiler's own suction temperature — measured here at just above **18 °C**. That is
mildly uncomfortable underfoot, and it is also the point where the Edel Eau gives
up on its heat pump and switches to its **electric element**. So an unattended
loop costs both comfort and efficiency.

The **loop guard** answers that by running one heat pump at compressor level 1
while the boiler is drawing, which is what the official Quatt Full Electric does.
It lives in
[`oq_loop_guard_logic.h`](../openquatt/includes/control/oq_loop_guard_logic.h)
with its runtime glue in
[`oq_loop_guard_runtime.h`](../openquatt/includes/control/oq_loop_guard_runtime.h),
called from the Power House runtime. Settings and behaviour are documented in
[Power House](power-house.md#lusbeveiliging-optioneel-fork); what matters here is
the wiring between the two features:

- This package sets `oq_ph_loop_guard_request` whenever PV ECO or PV MAX is
  asserted, and clears it otherwise. That is the **only** coupling, and it points
  one way: the Intuis package writes a flag the strategy owns. The strategy never
  references anything Intuis-specific, so profiles without this package still
  build and behave identically.
- Asserting the request does **not** start a heat pump. The guard has its own
  enable switch, its own thresholds, and defers entirely to real house demand.
- Because the request follows PV mode, **every planned PV window will also tend to
  start a compressor**, and so will a boost. Budget for that when reading the
  scheduler's cost estimates — see `intuis-ha-control-logic.md`, whose price model
  still assumes the boiler is the only load.

Measured on 2 September 2026: level 1 delivered **2740 W** of thermal output while
the boiler drew about **340 W** electrical.

How much the boiler takes *out of the loop* is `E × (COP − 1)`, so it depends on
the instantaneous COP rather than being a fixed figure. At the unit's rated COP of
3.5–4.4 that is roughly **850–1300 W**, and it falls as the tank heats: a recovery
from a cold tank sits near the top of that range, a 47 → 52 °C top-up well below
it. Do not use the whole-cycle COP the Home Assistant estimator reports for this —
it is a lower bound, biased down by heat the mid-height probe cannot see.

Either way the guard over-supplies the loop by roughly a factor of two, because
level 1 is the smallest step the compressor has. It therefore settles into a duty
cycle around the release threshold rather than running continuously, which the
hysteresis and `Loop guard recheck` between them are what bound.

The deliberate limitation: gating on PV mode means the guard does nothing when the
loop cools from standing loss, or when the boiler runs on its own internal time
slots rather than on our contacts. Relaxing that is a one-line change once there
is enough data to justify it.

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

## Update channels

Since v0.49.0 the WiFi and Ethernet entrypoints are thin aliases of a single
topology config, so the manifest URLs live in just
`configs/heatpump_controller_q/duo.yaml` and `single.yaml`. This fork repoints
the three `dev_*` manifests in each of those and leaves the `main_*` ones alone:

| Channel | Manifest source | Contains Intuis support |
|---|---|---|
| `main` | `OpenQuatt/OpenQuatt` official releases | **No** — installing it removes this feature |
| `dev` | `Rickvdt/OpenQuatt` `dev-latest` | Yes — builds from this fork's working branch |

So the built-in updater becomes the normal deployment path: commit to `personal`,
run Dev Build, then install from the web app's update panel.

> [!IMPORTANT]
> After flashing, set **Firmware Update Channel** to `dev` in the web app.
> That select uses `restore_value: true`, so a device that was previously on
> `main` stays on `main` even after flashing a dev build — and on `main` it will
> offer official firmware that overwrites the Intuis support.

Only the Heatpump Controller Q configs were repointed. The `waveshare` and
`heatpump_listener` configs still reference upstream on both channels, since that
is not the hardware this fork runs.

Note that upstream's dev counter runs far ahead of this fork's (`dev.714` versus
`dev.1` at the time of writing), which is another reason not to leave the channel
pointing at upstream dev.

## Building firmware

Compilation runs in CI on this fork: **Actions → Dev Build → Run workflow →** the
working branch, then download
`openquatt-heatpump-controller-q-duo.firmware.factory.bin` from the resulting
`dev-latest` release. Since v0.49.0 the assets are named by topology only, with
no separate WiFi and Ethernet builds.

Validate locally before pushing — `scripts/dev.py` does not run on Windows, but
these do:

```bash
esphome config configs/heatpump_controller_q/duo.yaml
python scripts/check_style_consistency.py
python scripts/check_docs_consistency.py
```

v0.49.0 also added host-side contract tests, which do run on Windows and cover
the control logic this fork touches:

```bash
python -m pytest scripts/tests -q
```
