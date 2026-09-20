# Intuis DHW — Home Assistant control logic

Everything learned and built on the **Home Assistant side**: the planner, the
executor, the boost, and the measured thermal behaviour that drives their
settings. The firmware, GPIO and relay work is out of scope here — see
[intuis-edel-eau-270.md](intuis-edel-eau-270.md) for that.

Status: working and live as of **2026-08-31**. Lives only in Home Assistant, not
in this repo — dashboard `control-openquatt`, view path `heatpump-boiler`.

---

## 1. Architecture

Three automations and a set of `input_*` helpers. No template sensors and no
YAML: this MCP server exposes no YAML-writing tool, and flow-helper listing needs
a custom component that is not installed. Everything is simple helpers plus
automations through the config API.

```
                 ┌─────────────────────────────┐
   forecasts ───▶│  Boiler: plan PV window     │──▶ input_datetime.boiler_window_start / _end
   prices    ───▶│  (optimiser, every 15 min)  │──▶ input_number.boiler_window_minutes
   tank temp ───▶│                             │──▶ input_text.boiler_window_summary
                 └─────────────────────────────┘
                              │  native time triggers
                              ▼
                 ┌─────────────────────────────┐
                 │  Boiler: run PV window      │──▶ select.openquatt_intuis_pv_mode
                 │  (executor, self-extending) │──▶ input_boolean.boiler_window_running
                 └─────────────────────────────┘
                              ▲
                 ┌─────────────────────────────┐
   button    ───▶│  Boiler: boost              │──▶ select.openquatt_intuis_pv_mode
                 │  (manual override)          │──▶ input_boolean.boiler_boost_active
                 └─────────────────────────────┘
```

### Single-writer ownership

`select.openquatt_intuis_pv_mode` has exactly one writer at any moment:

| State | Who owns the mode |
|---|---|
| Boost active | **Boost.** Executor has a top-level condition `boost_active = off`, so it is inert. Planner is also blocked. |
| Window running | **Executor.** Planner is blocked by `window_running = off` condition ("freeze once started"). |
| Otherwise | Nobody writes; planner only writes helpers. |

Precedence is therefore **boost > executor > planner**, enforced natively with
`condition: state` rather than templates.

### The 1970 sentinel

"No plan" is written as `1970-01-01 00:00:00` to both window datetimes. A `time`
trigger on a past datetime never fires, so the no-plan state is **inert by
construction** rather than needing a guard. The dashboard renders it as
"Planned window: none".

---

## 2. Helpers

**Interface**

| Helper | Purpose |
|---|---|
| `input_boolean.boiler_pv_auto` | Master enable for the scheduler |
| `input_select.boiler_pv_strategy` | `Solar` / `Price` / `Combined` |
| `input_number.boiler_pv_solar_weight` | Blend weight, **Combined only** |
| `input_number.boiler_target_temp` | Target tank temperature |
| `input_number.boiler_heat_rate` | Assumed °C/h while PV ECO runs |
| `input_number.boiler_cooling_rate` | Assumed standing loss °C/h |
| `input_number.boiler_restart_deadband` | Hysteresis, **same-day only** |
| `input_number.boiler_min_run_minutes` | Shortest window the planner will book |
| `input_number.boiler_max_hours` | Safety cap on run length |
| `input_number.boiler_min_temp` | Emergency floor |
| `input_datetime.boiler_ready_by` | Daily deadline (time only) |

**Boost**

`input_button.boiler_boost`, `input_boolean.boiler_boost_active`,
`input_number.boiler_boost_threshold`, `input_number.boiler_boost_max_minutes`,
`timer.boiler_boost`.

**Computed by the planner**

`input_datetime.boiler_window_start` / `_end`,
`input_number.boiler_window_minutes`, `input_text.boiler_window_summary`,
`input_boolean.boiler_window_running`.

> No helper carries `initial:` — that would disable last-state restore and reset
> it on every HA restart.

---

## 3. The planner

