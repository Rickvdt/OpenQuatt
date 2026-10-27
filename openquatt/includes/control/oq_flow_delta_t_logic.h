#pragma once

// Delta-T flow control (fork addition).
//
// Optional third flow mode next to "Flow Setpoint" and "Manual PWM". Instead of
// a fixed heating flow, the setpoint is moved slowly so the water temperature
// rise across each running heat pump approaches a target. A fixed flow is only
// right at one load: at low load the pump moves more water than the heat needs,
// and at high load the large temperature rise forces a hotter supply, which
// costs compressor efficiency.
//
// Heating only. In cooling the strategy pins the supply near the dew-point
// floor, so a lower flow mainly reduces the delivered cooling instead of saving
// energy; cooling keeps its fixed setpoint.
//
// The flow PI stays the owner of the pump. This only supplies the setpoint, and
// the PI's own setpoint ramp smooths every step.
//
// Pure logic, no id() and no ESPHome dependencies, so it can be exercised on the
// host. The runtime glue lives in oq_flow_delta_t_runtime.h.

#include <math.h>
#include <stdint.h>

namespace oq_flow_delta_t {

// Pacing is deliberately slow. The gain is in steady operation and heat demand
// changes over hours, so a slow loop loses almost nothing while staying well
// clear of the 10 s flow PI, the compressor and slow floor heating.
constexpr uint32_t kStepIntervalMs = 120000u;  // at most one step per 2 minutes
constexpr uint32_t kSettleMs = 300000u;        // after a start, or a unit joining or leaving
constexpr uint32_t kDefrostHoldMs = 600000u;   // after any defrost
constexpr int kMinSamples = 6;                 // at least a minute of 10 s samples per step
constexpr float kDeadbandK = 0.3f;             // ignore differences at sensor-error level
constexpr float kGain = 0.5f;                  // move half of the gap per step
constexpr float kMaxStepLph = 100.0f;          // and never more than this per step
constexpr float kLowFlowMarginLph = 50.0f;     // floor kept above the low-flow fault threshold
constexpr float kShortfallLph = 75.0f;         // flow this far below the setpoint: pump at its limit
constexpr float kCeilingMarginLph = 25.0f;
constexpr float kMinUsableRiseK = 0.2f;  // below this a sample says nothing about the flow

enum class Reason : uint8_t {
  OFF = 0,      // mode not selected
  NOT_HEATING,  // no heat pump heating in CM2
  SETTLING,     // waiting for stable temperatures
  DEFROST,      // defrost, or the hold after it
  NO_DATA,      // missing or stale temperatures or flow
  FLOW_FAULT,   // low-flow fault active
  WATER_LIMIT,  // water temperature protection is cutting power: maximum flow
  ON_TARGET,
  ADJUSTING,
  AT_MINIMUM,
  AT_MAXIMUM,
  PUMP_LIMIT,  // the pump cannot deliver more flow in this installation
};

inline const char* reason_text(Reason reason) {
  switch (reason) {
    case Reason::OFF:
      return "Off";
    case Reason::NOT_HEATING:
      return "Waiting for heat pump heating";
    case Reason::SETTLING:
      return "Settling";
    case Reason::DEFROST:
      return "Defrost - fixed flow";
    case Reason::NO_DATA:
      return "No reliable temperatures - fixed flow";
    case Reason::FLOW_FAULT:
      return "Flow fault - fixed flow";
    case Reason::WATER_LIMIT:
      return "Water temperature limit - maximum flow";
    case Reason::ON_TARGET:
      return "On target";
    case Reason::ADJUSTING:
      return "Adjusting";
    case Reason::AT_MINIMUM:
      return "At minimum flow";
    case Reason::AT_MAXIMUM:
      return "At maximum flow";
    case Reason::PUMP_LIMIT:
      return "Pump at its limit";
  }
  return "Off";
}

struct Unit {
  bool heating = false;      // compressor running in heating mode
  bool defrost = false;      // defrost flag set or 4-way valve reversed
  bool temps_fresh = false;  // inlet and outlet recently received and valid
  float in_c = NAN;
  float out_c = NAN;
  int level = 0;
};

struct Input {
  uint32_t now_ms = 0;
  bool enabled = false;       // Flow Control Mode is Delta T
  bool heating_mode = false;  // CM2, heat pump heating, outside service tasks
  Unit units[2]{};
  int unit_count = 1;             // 1 for a single, 2 for a Duo
  float flow_lph = NAN;           // measured flow, the PI process value
  float base_setpoint_lph = NAN;  // normal Flow Setpoint: start and fallback flow
  float target_k = 3.0f;          // temperature rise per running heat pump
  float min_lph = 500.0f;
  float max_lph = 1200.0f;
  float lowflow_threshold_lph = 400.0f;
  bool water_limit_active = false;  // water temperature protection is cutting power
  bool flow_fault = false;          // low-flow fault active
};

struct State {
  bool active = false;
  float setpoint_lph = NAN;
  bool settling = false;
  uint32_t settle_until_ms = 0;
  bool defrost_hold = false;
  uint32_t defrost_until_ms = 0;
  uint32_t last_step_ms = 0;
  uint8_t unit_mask = 0;
  int levels[2] = {0, 0};
  float rise_sum_k = 0.0f;
  int rise_count = 0;
  float ceiling_lph = NAN;
  Reason reason = Reason::OFF;
  float last_rise_k = NAN;
};

struct Output {
  State next{};
  float setpoint_lph = NAN;  // what the PI should track
  bool controlling = false;  // false: the base setpoint is passed through untouched
  int counted_units = 0;
  float min_lph = NAN;
  float max_lph = NAN;
};

// True while now is before until. Only meaningful for a deadline that was
// actually set; callers keep an explicit flag for that, because a zero
// deadline compared this way reads as "in the future" after 24.8 days uptime.
inline bool before(uint32_t now_ms, uint32_t until_ms) { return static_cast<int32_t>(now_ms - until_ms) < 0; }

inline float clampf(float value, float low, float high) { return value < low ? low : (value > high ? high : value); }

struct Band {
  float min_lph = NAN;
  float max_lph = NAN;
  bool valid = false;
};

// The usable flow band. The maximum never sits below the normal Flow Setpoint,
// so at full power this mode never moves less water than the fixed setpoint
// would. A lower maximum would push the supply up until the water temperature
// protection cut power, silently and exactly when it is coldest. The minimum
// stays clear of the low-flow fault and below the maximum.
inline Band band(const Input& in) {
  Band out;
  if (!isfinite(in.base_setpoint_lph) || in.base_setpoint_lph <= 0.0f || !isfinite(in.min_lph) ||
      !isfinite(in.max_lph) || !isfinite(in.lowflow_threshold_lph))
    return out;
  out.max_lph = fmaxf(in.max_lph, in.base_setpoint_lph);
  out.min_lph = fminf(fmaxf(in.min_lph, in.lowflow_threshold_lph + kLowFlowMarginLph), out.max_lph);
  out.valid = true;
  return out;
}

inline void reset_cycle(State& state) {
  state.active = false;
  state.setpoint_lph = NAN;
  state.settling = false;
  state.rise_sum_k = 0.0f;
  state.rise_count = 0;
  state.ceiling_lph = NAN;
  state.unit_mask = 0;
  state.levels[0] = 0;
  state.levels[1] = 0;
}

inline Output pass_through(Output out, const Input& in, Reason reason) {
  reset_cycle(out.next);
  out.next.reason = reason;
  out.setpoint_lph = in.base_setpoint_lph;
  out.controlling = false;
  return out;
}

inline Output decide(const Input& in, const State& state) {
  Output out;
  out.next = state;
  out.setpoint_lph = in.base_setpoint_lph;
  const Band limits = band(in);
  out.min_lph = limits.min_lph;
  out.max_lph = limits.max_lph;

  if (!in.enabled) return pass_through(out, in, Reason::OFF);
  if (!in.heating_mode) return pass_through(out, in, Reason::NOT_HEATING);

  const int units = in.unit_count >= 2 ? 2 : 1;
  bool any_defrost = false;
  for (int i = 0; i < units; ++i) any_defrost = any_defrost || in.units[i].defrost;
  if (any_defrost) {
    // Defrost pulls heat back out of the water, so it gets the known fixed
    // flow, and the hold restarts the cycle cleanly afterwards.
    Output held = pass_through(out, in, Reason::DEFROST);
    held.next.defrost_hold = true;
    held.next.defrost_until_ms = in.now_ms + kDefrostHoldMs;
    return held;
  }
  if (out.next.defrost_hold) {
    if (before(in.now_ms, out.next.defrost_until_ms)) return pass_through(out, in, Reason::DEFROST);
    out.next.defrost_hold = false;
  }

  uint8_t mask = 0;
  int counted = 0;
  for (int i = 0; i < units; ++i) {
    if (!in.units[i].heating) continue;
    mask = static_cast<uint8_t>(mask | (1u << i));
    ++counted;
  }
  out.counted_units = counted;
  if (counted == 0) return pass_through(out, in, Reason::NOT_HEATING);
  if (in.flow_fault) return pass_through(out, in, Reason::FLOW_FAULT);
  if (!limits.valid || !isfinite(in.target_k) || in.target_k <= 0.0f || !isfinite(in.flow_lph))
    return pass_through(out, in, Reason::NO_DATA);
  for (int i = 0; i < units; ++i) {
    if (!in.units[i].heating) continue;
    if (!in.units[i].temps_fresh || !isfinite(in.units[i].in_c) || !isfinite(in.units[i].out_c))
      return pass_through(out, in, Reason::NO_DATA);
  }

  State& next = out.next;
  if (!next.active) {
    // A new cycle starts from the known fixed flow, then waits for the water
    // temperatures to mean something before the first step.
    next.active = true;
    next.setpoint_lph = clampf(in.base_setpoint_lph, limits.min_lph, limits.max_lph);
    next.settling = true;
    next.settle_until_ms = in.now_ms + kSettleMs;
    next.last_step_ms = in.now_ms;
    next.unit_mask = mask;
    for (int i = 0; i < 2; ++i) next.levels[i] = in.units[i].level;
    next.rise_sum_k = 0.0f;
    next.rise_count = 0;
    next.ceiling_lph = NAN;
    next.reason = Reason::SETTLING;
  } else if (mask != next.unit_mask) {
    // The second heat pump joining roughly halves the flow needed per kW, so
    // let the temperatures settle before acting on the new situation.
    next.unit_mask = mask;
    next.settling = true;
    next.settle_until_ms = in.now_ms + kSettleMs;
    next.rise_sum_k = 0.0f;
    next.rise_count = 0;
    next.reason = Reason::SETTLING;
  }
  bool level_changed = false;
  for (int i = 0; i < units; ++i) level_changed = level_changed || next.levels[i] != in.units[i].level;
  if (level_changed) {
    // A level change only restarts the averaging window. A full settle on every
    // level step would starve the controller under Power House modulation.
    for (int i = 0; i < 2; ++i) next.levels[i] = in.units[i].level;
    next.rise_sum_k = 0.0f;
    next.rise_count = 0;
  }

  // The live band may have moved under a running cycle.
  if (isfinite(next.ceiling_lph) && in.flow_lph > next.ceiling_lph + 2.0f * kCeilingMarginLph)
    next.ceiling_lph = NAN;  // the installation takes more water again, for instance valves opened
  const float upper =
      isfinite(next.ceiling_lph) ? fmaxf(limits.min_lph, fminf(limits.max_lph, next.ceiling_lph)) : limits.max_lph;
  next.setpoint_lph = clampf(next.setpoint_lph, limits.min_lph, limits.max_lph);

  out.controlling = true;
  if (in.water_limit_active) {
    // Flow before power: cooler supply water lets the protection relax so the
    // compressor keeps its power. Hold it there for a settle period afterwards
    // so stepping down again does not re-trigger the protection at once.
    next.setpoint_lph = limits.max_lph;
    next.settling = true;
    next.settle_until_ms = in.now_ms + kSettleMs;
    next.rise_sum_k = 0.0f;
    next.rise_count = 0;
    next.reason = Reason::WATER_LIMIT;
    out.setpoint_lph = next.setpoint_lph;
    return out;
  }
  if (next.settling) {
    if (before(in.now_ms, next.settle_until_ms)) {
      next.reason = Reason::SETTLING;
      out.setpoint_lph = next.setpoint_lph;
      return out;
    }
    next.settling = false;  // never compare against this deadline again
  }
  if (next.reason == Reason::SETTLING || next.reason == Reason::WATER_LIMIT) next.reason = Reason::ADJUSTING;

  // Average temperature rise per running heat pump. A Duo runs in series on one
  // shared flow, so the flow cannot put each unit on its own target; the
  // average is the compromise, and Power House usually runs both alike.
  float rise_k = 0.0f;
  for (int i = 0; i < units; ++i) {
    if (in.units[i].heating) rise_k += in.units[i].out_c - in.units[i].in_c;
  }
  rise_k /= static_cast<float>(counted);
  if (rise_k >= kMinUsableRiseK) {
    next.rise_sum_k += rise_k;
    next.rise_count++;
  }

  if (static_cast<uint32_t>(in.now_ms - next.last_step_ms) < kStepIntervalMs || next.rise_count < kMinSamples) {
    out.setpoint_lph = next.setpoint_lph;
    return out;
  }

  const float average_k = next.rise_sum_k / static_cast<float>(next.rise_count);
  next.last_rise_k = average_k;
  next.rise_sum_k = 0.0f;
  next.rise_count = 0;
  next.last_step_ms = in.now_ms;

  // The pump could not follow the last setpoint: this is the ceiling of this
  // installation, so stop asking for more instead of winding up.
  if (next.setpoint_lph - in.flow_lph > kShortfallLph)
    next.ceiling_lph = fmaxf(limits.min_lph, in.flow_lph + kCeilingMarginLph);
  const float step_upper =
      isfinite(next.ceiling_lph) ? fmaxf(limits.min_lph, fminf(limits.max_lph, next.ceiling_lph)) : upper;

  if (fabsf(average_k - in.target_k) <= kDeadbandK) {
    next.setpoint_lph = clampf(next.setpoint_lph, limits.min_lph, step_upper);
    next.reason = Reason::ON_TARGET;
    out.setpoint_lph = next.setpoint_lph;
    return out;
  }

  // Heat equals flow times temperature rise, so over one step the flow that
  // would give the target follows directly from the measurement.
  const float desired_lph = in.flow_lph * average_k / in.target_k;
  float step = kGain * (desired_lph - next.setpoint_lph);
  step = clampf(step, -kMaxStepLph, kMaxStepLph);
  next.setpoint_lph = clampf(next.setpoint_lph + step, limits.min_lph, step_upper);

  if (next.setpoint_lph >= step_upper - 0.5f)
    next.reason =
        (isfinite(next.ceiling_lph) && step_upper < limits.max_lph - 0.5f) ? Reason::PUMP_LIMIT : Reason::AT_MAXIMUM;
  else if (next.setpoint_lph <= limits.min_lph + 0.5f)
    next.reason = Reason::AT_MINIMUM;
  else
    next.reason = Reason::ADJUSTING;
  out.setpoint_lph = next.setpoint_lph;
  return out;
}

}  // namespace oq_flow_delta_t
