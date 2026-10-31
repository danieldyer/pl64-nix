#include "ins/acquisition.hpp"
#include "ins/config.hpp"
#include "ins/estimator.hpp"
#include "ins/recording.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace ins {
int live(const nlohmann::json &, const std::filesystem::path &);
}
int main(int argc, char **argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: pl64-ins replay|live [--config file.json] [--schema "
                         "navigation.bfbs] [--output file.mcap] [--input file.mcap]\n";
            return 2;
        }
        std::map<std::string, std::string> args;
        for (int i = 2; i < argc; i += 2) {
            if (i + 1 >= argc)
                throw std::invalid_argument("Expected --option value");
            const std::string key = argv[i];
            if (key != "--config" && key != "--schema" && key != "--input" && key != "--output")
                throw std::invalid_argument("Unknown option: " + key);
            if (!args.emplace(key, argv[i + 1]).second)
                throw std::invalid_argument("Duplicate option: " + key);
        }
        nlohmann::json settings = nlohmann::json::object();
        if (args.contains("--config")) {
            std::ifstream in(args.at("--config"));
            if (!in)
                throw std::runtime_error("Cannot open configuration");
            in >> settings;
        }
        const auto schema = args.contains("--schema") ? std::filesystem::path(args.at("--schema"))
                                                      : std::filesystem::path(INS_SCHEMA_PATH);
        const std::string mode = argv[1];
        if (mode == "live")
            return ins::live(settings, schema);
        ins::Config config =
            ins::configuration(settings.value("navigation", nlohmann::json::object()));
        ins::Estimator estimator(config);
        ins::Acquisition acquisition(config);
        std::unique_ptr<ins::Recorder> recording;
        if (args.contains("--output"))
            recording = std::make_unique<ins::Recorder>(args.at("--output"), schema);
        std::uint64_t raw = 0, imu = 0, gnss = 0;
        ins::Stamp stamp;
        double max_update = 0;
        bool configuration_written = false;
        acquisition.on_imu = [&](const auto &s) {
            estimator.imu(s);
            ++imu;
            stamp = s.stamp;
        };
        acquisition.on_gnss = [&](const auto &f) {
            estimator.gnss(f);
            ++gnss;
        };
        acquisition.on_discontinuity = [&](auto reason) { estimator.reset(std::move(reason)); };
        acquisition.on_record = [&](const auto &r) {
            if (recording)
                recording->write(r);
        };
        const auto start = std::chrono::steady_clock::now();
        auto event = [&](const ins::Record &r) {
            if (recording && !configuration_written) {
                ins::Record meta;
                meta.kind = ins::Kind::metadata;
                meta.source = ins::Source::estimator;
                meta.stamp = r.stamp;
                meta.detail = nlohmann::json{
                    {"navigation", ins::configuration_json(config)},
                    {"schema_path", schema.string()},
                    {"mode",
                     mode}}.dump();
                recording->write(meta);
                configuration_written = true;
            }
            if (r.kind != ins::Kind::state && r.kind != ins::Kind::imu &&
                r.kind != ins::Kind::gnss && recording)
                recording->write(r);
            if (r.kind != ins::Kind::raw)
                return;
            ++raw;
            acquisition.accept(r);
            max_update = std::max(max_update, estimator.estimate().update_ms);
            if (r.source == ins::Source::imu && recording)
                recording->write(ins::state_record(estimator.estimate(), estimator.earth(), stamp));
        };
        if (mode == "replay") {
            if (!args.contains("--input"))
                throw std::invalid_argument("replay requires --input");
            ins::replay(args.at("--input"), event);
        } else
            throw std::invalid_argument("Unknown mode");
        if (recording)
            recording->close();
        const auto e = estimator.estimate();
        nlohmann::json report = {
            {"raw_records", raw},
            {"imu_samples", imu},
            {"gnss_epochs", gnss},
            {"stage", ins::stage_name(e.stage)},
            {"reason", e.reason},
            {"heading_observed", e.heading_observed},
            {"state", e.state.data},
            {"time_s", ins::seconds(e.state.time)},
            {"max_update_ms", max_update},
            {"wall_seconds",
             std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()},
            {"imu_bad_checksums", acquisition.imu_stats().bad_checksums}};
        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "pl64-ins: " << e.what() << '\n';
        return 1;
    }
}
