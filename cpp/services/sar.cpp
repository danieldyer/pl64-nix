#include "navigation_generated.h"
#include "options.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

int main(int argc, char **argv) try {
    using namespace pl64;
    using namespace pl64::service;
    const auto opt = options(argc, argv);
    std::filesystem::create_directories(endpoint(opt, "sar").parent_path());
    Publisher output(
        endpoint(opt, "sar", "records.sock"),
        {"/sar/events",
         "pl64.system.fb.Event",
         read_binary(opt.schema),
         {{"implementation",
           "subscription foundation; image formation and autofocus are not implemented"}}});
    std::atomic<bool> running = false;
    std::mutex mutex;
    Json status = {{"state", "Idle"},
                   {"processor_available", false},
                   {"message", "Image formation and autofocus are not installed"}};
    ControlServer control(endpoint(opt, "sar"), [&](const Json &request) {
        const auto op = request.value("op", std::string("status"));
        if (op == "start")
            running = true;
        else if (op == "stop")
            running = false;
        else if (op != "status")
            throw std::invalid_argument("SAR foundation supports status, start and stop");
        std::lock_guard lock(mutex);
        auto reply = status;
        reply["requested_running"] = running.load();
        return reply;
    });
    std::jthread worker([&](std::stop_token stop) {
        struct Input {
            std::unique_ptr<Subscription> stream;
            std::uint64_t count{}, bytes{}, sequence{}, identity{}, disconnects{}, retry{};
            std::string error;
        };
        Input navigation, radar;
        struct NavigationSample {
            std::int64_t measurement_ns{}, arrival_ns{}, utc_ns{};
            double timing_sigma_ns{};
            std::uint32_t flags{};
            std::vector<double> values, covariance;
        };
        // Navigation is small: keep decoded states for interpolation without
        // retaining transport credits. Radar's large payload stays mapped.
        std::deque<NavigationSample> navigation_states;
        std::uint64_t last_event = 0;
        bool was_running = false;
        while (!stop.stop_requested()) {
            const auto active = running.load();
            if (!active) {
                navigation.stream.reset();
                radar.stream.reset();
                navigation_states.clear();
            }
            if (active && !was_running)
                event(output, system::fb::Kind::Status, "sar",
                      {{"state", "Starting"}, {"processor_available", false}});
            if (!active && was_running)
                event(output, system::fb::Kind::Status, "sar", {{"state", "Idle"}});
            was_running = active;
            if (active)
                for (auto [name, input] :
                     {std::pair{"ins", &navigation}, std::pair{"radar", &radar}}) {
                    if (!input->stream && monotonic_ns() >= input->retry) {
                        try {
                            input->stream =
                                std::make_unique<Subscription>(endpoint(opt, name, "records.sock"));
                            input->error.clear();
                            input->sequence = 0;
                        } catch (const std::exception &e) {
                            input->error = e.what();
                            input->retry = monotonic_ns() + 1000000000;
                        }
                    }
                    if (!input->stream)
                        continue;
                    try {
                        for (unsigned budget = 0; budget < 64; ++budget) {
                            auto frame = input->stream->receive();
                            if (!frame)
                                break;
                            if (input->sequence && frame->sequence != input->sequence + 1) {
                                navigation_states.clear();
                                event(output, system::fb::Kind::Gap, "sar",
                                      {{"source", name},
                                       {"previous", input->sequence},
                                       {"next", frame->sequence}});
                            }
                            input->sequence = frame->sequence;
                            input->identity = frame->stream_id;
                            ++input->count;
                            input->bytes += frame->bytes().size();
                            if (std::string_view(name) == "ins") {
                                flatbuffers::Verifier verifier(frame->bytes().data(),
                                                               frame->bytes().size());
                                if (!pl64::ins::fb::VerifyRecordBuffer(verifier))
                                    throw std::runtime_error("Invalid navigation FlatBuffer");
                                const auto *record =
                                    pl64::ins::fb::GetRecord(frame->bytes().data());
                                if (record->kind() == pl64::ins::fb::Kind::State) {
                                    if ((record->values() && record->values()->size() > 28) ||
                                        (record->covariance() &&
                                         record->covariance()->size() > 225))
                                        throw std::runtime_error("Invalid navigation dimensions");
                                    NavigationSample sample{record->measurement_ns(),
                                                            record->arrival_ns(),
                                                            record->utc_ns(),
                                                            record->timing_sigma_ns(),
                                                            record->flags(),
                                                            {},
                                                            {}};
                                    if (record->values())
                                        sample.values.assign(record->values()->begin(),
                                                             record->values()->end());
                                    if (record->covariance())
                                        sample.covariance.assign(record->covariance()->begin(),
                                                                 record->covariance()->end());
                                    navigation_states.push_back(std::move(sample));
                                    while (navigation_states.size() > 6000)
                                        navigation_states.pop_front();
                                }
                            }
                            // A future processor receives this immutable mapped lease
                            // here, with navigation interpolation and explicit clock
                            // uncertainty. No CPU profile copy or fake image is made.
                        }
                    } catch (const std::exception &e) {
                        input->error = e.what();
                        ++input->disconnects;
                        input->stream.reset();
                        input->retry = monotonic_ns() + 1000000000;
                        navigation_states.clear();
                        event(output, system::fb::Kind::Gap, "sar",
                              {{"source", name}, {"error", e.what()}});
                    }
                }
            if (monotonic_ns() - last_event > 1000000000) {
                Json snapshot = {{"state", active ? "Monitoring" : "Idle"},
                                 {"processor_available", false},
                                 {"message", "Subscription foundation only; image formation and "
                                             "autofocus are not installed"},
                                 {"navigation_states_buffered", navigation_states.size()},
                                 {"inputs", Json::object()}};
                for (auto [name, input] :
                     {std::pair{"ins", &navigation}, std::pair{"radar", &radar}})
                    snapshot["inputs"][name] = {{"connected", bool(input->stream)},
                                                {"records", input->count},
                                                {"bytes", input->bytes},
                                                {"disconnects", input->disconnects},
                                                {"last_sequence", input->sequence},
                                                {"stream_id", input->identity},
                                                {"error", input->error}};
                {
                    std::lock_guard lock(mutex);
                    status = snapshot;
                }
                event(output, system::fb::Kind::Status, "sar", snapshot);
                last_event = monotonic_ns();
            }
            output.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
