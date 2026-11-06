#pragma once
#include <cstddef>
#include <cstdint>
#ifdef PL64_MCAP_USE_ZLIB_NG
#include <zlib-ng.h>
#else
#include <zlib.h>
#endif

namespace mcap::internal {
// Keep MCAP's reflected, unfinalized IEEE CRC-32 state and wire format.
// Delegate the byte scan to a maintained runtime-dispatched implementation.
static constexpr std::uint32_t CRC32_INIT = 0xffffffffU;
inline std::uint32_t crc32Update(std::uint32_t previous, const std::byte *data, std::size_t size) {
    if (!size)
        return previous; // zlib's nullptr convention otherwise resets state.
#ifdef PL64_MCAP_USE_ZLIB_NG
    return zng_crc32_z(previous ^ 0xffffffffU, reinterpret_cast<const std::uint8_t *>(data), size) ^
           0xffffffffU;
#else
    return std::uint32_t(::crc32_z(previous ^ 0xffffffffU, reinterpret_cast<const Bytef *>(data), size)) ^
           0xffffffffU;
#endif
}
inline std::uint32_t crc32Final(std::uint32_t value) {
    return value ^ 0xffffffffU;
}
} // namespace mcap::internal
