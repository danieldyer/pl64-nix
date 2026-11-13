#include "analysis.hpp"
#include "ins/config.hpp"
#include "ins/estimator.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
namespace ins::audit {
double history = 12;
std::ofstream output;
std::uint64_t event_index = 0;
void emit(nlohmann::json value) {
    value["input_index"] = event_index;
    output << value.dump() << '\n';
}
} // namespace ins::audit
template <class T> T read(std::istream &f) {
    T x{};
    f.read(reinterpret_cast<char *>(&x), sizeof(x));
    if (!f)
        throw std::runtime_error("Truncated event input");
    return x;
}
int main(int argc, char **argv) {
    using namespace ins;
    if (argc < 4) {
        std::cerr << "nav-audit INPUT.events CONFIG.json OUTPUT.jsonl [holdout_every] [PLAYBACK.jsonl]\n";
        return 2;
    }
    std::ifstream c(argv[2]);
    nlohmann::json j;
    c >> j;
    // Old decoded recording configs predate the production history parameter;
    // retain their 12 s baseline unless an explicit counterfactual overrides it.
    audit::history = j.value("analysis_history_s", j.value("coarse_alignment_history_s", 12.));
    j.erase("analysis_history_s");
    j["coarse_alignment_history_s"] = audit::history;
    const double speed_scale = j.value("analysis_velocity_sigma_scale", 1.);
    j.erase("analysis_velocity_sigma_scale");
    const double recorded_delay = j.value("analysis_recorded_imu_delay_s", .001);
    j.erase("analysis_recorded_imu_delay_s");
    Mat3 gyro_map = Mat3::Identity();
    if (j.contains("analysis_gyro_matrix")) {
        auto v = j["analysis_gyro_matrix"].get<std::array<double, 9>>();
        gyro_map = Eigen::Map<Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(v.data());
        j.erase("analysis_gyro_matrix");
    }
    const double accel_sign = j.value("analysis_accel_sign", 1.);
    j.erase("analysis_accel_sign");
    const int holdout = argc > 4 ? std::stoi(argv[4]) : 0;
    std::ofstream playback;
    if (argc > 5) {
        playback.open(argv[5]);
        if (!playback)
            throw std::runtime_error("Cannot open playback output");
    }
    Config config = configuration(j);
    audit::output.open(argv[3]);
    if (!audit::output || !std::isfinite(audit::history) || audit::history <= 0 ||
        !std::isfinite(speed_scale) || speed_scale <= 0)
        throw std::runtime_error("Invalid analysis settings or output path");
    std::unique_ptr<Estimator> estimator;
    std::map<std::string, std::uint64_t> stages;
    std::uint64_t imus = 0, gnss = 0, withheld = 0;
    int last_stage = -1;
    Time last_report{};
    Time last_playback{};
    int playback_stage = -1;
    std::uint64_t segment = 0;
    auto reset = [&] {
        estimator = std::make_unique<Estimator>(config);
        last_stage = -1;
        last_report = {};
        last_playback = {};
        playback_stage = -1;
        ++segment;
        audit::emit({{"event", "reset"}});
    };
    reset();
    std::ifstream in(argv[1], std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot open event input");
    std::string fatal;
    try {
        while (in.peek() != EOF) {
            const auto k = read<std::uint8_t>(in);
            const auto t = Time(read<std::int64_t>(in));
            const auto a = Time(read<std::int64_t>(in));
            const auto flags = read<std::uint32_t>(in);
            const auto counter = read<std::uint64_t>(in);
            ++audit::event_index;
            if (k == 254) {
                reset();
                continue;
            }
            if (k == 1) {
                ImuSample s;
                s.stamp.measurement = t - time_from_seconds(config.imu_delay_s - recorded_delay);
                s.stamp.arrival = a;
                s.stamp.uncertainty_s = config.imu_timing_sigma_s;
                for (int n = 0; n < 3; ++n)
                    s.omega[n] = read<double>(in);
                for (int n = 0; n < 3; ++n)
                    s.force[n] = read<double>(in);
                s.omega = gyro_map * s.omega.eval();
                s.force *= accel_sign;
                s.counter = counter;
                s.gap = flags & 1;
                s.saturated = flags & 2;
                estimator->imu(s);
                ++imus;
                const auto &e = estimator->estimate();
                const auto st = static_cast<int>(e.stage);
                ++stages[std::string(stage_name(e.stage))];
                // Optional display export; observation only, never feeds the estimator.
                // Keep the diagnostic stream and its original 1 Hz cadence unchanged.
                if (playback.is_open() &&
                    (st != playback_stage || seconds(t - last_playback) >= .05)) {
                    const auto *earth = estimator->earth();
                    nlohmann::json row{{"time", seconds(t)},
                                       {"state_time", seconds(e.state.time)},
                                       {"stage", st},
                                       {"segment", segment},
                                       {"heading_observed", e.heading_observed},
                                       {"gnss_age", e.gnss_age_s},
                                       {"reason", e.reason},
                                       {"state", e.state.data}};
                    if (earth) {
                        // The filter frame can change after a reset. Export the actual
                        // anchor, not an assumed first-fix origin.
                        row["earth_origin"] = {earth->origin.x(), earth->origin.y(), earth->origin.z()};
                        auto &basis = row["earth_to_ecef"] = nlohmann::json::array();
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j)
                                basis.push_back(earth->to_ecef(i, j));
                    }
                    const Vec3 down = e.state.attitude().conjugate() * Vec3::UnitZ();
                    const Mat3 pa = e.covariance.block<3, 3>(6, 6);
                    row["heading_sigma_deg"] = std::sqrt(std::max(0., down.dot(pa * down))) / degrees;
                    row["tilt_sigma_deg"] = std::sqrt(std::max(
                        0., (pa * (Mat3::Identity() - down * down.transpose())).trace())) / degrees;
                    playback << row.dump() << '\n';
                    playback_stage = st;
                    last_playback = t;
                }
                if (st != last_stage || seconds(t - last_report) >= 1) {
                    const Mat3 rot = e.state.attitude().toRotationMatrix();
                    const Vec3 down = rot.transpose() * Vec3::UnitZ();
                    const Mat3 pa = e.covariance.block<3, 3>(6, 6);
                    const double hs = std::sqrt(std::max(0., down.dot(pa * down))) / degrees,
                                 ts = std::sqrt(std::max(
                                          0., (pa * (Mat3::Identity() - down * down.transpose())).trace())) /
                                      degrees;
                    audit::emit({{"event", "state"},
                                 {"time", seconds(t)},
                                 {"stage", stage_name(e.stage)},
                                 {"heading_sigma_deg", hs},
                                 {"tilt_sigma_deg", ts},
                                 {"velocity_sigma_norm", std::sqrt(e.covariance.block<3, 3>(3, 3).trace())},
                                 {"gnss_age", e.gnss_age_s},
                                 {"nis", e.normalized_residual * e.normalized_residual},
                                 {"reason", e.reason},
                                 {"state", e.state.data}});
                    last_stage = st;
                    last_report = t;
                }
            } else if (k == 2) {
                GnssFix f;
                f.stamp.measurement = t;
                f.stamp.arrival = a;
                f.stamp.sequence = counter;
                f.stamp.uncertainty_s = config.gnss_timing_sigma_s;
                f.valid = flags & 1;
                f.latitude = read<double>(in);
                f.longitude = read<double>(in);
                f.height = read<double>(in);
                for (int n = 0; n < 3; ++n)
                    f.velocity[n] = read<double>(in);
                for (int n = 0; n < 3; ++n)
                    f.position_sigma[n] = read<double>(in);
                for (int n = 0; n < 3; ++n)
                    f.velocity_sigma[n] = read<double>(in) * speed_scale;
                ++gnss;
                const auto before_stage = estimator->estimate().stage;
                const bool tracking = before_stage == Stage::fine_alignment ||
                                      before_stage == Stage::navigating ||
                                      (before_stage == Stage::degraded &&
                                       estimator->estimate().reason ==
                                           "GNSS stale; uncertainty grows under inertial propagation");
                if (holdout && f.valid && tracking && gnss % holdout == 0) {
                    ++withheld;
                    const auto &e = estimator->estimate();
                    const auto *earth = estimator->earth();
                    if (earth) {
                        const double dt = seconds(t - e.state.time);
                        const Vec3 dp =
                            (earth->position(f) - e.state.position() - e.state.velocity() * dt).eval();
                        const Vec3 dv = (earth->velocity(f) - e.state.velocity()).eval();
                        audit::emit({{"event", "holdout"},
                                     {"stage", stage_name(e.stage)},
                                     {"time", seconds(t)},
                                     {"dt", dt},
                                     {"position_residual", {dp.x(), dp.y(), dp.z()}},
                                     {"velocity_residual", {dv.x(), dv.y(), dv.z()}},
                                     {"velocity_sigma",
                                      {f.velocity_sigma.x(), f.velocity_sigma.y(), f.velocity_sigma.z()}},
                                     {"velocity_prediction_variance",
                                      {e.covariance(3, 3), e.covariance(4, 4), e.covariance(5, 5)}}});
                    }
                } else
                    estimator->gnss(f);
            } else
                throw std::runtime_error("Unexpected event kind");
        }
    } catch (const std::exception &e) {
        fatal = e.what();
        audit::emit({{"event", "fatal"}, {"error", fatal}});
    }
    const auto &e = estimator->estimate();
    audit::emit({{"event", "summary"},
                 {"imu", imus},
                 {"gnss", gnss},
                 {"withheld", withheld},
                 {"stage_counts", stages},
                 {"final_stage", stage_name(e.stage)},
                 {"fatal", fatal}});
    std::cout << nlohmann::json{{"input", argv[1]}, {"stage_counts", stages}, {"fatal", fatal}}.dump()
              << std::endl;
    return fatal.empty() ? 0 : 1;
}
