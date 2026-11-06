#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace pl64 {
using Json = nlohmann::json;
class ControlServer {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    using Handler = std::function<Json(const Json &)>;
    ControlServer(const std::filesystem::path &, Handler);
    ~ControlServer();
    void poll(int timeout_ms = 0);
};
Json request(const std::filesystem::path &, const Json &, int timeout_ms = 500);
// Configuration changes are explicit and only accepted in Idle; every service
// reports an error string and state instead of silently restarting a worker.
enum class Phase { idle, starting, running, draining, fault };
std::string_view phase_name(Phase);
} // namespace pl64
