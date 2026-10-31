#pragma once
#include "types.hpp"
#include <expected>
#include <functional>

namespace ins {
struct WireStats {
    std::uint64_t frames = 0, bad_checksums = 0, discarded = 0;
};
class Framer {
    Source source_;
    std::vector<std::uint8_t> bytes_;

  public:
    explicit Framer(Source source) : source_(source) {}
    WireStats stats;
    void feed(std::span<const std::uint8_t>,
              const std::function<void(std::span<const std::uint8_t>)> &);
};
std::expected<ImuSample, std::string> decode_imu(std::span<const std::uint8_t>);
std::expected<GnssFix, std::string> decode_gnss(std::span<const std::uint8_t>);
std::vector<std::uint8_t> encode_imu(const Vec3 &omega, const Vec3 &force, std::uint8_t counter);
std::vector<std::uint8_t> encode_gnss(const GnssFix &, Time utc_epoch);
std::vector<std::uint8_t> imu_command(std::uint8_t code, std::optional<std::uint8_t> value = {});
} // namespace ins
