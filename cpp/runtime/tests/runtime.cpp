#include "pl64/control.hpp"
#include "pl64/ipc.hpp"
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <unistd.h>

void require(bool value, const char *text) {
    if (!value)
        throw std::runtime_error(text);
}
int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("pl64-runtime-test-" + std::to_string(::getpid()));
    std::filesystem::create_directory(root);
    try {
        pl64::Publisher publisher(
            root / "data.sock", {"/test", "test.Record", {1, 2, 3}, {{"clock", "test"}}}, 2, 4096);
        pl64::ControlServer control(root / "control.sock", [](const auto &request) {
            if (request.at("op") == "error")
                throw std::invalid_argument("intentional");
            return pl64::Json{{"echo", request.at("value")}};
        });
        std::jthread server([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                publisher.poll();
                control.poll(1);
            }
        });
        auto reply = pl64::request(root / "control.sock", {{"op", "echo"}, {"value", 42}});
        require(reply.at("ok") && reply.at("echo") == 42, "RPC reply");
        require(!pl64::request(root / "control.sock", {{"op", "error"}}).at("ok"), "RPC errors");
        pl64::Subscription first(root / "data.sock"), second(root / "data.sock");
        require(first.stream().schema == std::vector<std::uint8_t>({1, 2, 3}), "schema handshake");
        auto send = [&](const char *text) {
            const auto n = std::strlen(text) + 1;
            pl64::SharedBlock block(128);
            std::memcpy(block.writable().data() + 16, text, n);
            publisher.publish(std::move(block), 16, n, 123, 456);
        };
        send("first");
        auto a = first.receive(500), b = second.receive(500);
        require(a && b && a->sequence == b->sequence && a->utc == 456, "fanout metadata");
        require(std::strcmp(reinterpret_cast<const char *>(a->bytes().data()), "first") == 0,
                "mapped payload");
        b.reset();
        send("second");
        b = second.receive(500);
        b.reset();
        // First holds both credits; its third send must disconnect it without
        // modifying the memory held by the first outstanding lease.
        send("third");
        b = second.receive(500);
        require(b && publisher.stats().disconnected >= 1, "slow subscriber isolation");
        require(std::strcmp(reinterpret_cast<const char *>(a->bytes().data()), "first") == 0,
                "immutable lease after disconnect");
        a.reset();
        b.reset();
        server.request_stop();
        server.join();
        std::cout << "RPC, schema handshake, shared fanout, immutable leases and slow-reader "
                     "isolation passed\n";
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}
