// End-to-end publication benchmark: one producer writes each byte once; two
// independent subscribers map and checksum every byte. No DSP speed is implied.
#include "pl64/ipc.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <unistd.h>
int main() {
    const auto root =
        std::filesystem::temp_directory_path() / ("pl64-bandwidth-" + std::to_string(::getpid()));
    std::filesystem::create_directory(root);
    try {
        constexpr std::size_t size = 16 * 1024 * 1024, records = 64;
        pl64::Publisher publisher(root / "data.sock", {"/benchmark", "bytes", {}, {}});
        std::jthread polling([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                publisher.poll();
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        std::array<std::atomic<unsigned>, 2> received{};
        std::atomic<bool> failed = false;
        std::array<std::jthread, 2> readers;
        for (unsigned index = 0; index < readers.size(); ++index)
            readers[index] = std::jthread([&, index] {
                try {
                    pl64::Subscription input(root / "data.sock");
                    for (unsigned n = 0; n < records; ++n) {
                        auto frame = input.receive(10000);
                        if (!frame || frame->bytes().size() != size)
                            throw std::runtime_error("Incomplete benchmark stream");
                        std::uint64_t checksum = 0;
                        for (auto byte : frame->bytes())
                            checksum += byte;
                        if (checksum != std::uint64_t(n + 1) * size)
                            throw std::runtime_error("Benchmark data corruption");
                        frame.reset();
                        ++received[index];
                    }
                } catch (const std::exception &e) {
                    std::cerr << e.what() << '\n';
                    failed = true;
                }
            });
        while (publisher.stats().subscribers != 2 && !failed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto start = pl64::monotonic_ns();
        for (unsigned n = 0; n < records && !failed; ++n) {
            while (std::min(received[0].load(), received[1].load()) + 3 < n && !failed)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            pl64::SharedBlock block(size);
            std::memset(block.writable().data(), int(n + 1), size);
            publisher.publish(std::move(block), 0, size, pl64::monotonic_ns(), pl64::utc_ns());
        }
        for (auto &reader : readers)
            reader.join();
        const double seconds = double(pl64::monotonic_ns() - start) * 1e-9;
        polling.request_stop();
        polling.join();
        if (failed || received[0] != records || received[1] != records)
            throw std::runtime_error("Benchmark lost a subscriber");
        std::cout << nlohmann::json{{"bytes_per_record", size},
                                    {"records", records},
                                    {"subscribers", 2},
                                    {"seconds", seconds},
                                    {"producer_MiB_per_second", size * records / seconds / 1048576},
                                    {"aggregate_consumer_MiB_per_second",
                                     2 * size * records / seconds / 1048576}}
                         .dump(2)
                  << '\n';
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}
