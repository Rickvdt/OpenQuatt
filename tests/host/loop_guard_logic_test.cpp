#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "../../openquatt/includes/control/oq_loop_guard_logic.h"

namespace {

using namespace oq_loop_guard;

constexpr uint32_t kTickMs = 10000u;

// Heat draw present, flow above the minimum, a cold loop reading.
Input drawing(uint32_t now_ms, float loop_c, float flow_lph = 290.0f) {
  Input in;
  in.now_ms = now_ms;
  in.enabled = true;
  in.heat_draw = true;
  in.protections_ok = true;
  in.loop_c = loop_c;
  in.flow_lph = flow_lph;
  in.min_flow_lph = 250.0f;
  in.engage_c = 19.0f;
  in.release_c = 21.0f;
  in.settle_s = 120.0f;
  in.recheck_min = 30.0f;
  return in;
}

Output run(State& state, uint32_t& now_ms, int ticks, float loop_c, bool protections_ok = true) {
  Output out;
  for (int i = 0; i < ticks; ++i) {
    Input in = drawing(now_ms, loop_c);
    in.protections_ok = protections_ok;
    out = decide(in, state);
    state = out.next;
    now_ms += kTickMs;
  }
  return out;
}

void test_engages_only_after_flow_has_settled() {
  State state;
  uint32_t now = 1000;
  Output out = run(state, now, 11, 17.0f);  // 100 s of flow: not settled yet
  assert(!out.active && !out.reading_valid);
  out = run(state, now, 3, 17.0f);
  assert(out.reading_valid && out.active);
}

void test_releases_when_warm_and_locks_out() {
  State state;
  uint32_t now = 1000;
  run(state, now, 14, 17.0f);
  assert(state.active);
  Output out = run(state, now, 1, 21.5f);
  assert(!out.active && state.lockout_until_ms != 0);
  // A cold reading during the recheck lockout does not re-engage.
  out = run(state, now, 6, 17.0f);
  assert(!out.active && out.locked_out);
}

void test_protection_trip_backs_off_but_heat_draw_end_does_not() {
  State state;
  uint32_t now = 1000;
  run(state, now, 14, 17.0f);
  assert(state.active);
  // Protection trips: the guard drops and backs off for five minutes.
  Output out = run(state, now, 1, 17.0f, false);
  assert(!out.active && state.lockout_until_ms != 0);
  out = run(state, now, 6, 17.0f);  // protection clears a minute later
  assert(!out.active && out.locked_out);

  // The heat draw simply ending is normal: no lockout for the next window.
  State other;
  uint32_t t = 1000;
  run(other, t, 14, 17.0f);
  assert(other.active);
  Input in = drawing(t, 17.0f);
  in.heat_draw = false;
  out = decide(in, other);
  assert(!out.active && out.next.lockout_until_ms == 0);
}

void test_unset_lockout_does_not_block_after_long_uptime() {
  // millis() past 2^31 (24.8 days) with no lockout ever set must not read as
  // locked out.
  State state;
  uint32_t now = 0x90000000u;
  Output out = run(state, now, 14, 17.0f);
  assert(!out.locked_out && out.active);
}

void test_expired_lockout_is_cleared_and_never_compared_again() {
  State state;
  uint32_t now = 1000;
  run(state, now, 14, 17.0f);
  run(state, now, 1, 21.5f);  // release: 30 min lockout
  assert(state.lockout_until_ms != 0);
  run(state, now, 181, 21.5f);  // past the lockout
  assert(state.lockout_until_ms == 0);
  // 24.8 days later the old deadline must not resurface as a lockout.
  now += 0x80000000u;
  Output out = run(state, now, 14, 17.0f);
  assert(!out.locked_out && out.active);
}

void test_ownership_is_decided_against_level_one_output() {
  // P1 = 1800 W thermal: the ask is max(1.2 * P1, P1 + 450) = 2250 W.
  Plan plan = plan_request(true, 0, 0.0f, 1800.0f, 9000.0f, 100);
  assert(plan.owns_request && fabsf(plan.requested_w - 2250.0f) < 0.5f);
  // A house asking 2000 W wants more than level 1 delivers: the guard yields,
  // even though 2000 is below the inflated ask.
  plan = plan_request(true, 20, 2000.0f, 1800.0f, 9000.0f, 100);
  assert(!plan.owns_request);
  plan = plan_request(true, 10, 1200.0f, 1800.0f, 9000.0f, 100);
  assert(plan.owns_request);
  // Inactive guard: nothing owned, demand untouched.
  plan = plan_request(false, 7, 0.0f, 1800.0f, 9000.0f, 100);
  assert(!plan.owns_request && plan.demand == 7);
}

void test_status_reports_the_yielded_state() {
  State state;
  uint32_t now = 1000;
  Output out = run(state, now, 14, 17.0f);
  assert(out.active);
  char buf[96];
  format_status(buf, sizeof(buf), out, true, true, false, 17.0f, 19.0f, 2250.0f);
  assert(strncmp(buf, "Yielded to house demand", 23) == 0);
  format_status(buf, sizeof(buf), out, true, true, true, 17.0f, 19.0f, 2250.0f);
  assert(strncmp(buf, "Active - loop", 13) == 0);
}

}  // namespace

int main() {
  test_engages_only_after_flow_has_settled();
  test_releases_when_warm_and_locks_out();
  test_protection_trip_backs_off_but_heat_draw_end_does_not();
  test_unset_lockout_does_not_block_after_long_uptime();
  test_expired_lockout_is_cleared_and_never_compared_again();
  test_ownership_is_decided_against_level_one_output();
  test_status_reports_the_yielded_state();
  return 0;
}
