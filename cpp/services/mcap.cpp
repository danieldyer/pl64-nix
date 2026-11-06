#include "mcap.hpp"
#define MCAP_IMPLEMENTATION
#include <chrono>
#include <fcntl.h>
#include <map>
#include <mcap/reader.hpp>
#include <mcap/writer.hpp>
#include <unistd.h>
#include <utility>

namespace pl64::service {
namespace {
class File final : public mcap::IWritable {
    int fd_ = -1;
    std::uint64_t size_{};
    std::chrono::steady_clock::time_point synced_ = std::chrono::steady_clock::now();
    void handleWrite(const std::byte *bytes, std::uint64_t size) override {
        while (size) {
            const auto n = ::write(fd_, bytes, size);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::system_error(errno, std::generic_category(), "Recording write");
            bytes += n;
            size -= n;
            size_ += n;
        }
    }

  public:
    explicit File(const std::filesystem::path &path) {
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
        if (fd_ < 0)
            throw std::system_error(errno, std::generic_category(), "Create recording");
    }
    ~File() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    std::uint64_t size() const override { return size_; }
    void flush() override {
        if (std::chrono::steady_clock::now() - synced_ >= std::chrono::seconds(1)) {
            if (::fsync(fd_))
                throw std::system_error(errno, std::generic_category(), "Sync recording");
            synced_ = std::chrono::steady_clock::now();
        }
    }
    void end() override {
        if (fd_ < 0)
            return;
        if (::fsync(fd_))
            throw std::system_error(errno, std::generic_category(), "Finalize recording");
        const int fd = std::exchange(fd_, -1);
        if (::close(fd))
            throw std::system_error(errno, std::generic_category(), "Close recording");
    }
};
} // namespace
struct Recording::Impl {
    std::filesystem::path path;
    File file;
    mcap::McapWriter writer;
    struct Channel {
        mcap::Channel channel;
        std::uint64_t stream{};
        std::string name;
        std::vector<std::uint8_t> schema;
    };
    std::map<std::string, Channel> channels;
    bool closed{};
    explicit Impl(const std::filesystem::path &directory)
        : path(directory / ("session-" + std::to_string(utc_ns()) + "-" +
                            std::to_string(::getpid()) + ".partial.mcap")),
          file(path) {
        mcap::McapWriterOptions options("");
        options.compression = mcap::Compression::None;
        options.chunkSize = 4 * 1024 * 1024;
        options.enableDataCRC = true;
        try {
            writer.open(file, options);
        } catch (...) {
            writer.terminate();
            throw;
        }
    }
};
Recording::Recording(const std::filesystem::path &directory)
    : impl_(std::make_unique<Impl>(directory)) {
}
Recording::~Recording() {
    if (impl_ && !impl_->closed)
        impl_->writer.terminate();
}
void Recording::boundary(std::string_view source, const nlohmann::json &detail) {
    mcap::Metadata metadata;
    metadata.name = "pl64.boundary";
    metadata.metadata = {{"source", std::string(source)},
                         {"utc_ns", std::to_string(utc_ns())},
                         {"detail", detail.dump()}};
    const auto status = impl_->writer.write(metadata);
    if (!status.ok())
        throw std::runtime_error(status.message);
}
void Recording::write(std::string_view source, const Stream &stream, const Frame &frame) {
    if (impl_->closed)
        throw std::runtime_error("Recording closed");
    auto &channels = impl_->channels;
    auto found = channels.find(std::string(source));
    if (found == channels.end() || found->second.stream != frame.stream_id) {
        mcap::Schema definition(stream.schema_name, "flatbuffer",
                                std::string(reinterpret_cast<const char *>(stream.schema.data()),
                                            stream.schema.size()));
        impl_->writer.addSchema(definition);
        mcap::Channel channel(stream.topic, "flatbuffer", definition.id);
        channel.metadata = {{"source", std::string(source)},
                            {"stream_id", std::to_string(frame.stream_id)}};
        impl_->writer.addChannel(channel);
        channels[std::string(source)] = {channel, frame.stream_id, stream.schema_name,
                                         stream.schema};
        boundary(source, {{"kind", "attach"},
                          {"stream_id", frame.stream_id},
                          {"first_sequence", frame.sequence},
                          {"metadata", stream.metadata}});
        found = channels.find(std::string(source));
    }
    mcap::Message message;
    message.channelId = found->second.channel.id;
    message.sequence = std::uint32_t(frame.sequence);
    message.logTime = frame.utc;
    message.publishTime = frame.utc;
    message.data = reinterpret_cast<const std::byte *>(frame.bytes().data());
    message.dataSize = frame.bytes().size();
    const auto status = impl_->writer.write(message);
    if (!status.ok())
        throw std::runtime_error(status.message);
}
void Recording::close() {
    if (impl_->closed)
        return;
    try {
        impl_->writer.close();
        impl_->closed = true;
        const auto final =
            impl_->path.parent_path() / (impl_->path.filename().string().substr(
                                             0, impl_->path.filename().string().size() - 13) +
                                         ".mcap");
        std::filesystem::rename(impl_->path, final);
        impl_->path = final;
    } catch (...) {
        impl_->writer.terminate();
        impl_->closed = true;
        throw;
    }
}
std::uint64_t Recording::bytes() const {
    return impl_->file.size();
}
std::filesystem::path Recording::path() const {
    return impl_->path;
}
} // namespace pl64::service
