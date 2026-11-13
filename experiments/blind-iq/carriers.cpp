#include "modem.hpp"
#include <fstream>
#include <random>
int main(int argc, char **argv) {
    if (argc != 4 && argc != 5)
        return 2;
    unsigned symbols = std::stoul(argv[2]), seed = std::stoul(argv[3]);
    radar::Modem m;
    radar::Tps t;
    t.mode = 1;
    t.constellation = argc == 5 ? std::stoi(argv[4]) : 2;
    t.rate = 2;
    t.guard = 1;
    auto packets = (std::size_t(symbols) * 6048 * 6 * 3 / 4 + 8 * 204 - 1) / (8 * 204) + 12;
    std::vector<uint8_t> bytes(packets * 188);
    std::mt19937 rng(seed);
    for (auto &b : bytes)
        b = rng();
    for (size_t i = 0; i < packets; i++)
        bytes[i * 188] = i % 8 ? 0x47 : 0xb8;
    auto c = m.synthesize(bytes, t, symbols);
    std::ofstream out(argv[1], std::ios::binary);
    out.write(reinterpret_cast<const char *>(c.data()), c.size() * sizeof(c[0]));
    return !out;
}
