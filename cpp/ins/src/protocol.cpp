#include "ins/protocol.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace ins {
namespace {
template <class T> T read(std::span<const std::uint8_t> b, std::size_t n) {
    std::array<std::uint8_t, sizeof(T)> a{};
    std::copy_n(b.begin() + n, sizeof(T), a.begin());
    if constexpr (std::endian::native == std::endian::big)
        std::reverse(a.begin(), a.end());
    return std::bit_cast<T>(a);
}
template <class T> void write(std::vector<std::uint8_t> &b, std::size_t n, T value) {
    auto a = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
    if constexpr (std::endian::native == std::endian::big)
        std::reverse(a.begin(), a.end());
    std::copy(a.begin(), a.end(), b.begin() + n);
}
std::pair<std::uint8_t, std::uint8_t> ubx_checksum(std::span<const std::uint8_t> s) {
    std::uint8_t a = 0, b = 0;
    for (auto v : s) {
        a += v;
        b += a;
    }
    return {a, b};
}
} // namespace
void Framer::feed(std::span<const std::uint8_t> incoming,
                  const std::function<void(std::span<const std::uint8_t>)> &callback) {
    bytes_.insert(bytes_.end(), incoming.begin(), incoming.end());
    std::size_t offset = 0;
    while (bytes_.size() - offset >= (source_ == Source::imu ? 2U : 6U)) {
        auto s = std::span<const std::uint8_t>(bytes_).subspan(offset);
        std::size_t size = 0;
        if (source_ == Source::imu) {
            if (s[0] != 0x77 || s[1] < 4 || s[1] > 128) {
                ++offset;
                ++stats.discarded;
                continue;
            }
            size = s[1] + 1;
        } else {
            if (s[0] != 0xb5 || s[1] != 0x62) {
                ++offset;
                ++stats.discarded;
                continue;
            }
            size = 8 + read<std::uint16_t>(s, 4);
            if (size > 8192) {
                ++offset;
                ++stats.discarded;
                continue;
            }
        }
        if (s.size() < size)
            break;
        const auto f = s.first(size);
        bool good;
        if (source_ == Source::imu)
            good = (std::accumulate(f.begin() + 1, f.end() - 1, 0U) & 255) == f.back();
        else {
            auto [a, b] = ubx_checksum(f.subspan(2, size - 4));
            good = a == f[size - 2] && b == f[size - 1];
        }
        if (!good) {
            ++offset;
            ++stats.bad_checksums;
            ++stats.discarded;
            continue;
        }
        ++stats.frames;
        callback(f);
        offset += size;
    }
    bytes_.erase(bytes_.begin(), bytes_.begin() + offset);
}
std::expected<ImuSample, std::string> decode_imu(std::span<const std::uint8_t> f) {
    if (f.size() != 33 || f[0] != 0x77 || f[1] != 32 || f[2] != 0 || f[3] != 0x70)
        return std::unexpected("Not an IMU float frame");
    if ((std::accumulate(f.begin() + 1, f.end() - 1, 0U) & 255) != f.back())
        return std::unexpected("IMU checksum");
    ImuSample s;
    for (int i = 0; i < 3; ++i) {
        s.omega[i] = read<float>(f, 4 + 4 * i) * degrees;
        s.force[i] = read<float>(f, 16 + 4 * i) * standard_gravity;
    }
    if (!s.omega.allFinite() || !s.force.allFinite())
        return std::unexpected("Non-finite IMU sample");
    unsigned counter = 0;
    for (int i = 28; i < 30; ++i) {
        if ((f[i] >> 4) > 9 || (f[i] & 15) > 9)
            return std::unexpected("Bad IMU BCD counter");
        counter = counter * 100 + (f[i] >> 4) * 10 + (f[i] & 15);
    }
    if (counter > 255 || f[30] || f[31])
        return std::unexpected("Bad IMU counter range");
    s.counter = static_cast<std::uint8_t>(counter);
    s.saturated = (s.omega.cwiseAbs().maxCoeff() >= 399. * degrees ||
                   s.force.cwiseAbs().maxCoeff() >= 1.99 * standard_gravity);
    return s;
}
std::expected<GnssFix, std::string> decode_gnss(std::span<const std::uint8_t> f) {
    if (f.size() != 92 || f[0] != 0xb5 || f[1] != 0x62 || f[2] != 1 || f[3] != 7 ||
        read<std::uint16_t>(f, 4) != 84)
        return std::unexpected("Not a u-blox 7 NAV-PVT frame");
    auto [a, b] = ubx_checksum(f.subspan(2, f.size() - 4));
    if (a != f[f.size() - 2] || b != f.back())
        return std::unexpected("UBX checksum");
    const auto p = f.subspan(6, 84);
    GnssFix fix;
    const auto year = read<std::uint16_t>(p, 4);
    if (year < 2000 || year > 2100 ||
        std::abs(std::int64_t(read<std::int32_t>(p, 16))) > 1000000000)
        return std::unexpected("GNSS time outside supported epoch range");
    const std::chrono::year_month_day day{std::chrono::year{year}, std::chrono::month{p[6]},
                                          std::chrono::day{p[7]}};
    if (!day.ok() || p[8] > 23 || p[9] > 59 || p[10] > 59 || (p[11] & 7) != 7)
        return std::unexpected("GNSS time invalid or leap second");
    fix.stamp.measurement =
        std::chrono::duration_cast<Time>(std::chrono::sys_days{day}.time_since_epoch()) +
        std::chrono::hours{p[8]} + std::chrono::minutes{p[9]} + std::chrono::seconds{p[10]} +
        Time{read<std::int32_t>(p, 16)};
    fix.stamp.sequence = read<std::uint32_t>(p, 0);
    fix.valid = p[20] == 3 && (p[21] & 1);
    fix.satellites = p[23];
    fix.longitude = read<std::int32_t>(p, 24) * 1e-7 * degrees;
    fix.latitude = read<std::int32_t>(p, 28) * 1e-7 * degrees;
    fix.height = read<std::int32_t>(p, 32) * .001;
    fix.position_sigma = Vec3(read<std::uint32_t>(p, 40) * .001, read<std::uint32_t>(p, 40) * .001,
                              read<std::uint32_t>(p, 44) * .001);
    for (int i = 0; i < 3; ++i)
        fix.velocity[i] = read<std::int32_t>(p, 48 + 4 * i) * .001;
    fix.velocity_sigma = Vec3::Constant(read<std::uint32_t>(p, 68) * .001);
    if (std::abs(fix.latitude) > pi / 2 || std::abs(fix.longitude) > pi ||
        fix.velocity.norm() > 2000 || fix.height < -1000 || fix.height > 100000)
        fix.valid = false;
    return fix;
}
std::vector<std::uint8_t> encode_imu(const Vec3 &w, const Vec3 &a, std::uint8_t count) {
    std::vector<std::uint8_t> f(33);
    f[0] = 0x77;
    f[1] = 32;
    f[3] = 0x70;
    for (int i = 0; i < 3; ++i) {
        write<float>(f, 4 + 4 * i, static_cast<float>(w[i] / degrees));
        write<float>(f, 16 + 4 * i, static_cast<float>(a[i] / standard_gravity));
    }
    const unsigned hi = count / 100, lo = count % 100;
    f[28] = static_cast<std::uint8_t>((hi / 10) * 16 + hi % 10);
    f[29] = static_cast<std::uint8_t>((lo / 10) * 16 + lo % 10);
    f[32] = static_cast<std::uint8_t>(std::accumulate(f.begin() + 1, f.end() - 1, 0U));
    return f;
}
std::vector<std::uint8_t> encode_gnss(const GnssFix &s, Time utc) {
    std::vector<std::uint8_t> f(92);
    f[0] = 0xb5;
    f[1] = 0x62;
    f[2] = 1;
    f[3] = 7;
    f[4] = 84;
    const auto epoch = std::chrono::sys_time<Time>{utc};
    const auto day = std::chrono::floor<std::chrono::days>(epoch);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::hh_mm_ss tod{epoch - day};
    // iTOW is GPST; this simulation epoch uses 18 leap seconds (recorded in metadata).
    const auto gps_epoch = std::chrono::sys_days{std::chrono::year{1980} / 1 / 6};
    const auto tow = std::chrono::duration_cast<std::chrono::milliseconds>(epoch - gps_epoch +
                                                                           std::chrono::seconds{18})
                         .count() %
                     604800000;
    write<std::uint32_t>(f, 6, static_cast<std::uint32_t>(tow));
    write<std::uint16_t>(f, 10, static_cast<int>(ymd.year()));
    f[12] = static_cast<unsigned>(ymd.month());
    f[13] = static_cast<unsigned>(ymd.day());
    f[14] = tod.hours().count();
    f[15] = tod.minutes().count();
    f[16] = tod.seconds().count();
    f[17] = 7;
    write<std::uint32_t>(f, 18, 100);
    write<std::int32_t>(f, 22, tod.subseconds().count());
    f[26] = s.valid ? 3 : 0;
    f[27] = s.valid ? 1 : 0;
    f[29] = s.satellites;
    write<std::int32_t>(f, 30, std::llround(s.longitude / degrees * 1e7));
    write<std::int32_t>(f, 34, std::llround(s.latitude / degrees * 1e7));
    write<std::int32_t>(f, 38, std::llround(s.height * 1000));
    write<std::int32_t>(f, 42, std::llround(s.height * 1000));
    write<std::uint32_t>(f, 46, std::llround(s.position_sigma.x() * 1000));
    write<std::uint32_t>(f, 50, std::llround(s.position_sigma.z() * 1000));
    for (int i = 0; i < 3; ++i)
        write<std::int32_t>(f, 54 + 4 * i, std::llround(s.velocity[i] * 1000));
    write<std::int32_t>(f, 66, std::llround(s.velocity.head<2>().norm() * 1000));
    write<std::int32_t>(f, 70,
                        std::llround(std::atan2(s.velocity.y(), s.velocity.x()) / degrees * 1e5));
    write<std::uint32_t>(f, 74, std::llround(s.velocity_sigma.maxCoeff() * 1000));
    write<std::uint32_t>(f, 78, 100000);
    auto [a, b] = ubx_checksum(std::span(f).subspan(2, 88));
    f[90] = a;
    f[91] = b;
    return f;
}
std::vector<std::uint8_t> imu_command(std::uint8_t code, std::optional<std::uint8_t> value) {
    std::vector<std::uint8_t> f{0x77, static_cast<std::uint8_t>(value ? 5 : 4), 0, code};
    if (value)
        f.push_back(*value);
    f.push_back(static_cast<std::uint8_t>(std::accumulate(f.begin() + 1, f.end(), 0U)));
    return f;
}
} // namespace ins
