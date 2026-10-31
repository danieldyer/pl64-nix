#include "ins/acquisition.hpp"
#include "ins/estimator.hpp"
#include "ins/ukf.hpp"
#include "simulator.hpp"
#include <Eigen/Cholesky>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace ins;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void linear_update() {
    Config cfg;
    Earth earth(-38 * degrees, 145 * degrees, 50);
    State state;
    Mat18 p = Mat18::Identity() * .01;
    p.topLeftCorner<6, 6>() *= 100;
    p.block<3, 3>(0, 15) = Mat3::Identity() * -.02;
    p.block<3, 3>(15, 0) = p.block<3, 3>(0, 15).transpose();
    Ukf filter(cfg, earth, state, Vec3::Zero(), p);
    GnssFix fix;
    fix.valid = true;
    fix.latitude = -38 * degrees;
    fix.longitude = 145 * degrees;
    fix.height = 50.25;
    fix.velocity = {.1, -.2, .05};
    fix.stamp.uncertainty_s = 1e-9;
    ImuSample imu;
    imu.omega = earth.omega;
    imu.force = -earth.gravity(Vec3::Zero());
    Eigen::Matrix<double, 6, 18> h = Eigen::Matrix<double, 6, 18>::Zero();
    h.leftCols<6>().setIdentity();
    h.block<3, 3>(0, 15).setIdentity();
    const Mat6 noise = gnss_covariance(state, imu.force, fix, earth, cfg);
    const Mat6 innovation = h * p * h.transpose() + noise;
    const Eigen::Matrix<double, 18, 6> gain = innovation.llt().solve(h * p).transpose();
    Vec6 observation;
    observation << earth.position(fix), earth.velocity(fix);
    const Vec18 increment = gain * observation;
    const Mat18 expected = p - gain * innovation * gain.transpose();
    check(filter.observe(fix, imu), "ordinary Gaussian observation rejected");
    check((difference(displaced(state, increment.head<15>()), filter.state())).norm() < 1e-11,
          "square-root UKF does not match analytic Kalman mean");
    check((filter.root() * filter.root().transpose() - expected).norm() < 1e-10,
          "square-root UKF does not match analytic Kalman covariance");
    const auto before = filter;
    fix.height += 10000;
    check(!filter.observe(fix, imu), "gross outlier accepted");
    check(difference(before.state(), filter.state()).norm() == 0 &&
              (before.root() - filter.root()).norm() == 0,
          "rejected fix changed posterior");
}
void built_in_test() {
    auto drive = [](Estimator &estimator, double start, unsigned count, unsigned imu_hz,
                    unsigned gnss_every, bool valid = true, bool gravity = true) {
        for (unsigned i = 0; i < count; ++i) {
            ImuSample imu;
            imu.stamp.arrival = imu.stamp.measurement =
                time_from_seconds(start + double(i) / imu_hz);
            if (gravity)
                imu.force.z() = -standard_gravity;
            estimator.imu(imu);
            if (gnss_every && i % gnss_every == 0) {
                GnssFix fix;
                fix.stamp = imu.stamp;
                fix.valid = valid;
                fix.latitude = -38 * degrees;
                fix.longitude = 145 * degrees;
                estimator.gnss(fix);
            }
        }
    };
    auto bit = [](const Estimator &e) {
        check(e.estimate().stage == Stage::built_in_test && !e.earth() &&
                  !e.estimate().heading_observed,
              "BIT admitted missing, stale, invalid or wrong-rate sensors");
    };
    Estimator absent, no_fix, slow_imu, slow_gnss, zero_force, stale;
    drive(absent, 1, 600, 200, 0);
    drive(no_fix, 1, 600, 200, 20, false);
    drive(slow_imu, 1, 300, 100, 10);
    drive(slow_gnss, 1, 2400, 200, 200);
    drive(zero_force, 1, 600, 200, 20, true, false);
    // Enough old fixes, but initially too few IMU samples to leave BIT.
    drive(stale, 1, 180, 200, 18, true, false);
    for (unsigned i = 180; i < 600; ++i) {
        ImuSample imu;
        imu.stamp.arrival = imu.stamp.measurement = time_from_seconds(1 + i * .005);
        // Keep force unusable until all stored fixes are stale.
        if (i >= 400)
            imu.force.z() = -standard_gravity;
        stale.imu(imu);
    }
    for (auto *e : {&absent, &no_fix, &slow_imu, &slow_gnss, &zero_force, &stale})
        bit(*e);

    Estimator normal;
    drive(normal, 1, 400, 200, 20);
    check(normal.estimate().stage == Stage::coarse_alignment && normal.earth(),
          "healthy sensors did not complete BIT");
    normal.reset("Sensor restarted");
    bit(normal);
    drive(normal, 3, 100, 200, 20);
    bit(normal); // Previous history must not shorten the next BIT.
    drive(normal, 3.5, 300, 200, 20);
    check(normal.estimate().stage == Stage::coarse_alignment,
          "fresh sensors did not complete BIT after restart");
    ImuSample gap;
    gap.stamp.arrival = gap.stamp.measurement = time_from_seconds(5);
    gap.gap = true;
    gap.force.z() = -standard_gravity;
    normal.imu(gap);
    bit(normal);
    drive(normal, 5.005, 400, 200, 0);
    bit(normal);
    drive(normal, 7.005, 400, 200, 20);
    check(normal.estimate().stage == Stage::coarse_alignment, "post-gap BIT could not reacquire");
}
void navigation(const std::string &scenario, double heading, bool delayed = false) {
    Simulation sim;
    sim.scenario = scenario;
    sim.heading = heading;
    sim.duration_s = 70;
    sim.start_delay_s = scenario == "maneuver" ? 5 : 0;
    sim.stop_duration_s = 20;
    if (scenario == "aircraft") {
        sim.roll = 20 * degrees;
        sim.pitch = -15 * degrees;
    }
    sim.dropout_start_s = 65;
    sim.dropout_end_s = 100;
    Config cfg;
    cfg.gnss_lever = sim.gnss_lever;
    Estimator immediate(cfg), late(cfg);
    Acquisition input(cfg);
    std::multimap<Time, GnssFix> pending;
    Time arrival, last_imu;
    unsigned ready = 0, wrong = 0;
    bool fine = false;
    input.on_imu = [&](const auto &sample) {
        last_imu = sample.stamp.measurement;
        immediate.imu(sample);
        late.imu(sample);
    };
    input.on_gnss = [&](const auto &fix) {
        immediate.gnss(fix);
        const double t = seconds(fix.stamp.measurement);
        if (delayed && t > 30)
            pending.emplace(
                arrival + time_from_seconds((fix.stamp.sequence / 100) % 7 == 0 ? 1.2 : .3), fix);
        else
            late.gnss(fix);
    };
    input.on_discontinuity = [&](auto why) {
        immediate.reset(why);
        late.reset(why);
    };
    simulate(sim, [&](const Record &record) {
        arrival = record.stamp.arrival;
        input.accept(record);
        while (!pending.empty() && pending.begin()->first <= arrival) {
            late.gnss(pending.begin()->second);
            late.gnss(pending.begin()->second); // duplicates must not add information
            pending.erase(pending.begin());
        }
        if (record.kind != Kind::raw || record.source != Source::imu || !immediate.earth())
            return;
        const auto &e = immediate.estimate();
        check(std::all_of(e.state.data.begin(), e.state.data.end(),
                          [](double x) { return std::isfinite(x); }) &&
                  e.covariance.allFinite(),
              "nonfinite navigation state");
        check(e.covariance.llt().info() == Eigen::Success,
              "navigation covariance lost positive definiteness");
        fine |= e.stage == Stage::fine_alignment || e.stage == Stage::navigating;
        if (e.heading_observed) {
            ++ready;
            const Earth origin(sim.latitude, sim.longitude, sim.height);
            const auto &frame = *immediate.earth();
            auto truth = trajectory(sim, seconds(e.state.time)).state;
            truth.attitude() = Quat(frame.to_ecef.transpose() * origin.to_ecef) * truth.attitude();
            const Vec3 error =
                log_rotation(Quat(e.state.attitude().conjugate() * truth.attitude()));
            if (error.norm() > 10 * degrees)
                ++wrong;
        }
    });
    const auto &a = immediate.estimate(), &b = late.estimate();
    std::cout << scenario << " heading=" << heading / degrees << " fine=" << fine
              << " ready_samples=" << ready << " wrong=" << wrong << " end=" << stage_name(a.stage)
              << "\n";
    if (scenario == "static" || scenario == "straight")
        check(!fine && !ready, "unexcited data falsely localized heading");
    else if (scenario != "curved_ship")
        check(fine && ready, "informative motion failed to align");
    check(wrong == 0, "incorrect attitude declared aligned");
    check(a.state.time == last_imu, "navigation output lags the latest IMU");
    if (fine)
        check(a.stage == Stage::degraded, "GNSS outage did not mark navigation degraded");
    if (delayed) {
        std::cout << "delayed difference " << difference(a.state, b.state).norm() << " "
                  << (a.covariance - b.covariance).norm() << '\n';
        check(difference(a.state, b.state).norm() < 1e-8 &&
                  (a.covariance - b.covariance).norm() < 1e-8,
              "out-of-order/duplicate GNSS differs from chronological UKF");
        const auto before = late.estimate();
        GnssFix stale;
        stale.valid = true;
        stale.latitude = sim.latitude;
        stale.longitude = sim.longitude;
        stale.height = sim.height;
        stale.stamp.measurement = time_from_seconds(5);
        late.gnss(stale);
        check(difference(before.state, late.estimate().state).norm() == 0 &&
                  (before.covariance - late.estimate().covariance).norm() == 0,
              "expired GNSS changed the posterior");
        ImuSample fault;
        fault.stamp.measurement = last_imu + std::chrono::milliseconds(5);
        fault.saturated = true;
        late.imu(fault);
        check(late.estimate().stage == Stage::built_in_test && !late.estimate().heading_observed &&
                  !late.earth(),
              "IMU saturation retained a usable navigation state");
    }
}
} // namespace
int main() {
    try {
        linear_update();
        built_in_test();
        for (const auto *scenario : {"static", "straight", "maneuver", "traffic", "aircraft",
                                     "curved_ship", "winding_road"})
            navigation(scenario, 125 * degrees, std::string(scenario) == "maneuver");
        navigation("maneuver", -175 * degrees);
        navigation("aircraft", -40 * degrees);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
