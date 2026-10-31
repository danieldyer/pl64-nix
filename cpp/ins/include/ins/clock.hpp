#pragma once
#include "types.hpp"
#include <optional>

namespace ins {
// Cadence reconstruction from packet count. Arrival times cannot identify
// absolute sensor latency; configured uncertainty remains in every output.
class ImuClock {
    double period_, delay_, sigma_;
    std::optional<std::uint8_t> previous_;
    Time epoch_{}, last_arrival_{}, last_sample_{};
    std::uint64_t count_ = 0;

  public:
    explicit ImuClock(const Config &);
    void reset();
    bool stamp(ImuSample &, Stamp arrival);
};
} // namespace ins