Runs every 15 minutes, plus on any interface change, any forecast/price refresh,
HA start, and window/boost release. Blocked while a window or boost is running.

The whole optimiser is one Jinja block in the automation's `variables:` — the one
place the best-practice guide permits templates alongside `data:`.

### Data grid

- **15-minute slots** from the next quarter-hour, up to 192 slots (48 h).
- Solar: Solcast `detailedHourly` from the *today* **and** *tomorrow* sensors
  concatenated, so the series crosses midnight. Hourly kW applied to each quarter.
- Price: EPEX Spot `sensor.epex_spot_data_total_price` `data` attribute, native
  15-minute resolution. **`total_price`, not `market_price`** — it includes taxes
  and surcharges, so it is the number that actually drives a heat-now decision.
- **Horizon clamp:** only slots where *both* solar and price exist are kept. This
  self-truncates gracefully before EPEX publishes the next day (see §6).

### Sizing each candidate

Each candidate start is sized from the tank it will actually find, not the tank
now:

```
predicted = tank_now − cooling_rate × hours_until_that_start
deficit   = target − predicted
minutes   = clamp( ceil(deficit / heat_rate × 60), min_run, max_hours × 60 )
```

Duration is in **minutes**, and the end time is start + minutes exactly. Starts
land on quarter-hours because that is the finest resolution the price data
supports; finer would be invented precision.

### Same-day deadband

`deficit < deadband` suppresses a candidate — but **only for candidates starting
today**. Candidates on a later calendar day ignore the deadband entirely and are
sized to reach target properly.

Rationale: the deadband exists to stop a compressor start after every shower,
which is a near-term concern. It should not muzzle tomorrow's plan.

### Deadline passes

Candidates must finish before a deadline. Three passes, first non-empty wins:

1. Next occurrence of `Ready by` — normal case.
2. `+1 day` — tagged **(next cycle)**.
3. No deadline — tagged **(after ready-by)**, meaning the deadline is
   unsatisfiable and it heats as soon as it is allowed to.

If nothing qualifies at all but heat is needed now, it falls back to starting
**immediately** (not the next hour boundary), tagged **(late, started now)**.

### Scoring

For each candidate, solar and price are reduced to **means over the window**,
then min-max normalised across candidates:

| Strategy | Objective |
|---|---|
| `Solar` | maximise normalised mean solar |
| `Price` | maximise `1 − normalised mean price` |
| `Combined` | `w · solar_n + (1−w) · (1 − price_n)` |

> **Means, not sums.** Windows now have different lengths, so a sum would reward
> a longer window purely for being longer, biasing selection toward later,
> colder-tank slots. Means make unequal windows comparable. For equal-length
> windows the ranking is unchanged.

### Emergency floor

`tank < min_temp` bypasses everything — price, sun and the deadline — and starts
at the next minute. Only path that ignores the deadline.

### Summary output

`input_text.boiler_window_summary` reports **all three strategies' picks**, not
just the active one, so the choice is informed:

```
Solar Mon 12:30-13:57 (87 min) (next cycle) | Solar Mon 12:30/87m | Price Mon 11:30/84m | Comb Mon 12:30/87m
```

---

## 4. The executor

Native `trigger: time, at: input_datetime.…` on the planner's output — no
polling, no template triggers.

| Trigger | Behaviour |
|---|---|
| `start` | Select `PV ECO`, set `window_running` |
| `stop` | **Extend or stop** — see below |
| `target` | Tank crossed target → `Off` (early finish) |
| `abort` | `boiler_pv_auto` → off → `Off` |
| `boot` | Release the window; planner re-plans from current temperature |

### Self-extending window

At `window_end`, four guards decide extend vs stop:

1. `window_running` is on (native `state`)
2. tank below target (native `numeric_state`)
3. before `Ready by` (native `condition: time` with `before:` an `input_datetime`
   — a one-sided `before` anchors at midnight, which is exactly the semantics)
