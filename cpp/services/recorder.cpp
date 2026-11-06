#include "mcap.hpp"
#include "options.hpp"
#include <atomic>
#include <mutex>
#include <thread>

int main(int argc, char **argv) try {
    using namespace pl64;
    using namespace pl64::service;
    const auto opt = options(argc, argv);
    std::filesystem::create_directories(endpoint(opt, "recorder").parent_path());
    std::filesystem::create_directories(opt.directory);
    std::mutex mutex;
    Json status = {
        {"state", "Idle"}, {"enabled", true}, {"sources", Json::object()}, {"error", ""}};
    std::atomic<bool> enabled = true;
    std::atomic<unsigned> generation = 0;
    std::map<std::string, bool> selected{{"ins", true}, {"radar", true}, {"sar", true}};
    ControlServer control(endpoint(opt, "recorder"), [&](const Json &request) {
        const auto op = request.value("op", std::string("status"));
        std::lock_guard lock(mutex);
        if (op == "recording") {
            enabled = request.at("enabled").get<bool>();
            ++generation;
        } else if (op == "select") {
            const auto name = request.at("source").get<std::string>();
            if (!selected.contains(name))
                throw std::invalid_argument("Unknown recording source");
            selected[name] = request.at("enabled").get<bool>();
            ++generation;
        } else if (op != "status")
            throw std::invalid_argument("Recorder supports status, recording and select");
        auto reply = status;
        reply["enabled"] = enabled.load();
        reply["selected"] = selected;
        return reply;
    });
    std::jthread worker([&](std::stop_token stop) {
        struct Source {
            std::unique_ptr<Subscription> input;
            std::uint64_t count{}, bytes{}, disconnects{}, sequence{}, identity{};
            std::string error;
            std::uint64_t retry{};
        };
        std::map<std::string, Source> sources;
        for (const auto &[name, _] : selected)
            sources.try_emplace(name);
        std::unique_ptr<Recording> writer;
        unsigned seen = generation.load();
        std::uint64_t opened = 0;
        std::string fault, last_path;
        bool faulted = false;
        while (true) {
            const bool shutting_down = stop.stop_requested();
            std::map<std::string, bool> selection;
            bool write_enabled;
            unsigned requested_generation;
            {
                std::lock_guard lock(mutex);
                selection = selected;
                write_enabled = enabled.load();
                requested_generation = generation.load();
            }
            bool drain_recording = shutting_down && bool(writer);
            if (shutting_down)
                write_enabled = false;
            try {
                if (seen != requested_generation) {
                    seen = requested_generation;
                    drain_recording = writer && !write_enabled;
                    if (writer && !drain_recording) {
                        writer->boundary("recorder", {{"kind", "configuration_boundary"}});
                        writer->close();
                        last_path = writer->path();
                        writer.reset();
                    }
                    fault.clear();
                    faulted = false;
                }
                if (writer && (writer->bytes() > 1024ULL * 1024 * 1024 ||
                               monotonic_ns() - opened > 900ULL * 1000000000)) {
                    writer->close();
                    last_path = writer->path();
                    writer.reset();
                }
                for (auto &[name, source] : sources) {
                    if (!shutting_down && !source.input && monotonic_ns() >= source.retry) {
                        try {
                            source.input =
                                std::make_unique<Subscription>(endpoint(opt, name, "records.sock"));
                            source.error.clear();
                        } catch (const std::exception &e) {
                            source.error = e.what();
                            source.retry = monotonic_ns() + 1000000000;
                        }
                    }
                    if (!source.input)
                        continue;
                    for (unsigned budget = 0; budget < 64; ++budget) {
                        std::unique_ptr<Frame> frame;
                        try {
                            frame = source.input->receive();
                        } catch (const std::exception &e) {
                            if (writer)
                                writer->boundary(name, {{"kind", "disconnect"},
                                                        {"last_sequence", source.sequence},
                                                        {"stream_id", source.identity},
                                                        {"error", e.what()}});
                            source.error = e.what();
                            ++source.disconnects;
                            source.input.reset();
                            source.retry = monotonic_ns() + 1000000000;
                            break;
                        }
                        if (!frame)
                            break;
                        ++source.count;
                        source.bytes += frame->bytes().size();
                        if (source.sequence && source.identity == frame->stream_id &&
                            frame->sequence != source.sequence + 1 && writer)
                            writer->boundary(name, {{"kind", "sequence_gap"},
                                                    {"previous", source.sequence},
                                                    {"next", frame->sequence}});
                        source.sequence = frame->sequence;
                        source.identity = frame->stream_id;
                        if ((write_enabled || drain_recording) && selection.at(name) && !faulted) {
                            if (!writer) {
                                if (std::filesystem::space(opt.directory).available <
                                    512ULL * 1024 * 1024)
                                    throw std::runtime_error(
                                        "Less than 512 MiB free; recording stopped");
                                writer = std::make_unique<Recording>(opt.directory);
                                opened = monotonic_ns();
                            }
                            writer->write(name, source.input->stream(), *frame);
                            if (writer->bytes() % (16 * 1024 * 1024) < frame->bytes().size() &&
                                std::filesystem::space(opt.directory).available <
                                    512ULL * 1024 * 1024)
                                throw std::runtime_error(
                                    "Less than 512 MiB free; recording stopped");
                        }
                    }
                }
                if (!write_enabled && writer) {
                    writer->boundary("recorder", {{"kind", "recording_disabled_after_drain"}});
                    writer->close();
                    last_path = writer->path();
                    writer.reset();
                }
            } catch (const std::exception &e) {
                fault = e.what();
                faulted = true;
                if (writer) {
                    last_path = writer->path();
                    writer.reset();
                }
            }
            {
                std::lock_guard lock(mutex);
                status = {{"state", faulted  ? "Fault"
                                    : writer ? "Recording"
                                             : "Idle"},
                          {"error", fault},
                          {"file", writer ? writer->path().string() : last_path},
                          {"bytes", writer ? writer->bytes() : 0},
                          {"directory", opt.directory.string()},
                          {"sources", Json::object()}};
                for (const auto &[name, s] : sources)
                    status["sources"][name] = {{"connected", bool(s.input)},
                                               {"records", s.count},
                                               {"bytes", s.bytes},
                                               {"disconnects", s.disconnects},
                                               {"last_sequence", s.sequence},
                                               {"stream_id", s.identity},
                                               {"error", s.error}};
            }
            if (shutting_down)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (writer)
            try {
                writer->close();
            } catch (const std::exception &e) {
                std::cerr << e.what() << '\n';
            }
    });
    while (!interrupted)
        control.poll(20);
    worker.request_stop();
    worker.join();
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
