#pragma once
#include "ukf.hpp"

namespace ins {
// Velocity-integral coarse alignment. Returns a localized attitude only when
// independent specific-force directions constrain all three rotation axes.
std::optional<NavigationState> align(const Config &, const Earth &, std::span<const ImuSample>,
                                     std::span<const GnssFix>);
} // namespace ins
