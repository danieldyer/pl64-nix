#include "pl64/control.hpp"
#include <iostream>
int main(int argc, char **argv) try {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: pl64-ctl SERVICE status|start|stop|reset|JSON [RUNTIME_DIR]\n";
        return 2;
    }
    const std::string service = argv[1], command = argv[2];
    if (service != "ins" && service != "radar" && service != "sar" && service != "recorder")
        throw std::invalid_argument("Unknown service");
    const auto root = std::filesystem::path(argc == 4 ? argv[3] : "/run");
    const auto message =
        command.starts_with("{") ? pl64::Json::parse(command) : pl64::Json{{"op", command}};
    const auto reply = pl64::request(root / ("pl64-" + service) / "control.sock", message, 1000);
    std::cout << reply.dump(2) << '\n';
    return reply.value("ok", false) ? 0 : 1;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
