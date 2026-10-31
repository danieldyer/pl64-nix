#include "ins/acquisition.hpp"
#include <cmath>
namespace ins {
void Acquisition::accept(const Record &r) {
    if (r.kind != Kind::raw)
        return;
    if (r.source == Source::imu)
        imu_.feed(r.payload, [&](auto frame) {
            auto decoded = decode_imu(frame);
            if (!decoded)
                return;
            auto s = *decoded;
            if (!clock_.stamp(s, r.stamp))
                return;
            s.omega = config_.sensor_to_body * s.omega;
            s.force = config_.sensor_to_body * s.force;
            if (on_record) {
                Record out;
                out.kind = Kind::imu;
                out.source = Source::imu;
                out.stamp = s.stamp;
                out.native_counter = s.counter;
                for (double v : s.omega)
                    out.values.push_back(v);
                for (double v : s.force)
                    out.values.push_back(v);
                out.flags = (s.gap ? 1 : 0) | (s.saturated ? 2 : 0);
                on_record(out);
            }
            if (on_imu)
                on_imu(s);
        });
    else if (r.source == Source::gnss)
        gnss_.feed(r.payload, [&](auto frame) {
            const bool navigation = frame.size() == 92 && frame[2] == 1 && frame[3] == 7;
            if (navigation) {
                const std::uint32_t tow = std::uint32_t(frame[6]) | (std::uint32_t(frame[7]) << 8) |
                                          (std::uint32_t(frame[8]) << 16) |
                                          (std::uint32_t(frame[9]) << 24);
                const bool time_valid = (frame[17] & 7) == 7;
                if (on_gnss_packet)
                    on_gnss_packet(tow, time_valid, r.stamp);
                if (!time_valid)
                    gnss_tow_.reset();
                else if (gnss_tow_ == tow)
                    return; // A repeated native epoch is never a new observation.
                else
                    gnss_tow_ = tow;
            }
            auto decoded = decode_gnss(frame);
            if (!decoded) {
                if (navigation && on_gnss)
                    on_gnss(GnssFix{}); // Invalid UTC also breaks a run of valid BIT fixes.
                return;
            }
            auto f = *decoded;
            const Time offset = r.stamp.utc - r.stamp.arrival;
            if (utc_offset_ && std::abs(seconds(offset - *utc_offset_)) > .05) {
                if (on_record) {
                    Record h;
                    h.kind = Kind::health;
                    h.source = Source::clock;
                    h.stamp = r.stamp;
                    h.detail = "System UTC stepped; GNSS clock mapping changed";
                    on_record(h);
                }
                f.valid = false;
                if (on_discontinuity)
                    on_discontinuity("System UTC discontinuity; reacquiring GNSS/IMU alignment");
            }
            utc_offset_ = offset;
            const auto native = f.stamp.measurement;
            f.stamp.measurement = native - offset;
            f.stamp.arrival = r.stamp.arrival;
            f.stamp.utc = r.stamp.utc;
            f.stamp.uncertainty_s = config_.gnss_timing_sigma_s;
            if (seconds(f.stamp.measurement - r.stamp.arrival) > .1 ||
                seconds(r.stamp.arrival - f.stamp.measurement) > 10)
                f.valid = false;
            if (on_record) {
                Record out;
                out.kind = Kind::gnss;
                out.source = Source::gnss;
                out.stamp = f.stamp;
                out.native_counter = static_cast<std::uint64_t>(native.count());
                out.flags = f.valid ? 1 : 0;
                out.values = {f.latitude, f.longitude, f.height};
                for (const auto *v : {&f.velocity, &f.position_sigma, &f.velocity_sigma})
                    for (double x : *v)
                        out.values.push_back(x);
                on_record(out);
            }
            if (on_gnss)
                on_gnss(f);
        });
}
} // namespace ins
