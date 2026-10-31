#pragma once
#include "model.hpp"
#include <filesystem>
#include <functional>
#include <memory>
namespace ins {
class Recorder {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Recorder(const std::filesystem::path &file, const std::filesystem::path &schema);
    ~Recorder();
    void write(const Record &);
    void close();
};
void replay(const std::filesystem::path &, const std::function<void(const Record &)> &);
std::vector<std::uint8_t> serialize(const Record &);
Record state_record(const Estimate &, const Earth *, Stamp);
} // namespace ins
