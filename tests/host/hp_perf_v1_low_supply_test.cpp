#include <assert.h>

#include <cmath>
#include <string>

class FakeGenerationSelect {
 public:
  bool has_state() const { return true; }
  std::string current_option() const { return "V1.5"; }
};

FakeGenerationSelect hp_generation;

#define id(value) value
#include "../../openquatt/includes/performance/hp_perf_frequency.h"
#undef id

namespace {

using oq_perf_v1_low_supply::electrical_factor;

void assert_near(float actual, float expected, float tolerance) { assert(std::fabs(actual - expected) < tolerance); }

void assert_untouched_from_35c() {
  for (const bool v1_5 : {false, true}) {
    for (const float hz : {30.0f, 55.0f, 90.0f}) {
      for (const float outside : {-15.0f, 2.0f, 12.0f}) {
        for (const float supply : {35.0f, 40.0f, 55.0f}) assert(electrical_factor(v1_5, hz, outside, supply) == 1.0f);
        // No step at the 35 C node.
        assert_near(electrical_factor(v1_5, hz, outside, std::nextafter(35.0f, 0.0f)), 1.0f, 1e-4f);
      }
    }
  }
  assert(electrical_factor(true, NAN, 7.0f, 25.0f) == 1.0f);
  assert(electrical_factor(true, 30.0f, NAN, 25.0f) == 1.0f);
  assert(electrical_factor(true, 30.0f, 7.0f, NAN) == 1.0f);
}

void assert_fitted_values() {
  // Reference values from the fleet fit (natural-log model evaluated in double precision).
  assert_near(electrical_factor(true, 30.0f, 7.0f, 25.0f), 0.75662f, 2e-4f);
  assert_near(electrical_factor(false, 30.0f, 7.0f, 25.0f), 0.72093f, 2e-4f);
  assert_near(electrical_factor(true, 55.0f, 2.0f, 30.0f), 1.00295f, 2e-4f);
  assert_near(electrical_factor(true, 39.0f, 12.0f, 23.0f), 0.67926f, 2e-4f);
  assert_near(electrical_factor(true, 79.0f, 5.0f, 32.0f), 1.00012f, 2e-4f);
}

void assert_held_beyond_the_data() {
  // The outdoor trend stops 5 K below the coldest data, the depth below 23 C supply.
  assert(electrical_factor(true, 61.0f, -10.0f, 28.0f) == electrical_factor(true, 61.0f, -3.0f, 28.0f));
  assert_near(electrical_factor(true, 61.0f, -3.0f, 28.0f), 1.03274f, 2e-4f);
  assert(electrical_factor(true, 49.0f, 5.0f, 75.0f) == 1.0f);
  assert(electrical_factor(true, 90.0f, 5.0f, 30.0f) == electrical_factor(true, 75.0f, 5.0f, 30.0f));
}

void assert_prediction_scales_electrical_only() {
  for (const auto variant : {oq_odu::Variant::V1, oq_odu::Variant::V1_5}) {
    const bool v1_5 = variant == oq_odu::Variant::V1_5;
    for (const float supply : {25.0f, 30.0f, 35.0f, 45.0f}) {
      const auto p = oq_perf::predict_heating_hz(variant, 39.0f, 7.0f, supply);
      assert(p.available);
      const float factor = electrical_factor(v1_5, 39.0f, 7.0f, supply);
      // Thermal power is the map's; electrical power and COP carry the factor.
      assert_near(p.pth_w, oq_perf_v1::interp_power_th_w(2, 7.0f, supply), 0.01f);
      assert_near(p.pel_w, oq_perf_v1::interp_power_el_w(2, 7.0f, supply) * factor, 0.01f);
      assert_near(p.pth_w / p.cop, p.pel_w, 0.05f);
    }
  }
}

}  // namespace

int main() {
  assert_untouched_from_35c();
  assert_fitted_values();
  assert_held_beyond_the_data();
  assert_prediction_scales_electrical_only();
  return 0;
}
