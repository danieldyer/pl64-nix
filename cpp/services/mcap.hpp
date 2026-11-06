#pragma once
#include "pl64/ipc.hpp"
#include <string>

namespace pl64::service {
class Recording {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit Recording(const std::filesystem::path &);
    ~Recording();
    void write(std::string_view source, const Stream &, const Frame &);
    void boundary(std::string_view source, const nlohmann::json &);
    void close();
    std::uint64_t bytes() const;
    std::filesystem::path path() const;
};
} // namespace pl64::service
