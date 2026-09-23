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

} // namespace MacOSZoom
