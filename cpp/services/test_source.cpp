// Synthetic navigation publisher for end-to-end IPC/GUI tests, never installed.
#include "navigation_generated.h"
#include "pl64/control.hpp"
#include "pl64/flatbuffer.hpp"
#include <csignal>
#include <thread>
volatile std::sig_atomic_t stopped = 0;
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    std::signal(SIGTERM, [](int) { stopped = 1; });
    const auto path = std::filesystem::path(argv[1]) / "pl64-ins";
    std::filesystem::create_directories(path);
    pl64::Publisher output(path / "records.sock",
                           {"/navigation/events",
                            "pl64.ins.fb.Record",
                            pl64::read_binary(argv[2]),
                            {{"simulation", true}}},
                           64);
    pl64::ControlServer control(path / "control.sock", [](const auto &) {
        return pl64::Json{{"state", "Running"},
                          {"stage", "Simulator"},
                          {"simulation", true},
                          {"state_monotonic_ns", pl64::monotonic_ns()}};
    });
    std::uint64_t sequence = 0;
    while (!stopped) {
        control.poll();
        output.poll();
        pl64::MappedBuilder memory(4096);
        auto &b = memory.builder();
        const auto mono = pl64::monotonic_ns(), utc = pl64::utc_ns();
        const auto value = pl64::ins::fb::CreateRecord(
            b, pl64::ins::fb::Kind::State, pl64::ins::fb::Source::Simulator, sequence++, mono, utc,
            mono, 1e9, 0, 0, 0, 0, 0,
            b.CreateString("Synthetic integration test; no physical navigation solution"));
        pl64::ins::fb::FinishRecordBuffer(b, value);
        memory.publish(output, mono, utc);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
