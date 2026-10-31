#include "ins/clock.hpp"
#include <algorithm>
#include <cmath>

namespace ins {
ImuClock::ImuClock(const Config &c)
    : period_(1 / c.imu_rate), delay_(c.imu_delay_s), sigma_(c.imu_timing_sigma_s) {}
void ImuClock::reset() {
    previous_.reset();
    count_ = 0;
}
bool ImuClock::stamp(ImuSample &s, Stamp arrival) {
    if (previous_ && arrival.arrival < last_arrival_) {
        reset();
        s.gap = true;
    }
    if (!previous_) {
        epoch_ = arrival.arrival - time_from_seconds(delay_);
        last_sample_ = epoch_ - time_from_seconds(period_);
    } else {
        const unsigned step = static_cast<std::uint8_t>(s.counter - *previous_);
        if (!step && seconds(arrival.arrival - last_arrival_) < .5)
            return false;
        if (step != 1 || seconds(arrival.arrival - last_arrival_) > period_ * 128) {
            s.gap = true;
            epoch_ = arrival.arrival - time_from_seconds(delay_);
            count_ = 0;
        } else
            ++count_;
    }
    Time predicted = epoch_ + time_from_seconds(count_ * period_);
    const double error = seconds(arrival.arrival - time_from_seconds(delay_) - predicted);
    // A slow bounded phase correction follows crystal drift without integrating
    // USB scheduling jitter as angular/linear motion. Gaps restart the segment.
    if (count_ > 0) {
        const double correction = std::clamp(error * .001, -period_ * .01, period_ * .01);
        epoch_ += time_from_seconds(correction);
        predicted += time_from_seconds(correction);
    }
    if (predicted <= last_sample_) {
        s.gap = true;
        predicted = last_sample_ + time_from_seconds(period_);
    }
    s.stamp = arrival;
    s.stamp.measurement = predicted;
    s.stamp.uncertainty_s = std::max(sigma_, std::abs(error));
    s.stamp.sequence = count_;
    previous_ = s.counter;
    last_arrival_ = arrival.arrival;
    last_sample_ = predicted;
    return true;
}
} // namespace ins
