#include "ins/recording.hpp"
#include "navigation_generated.h"
#define MCAP_IMPLEMENTATION
#include <cstring>
#include <fcntl.h>
#include <flatbuffers/verifier.h>
#include <fstream>
#include <mcap/reader.hpp>
#include <mcap/writer.hpp>
#include <stdexcept>
#include <unistd.h>

namespace ins {
namespace fb = pl64::ins::fb;
namespace {
// MCAP's stdio writer only asserts on short writes. This sink reports storage
// errors in Release builds too, creates files exclusively, and bounds the
// unsynced interval without putting disk I/O on the acquisition thread.
class File final : public mcap::IWritable {
    int fd_ = -1;
    std::uint64_t size_ = 0;
    std::chrono::steady_clock::time_point synced_ = std::chrono::steady_clock::now();
    void handleWrite(const std::byte *data, std::uint64_t size) override {
        while (size) {
            const auto n = ::write(fd_, data, size);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::system_error(errno, std::generic_category(), "MCAP write");
            data += n;
            size -= n;
            size_ += n;
        }
    }

  public:
    explicit File(const std::filesystem::path &path)
        : fd_(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640)) {
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
            throw std::system_error(errno, std::generic_category(), "Finish recording");
        const int fd = std::exchange(fd_, -1);
        if (::close(fd))
            throw std::system_error(errno, std::generic_category(), "Close recording");
    }
};
} // namespace
std::vector<std::uint8_t> serialize(const Record &r) {
    flatbuffers::FlatBufferBuilder b;
    const auto record = fb::CreateRecordDirect(
        b, static_cast<fb::Kind>(r.kind), static_cast<fb::Source>(r.source), r.stamp.sequence,
        r.stamp.arrival.count(), r.stamp.utc.count(), r.stamp.measurement.count(),
        r.stamp.uncertainty_s * 1e9, r.native_counter, &r.payload, &r.values, &r.covariance,
        r.flags, r.detail.c_str(), r.gnss_age_s, r.update_ms, r.normalized_residual);
    fb::FinishRecordBuffer(b, record);
    return {b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()};
}
struct Recorder::Impl {
    File file;
    mcap::McapWriter writer;
    mcap::Channel channel;
    bool closed = false;
    Impl(const std::filesystem::path &path, const std::filesystem::path &schema) : file(path) {
        std::ifstream in(schema, std::ios::binary);
        if (!in)
            throw std::runtime_error("Cannot open FlatBuffers schema");
        const std::string bfbs{std::istreambuf_iterator<char>(in), {}};
        if (bfbs.size() < 8)
            throw std::runtime_error("Invalid binary schema");
        mcap::McapWriterOptions options("");
        options.compression = mcap::Compression::None;
        options.chunkSize = 64 * 1024;
        options.enableDataCRC = true;
        try {
            writer.open(file, options);
        } catch (...) {
            writer.terminate();
            throw;
        }
        mcap::Schema definition("pl64.ins.fb.Record", "flatbuffer", bfbs);
        writer.addSchema(definition);
        channel = mcap::Channel("/navigation/events", "flatbuffer", definition.id);
        writer.addChannel(channel);
    }
};
Recorder::Recorder(const std::filesystem::path &path, const std::filesystem::path &schema)
    : impl_(std::make_unique<Impl>(path, schema)) {}