4. elapsed since `window_start` under `Max hours` (template — the pattern the
   best-practice guide itself prescribes for durations)

All pass → push `window_end` **+15 min** and bump `window_minutes`. Pushing the
end re-arms the `stop` trigger, so it nudges repeatedly until target, deadline or
the cap. Any guard fails → stop as before.

This converts a window from *a fixed slot* into *a start time plus a bounded
commitment to finish*, which makes the heat-rate estimate far less critical: get
it wrong low and the run simply lasts longer.

Only `PV ECO` is ever selected. `PV MAX` is never automated.

---

## 5. The boost

Manual override for heavy demand, independent of `boiler_pv_auto`.

| Event | Action |
|---|---|
| Press, tank < threshold | **PV MAX** (heat pump + electric back-up) |
| Press, tank ≥ threshold | **PV ECO** |
| Crosses up through threshold | hand over to **PV ECO** |
| Drops back under threshold | return to **PV MAX**, repeatably |
| Target reached / Stop / timer expiry | end boost, mode `Off` |

Boost **ignores the restart deadband** — it runs to target, not to
target−deadband. It also releases any running window and inhibits the planner
until it ends.

`timer.boiler_boost` is a safety backstop: if the tank probe ever goes
unavailable, the target trigger can never fire and PV MAX would otherwise leave
the resistive element running indefinitely.

---

## 6. Measured thermal behaviour

The important part — the settings are only as good as these numbers.
**Measured** unless marked otherwise.

### Standing loss

| Segment | Drop | Duration | Rate |
|---|---|---|---|
| 49.6 → 46.3 °C | 3.3 °C | 11 h | **0.30 °C/h** |
| 47.1 → 44.2 °C | 2.9 °C | 21 h | **0.14 °C/h** |

Faster when hotter, as expected from a larger ΔT to ambient. `cooling_rate`
default **0.2 °C/h**.

### Plume offset — the sensor overstates while heating

The probe sits inside the tank next to the boiler's own sensor, near the
condenser. While the compressor runs it reads locally-warmed water; when it
stops, the tank equalises and the reading falls.

| Stop | Peak indicated | Settled | Offset | Settling |
|---|---|---|---|---|
| 2026-08-30 15:04 | 52.56 | ~48.9 | **−3.6 °C** | ~12 min |
| 2026-08-30 16:23 | 55.13 | ~52.0 | **−3.1 °C** | ~11 min |

Consequence: stopping at 55 **indicated** stores roughly 52 **settled**. Not a
safety issue, but "55" is optimistic by ~3 °C.

### Heat rate is not a constant

| Run | Duration | Indicated rise | Apparent rate |
|---|---|---|---|
| 2026-08-30 15:21→16:23 | 62 min | 48.9 → 55.1 | **~6.0 °C/h** |
| 2026-08-31 12:00→13:27 | 87 min | 49.8 → 53.0 | **~2.2 °C/h** |

Settled-to-settled on the 08-30 run gives a **true bulk rate of ~3.1 °C/h**. The
apparent rate is inflated because the run starts from a settled reading and stops
on an inflated one:

```
apparent ≈ true_rate + plume_offset / window_hours
```

so ~6 °C/h for a 1 h window, ~4.5 for 2 h, ~4 for 3 h. **No single constant is
correct.** The 08-31 run at 2.2 °C/h was slower still, consistent with a colder
floor-loop source — the Edel Eau draws from the floor-heating return and its
output tracks that temperature (below 18 °C the boiler abandons the heat pump
entirely and uses the element).

This is the core justification for the self-extending window: compensating for a
wrong estimate beats chasing the right one.

### Stratification — the sensor is mid-height

Cold water enters at the bottom during a draw; hot sits above. That is
**gravitationally stable** and does not self-mix. Erosion mechanisms are all slow:
water conduction smears the thermocline only ~6 cm over 8 h
(√(1.4×10⁻⁷ × 28800)), plus weak wall conduction and standing loss.

