#pragma once

// Loop guard (fork addition).
//
// Keeps the heating loop from being chilled by an appliance that draws heat out
// of it while the house itself needs none - a water-source DHW heat pump, for
// instance. The Quatt then runs one compressor at level 1, which is what the
// official Full Electric does. See docs/power-house.md.
//
// The hard part is that the loop temperature sensor sits on the outdoor unit, so
// with the circulation pump stopped it measures stagnant water near outdoor
// ambient rather than the floor loop. Measured 2026-09-01: 16.3 C with no flow
// against 21.5 C once flow had settled, an error that tracks ambient. A cold
// reading at standstill is therefore only an invitation to look, never a
// decision; the release test waits until flow has been present long enough for
// the reading to mean something.
//
// Pure logic, no id() and no ESPHome dependencies, so it can be exercised on the
// host. The runtime glue lives in oq_power_house_runtime.h.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace oq_loop_guard {

struct Input {
  uint32_t now_ms = 0;
  bool enabled = false;        // master enable switch
  bool heat_draw = false;      // an appliance is drawing heat from the loop
  bool protections_ok = true;  // water-temperature limiter and flow faults clear
  float loop_c = NAN;          // loop return temperature
  float flow_lph = NAN;        // flow past the loop temperature probe
  float min_flow_lph = 250.0f;
  float engage_c = 19.0f;
  float release_c = 21.0f;
  float settle_s = 120.0f;
  float recheck_min = 30.0f;
};

struct State {
  bool active = false;
  uint32_t flow_ok_since_ms = 0;
  uint32_t lockout_until_ms = 0;
};

struct Output {
  State next{};
  bool active = false;
  bool permitted = false;
  bool flow_ok = false;
  bool reading_valid = false;
  uint32_t settled_s = 0;
  uint32_t settle_target_s = 0;
  uint32_t recheck_remaining_min = 0;
  bool locked_out = false;
};

inline Output decide(const Input& in, const State& state) {
  Output out;
  out.next = state;

  out.flow_ok = std::isfinite(in.flow_lph) && in.flow_lph >= in.min_flow_lph;
  if (!out.flow_ok) {
    out.next.flow_ok_since_ms = 0;
  } else if (out.next.flow_ok_since_ms == 0) {
    // Zero doubles as "no flow yet", so never store it as a real timestamp.
    out.next.flow_ok_since_ms = in.now_ms == 0 ? 1u : in.now_ms;
  }

  const uint32_t settle_ms = static_cast<uint32_t>(std::max(0.0f, in.settle_s) * 1000.0f);
  const uint32_t held_ms = out.flow_ok ? in.now_ms - out.next.flow_ok_since_ms : 0u;
  out.settled_s = held_ms / 1000u;
  out.settle_target_s = settle_ms / 1000u;
  out.reading_valid = out.flow_ok && held_ms >= settle_ms;

  // Signed difference so the comparison survives the millis() wrap.
  out.locked_out = static_cast<int32_t>(in.now_ms - state.lockout_until_ms) < 0;
  out.recheck_remaining_min =
      out.locked_out ? static_cast<uint32_t>(state.lockout_until_ms - in.now_ms) / 60000u + 1u : 0u;

  out.permitted = in.enabled && in.heat_draw && in.protections_ok && std::isfinite(in.loop_c);
  if (!out.permitted) {
    out.next.active = false;
  } else {
    // Hold the release threshold above the engage threshold whatever the user
    // typed, so the pair can never collapse into chatter.
    const float release_c = std::max(in.release_c, in.engage_c + 0.5f);
    if (!state.active) {
      if (!out.locked_out && in.loop_c < in.engage_c) out.next.active = true;
    } else if (out.reading_valid && in.loop_c > release_c) {
      out.next.active = false;
      // The stagnant reading falls back below the engage threshold within
      // minutes, so hold off re-checking or the guard restarts on every pass.
      out.next.lockout_until_ms = in.now_ms + static_cast<uint32_t>(std::max(0.0f, in.recheck_min) * 60000.0f);
    }
  }

  out.active = out.next.active;
  return out;
}

// Power to request while the guard is active: the level-1 thermal output plus
// enough margin to arm the supervisor's Power House low-load latch, which
// compares the requested power against exactly that figure. Asking for less is
// correctly refused, because one compressor step would only cycle.
inline float requested_power_w(float level1_thermal_w) {
  float p1 = level1_thermal_w;
  if (!std::isfinite(p1) || p1 <= 0.0f) p1 = 1800.0f;
  // The latch arms at on_factor * P1 (on_factor <= 1.10) but no lower than
  // off_w + min_hysteresis (<= 0.90 * P1 + 400 W); clear both forms.
  return std::max(p1 * 1.20f, p1 + 450.0f);
}

struct Plan {
  bool owns_request = false;
  float requested_w = NAN;
  int demand = 0;
};

// What to ask for while the guard is active.
//
// owns_request is true only when the guard, not the house model, raised the
// request. That distinction is what keeps real heat demand in charge: when the
// house asks for more than the guard needs, the guard changes nothing and the
// dispatcher is left entirely alone.
//
// Demand has to rise along with the request, because the request is separately
// capped at rated_power * f / demand_max further down the chain and would
// otherwise be clamped straight back below the low-load threshold.
inline Plan plan_request(bool active, int raw_demand, float model_requested_w, float level1_thermal_w, float rated_w,
                         int demand_max_f) {
  Plan out;
  out.demand = raw_demand;
  if (!active) return out;
  out.requested_w = requested_power_w(level1_thermal_w);
  out.owns_request = !std::isfinite(model_requested_w) || model_requested_w < out.requested_w;
  int needed = 1;
  if (std::isfinite(rated_w) && rated_w > 0.0f && demand_max_f > 0)
    needed = static_cast<int>(std::ceil(demand_max_f * (out.requested_w / rated_w)));
  out.demand = std::max(raw_demand, std::max(1, std::min(demand_max_f, needed)));
  return out;
}

// One-line status for the diagnostic text sensor. An idle guard is otherwise
// opaque: it may be disabled, waiting on a heat draw, still flushing the sensor,
// or locked out after a settled reading showed the loop was already fine.
inline void format_status(char* buf, size_t n, const Output& g, bool enabled, bool heat_draw, float loop_c,
                          float engage_c, float ask_w) {
  if (!enabled)
    snprintf(buf, n, "Disabled");
  else if (!heat_draw)
    snprintf(buf, n, "Idle - no heat draw on the loop");
  else if (!g.permitted)
    snprintf(buf, n, "Blocked by a protection or a missing reading");
  else if (g.active && g.reading_valid)
    snprintf(buf, n, "Active - loop %.1f C, asking %.0f W", loop_c, ask_w);
  else if (g.active && !g.flow_ok)
    snprintf(buf, n, "Active - waiting for flow, asking %.0f W", ask_w);
  else if (g.active)
    snprintf(buf, n, "Active - flushing sensor %u/%u s", static_cast<unsigned>(g.settled_s),
             static_cast<unsigned>(g.settle_target_s));
  else if (g.locked_out)
    snprintf(buf, n, "Loop was warm - rechecking in %u min", static_cast<unsigned>(g.recheck_remaining_min));
  else
    snprintf(buf, n, "Armed - loop %.1f C, engage below %.1f C", loop_c, engage_c);
}

}  // namespace oq_loop_guard
