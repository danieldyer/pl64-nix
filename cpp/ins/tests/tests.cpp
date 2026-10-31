#include "ins/acquisition.hpp"
#include "ins/estimator.hpp"
#include "ins/recording.hpp"
#include "simulator.hpp"
#include <Eigen/Eigenvalues>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool yes, const char *message) {
    if (!yes)
        throw std::runtime_error(message);
}
void protocol() {
    // Independent hardware fixture from the verified 100 Hz capture, not from
    // the simulator's encoder. The four decimal counter bytes encode 197.
    const std::array<std::uint8_t, 33> captured{
        0x77, 0x20, 0x00, 0x70, 0xb8, 0xd1, 0x23, 0x3d, 0x00, 0xce, 0x35,
        0xbd, 0x20, 0xc6, 0x3a, 0xbd, 0x5c, 0xe2, 0x3b, 0x3b, 0x30, 0x03,
        0x11, 0x3c, 0xc9, 0xf6, 0x7f, 0x3f, 0x01, 0x97, 0x00, 0x00, 0x5f};
    const auto observed = ins::decode_imu(captured);
    check(observed && observed->counter == 197, "recorded hardware counter");
    check(std::abs(observed->omega.x() / ins::degrees - .03999492526054382) < 1e-10 &&
              std::abs(observed->force.z() / ins::standard_gravity - .9998593926429749) < 1e-10,
          "recorded hardware floating-point units");
    const ins::Vec3 w(.123, -.456, .789), a(1, 2, -9.8);
    for (unsigned n = 0; n < 256; ++n) {
        auto wire = ins::encode_imu(w, a, n);
        auto s = ins::decode_imu(wire);
        check(s.has_value(), "IMU decoder");
        check(s->counter == n, "BCD counter");
        check((s->omega - w).norm() < 1e-7 && (s->force - a).norm() < 1e-6, "IMU SI units");
    }
    ins::Framer parser(ins::Source::imu);
    unsigned count = 0;
    auto good = ins::encode_imu(w, a, 12), bad = good;
    bad[7] ^= 1;
    parser.feed(bad, [&](auto) { ++count; });
    for (auto b : good)
        parser.feed(std::span(&b, 1), [&](auto f) {
            check(ins::decode_imu(f).has_value(), "fragmented frame");
            ++count;
        });
    check(count == 1 && parser.stats.bad_checksums > 0, "checksum recovery");
    check(ins::imu_command(0x0c, 7) == std::vector<std::uint8_t>({0x77, 5, 0, 0x0c, 7, 0x18}),
          "200 Hz command");
    ins::GnssFix f;
    f.latitude = -38 * ins::degrees;
    f.longitude = 145 * ins::degrees;
    f.height = 123.456;
    f.velocity = {1.25, -2.5, .125};
    f.valid = true;
    f.satellites = 12;
    const ins::Time utc =
        std::chrono::duration_cast<ins::Time>(
            std::chrono::sys_days{std::chrono::year{2026} / 10 / 25}.time_since_epoch()) +
        ins::time_from_seconds(.123456);
    auto gps = ins::decode_gnss(ins::encode_gnss(f, utc));
    check(gps && gps->stamp.measurement == utc && gps->valid, "GNSS epoch");
    check((gps->velocity - f.velocity).norm() < 1e-9, "GNSS velocity");
    check(ins::decode_imu(
              ins::encode_imu(ins::Vec3::Zero(), ins::Vec3(0, 0, 2 * ins::standard_gravity), 0))
              ->saturated,
          "2 g saturation");
}
void physics() {
    ins::Earth earth(-38 * ins::degrees, 145 * ins::degrees, 50);
    const auto llh = ins::ecef_to_geodetic(earth.origin);
    check(std::abs(llh.x() + 38 * ins::degrees) < 1e-12 && std::abs(llh.z() - 50) < 1e-6,
          "geodetic roundtrip");
    check(std::abs(earth.gravity(ins::Vec3::Zero()).norm() - 9.80) < .02, "gravity magnitude");
    ins::State initial;
    initial.attitude() = ins::Quat(Eigen::AngleAxisd(1.3, ins::Vec3(1, 2, 3).normalized()));
    ins::ImuSample sample;
    sample.omega = initial.attitude().conjugate() * earth.omega;
    sample.force = initial.attitude().conjugate() * (-earth.gravity(ins::Vec3::Zero()));
    std::vector<ins::ImuSample> samples;
    for (int i = 0; i <= 2000; ++i) {
        sample.stamp.measurement = ins::time_from_seconds(i * .005);
        samples.push_back(sample);
    }
    auto end = ins::propagate(initial, samples, earth);
    check(end.position().norm() < 1e-8 && end.velocity().norm() < 1e-8,
          "Earth-fixed static propagation");
    check(ins::log_rotation(ins::Quat(initial.attitude().conjugate() * end.attitude())).norm() <
              1e-10,
          "Earth rate cancellation");
    const auto p = ins::process_covariance(initial, std::span(samples).first(21), earth, {});
    check(Eigen::SelfAdjointEigenSolver<ins::Mat15>(p).eigenvalues().minCoeff() > 0,
          "positive IMU covariance");
    for (const auto scenario : {"aircraft", "curved_ship", "winding_road", "traffic"}) {
        ins::Simulation motion;
        motion.scenario = scenario;
        motion.start_delay_s = 0;
        auto truth = ins::trajectory(motion, 0);
        auto propagated = truth.state;
        std::optional<ins::ImuSample> previous;
        for (int i = 0; i <= 2000; ++i) {
            const double t = i * .005;
            truth = ins::trajectory(motion, t);
            ins::ImuSample s;
            s.stamp.measurement = ins::time_from_seconds(t);
            s.omega = truth.body_rate + truth.state.attitude().conjugate() * earth.omega;
            s.force = truth.state.attitude().conjugate() *
                      (truth.acceleration - earth.gravity(ins::Vec3(truth.state.position())) +
                       2 * earth.omega.cross(truth.state.velocity()));
            if (previous)
                ins::integrate(propagated.data.data(), *previous, s, earth);
            previous = s;
        }
        check((propagated.position() - truth.state.position()).norm() < .003,
              "independent analytic 3D position trajectory");
        check((propagated.velocity() - truth.state.velocity()).norm() < .001,
              "independent analytic 3D velocity trajectory");
        check(
            ins::log_rotation(ins::Quat(propagated.attitude().conjugate() * truth.state.attitude()))
                    .norm() < 1e-5,
            "coning attitude integration");
    }
}
void clocks_and_noise() {
    ins::Config config;
    ins::ImuClock clock(config);
    ins::ImuSample sample;
    sample.counter = 254;
    ins::Stamp arrival;
    arrival.arrival = ins::time_from_seconds(1);
    check(clock.stamp(sample, arrival), "first clock stamp");
    for (unsigned counter : {255, 0, 1}) {
        sample.counter = counter;
        arrival.arrival += ins::time_from_seconds(.005);
        check(clock.stamp(sample, arrival) && !sample.gap, "counter rollover");
    }
    check(!clock.stamp(sample, arrival), "duplicate suppression");
    sample.counter = 3;
    arrival.arrival += ins::time_from_seconds(.010);
    check(clock.stamp(sample, arrival) && sample.gap, "missing counter detection");
    ins::Acquisition acquisition(config);
    unsigned discontinuities = 0;
    unsigned fixes = 0;
    acquisition.on_gnss = [&](const auto &) { ++fixes; };
    acquisition.on_discontinuity = [&](auto) { ++discontinuities; };
    ins::GnssFix fix;
    fix.valid = true;
    ins::Record r;
    r.source = ins::Source::gnss;
    r.stamp.utc = std::chrono::duration_cast<ins::Time>(
        std::chrono::sys_days{std::chrono::year{2026} / 10 / 25}.time_since_epoch());
    r.stamp.arrival = ins::time_from_seconds(1);
    r.payload = ins::encode_gnss(fix, r.stamp.utc);
    acquisition.accept(r);
    acquisition.accept(r);
    check(fixes == 1, "duplicate native GNSS epoch suppressed");
    r.stamp.utc += std::chrono::seconds(1);
    r.payload = ins::encode_gnss(fix, r.stamp.utc);
    acquisition.accept(r);
    check(discontinuities == 1, "UTC step reset");
    ins::Simulation sim;
    sim.scenario = "static";
    sim.duration_s = 60;
    sim.clock_ppm = 0;
    sim.gyro_bias.setZero();
    sim.accel_bias.setZero();
    sim.gyro_scale_ppm.setZero();
    sim.accel_scale_ppm.setZero();
    sim.gyro_temperature.setZero();
    sim.accel_temperature.setZero();
    sim.cross_axis = 0;
    sim.gyro_g_sensitivity = 0;
    sim.sensor_noise.gyro_bias_walk.setZero();
    sim.sensor_noise.accel_bias_walk.setZero();
    ins::Vec3 sum = ins::Vec3::Zero(), squares = ins::Vec3::Zero();
    unsigned count = 0;
    ins::Time previous_time{};
    ins::simulate(sim, [&](const auto &event) {
        check(event.stamp.arrival >= previous_time, "simulation arrival order");
        previous_time = event.stamp.arrival;
        if (event.kind != ins::Kind::raw || event.source != ins::Source::imu)
            return;
        const auto s = ins::decode_imu(event.payload);
        check(s.has_value(), "simulated wire frame");
        sum += s->omega;
        squares += s->omega.array().square().matrix();
        ++count;
    });
    check(count == 12001, "200 Hz simulation cadence");
    const ins::Vec3 variance = squares / count - (sum / count).array().square().matrix();
    check(((variance.array().sqrt() * std::sqrt(.005) / sim.sensor_noise.gyro_density.array()) - 1)
                  .abs()
                  .maxCoeff() < .04,
          "measured gyro noise density reproduced");
    const auto roundtrip = ins::simulation_configuration(ins::simulation_json(sim));
    check(ins::simulation_json(roundtrip) == ins::simulation_json(sim),
          "complete reproducible simulator configuration");
    check(ins::configuration_json(ins::configuration(ins::configuration_json(config))) ==
              ins::configuration_json(config),
          "navigation configuration roundtrip");
}
void recording() {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("pl64-ins-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".mcap");
    ins::Record r;
    r.payload = ins::encode_imu(ins::Vec3::Zero(), ins::Vec3(0, 0, -9.8), 17);
    r.stamp.arrival = ins::Time(123);
    r.stamp.utc = ins::Time(456);
    r.stamp.measurement = ins::Time(100);
    {
        ins::Recorder out(path, INS_TEST_SCHEMA);
        out.write(r);
        for (unsigned size = 1; size <= 16; ++size) {
            auto varied = r;
            varied.payload.assign(size, static_cast<std::uint8_t>(size));
            varied.values = {1.25, -2.5};
            varied.covariance = {0.125};
            varied.detail = std::string(size, 'x');
            out.write(varied);
        }
        out.close();
    }
    unsigned count = 0;
    ins::replay(path, [&](const auto &v) {
        if (!count)
            check(v.payload == r.payload && v.stamp.arrival == r.stamp.arrival,
                  "MCAP raw roundtrip");
        else
            check(v.payload == std::vector<std::uint8_t>(count, count) &&
                      v.values == std::vector<double>{1.25, -2.5} &&
                      v.covariance == std::vector<double>{0.125} &&
                      v.detail == std::string(count, 'x'),
                  "MCAP variable-length records preserve aligned scalar access");
        ++count;
    });
    check(count == 17, "MCAP message count");
    bool refused = false;
    try {
        ins::Recorder duplicate(path, INS_TEST_SCHEMA);
    } catch (const std::exception &) {
        refused = true;
    }
    check(refused, "never overwrite recording");
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
        const auto at = std::search(bytes.begin(), bytes.end(), r.payload.begin(), r.payload.end());
        check(at != bytes.end(), "recorded chunk fixture");
        const auto offset = at - bytes.begin() + 8;
        bytes[offset] ^= 1;
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        }
        refused = false;
        try {
            ins::replay(path, [](const auto &) {});
        } catch (const std::exception &) {
            refused = true;
        }
        check(refused, "corrupted chunk CRC rejected");
        bytes[offset] ^= 1;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
    refused = false;
    try {
        ins::replay(path, [](const auto &) {});
    } catch (const std::exception &) {
        refused = true;
    }
    check(refused, "truncated recording rejected");
    std::filesystem::remove(path);
}
void deterministic_replay() {
    struct Harness {
        ins::Estimator estimator;
        ins::Acquisition acquisition;
        explicit Harness(ins::Config config) : estimator(config), acquisition(config) {
            acquisition.on_imu = [this](const auto &s) { estimator.imu(s); };
            acquisition.on_gnss = [this](const auto &f) { estimator.gnss(f); };
            acquisition.on_discontinuity = [this](auto reason) {
                estimator.reset(std::move(reason));
            };
        }
        void accept(const ins::Record &r) {
            if (r.kind != ins::Kind::raw)
                return;
            acquisition.accept(r);
        }
    };
    ins::Simulation simulation;
    simulation.duration_s = 12;
    ins::Config config;
    config.gnss_lever = simulation.gnss_lever;
    Harness original(config), replayed(config);
    const auto path =
        std::filesystem::temp_directory_path() /
        ("pl64-ins-replay-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".mcap");
    {
        ins::Recorder recorder(path, INS_TEST_SCHEMA);
        ins::simulate(simulation, [&](const auto &record) {
            recorder.write(record);
            original.accept(record);
        });
        recorder.close();
    }
    ins::replay(path, [&](const auto &record) { replayed.accept(record); });
    const auto a = original.estimator.estimate(), b = replayed.estimator.estimate();
    check(a.stage != ins::Stage::built_in_test && a.stage == b.stage,
          "simulation/replay stage equivalence");
    check(a.state.time == b.state.time && ins::difference(a.state, b.state).norm() < 1e-12 &&
              (a.covariance - b.covariance).norm() < 1e-10,
          "wire-only MCAP replay reproduces navigation state and covariance");
    std::filesystem::remove(path);
}
} // namespace
int main() {
    try {
        protocol();
        physics();
        clocks_and_noise();
        recording();
        deterministic_replay();
        std::cout << "Hardware protocol, rotating-Earth dynamics, clock faults, sensor noise and "
                     "deterministic MCAP replay tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
