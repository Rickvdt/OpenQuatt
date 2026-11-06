#pragma once

// Electrical-power correction for the V1/V1.5 map below 35 C supply.
//
// The V1/V1.5 table starts at 35 C supply and clamps lower water temperatures
// to that node. Fleet telemetry (53 V1/V1.5 installations, October-November
// 2026, steady 10-minute blocks) shows the clamped map overestimating the
// electrical power there by 10-30 %, most at low frequency in mild weather.
// This factor scales the map's electrical power below 35 C only. Thermal power,
// and everything from 35 C supply up, stay Quatt's specification.
//
// Every term is multiplied by depth = (35 - supply) / 10, so the factor is
// exactly 1 at 35 C and grows from there without a step. Validated by holding
// out whole installations: below 35 C the median cell error drops from 12 % to
// 1.1 %, and every well-covered cell is within 2 %.
//
// The data reached about 2 C outside and 23 C supply. The lift term follows its
// measured trend to -3 C outside, 5 K beyond the data (the distance a fit on
// warm data still predicted held-back cold data within 3 %), and holds there.
// Depth holds below 23 C. Values in frost are therefore an estimate, to be
// refitted once winter data is available.

#include <algorithm>
#include <cmath>

namespace oq_perf_v1_low_supply {

constexpr float kTopSupplyC = 35.0f;
constexpr float kMaxDepth = 1.2f;  // 23 C supply
constexpr float kMinOutsideC = -3.0f;
constexpr float kMaxOutsideC = 15.0f;
constexpr float kMinLiftSupplyC = 20.0f;
constexpr float kMaxFrequencyTerm = 1.5f;  // 75 Hz

// Natural-log coefficients, fitted on the fleet data.
constexpr float kDepth = -0.35733f;
constexpr float kDepthFrequency = 0.41467f;
constexpr float kDepthFrequency2 = -0.13077f;
constexpr float kDepthLift = -0.30815f;
constexpr float kDepthFrequencyLift = 0.10100f;
constexpr float kDepth2 = 0.20164f;
constexpr float kDepth2Frequency = -0.23574f;
constexpr float kDepth2Lift = 0.06312f;
constexpr float kDepthV15 = 0.04831f;
constexpr float kDepthV15Frequency = -0.00020f;

// Multiplier for the map's electrical power; 1 from 35 C supply up and for any
// non-finite input.
inline float electrical_factor(bool v1_5, float frequency_hz, float outside_c, float supply_c) {
  if (!std::isfinite(frequency_hz) || !std::isfinite(outside_c) || !std::isfinite(supply_c) ||
      !(supply_c < kTopSupplyC))
    return 1.0f;
  const float depth = std::clamp((kTopSupplyC - supply_c) / 10.0f, 0.0f, kMaxDepth);
  const float frequency = std::clamp((frequency_hz - 30.0f) / 30.0f, 0.0f, kMaxFrequencyTerm);
  // Positive for a small lift (water close to the outside air), negative for a large one.
  const float lift = std::max(supply_c, kMinLiftSupplyC) - std::clamp(outside_c, kMinOutsideC, kMaxOutsideC);
  const float small_lift = -(lift - 25.0f) / 10.0f;
  const float v15 = v1_5 ? 1.0f : 0.0f;
  const float per_depth = kDepth + kDepthFrequency * frequency + kDepthFrequency2 * frequency * frequency +
                          kDepthLift * small_lift + kDepthFrequencyLift * frequency * small_lift + kDepthV15 * v15 +
                          kDepthV15Frequency * v15 * frequency;
  const float per_depth2 = kDepth2 + kDepth2Frequency * frequency + kDepth2Lift * small_lift;
  return std::exp(depth * per_depth + depth * depth * per_depth2);
}

}  // namespace oq_perf_v1_low_supply
