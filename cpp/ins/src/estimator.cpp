#include "ins/estimator.hpp"
#include "ins/alignment.hpp"
#include <algorithm>
#include <map>

namespace ins {
struct Estimator::Impl {
    struct Checkpoint {
        Ukf filter;
        Time accepted;
    };
    Config config;
    std::optional<Earth> frame;
    std::optional<Ukf> filter;
    std::vector<ImuSample> samples;
    std::map<Time, GnssFix> fixes;
    std::map<Time, Checkpoint> checkpoints;
    std::optional<Time> ready_since;
    Time last_accepted{}, last_alignment{};
    Estimate output;

    explicit Impl(Config c) : config(std::move(c)) { config.validate(); }
    void clear(std::string reason) {
        frame.reset();
        filter.reset();
        samples.clear();
        fixes.clear();
        checkpoints.clear();
        ready_since.reset();
        last_accepted = {};
        last_alignment = {};
        output = {};
        output.stage = Stage::built_in_test;
        output.reason = std::move(reason);
    }
    void publish() {
        if (!filter)
            return;
        output.state = filter->state();
        const Mat18 covariance = filter->root() * filter->root().transpose();
        output.covariance = covariance.topLeftCorner<15, 15>();
        output.gnss_age_s = seconds(output.state.time - last_accepted);
        output.normalized_residual = std::sqrt(filter->innovation_squared());
        const Mat3 rotation = output.state.attitude().toRotationMatrix();
        // Rotation uncertainty about local vertical is coordinate independent;
        // unlike Euler heading it remains defined for vertical body attitudes.
        const Vec3 down = rotation.transpose() * Vec3::UnitZ();
        const double heading_variance = down.dot(covariance.block<3, 3>(6, 6) * down);
        const double tilt_variance =
            (covariance.block<3, 3>(6, 6) * (Mat3::Identity() - down * down.transpose())).trace();
        const bool precise = heading_variance < std::pow(config.navigation_heading_sigma, 2) &&
                             tilt_variance < std::pow(1 * degrees, 2) &&
                             covariance.block<3, 3>(3, 3).trace() < .3 * .3;
        if (!precise || output.gnss_age_s > 2)
            ready_since.reset();
        else if (!ready_since)
            ready_since = output.state.time;
        output.heading_observed = ready_since && seconds(output.state.time - *ready_since) >= 5;
        if (output.gnss_age_s > 2) {
            output.stage = Stage::degraded;
            output.reason = "GNSS stale; uncertainty grows under inertial propagation";
        } else if (output.heading_observed) {
            output.stage = Stage::navigating;
            output.reason = "Attitude and velocity uncertainty meet navigation limits";
        } else {
            output.stage = Stage::fine_alignment;
            output.reason = "UKF refining attitude and IMU biases";
        }
    }
    void trim() {
        if (samples.size() < 2)
            return;
        Time first = samples.back().stamp.measurement - std::chrono::seconds(12);
        if (filter) {
            const auto cutoff =
                samples.back().stamp.measurement - time_from_seconds(config.history_s);
            auto boundary = checkpoints.upper_bound(cutoff);
            if (boundary != checkpoints.begin()) {
                --boundary;
                checkpoints.erase(checkpoints.begin(), boundary);
            }
            first = checkpoints.begin()->first;
            fixes.erase(fixes.begin(), fixes.upper_bound(first));
        } else {
            fixes.erase(fixes.begin(), fixes.lower_bound(first));
        }
        auto before =
            std::lower_bound(samples.begin(), samples.end(), first,
                             [](const auto &s, Time t) { return s.stamp.measurement < t; });
        if (before != samples.begin())
            --before;
        samples.erase(samples.begin(), before);
    }
    void correct(Time dirty) {
        if (!filter || dirty > samples.back().stamp.measurement)
            return;
        auto at = checkpoints.lower_bound(dirty);
        if (at == checkpoints.begin())
            return;
        --at;
        auto next = at->second.filter;
        auto accepted = at->second.accepted;
        checkpoints.erase(std::next(at), checkpoints.end());
        for (auto f = fixes.upper_bound(next.state().time);
             f != fixes.end() && f->first <= samples.back().stamp.measurement; ++f) {
            const auto block = interval(samples, next.state().time, f->first);
            next.propagate(block);
            if (next.observe(f->second, block.back()))
                accepted = f->first;
            checkpoints.emplace(f->first, Checkpoint{next, accepted});
        }
        if (next.state().time < samples.back().stamp.measurement)
            next.propagate(interval(samples, next.state().time, samples.back().stamp.measurement));
        filter = std::move(next);
        last_accepted = accepted;
        ++output.revision;
        publish();
    }
    void coarse() {
        if (samples.size() < 200 || fixes.size() < 5)
            return;
        std::vector<GnssFix> available;
        for (const auto &[t, f] : fixes)
            if (t >= samples.front().stamp.measurement && t <= samples.back().stamp.measurement)
                available.push_back(f);
        if (available.size() < 10)
            return;
        const auto &last = available.back();
        const double age = seconds(samples.back().stamp.measurement - last.stamp.measurement);
        if (age > .5)
            return; // Old fixes cannot complete BIT or initialize a new filter.
        if (!frame) {
            const double imu_dt =
                seconds(samples.back().stamp.arrival - samples[samples.size() - 200].stamp.arrival);
            const auto begin = available.size() > 10 ? available.size() - 10 : 0;
            const double gps_dt =
                seconds(last.stamp.measurement - available[begin].stamp.measurement);
            bool continuous = true;
            for (auto i = begin + 1; i < available.size(); ++i) {
                const double dt =
                    seconds(available[i].stamp.measurement - available[i - 1].stamp.measurement);
                continuous &= dt >= .05 && dt <= .15;
            }
            if (!continuous || imu_dt <= 0 || 199 / imu_dt < 180 || 199 / imu_dt > 220 ||
                gps_dt <= 0 || (available.size() - begin - 1) / gps_dt < 8 ||
                (available.size() - begin - 1) / gps_dt > 12) {
                output.reason = "Built-in test requires continuous 200 Hz IMU and 10 Hz valid GNSS";
                return;
            }
            Vec3 force = Vec3::Zero();
            for (auto i = samples.end() - 200; i != samples.end(); ++i)
                force += i->force;
            if (force.norm() / 200 < .1 * standard_gravity) {
                output.reason = "Built-in test requires a usable specific-force direction";
                return;
            }
            frame.emplace(last.latitude, last.longitude, last.height);
            output.state.attitude() = Quat::FromTwoVectors(force, -frame->gravity(Vec3::Zero()));
            output.covariance.setZero();
            output.covariance.diagonal() << Vec3::Constant(100), Vec3::Constant(1),
                Vec3::Constant(pi * pi),
                Vec3::Constant(config.gyro_initial_sigma * config.gyro_initial_sigma),
                Vec3::Constant(config.accel_initial_sigma * config.accel_initial_sigma);
        }
        output.state.time = samples.back().stamp.measurement;
        output.state.position() = frame->position(last);
        output.state.velocity() = frame->velocity(last);
        output.stage = Stage::coarse_alignment;
        output.reason = "Heading unresolved; waiting for independent force/velocity directions";
        output.heading_observed = false;
        output.gnss_age_s = seconds(output.state.time - last.stamp.measurement);
        last_accepted = last.stamp.measurement;
        if (output.gnss_age_s > 2) {
            output.stage = Stage::degraded;
            output.reason = "GNSS stale during coarse alignment";
        }
        if (seconds(output.state.time - last_alignment) < 1)
            return;
        last_alignment = output.state.time;
        if (auto prior = align(config, *frame, samples, available)) {
            filter.emplace(config, *frame, prior->state, prior->receiver, prior->covariance);
            last_accepted = prior->state.time;
            // Every observation through this epoch contributed to the prior.
            // Start correction strictly afterward, never assimilating it twice.
            checkpoints.emplace(prior->state.time, Checkpoint{*filter, last_accepted});
            fixes.erase(fixes.begin(), fixes.upper_bound(prior->state.time));
            if (!fixes.empty() && fixes.begin()->first <= samples.back().stamp.measurement)
                correct(fixes.begin()->first);
            else if (filter->state().time < samples.back().stamp.measurement)
                filter->propagate(
                    interval(samples, filter->state().time, samples.back().stamp.measurement));
            ++output.revision;
            publish();
        }
    }
};
Estimator::Estimator(Config c) : impl_(std::make_unique<Impl>(std::move(c))) {}
Estimator::~Estimator() = default;
Estimator::Estimator(Estimator &&) noexcept = default;
Estimator &Estimator::operator=(Estimator &&) noexcept = default;
void Estimator::reset(std::string reason) { impl_->clear(std::move(reason)); }
void Estimator::imu(const ImuSample &input) {
    auto &p = *impl_;
    if (!input.omega.allFinite() || !input.force.allFinite()) {
        p.clear("Invalid IMU measurement; reacquiring alignment");
        return;
    }
    if (!p.samples.empty() && input.stamp.measurement <= p.samples.back().stamp.measurement)
        return;
    if (input.gap || input.saturated ||
        (!p.samples.empty() &&
         seconds(input.stamp.measurement - p.samples.back().stamp.measurement) > .02)) {
        p.clear("IMU discontinuity or saturation; reacquiring alignment");
        if (input.saturated)
            return;
    }
    auto sample = input;
    sample.gap = false; // A new history begins after a gap; it is not bridged.
    p.samples.push_back(sample);
    if (p.filter) {
        const Time previous = p.filter->state().time;
        p.filter->propagate(std::span(p.samples).last(2));
        auto pending = p.fixes.upper_bound(previous);
        if (pending != p.fixes.end() && pending->first <= sample.stamp.measurement)
            p.correct(pending->first);
        if (seconds(sample.stamp.measurement - p.checkpoints.rbegin()->first) >= .1)
            p.checkpoints.emplace(sample.stamp.measurement,
                                  Impl::Checkpoint{*p.filter, p.last_accepted});
        p.publish();
    } else {
        if (p.frame && p.samples.size() >= 2) {
            const double dt = seconds(sample.stamp.measurement -
                                      p.samples[p.samples.size() - 2].stamp.measurement);
            auto q = p.output.state.attitude();
            q = (exp_rotation((-p.frame->omega * dt).eval()) * q *
                 exp_rotation((sample.omega * dt).eval()))
                    .normalized();
            const double g = p.frame->gravity(Vec3::Zero()).norm();
            if (std::abs(sample.force.norm() - g) < .05 * g) {
                const Quat correction =
                    Quat::FromTwoVectors(q * sample.force, -p.frame->gravity(Vec3::Zero()));
                q = (Quat::Identity().slerp(1 - std::exp(-dt / 2), correction) * q).normalized();
            }
        }
        if (!p.frame || seconds(sample.stamp.measurement - p.last_alignment) >= 1)
            p.coarse();
        if (p.frame && !p.filter) {
            p.output.state.time = sample.stamp.measurement;
            p.output.gnss_age_s = seconds(sample.stamp.measurement - p.last_accepted);
            if (p.output.gnss_age_s > 2) {
                p.output.stage = Stage::degraded;
                p.output.reason = "GNSS stale during coarse alignment";
            }
        }
    }
    p.trim();
}
void Estimator::gnss(const GnssFix &f) {
    auto &p = *impl_;
    if (!f.valid) {
        if (!p.filter)
            p.fixes.clear(); // BIT/alignment require a fresh run of valid fixes.
        return;
    }
    if (!std::isfinite(f.latitude) || !std::isfinite(f.longitude) || !std::isfinite(f.height) ||
        std::abs(f.latitude) > pi / 2 || std::abs(f.longitude) > pi || !f.velocity.allFinite() ||
        !f.position_sigma.allFinite() || !f.velocity_sigma.allFinite() ||
        f.position_sigma.minCoeff() <= 0 || f.velocity_sigma.minCoeff() <= 0 ||
        !std::isfinite(f.stamp.uncertainty_s) || f.stamp.uncertainty_s < 0 || p.samples.empty())
        return;
    if (f.stamp.measurement < p.samples.front().stamp.measurement ||
        f.stamp.measurement > p.samples.back().stamp.measurement + std::chrono::seconds(1) ||
        (p.filter && f.stamp.measurement <= p.checkpoints.begin()->first))
        return;
    if (!p.fixes.emplace(f.stamp.measurement, f).second)
        return;
    const auto start = std::chrono::steady_clock::now();
    if (p.filter)
        p.correct(f.stamp.measurement);
    else
        p.coarse();
    p.output.update_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    p.trim();
}
const Estimate &Estimator::estimate() const { return impl_->output; }
const Earth *Estimator::earth() const { return impl_->frame ? &*impl_->frame : nullptr; }
} // namespace ins
