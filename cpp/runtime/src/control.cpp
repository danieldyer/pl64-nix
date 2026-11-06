#include "pl64/control.hpp"
#include "unix.hpp"
#include <chrono>
#include <vector>

namespace pl64 {
using namespace detail;
struct ControlServer::Impl {
    Listener listener;
    Handler handler;
    struct Client {
        Fd fd;
        std::string input, output;
        std::size_t sent{};
        std::chrono::steady_clock::time_point opened = std::chrono::steady_clock::now();
    };
    std::vector<Client> clients;
    Impl(const std::filesystem::path &path, Handler work)
        : listener(path), handler(std::move(work)) {}
};
ControlServer::ControlServer(const std::filesystem::path &path, Handler handler)
    : impl_(std::make_unique<Impl>(path, std::move(handler))) {
}
ControlServer::~ControlServer() = default;
void ControlServer::poll(int timeout_ms) {
    auto &p = *impl_;
    if (timeout_ms > 0) {
        std::vector<pollfd> ready{{p.listener.fd.fd, POLLIN, 0}};
        for (const auto &c : p.clients)
            ready.push_back({c.fd.fd, short(c.output.empty() ? POLLIN : POLLOUT), 0});
        ::poll(ready.data(), ready.size(), timeout_ms);
    }
    while (p.clients.size() < 16) {
        Fd fd(::accept(p.listener.fd.fd, nullptr, nullptr));
        if (fd.fd < 0)
            break;
        setup(fd.fd);
        p.clients.push_back({std::move(fd), {}, {}});
    }
    std::erase_if(p.clients, [&](auto &c) {
        if (std::chrono::steady_clock::now() - c.opened > std::chrono::seconds(2))
            return true;
        if (c.output.empty()) {
            char data[4096];
            const auto n = ::recv(c.fd.fd, data, sizeof(data), 0);
            if (n == 0 || (n < 0 && !again()))
                return true;
            if (n > 0)
                c.input.append(data, n);
            if (c.input.size() > 65536)
                return true;
            const auto end = c.input.find('\n');
            if (end != std::string::npos) {
                Json reply;
                try {
                    reply = p.handler(Json::parse(c.input.substr(0, end)));
                    reply["ok"] = true;
                } catch (const std::exception &e) {
                    reply = {{"ok", false}, {"error", e.what()}};
                }
                c.output = reply.dump() + "\n";
                if (c.output.size() > 1024 * 1024)
                    return true;
            }
        }
        if (!c.output.empty()) {
            const auto n =
                ::send(c.fd.fd, c.output.data() + c.sent, c.output.size() - c.sent, send_flags());
            if (n < 0)
                return !again();
            c.sent += std::size_t(n);
            return c.sent == c.output.size();
        }
        return false;
    });
}
Json request(const std::filesystem::path &path, const Json &message, int timeout_ms) {
    auto fd = detail::connect(path, timeout_ms);
    const auto text = message.dump() + "\n";
    if (text.size() > 65536)
        throw std::invalid_argument("Control request too large");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::size_t sent = 0;
    std::string answer;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd ready{fd.fd, short(sent < text.size() ? POLLOUT : POLLIN), 0};
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now())
                        .count();
        if (::poll(&ready, 1, int(std::max<std::int64_t>(0, left))) <= 0)
            continue;
        if (sent < text.size()) {
            const auto n = ::send(fd.fd, text.data() + sent, text.size() - sent, send_flags());
            if (n > 0)
                sent += std::size_t(n);
            else if (n == 0 || !again())
                throw std::runtime_error("Control connection closed");
        } else {
            char data[8192];
            const auto n = ::recv(fd.fd, data, sizeof(data), 0);
            if (n > 0)
                answer.append(data, n);
            else if (n == 0 || !again())
                throw std::runtime_error("Control connection closed");
            if (answer.size() > 1024 * 1024)
                throw std::runtime_error("Control response too large");
            if (const auto end = answer.find('\n'); end != std::string::npos)
                return Json::parse(answer.substr(0, end));
        }
    }
    throw std::runtime_error("Control request timed out");
}
std::string_view phase_name(Phase phase) {
    switch (phase) {
    case Phase::idle:
        return "Idle";
    case Phase::starting:
        return "Starting";
    case Phase::running:
        return "Running";
    case Phase::draining:
        return "Draining";
    case Phase::fault:
        return "Fault";
    }
    return "Fault";
}
} // namespace pl64