Worked example: 270 L at 50 °C, draw 50 L, 50 L of 10 °C mains enters →
fully-mixed average would be **42.6 °C**, but you will measure nothing like it.
You get ~220 L at ~50 °C over ~50 L at ~10 °C, and by morning that boundary has
softened, not vanished.

Evidence in our own history: the tank went **46 → 32 °C inside one hour** — a
cliff, not a decay. That is the thermocline passing the probe.

Consequences:

- **Stored energy is overestimated after a draw.** The probe stays hot while a
  fifth of the tank is cold, so the deadband sees "no action needed" and `need`
  is under-called.
- **"Is there hot water now" is well estimated** — the next draw comes off the
  hot top. But the signal fails abruptly rather than gracefully.

### Electrical context

`sensor.inverter_grid_power`: **negative = export**, confirmed over 7 days.
Export peaks −1.2 to −3.2 kW; PV peaks 3.2–4.0 kW (≈4 kWp). `sensor.p1_meter_vermogen`
is **net grid**, not house consumption (house = P1 + PV).

---

## 7. Bugs found and fixed

| Symptom | Cause | Fix |
|---|---|---|
| Reheated every ~30 min after reaching target | No hysteresis; the plume offset made the tank read ~3 °C lower once settled, so `ceil()` booked a fresh hour | Restart deadband |
| Planned-window band covered the whole night on the chart | apexcharts-card defaults `extend_to: end`, stretching the last point to the graph edge | `extend_to: false` + `curve: stepline` |
| "Deadline missed" but it waited for the next hour | Fallback used the hour grid instead of starting immediately | Late path starts at the next minute |
| Window sizes always whole hours | Duration derived in hours | 15-min grid, minutes throughout |
| Plan showed `1/1/1970` | Deadline unsatisfiable → sentinel, and the raw `input_datetime` tiles rendered it | Three deadline passes; formatted markdown (also read-only, so the plan can't be edited by a stray tap) |
| Longer windows unfairly favoured | Solar scored as a sum | Solar scored as a mean |

---

## 8. Known gaps

- **The same-day deadband boundary is midnight.** A window planned for "tomorrow"
  becomes "today" at 00:00 and the deadband then applies to it, which can cancel
  a plan that was correct when made. Observed 2026-08-31: with the deadband at
  10 °C and the tank at 50 °C, nothing was plannable at all.
- **Solar score uses raw forecast production, not surplus.** On a sunny day with
  heavy house load, "lots of sun" does not guarantee export. No load forecast
  exists to fix this properly.
- **`need` is under-called after draws** because of mid-height sensing. A second
  DS18B20 low in the tank would allow real stored-energy estimation — the 1-Wire
  bus supports multiple sensors, provided both are pinned by explicit `address:`
  rather than `index:`.
- **Heat rate is a single constant** for a quantity that is not constant. The
  self-extension compensates; learning it from completed runs would be better.
- **PV MAX is never automated** — deliberate. It engages resistive heat and rarely
  fits inside a 2–3 kW surplus.
- **Lost-update risk.** Lovelace saves are whole-config replaces. Editing the
  dashboard in the UI while an agent writes to it silently discards the agent's
  changes (this destroyed the boost section once). Agent writes use `config_hash`
  optimistic locking, so the protection only runs one way.

---

## 9. Operational notes

- The `ha-mcp` server enforces strict best-practices mode: gated write tools need
  an acknowledgment key that **rotates hourly** and is published in the skill
  reference content.
- Verify planner changes with `ha_eval_template` before deploying — the whole
  optimiser can be pasted in and rendered against live state.
- Jinja gotcha hit twice: assignments inside a `for` loop do not escape it.
  Accumulate through a `namespace`.
- EPEX publishes next-day prices early afternoon CET. Before that the planning
  horizon is legitimately short and the horizon clamp handles it — a "no window"
  result in the morning may simply mean tomorrow is not visible yet.
- Solcast runs on an API budget (10 calls/day). The solar side refreshes a
  handful of times daily, not continuously.
