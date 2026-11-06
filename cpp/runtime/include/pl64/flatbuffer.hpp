#pragma once
#include "ipc.hpp"
#include <flatbuffers/flatbuffers.h>
#include <fstream>

namespace pl64 {
inline std::vector<std::uint8_t> read_binary(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open schema: " + path.string());
    return {std::istreambuf_iterator<char>(file), {}};
}
// FlatBuffers builds directly in the producer's shared mapping. Reallocation
// is deliberately rejected instead of introducing an unexpected large copy.
class MappedBuilder {
    SharedBlock block_;
    class Allocator final : public flatbuffers::Allocator {
        std::span<std::byte> memory_;
        bool allocated_{};

      public:
        explicit Allocator(std::span<std::byte> memory) : memory_(memory) {}
        std::uint8_t *allocate(std::size_t size) override {
            if (allocated_ || size > memory_.size())
                throw std::length_error("Shared FlatBuffer capacity exceeded");
            allocated_ = true;
            return reinterpret_cast<std::uint8_t *>(memory_.data());
        }
        void deallocate(std::uint8_t *, std::size_t) override {}
        std::uint8_t *reallocate_downward(std::uint8_t *, std::size_t, std::size_t, std::size_t,
                                          std::size_t) override {
            throw std::length_error("Shared FlatBuffer capacity exceeded");
        }
    } allocator_;
    std::unique_ptr<flatbuffers::FlatBufferBuilder> builder_;

  public:
    explicit MappedBuilder(std::size_t capacity)
        : block_((capacity + 63) & ~std::size_t(63)), allocator_(block_.writable()),
          builder_(std::make_unique<flatbuffers::FlatBufferBuilder>(block_.writable().size(),
                                                                    &allocator_)) {}
    flatbuffers::FlatBufferBuilder &builder() { return *builder_; }
    void publish(Publisher &publisher, std::uint64_t mono, std::uint64_t utc) {
        const auto size = builder_->GetSize();
        const auto offset = std::size_t(builder_->GetBufferPointer() -
                                        reinterpret_cast<std::uint8_t *>(block_.writable().data()));
        builder_.reset();
        publisher.publish(std::move(block_), offset, size, mono, utc);
    }
};
} // namespace pl64
