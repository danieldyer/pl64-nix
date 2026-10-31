#include "ins/acquisition.hpp"
#include "ins/config.hpp"
#include "ins/recording.hpp"
#include <fstream>
#include <iostream>
#include <map>

// Offline audit of a real recording. Re-decode raw bytes independently of the
// stored decoded records; retain the recorded live states without recomputing
// them or treating them as ground truth.
int main(int argc, char **argv) {
    using namespace ins;
    try {
        if (argc != 3)
            throw std::invalid_argument("ins-capture INPUT.mcap OUTPUT.jsonl");
        std::ofstream out(argv[2]);
        if (!out)
            throw std::runtime_error("Cannot open audit output");
        auto write = [&](const nlohmann::json &r) { out << r.dump() << '\n'; };
        std::unique_ptr<Acquisition> input;
        std::map<std::string, unsigned> stages;
        Framer gnss_wire(Source::gnss);
        std::map<std::string, unsigned> gnss_rejected, gnss_messages;
        std::uint64_t imu = 0, gnss = 0, valid = 0, gaps = 0, saturation = 0, raw = 0;
        Time imu_first{}, imu_last{}, gnss_first{}, gnss_last{};
        auto setup = [&](const Config &config) {
            input = std::make_unique<Acquisition>(config);
            input->on_imu = [&](const ImuSample &s) {
                if (!imu)
                    imu_first = s.stamp.measurement;
                imu_last = s.stamp.measurement;
                ++imu;
                gaps += s.gap;
                saturation += s.saturated;
                write({{"kind", "imu"},
                       {"time", seconds(s.stamp.measurement)},
                       {"arrival", seconds(s.stamp.arrival)},
                       {"counter", s.counter},
                       {"gap", s.gap},
                       {"saturated", s.saturated},
                       {"omega", {s.omega.x(), s.omega.y(), s.omega.z()}},
                       {"force", {s.force.x(), s.force.y(), s.force.z()}}});
            };
            input->on_gnss = [&](const GnssFix &f) {
                if (!gnss)
                    gnss_first = f.stamp.measurement;
                gnss_last = f.stamp.measurement;
                ++gnss;
                valid += f.valid;
                write({{"kind", "gnss"},
                       {"sequence", f.stamp.sequence},
                       {"time", seconds(f.stamp.measurement)},
                       {"arrival", seconds(f.stamp.arrival)},
                       {"valid", f.valid},
                       {"velocity", {f.velocity.x(), f.velocity.y(), f.velocity.z()}},
                       {"position_sigma",
                        {f.position_sigma.x(), f.position_sigma.y(), f.position_sigma.z()}},
                       {"velocity_sigma",
                        {f.velocity_sigma.x(), f.velocity_sigma.y(), f.velocity_sigma.z()}}});
            };
            input->on_discontinuity = [&](const std::string &reason) {
                write({{"kind", "discontinuity"}, {"reason", reason}});
            };
        };
        replay(argv[1], [&](const Record &r) {
            if (r.kind == Kind::metadata || r.kind == Kind::health) {
                write({{"kind", r.kind == Kind::metadata ? "metadata" : "health"},
                       {"detail", r.detail},
                       {"payload", r.payload},
                       {"arrival", seconds(r.stamp.arrival)}});
                if (!input && r.kind == Kind::metadata && r.source == Source::estimator) {
                    const auto settings = nlohmann::json::parse(r.detail);
                    setup(configuration(settings.at("navigation")));
                }
            }
            if (r.kind == Kind::raw) {
                if (!input)
                    throw std::runtime_error("Raw data precede recorded configuration");
                ++raw;
                if (r.source == Source::gnss)
                    gnss_wire.feed(r.payload, [&](auto frame) {
                        ++gnss_messages[std::to_string(frame[2]) + ":" + std::to_string(frame[3])];
                        const auto decoded = decode_gnss(frame);
                        if (!decoded)
                            ++gnss_rejected[decoded.error()];
                    });
                input->accept(r);
            }
            if (r.kind == Kind::state) {
                const auto stage = stage_name(static_cast<Stage>(r.flags & 0xff));
                ++stages[std::string(stage)];
                write({{"kind", "state"},
                       {"time", seconds(r.stamp.measurement)},
                       {"arrival", seconds(r.stamp.arrival)},
                       {"flags", r.flags},
                       {"stage", stage},
                       {"reason", r.detail},
                       {"values", r.values},
                       {"covariance", r.covariance},
                       {"gnss_age_s", r.gnss_age_s},
                       {"update_ms", r.update_ms},
                       {"normalized_residual", r.normalized_residual}});
            }
        });
        if (!input)
            throw std::runtime_error("Capture lacks navigation configuration");
        const auto rate = [](std::uint64_t count, Time first, Time last) -> nlohmann::json {
            if (count < 2 || last <= first)
                return nullptr; // Missing sensors are a valid startup/fault capture.
            return (count - 1) / seconds(last - first);
        };
        const nlohmann::json summary = {
            {"kind", "summary"},
            {"raw_records", raw},
            {"imu_samples", imu},
            {"gnss_epochs", gnss},
            {"valid_gnss_epochs", valid},
            {"imu_rate_hz", rate(imu, imu_first, imu_last)},
            {"gnss_rate_hz", rate(gnss, gnss_first, gnss_last)},
            {"imu_bad_checksums", input->imu_stats().bad_checksums},
            {"gnss_bad_checksums", input->gnss_stats().bad_checksums},
            {"gnss_messages", gnss_messages},
            {"gnss_decode_rejections", gnss_rejected},
            {"imu_gaps", gaps},
            {"imu_saturated", saturation},
            {"live_stages", stages}};
        write(summary);
        std::cout << summary.dump(2) << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