Recorder::~Recorder() {
    try {
        close();
    } catch (...) {
        impl_->writer.terminate();
    }
}
void Recorder::write(const Record &record) {
    if (impl_->closed)
        throw std::runtime_error("Recording closed");
    const auto data = serialize(record);
    mcap::Message message;
    message.channelId = impl_->channel.id;
    message.sequence = static_cast<std::uint32_t>(record.stamp.sequence);
    message.logTime =
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, record.stamp.utc.count()));
    message.publishTime = message.logTime;
    message.data = reinterpret_cast<const std::byte *>(data.data());
    message.dataSize = data.size();
    try {
        if (const auto status = impl_->writer.write(message); !status.ok())
            throw std::runtime_error(status.message);
    } catch (...) {
        impl_->writer.terminate();
        impl_->closed = true;
        throw;
    }
}
void Recorder::close() {
    if (!impl_->closed) {
        try {
            impl_->writer.close();
            impl_->closed = true;
        } catch (...) {
            impl_->writer.terminate();
            impl_->closed = true;
            throw;
        }
    }
}
void replay(const std::filesystem::path &file,
            const std::function<void(const Record &)> &callback) {
    std::ifstream tail(file, std::ios::binary);
    if (!tail)
        throw std::runtime_error("Cannot open recording");
    tail.seekg(-8, std::ios::end);
    std::array<unsigned char, 8> magic{};
    tail.read(reinterpret_cast<char *>(magic.data()), magic.size());
    if (!tail || magic != std::array<unsigned char, 8>{0x89, 'M', 'C', 'A', 'P', '0', '\r', '\n'})
        throw std::runtime_error("Recording was not finalized; recover the MCAP before replay");
    mcap::McapReader reader;
    if (const auto status = reader.open(file.string()); !status.ok())
        throw std::runtime_error(status.message);
    mcap::TypedRecordReader validation(*reader.dataSource(), 8, reader.dataSource()->size() - 8);
    validation.onChunk = [](const mcap::Chunk &chunk, mcap::ByteOffset) {
        if (!chunk.compression.empty() || chunk.compressedSize != chunk.uncompressedSize)
            throw std::runtime_error("Replay requires uncompressed navigation MCAP chunks");
        if (chunk.uncompressedCrc && mcap::internal::crc32Final(mcap::internal::crc32Update(
                                         mcap::internal::CRC32_INIT, chunk.records,
                                         chunk.uncompressedSize)) != chunk.uncompressedCrc)
            throw std::runtime_error("MCAP chunk CRC mismatch");
    };
    while (validation.next()) {
    }
    if (!validation.status().ok())
        throw std::runtime_error(validation.status().message);
    // MCAP messages can start at any byte offset. FlatBuffers accessors require
    // alignment for the schema's 64-bit scalars, including vectors of doubles.
    std::vector<std::uint64_t> aligned;
    for (const auto &view :
         reader.readMessages([](const mcap::Status &s) { throw std::runtime_error(s.message); })) {
        if (view.channel->messageEncoding != "flatbuffer" || !view.schema ||
            view.schema->name != "pl64.ins.fb.Record")
            throw std::runtime_error("Unexpected recording schema");
        if (view.message.dataSize == 0)
            throw std::runtime_error("Empty FlatBuffer in recording");
        aligned.resize(view.message.dataSize / 8 + (view.message.dataSize % 8 != 0));
        std::memcpy(aligned.data(), view.message.data, view.message.dataSize);
        flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t *>(aligned.data()),
                                       view.message.dataSize);
        if (!fb::VerifyRecordBuffer(verifier))
            throw std::runtime_error("Invalid FlatBuffer in recording");
        const auto *f = fb::GetRecord(aligned.data());
        Record r;
        if (f->kind() > fb::Kind::Truth || f->source() > fb::Source::Simulator)
            throw std::runtime_error("Unknown record enum");
        r.kind = static_cast<Kind>(f->kind());
        r.source = static_cast<Source>(f->source());
        r.stamp = {Time(f->arrival_ns()), Time(f->utc_ns()), Time(f->measurement_ns()),
                   f->timing_sigma_ns() * 1e-9, f->sequence()};
        r.native_counter = f->native_counter();
        r.flags = f->flags();
        r.gnss_age_s = f->gnss_age_s();
        r.update_ms = f->update_ms();
        r.normalized_residual = f->normalized_residual();
        if (f->payload())
            r.payload.assign(f->payload()->begin(), f->payload()->end());
        if (f->values())
            r.values.assign(f->values()->begin(), f->values()->end());
        if (f->covariance())
            r.covariance.assign(f->covariance()->begin(), f->covariance()->end());
        if (f->detail())
            r.detail = f->detail()->str();
        callback(r);
    }
}
Record state_record(const Estimate &e, const Earth *earth, Stamp stamp) {
    Record r;
    r.kind = Kind::state;
    r.source = Source::estimator;
    r.stamp = stamp;
    r.stamp.measurement = e.state.time;
    r.stamp.sequence = e.revision;
    r.values.assign(e.state.data.begin(), e.state.data.end());
    for (int i = 0; i < 15; ++i)
        for (int j = 0; j < 15; ++j)
            r.covariance.push_back(e.covariance(i, j));
    r.flags =
        static_cast<std::uint32_t>(e.stage) | (e.heading_observed ? 256 : 0) | (earth ? 512 : 0);
    r.gnss_age_s = e.gnss_age_s;
    r.update_ms = e.update_ms;
    r.normalized_residual = e.normalized_residual;
    r.detail = std::string(stage_name(e.stage)) + ": " + e.reason;
    if (earth) {
        for (double v : earth->origin)
            r.values.push_back(v);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.values.push_back(earth->to_ecef(i, j));
    }
    return r;
}
} // namespace ins
