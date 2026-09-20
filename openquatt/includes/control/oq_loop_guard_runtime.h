#pragma once

// Loop guard runtime glue (fork addition).
//
// Kept in its own header rather than inside oq_power_house_runtime.h for two
// reasons: the strategy runtimes are held to a line budget by
// scripts/tests/test_strategy_runtime_contract.py, and keeping fork code in fork
// files keeps the conflict surface small when rebasing onto upstream.
//
// All decisions live in oq_loop_guard_logic.h; everything here is id() marshalling.

#include <cmath>
#include <string>

#include "../performance/hp_perf_frequency.h"
#include "oq_loop_guard_logic.h"
#include "oq_power_house_dispatch_logic.h"

namespace oq_loop_guard_runtime {

// Evaluates the guard and, when the guard rather than the house model owns the
// request, raises both the demand level and the requested power in place. Both
// are left untouched while the guard is inactive, so it is inert unless
// deliberately enabled.
//
// v0.50 removed the demand ramp filter, so these are now the Power House
// runtime's own locals: raw_demand feeds oq_demand_raw and the capped demand,
// requested_w feeds oq_phouse_req_w and the dispatcher. Both must rise together
// or the request is clamped straight back below the supervisor's low-load latch.
inline void apply(uint32_t now_ms, int demand_max_f, int& raw_demand, float& requested_w) {
  const float loop_c = id(hp1_water_in_temp).has_state() ? id(hp1_water_in_temp).state : NAN;
  const bool protections_ok =
      id(oq_water_temp_limit_factor) > 0.0f && !id(oq_water_temp_hard_trip_active) && !id(oq_lowflow_fault_active);
  const oq_loop_guard::Input in{now_ms,
                                id(ph_loop_guard_enable).state,
                                id(oq_ph_loop_guard_request),
                                protections_ok,
                                loop_c,
                                id(hp1_flow).has_state() ? id(hp1_flow).state : NAN,
                                id(ph_loop_guard_min_flow_lph).state,
                                id(ph_loop_guard_on_c).state,
                                id(ph_loop_guard_off_c).state,
                                id(ph_loop_guard_flow_settle_s).state,
                                id(ph_loop_guard_recheck_min).state};
  const auto guard = oq_loop_guard::decide(
      in, {id(oq_ph_loop_guard_active), id(oq_ph_loop_guard_flow_ok_since_ms), id(oq_ph_loop_guard_lockout_until_ms)});
  id(oq_ph_loop_guard_active) = guard.next.active;
  id(oq_ph_loop_guard_flow_ok_since_ms) = guard.next.flow_ok_since_ms;
  id(oq_ph_loop_guard_lockout_until_ms) = guard.next.lockout_until_ms;

  const float outside_c = id(outside_temp_selected).state;
  const float supply_c = id(oq_system_supply_temp).state;
  const float level1_w = std::isfinite(outside_c) && std::isfinite(supply_c)
                             ? oq_perf::interp_power_th_w_hz(oq_perf::model_frequency_hz(1), outside_c, supply_c)
                             : NAN;
  const auto plan = oq_loop_guard::plan_request(guard.active, raw_demand, requested_w, level1_w,
                                                id(house_rated_power_w).state, demand_max_f);
  id(oq_ph_loop_guard_owns_request) = plan.owns_request;
  if (plan.owns_request) requested_w = plan.requested_w;
  if (guard.active) raw_demand = plan.demand;

  char buf[96];
  oq_loop_guard::format_status(buf, sizeof(buf), guard, in.enabled, in.heat_draw, loop_c, in.engage_c,
                               plan.requested_w);
  static std::string last_status;
  if (last_status != buf) {
    last_status = buf;
    id(oq_ph_loop_guard_status).publish_state(buf);
  }
}

// Pins one compressor at level 1 when the guard, not the house model, raised
// the request. Real heat demand always wins: if the house asked for more,
// owns_request is false and this does nothing, so a duo run is never capped.
// The pin is deliberate rather than left to the dispatcher - the guard inflates
// the request to clear the supervisor's low-load latch, and the dispatcher
// would otherwise read that margin as a call for level 2.
template <typename Dispatch>
inline void pin_request(const Dispatch& dispatch) {
  if (!id(oq_ph_loop_guard_owns_request) || !dispatch.output_valid) return;
  const bool hp2_only = dispatch.hp2_level > 0 && dispatch.hp1_level == 0;
  id(oq_ph_request_hp1_level) = hp2_only ? 0 : 1;
  id(oq_ph_request_hp2_level) = hp2_only ? 1 : 0;
  id(oq_ph_request_owner_hp) = hp2_only ? 2 : 1;
  id(oq_ph_request_reason_code) = static_cast<int>(oq_power_house_dispatch::Reason::LOOP_GUARD);
}

inline void reset() {
  id(oq_ph_loop_guard_active) = false;
  id(oq_ph_loop_guard_owns_request) = false;
}

}  // namespace oq_loop_guard_runtime
