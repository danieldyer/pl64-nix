#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>

namespace pl64 {
std::uint64_t monotonic_ns();
std::uint64_t utc_ns();
// A producer writes directly into this mapping, then commits an immutable
// slice. On Linux commit seals the memfd against all subsequent writes.
class SharedBlock {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class Publisher;

  public:
    explicit SharedBlock(std::size_t capacity);
    ~SharedBlock();
    SharedBlock(SharedBlock &&) noexcept;
    SharedBlock &operator=(SharedBlock &&) noexcept;
    std::span<std::byte> writable();
};
struct Stream {
    std::string topic, schema_name;
    std::vector<std::uint8_t> schema;
    nlohmann::json metadata = nlohmann::json::object();
};
struct PublisherStats {
    std::uint64_t published{}, bytes{}, disconnected{};
    std::size_t subscribers{}, outstanding_bytes{};
};
class Publisher {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Publisher(const std::filesystem::path &, Stream, std::size_t credits = 8,
              std::size_t byte_limit = 128 * 1024 * 1024);
    ~Publisher();
    void poll();
    void publish(SharedBlock, std::size_t offset, std::size_t size, std::uint64_t mono,
                 std::uint64_t utc);
    // Convenience for small records. High-rate producers should build directly
    // into SharedBlock and call the overload above.
    void publish(std::span<const std::uint8_t>, std::uint64_t mono, std::uint64_t utc);
    PublisherStats stats() const;
};
class Subscription;
class Frame {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class Subscription;
    explicit Frame(std::unique_ptr<Impl>);

  public:
    ~Frame();
    Frame(Frame &&) noexcept;
    Frame &operator=(Frame &&) noexcept;
    std::uint64_t sequence{}, monotonic{}, utc{}, stream_id{};
    std::span<const std::uint8_t> bytes() const;
};
// A lease owns its mapping and acknowledges only on destruction. Records are
// immutable even after a producer restarts or a slow subscriber is disconnected.
class Subscription {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit Subscription(const std::filesystem::path &, int timeout_ms = 500);
    ~Subscription();
    const Stream &stream() const;
    std::unique_ptr<Frame> receive(int timeout_ms = 0);
};
} // namespace pl64
