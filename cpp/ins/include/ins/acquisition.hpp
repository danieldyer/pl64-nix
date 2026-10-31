#pragma once
#include "clock.hpp"
#include "protocol.hpp"
#include <functional>

namespace ins {
// The only path from wire bytes to navigation measurements. Replay deliberately
// ignores decoded/truth records and re-runs this parser and clock reconstruction.
class Acquisition {
    Config config_;
    Framer imu_{Source::imu}, gnss_{Source::gnss};
    ImuClock clock_;
    std::optional<Time> utc_offset_;
    std::optional<std::uint32_t> gnss_tow_;

  public:
    explicit Acquisition(Config config) : config_(std::move(config)), clock_(config_) {}
    std::function<void(const ImuSample &)> on_imu;
    std::function<void(const GnssFix &)> on_gnss;
    // Transport health is independent of fix/time validity during cold acquisition.
    std::function<void(std::uint32_t, bool, const Stamp &)> on_gnss_packet;
    std::function<void(const Record &)> on_record;
    std::function<void(std::string)> on_discontinuity;
    void accept(const Record &);
    WireStats imu_stats() const { return imu_.stats; }
    WireStats gnss_stats() const { return gnss_.stats; }
};
} // namespace ins
