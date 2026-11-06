#pragma once
#include "pl64/control.hpp"
#include "pl64/flatbuffer.hpp"
#include "system_generated.h"
#include <csignal>
#include <iostream>
#include <unistd.h>

namespace pl64::service {
inline volatile std::sig_atomic_t interrupted = 0;
inline void interrupt(int) {
    interrupted = 1;
}
struct Options {
    std::filesystem::path root = "/run", directory = "/var/lib/pl64-recordings",
                          schema = PL64_SYSTEM_SCHEMA;
};
inline Options options(int argc, char **argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") {
            std::cout << "Options: --runtime-dir DIR --directory DIR --schema FILE\n";
            std::exit(0);
        }
        if (++i == argc)
            throw std::invalid_argument("Missing option value");
        if (key == "--runtime-dir")
            result.root = argv[i];
        else if (key == "--directory")
            result.directory = argv[i];
        else if (key == "--schema")
            result.schema = argv[i];
        else
            throw std::invalid_argument("Unknown option: " + key);
    }
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    return result;
}
inline auto endpoint(const Options &opt, std::string_view service,
                     std::string_view socket = "control.sock") {
    return opt.root / ("pl64-" + std::string(service)) / socket;
}
inline void event(Publisher &pub, system::fb::Kind kind, std::string_view source,
                  const Json &detail) {
    const auto text = detail.dump();
    MappedBuilder memory(text.size() + 2048);
    auto &b = memory.builder();
    const auto mono = monotonic_ns(), utc = utc_ns();
    const auto record = system::fb::CreateEvent(b, kind, b.CreateString(source), 0, mono, utc,
                                                b.CreateString(text));
    system::fb::FinishEventBuffer(b, record);
    memory.publish(pub, mono, utc);
}
} // namespace pl64::service
