#pragma once

// Delta T flow control runtime glue (fork addition).
//
// Kept in its own header so the upstream flow runtime gains a single call,
// keeping the rebase conflict surface small. All decisions live in
// oq_flow_delta_t_logic.h; everything here is id() marshalling.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string>

#include "oq_flow_delta_t_logic.h"
#include "../sources/oq_source_receipt_runtime.h"

namespace oq_flow_delta_t_runtime {

// Position of "Delta T" in the Flow Control Mode select.
constexpr size_t kModeIndex = 2;
// The same freshness window the water performance model uses.
constexpr uint64_t kFreshMs = 30000;

// A unit counts as heating with exactly the rule the boot-resume and water
// performance code use: heating working mode with the compressor turning.
inline oq_flow_delta_t::Unit unit_(const oq_sources::HeatPumpReceipts& raw, uint64_t now_ms, float in_offset_c,
                                   float out_offset_c, bool defrost, int level) {
  oq_flow_delta_t::Unit unit;
  unit.heating = raw.working_mode.fresh(now_ms, kFreshMs) && raw.working_mode.value == 2.0f &&
                 raw.compressor_frequency.fresh(now_ms, kFreshMs) && raw.compressor_frequency.value > 0.5f;
  unit.defrost = defrost;
  unit.temps_fresh = raw.water_in.fresh(now_ms, kFreshMs) && raw.water_out.fresh(now_ms, kFreshMs) &&
                     isfinite(in_offset_c) && isfinite(out_offset_c);
  unit.in_c = raw.water_in.value + in_offset_c;
  unit.out_c = raw.water_out.value + out_offset_c;
  unit.level = level;
  return unit;
}

inline void publish_(const oq_flow_delta_t::Output& out) {
  static std::string last_status;
  static float last_rise = NAN;
  static float last_setpoint = NAN;

  char buf[96];
  const char* reason = oq_flow_delta_t::reason_text(out.next.reason);
  if (out.controlling && isfinite(out.next.last_rise_k)) {
    snprintf(buf, sizeof(buf), "%s - rise %.1f K, %d unit%s, %.0f L/h", reason, out.next.last_rise_k, out.counted_units,
             out.counted_units == 1 ? "" : "s", out.setpoint_lph);
  } else {
    snprintf(buf, sizeof(buf), "%s", reason);
  }
  if (last_status != buf) {
    last_status = buf;
    id(oq_flow_dt_status).publish_state(buf);
  }

  const float rise = out.controlling ? out.next.last_rise_k : NAN;
  if (isfinite(rise) != isfinite(last_rise) || (isfinite(rise) && fabsf(rise - last_rise) >= 0.05f)) {
    last_rise = rise;
    id(oq_flow_dt_rise_k).publish_state(rise);
  }
  const float setpoint = out.controlling ? out.setpoint_lph : NAN;
  if (isfinite(setpoint) != isfinite(last_setpoint) ||
      (isfinite(setpoint) && fabsf(setpoint - last_setpoint) >= 1.0f)) {
    last_setpoint = setpoint;
    id(oq_flow_dt_setpoint_lph).publish_state(setpoint);
  }
}

// Returns the setpoint the flow PI should track. Outside Delta T mode, or
// whenever the mode steps aside, this is selected_setpoint_lph unchanged.
inline float apply(float selected_setpoint_lph, int cm_code) {
  static oq_flow_delta_t::State state;
  const uint64_t now_ms = oq_sources::monotonic_ms();

  oq_flow_delta_t::Input in;
  in.now_ms = static_cast<uint32_t>(millis());
  in.enabled = id(oq_flow_control_mode).active_index().value_or(0) == kModeIndex;
  // Heat pump heating only: cooling, preflow, the boiler modes, frost
  // protection and service tasks keep the normal setpoints.
  in.heating_mode = cm_code == 2;
  in.units[0] = unit_(oq_sources::hp1, now_ms, id(hp1_water_in_temp_offset).state, id(hp1_water_out_temp_offset).state,
                      id(hp1_defrost).state || id(hp1_4_way_valve).state, static_cast<int>(id(hp1_last_applied_level)));
#if OQ_TOPOLOGY_DUO
  in.unit_count = 2;
  in.units[1] = unit_(oq_sources::hp2, now_ms, id(hp2_water_in_temp_offset).state, id(hp2_water_out_temp_offset).state,
                      id(hp2_defrost).state || id(hp2_4_way_valve).state, static_cast<int>(id(hp2_last_applied_level)));
#else
  in.unit_count = 1;
#endif
  in.flow_lph = id(flow_rate_selected).has_state() ? id(flow_rate_selected).state : NAN;
  in.base_setpoint_lph = selected_setpoint_lph;
  in.target_k = id(oq_flow_dt_target_k).state;
  in.min_lph = id(oq_flow_dt_min_lph).state;
  in.max_lph = id(oq_flow_dt_max_lph).state;
  in.lowflow_threshold_lph = id(oq_flow_dt_lowflow_threshold_lph);
  in.water_limit_active = id(oq_water_temp_limit_factor) < 0.999f;
  in.flow_fault = id(oq_lowflow_fault_active);

  const auto out = oq_flow_delta_t::decide(in, state);
  state = out.next;
  publish_(out);
  return out.setpoint_lph;
}

}  // namespace oq_flow_delta_t_runtime
