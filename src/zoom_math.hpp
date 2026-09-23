#pragma once

#include <algorithm>
#include <cmath>

namespace MacOSZoom {

inline float boundedZoom(float current, float multiplier, float minimum,
                         float maximum, float snapThreshold) {
  if (!std::isfinite(current) || !std::isfinite(multiplier) ||
      multiplier <= 0.F)
    return std::clamp(1.F, minimum, maximum);

  const auto next = std::clamp(current * multiplier, minimum, maximum);
  return next < snapThreshold ? minimum : next;
}

inline float scrollZoom(float current, float delta, float sensitivity,
                         float minimum, float maximum, float snapThreshold,
                         bool invertScroll) {
  const auto zoomOutDelta = invertScroll ? -delta : delta;
  const auto next = std::clamp(current - zoomOutDelta * sensitivity, minimum,
                               maximum);
  return zoomOutDelta > 0.F && next < snapThreshold ? minimum : next;
}

struct SMomentumStep {
  float zoom;
  float velocity;
};

// Velocity is measured in zoom-factor units per second. Integrating the
// exponential rather than applying velocity once per frame keeps the travel
// distance consistent across different refresh rates.
inline SMomentumStep momentumStep(float current, float velocity, float seconds,
                                 float decayMs, float minimum, float maximum,
                                 float snapThreshold) {
  const auto tau = decayMs / 1000.F;
  const auto decay = std::exp(-seconds / tau);
  auto next = std::clamp(current + velocity * tau * (1.F - decay), minimum,
                         maximum);
  if (velocity < 0.F && next < snapThreshold)
    next = minimum;
  return {next, next == minimum || next == maximum ? 0.F : velocity * decay};
}

} // namespace MacOSZoom
