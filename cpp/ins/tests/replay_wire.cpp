// Offline field-recording validation through the uninstrumented production
// Framer -> decode -> Acquisition/clock -> Estimator path. The compact .wire
// format is emitted by experiments/navigation-replay/decode.py from MCAP bytes.
#include "ins/acquisition.hpp"
#include "ins/config.hpp"
#include "ins/estimator.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <memory>

template <class T> T read(std::istream &in) {
    T value{};
    in.read(reinterpret_cast<char *>(&value), sizeof(value));
    if (!in)
        throw std::runtime_error("Truncated wire input");
    return value;
}
int main(int argc, char **argv) {
    using namespace ins;
    try {
        if (argc != 4)
            throw std::invalid_argument("ins-replay-wire INPUT.wire CONFIG.json OUTPUT.jsonl");
        std::ifstream input(argv[1], std::ios::binary), settings(argv[2]);
        std::ofstream output(argv[3]);
        if (!input || !settings || !output)
            throw std::runtime_error("Cannot open replay inputs/output");
        nlohmann::json j;
        settings >> j;
        const Config config = configuration(j.contains("navigation") ? j.at("navigation") : j);
        output << nlohmann::json{{"event", "configuration"},
                                 {"navigation", configuration_json(config)},
                                 {"imu_frame_convention", imu_frame_convention}}.dump() << '\n';
        std::unique_ptr<Estimator> estimator;
        std::unique_ptr<Acquisition> acquisition;
        std::map<std::string, std::uint64_t> stages;
        std::uint64_t samples = 0, fixes = 0, checksums = 0, segments = 0;
        double max_update_ms = 0;
        Time last_report{}, first{}, last{};
        int last_stage = -1;
        auto reset = [&] {
            if (acquisition)
                checksums += acquisition->imu_stats().bad_checksums;
            estimator = std::make_unique<Estimator>(config);
            acquisition = std::make_unique<Acquisition>(config);
            last_report = {};
            last_stage = -1;
            ++segments;
            acquisition->on_imu = [&](const ImuSample &s) {
                const auto start = std::chrono::steady_clock::now();
                estimator->imu(s);
                const auto elapsed = std::chrono::steady_clock::now() - start;
                max_update_ms = std::max(max_update_ms,
                    std::chrono::duration<double, std::milli>(elapsed).count());
                if (!samples)
                    first = s.stamp.measurement;
                last = s.stamp.measurement;
                ++samples;
                const auto &e = estimator->estimate();
                ++stages[std::string(stage_name(e.stage))];
                const int stage = static_cast<int>(e.stage);
                if (stage != last_stage || seconds(last - last_report) >= 1) {
                    const Vec3 down = e.state.attitude().conjugate() * Vec3::UnitZ();
                    const Mat3 pa = e.covariance.block<3, 3>(6, 6);
                    nlohmann::json row{{"event", "state"}, {"time", seconds(last)},
                        {"segment", segments}, {"stage", stage_name(e.stage)},
                        {"state", e.state.data}, {"gnss_age", e.gnss_age_s},
                        {"heading_observed", e.heading_observed}, {"reason", e.reason},
                        {"heading_sigma_deg", std::sqrt(std::max(0., down.dot(pa * down))) / degrees},
                        {"tilt_sigma_deg", std::sqrt(std::max(0.,
                            (pa * (Mat3::Identity() - down * down.transpose())).trace())) / degrees}};
                    if (const auto *earth = estimator->earth()) {
                        row["earth_origin"] = {earth->origin.x(), earth->origin.y(), earth->origin.z()};
                        auto &basis = row["earth_to_ecef"] = nlohmann::json::array();
                        for (int i = 0; i < 3; ++i)
                            for (int k = 0; k < 3; ++k)
                                basis.push_back(earth->to_ecef(i, k));
                    }
                    output << row.dump() << '\n';
                    last_report = last;
                    last_stage = stage;
                }
            };
            acquisition->on_gnss = [&](const GnssFix &f) {
                estimator->gnss(f);
                ++fixes;
            };
            acquisition->on_discontinuity = [&](auto reason) { estimator->reset(std::move(reason)); };
        };
        reset();
        while (input.peek() != EOF) {
            const auto source = read<std::uint8_t>(input);
            Record r;
            r.source = static_cast<Source>(source);
            r.stamp.arrival = Time(read<std::int64_t>(input));
            r.stamp.utc = Time(read<std::int64_t>(input));
            r.stamp.measurement = Time(read<std::int64_t>(input));
            r.stamp.uncertainty_s = read<double>(input);
            r.stamp.sequence = read<std::uint64_t>(input);
            const auto size = read<std::uint32_t>(input);
            if (size > 1024 * 1024)
                throw std::runtime_error("Invalid serial read length");
            r.payload.resize(size);
            input.read(reinterpret_cast<char *>(r.payload.data()), size);
            if (!input)
                throw std::runtime_error("Truncated serial payload");
            if (source == 254)
                reset();
            else if (source <= 1)
                acquisition->accept(r);
            else
                throw std::runtime_error("Invalid serial source");
        }
        checksums += acquisition->imu_stats().bad_checksums;
        const nlohmann::json summary{{"event", "summary"}, {"imu", samples}, {"gnss", fixes},
            {"stage_counts", stages}, {"duration_s", seconds(last - first)},
            {"max_imu_update_ms", max_update_ms}, {"imu_bad_checksums", checksums}};
        output << summary.dump() << '\n';
        std::cout << summary.dump() << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
