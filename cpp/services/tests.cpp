#include "mcap.hpp"
#include "pl64/flatbuffer.hpp"
#include "system_generated.h"
#include <iostream>
#include <mcap/reader.hpp>
#include <thread>
#include <unistd.h>
int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("pl64-recording-test-" + std::to_string(::getpid()));
    std::filesystem::create_directory(root);
    try {
        pl64::Publisher pub(root / "stream.sock", {"/test",
                                                   "pl64.system.fb.Event",
                                                   pl64::read_binary(PL64_TEST_SCHEMA),
                                                   {{"clock", "test"}}});
        std::jthread polling([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                pub.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        pl64::Subscription sub(root / "stream.sock");
        pl64::MappedBuilder memory(4096);
        auto &b = memory.builder();
        auto event =
            pl64::system::fb::CreateEvent(b, pl64::system::fb::Kind::Status, b.CreateString("test"),
                                          0, 123, 456, b.CreateString("{}"));
        pl64::system::fb::FinishEventBuffer(b, event);
        memory.publish(pub, 123, 456);
        auto frame = sub.receive(500);
        if (!frame)
            throw std::runtime_error("No test frame");
        pl64::service::Recording recording(root);
        recording.write("test", sub.stream(), *frame);
        recording.close();
        if (recording.path().filename().string().find("partial") != std::string::npos)
            throw std::runtime_error("Final recording not renamed");
        mcap::McapReader reader;
        auto status = reader.open(recording.path().string());
        if (!status.ok())
            throw std::runtime_error(status.message);
        unsigned messages = 0;
        for (const auto &view : reader.readMessages()) {
            if (!view.schema || view.schema->name != "pl64.system.fb.Event" ||
                view.message.logTime != 456 || view.message.dataSize != frame->bytes().size() ||
                std::memcmp(view.message.data, frame->bytes().data(), frame->bytes().size()) != 0)
                throw std::runtime_error(
                    "Recording did not preserve schema, payload or timestamps");
            ++messages;
        }
        if (messages != 1)
            throw std::runtime_error("Recording message count");
        polling.request_stop();
        polling.join();
        std::cout << "Mapped FlatBuffer -> MCAP -> replay preserved bytes, schema and timestamps\n";
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}
