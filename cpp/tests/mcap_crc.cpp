#include <algorithm>
#include <iostream>
#include <mcap/crc32.hpp>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>
std::uint32_t reference(std::uint32_t state, const std::byte *bytes, std::size_t count) {
    while (count--) {
        state ^= std::uint8_t(*bytes++);
        for (unsigned bit = 0; bit < 8; ++bit)
            state = (state >> 1) ^ ((state & 1U) ? 0xedb88320U : 0);
    }
    return state;
}
int main() {
    using namespace mcap::internal;
    const std::string_view known = "123456789";
    if (crc32Final(crc32Update(CRC32_INIT, reinterpret_cast<const std::byte *>(known.data()),
                               known.size())) != 0xcbf43926U)
        throw std::runtime_error("IEEE CRC-32 check vector differs");
    if (crc32Update(0x12345678U, nullptr, 0) != 0x12345678U)
        throw std::runtime_error("Empty update changed the running CRC");
    std::mt19937 random(382176);
    std::vector<std::byte> bytes(8192 + 64);
    for (auto &value : bytes)
        value = std::byte(random());
    for (unsigned offset = 0; offset < 64; ++offset)
        for (unsigned size : {0U, 1U, 7U, 8U, 31U, 255U, 1024U, 8192U}) {
            const auto expected = reference(CRC32_INIT, bytes.data() + offset, size);
            if (crc32Update(CRC32_INIT, bytes.data() + offset, size) != expected)
                throw std::runtime_error("CRC offset/length differs");
            auto state = CRC32_INIT;
            for (unsigned at = 0; at < size;) {
                const unsigned count = std::min(size - at, 17U);
                state = crc32Update(state, bytes.data() + offset + at, count);
                at += count;
            }
            if (state != expected)
                throw std::runtime_error("Incremental CRC differs");
        }
    std::cout << "IEEE CRC-32 vector, offsets, empty/null, partial and streamed records passed\n";
}
