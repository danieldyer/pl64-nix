#include "pl64/ipc.hpp"
#include "unix.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <deque>
#include <mutex>
#include <random>
#include <sys/mman.h>

namespace pl64 {
using namespace detail;
namespace {
constexpr std::size_t max_record = 64 * 1024 * 1024;
static_assert(std::endian::native == std::endian::little);
struct Header {
    std::uint32_t magic = 0x50363442, version = 1;
    std::uint64_t stream{}, sequence{}, mono{}, utc{}, offset{}, size{}, capacity{};
};
static_assert(sizeof(Header) == 64);
struct Connection {
    Fd fd;
    std::mutex mutex;
    explicit Connection(Fd value) : fd(std::move(value)) {}
    void ack(std::uint64_t sequence) {
        std::lock_guard lock(mutex);
        if (fd.fd >= 0 &&
            ::send(fd.fd, &sequence, sizeof(sequence), send_flags()) != sizeof(sequence))
            fd = Fd();
    }
};
bool send_record(int socket, int file, const Header &header) {
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> ancillary{};
    iovec iov{const_cast<Header *>(&header), sizeof(header)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ancillary.data();
    msg.msg_controllen = ancillary.size();
    auto *control = CMSG_FIRSTHDR(&msg);
    control->cmsg_level = SOL_SOCKET;
    control->cmsg_type = SCM_RIGHTS;
    control->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(control), &file, sizeof(file));
    // Small headers normally fit atomically. A short write disconnects this
    // consumer; it never leaves an ambiguous stream usable for another record.
    return ::sendmsg(socket, &msg, send_flags()) == sizeof(header);
}
std::string file_text(const std::filesystem::path &path) {
    Fd fd(::open(path.c_str(), O_RDONLY));
    if (fd.fd < 0)
        return {};
    char text[128];
    const auto n = ::read(fd.fd, text, sizeof(text));
    return n > 0 ? std::string(text, n) : std::string();
}
} // namespace
std::uint64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::uint64_t utc_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
struct SharedBlock::Impl {
    Fd write, read;
    std::byte *data{};
    std::size_t capacity{};
    bool sealed{};
    explicit Impl(std::size_t bytes) : capacity(bytes) {
        if (!bytes || bytes > max_record)
            throw std::invalid_argument("Shared record size outside 1..64 MiB");
#ifdef __linux__
        write = Fd(::memfd_create("pl64-record", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        check(write.fd >= 0, "memfd_create");
#else
        // Portable test/development fallback. Production Linux always uses RAM
        // backed, kernel-sealed memfd objects, never temporary recording files.
        std::string name = (std::filesystem::temp_directory_path() / "pl64-record-XXXXXX").string();
        write = Fd(::mkstemp(name.data()));
        check(write.fd >= 0, "shared temporary file");
        read = Fd(::open(name.c_str(), O_RDONLY | O_CLOEXEC));
        ::unlink(name.c_str());
        check(read.fd >= 0, "read-only shared file");
#endif
        check(::ftruncate(write.fd, bytes) == 0, "size shared record");
        void *view = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, write.fd, 0);
        check(view != MAP_FAILED, "map shared record");
        data = static_cast<std::byte *>(view);
    }
    void seal() {
        if (sealed)
            throw std::logic_error("Record already committed");
        check(::munmap(data, capacity) == 0, "unmap producer record");
        data = nullptr;
#ifdef __linux__
        check(::fcntl(write.fd, F_ADD_SEALS,
                      F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0,
              "seal shared record");
        read = std::move(write);
#else
        write = Fd();
#endif
        sealed = true;
    }
    ~Impl() {
        if (data)
            ::munmap(data, capacity);
    }
};
SharedBlock::SharedBlock(std::size_t size) : impl_(std::make_unique<Impl>(size)) {
}
SharedBlock::~SharedBlock() = default;
SharedBlock::SharedBlock(SharedBlock &&) noexcept = default;
SharedBlock &SharedBlock::operator=(SharedBlock &&) noexcept = default;
std::span<std::byte> SharedBlock::writable() {
    if (!impl_ || impl_->sealed)
        throw std::logic_error("Record is immutable");
    return {impl_->data, impl_->capacity};
}
struct Publisher::Impl {
    Listener listener;
    Stream stream;
    SharedBlock schema;
    std::size_t schema_size{}, credits, limit;
    std::uint64_t identity{}, sequence{};
    mutable std::mutex mutex;
    PublisherStats counters;
    struct Pending {
        std::uint64_t sequence;
        std::size_t bytes;
    };
    struct Client {
        Fd fd;
        std::deque<Pending> pending;
        std::size_t outstanding{};
        std::array<char, 8> ack{};
        std::size_t ack_size{};
    };
    std::vector<Client> clients;
    static std::string description(const Stream &stream) {
        return nlohmann::json{{"protocol", 1},
                              {"topic", stream.topic},
                              {"schema_name", stream.schema_name},
                              {"schema", stream.schema},
                              {"metadata", stream.metadata}}
            .dump();
    }
    Impl(const std::filesystem::path &path, Stream info, std::size_t count, std::size_t bytes)
        : listener(path), stream(std::move(info)), schema(description(stream).size()),
          credits(count), limit(bytes) {
        if (!credits || credits > 128 || limit < 1 || limit > 1024ULL * 1024 * 1024)
            throw std::invalid_argument("Invalid subscriber credit limits");
        std::random_device random;
        identity = (std::uint64_t(random()) << 32) ^ random() ^ monotonic_ns();
        stream.metadata["boot_id"] = file_text("/proc/sys/kernel/random/boot_id");
        std::error_code executable_error;
        const auto executable = std::filesystem::read_symlink("/proc/self/exe", executable_error);
        if (!executable_error)
            stream.metadata["executable"] = executable.string();
        // Reallocate after adding boot identity to the handshake.
        const auto text = description(stream);
        schema = SharedBlock(text.size());
        schema_size = text.size();
        std::memcpy(schema.writable().data(), text.data(), text.size());
        schema.impl_->seal();
    }
    void poll() {
        while (clients.size() < 16) {
            Fd fd(::accept(listener.fd.fd, nullptr, nullptr));
            if (fd.fd < 0)
                break;
            setup(fd.fd);
            Header header;
            header.stream = identity;
            header.size = schema_size;
            header.capacity = schema_size;
            if (send_record(fd.fd, schema.impl_->read.fd, header))
                clients.push_back({std::move(fd), {}});
        }
        std::erase_if(clients, [&](auto &client) {
            for (;;) {
                const auto n = ::recv(client.fd.fd, client.ack.data() + client.ack_size,
                                      8 - client.ack_size, 0);
                if (n == 0 || (n < 0 && !again())) {
                    ++counters.disconnected;
                    return true;
                }
                if (n < 0)
                    break;
                client.ack_size += std::size_t(n);
                if (client.ack_size == 8) {
                    std::uint64_t sequence;
                    std::memcpy(&sequence, client.ack.data(), 8);
                    client.ack_size = 0;
                    auto pending =
                        std::find_if(client.pending.begin(), client.pending.end(),
                                     [&](const auto &item) { return item.sequence == sequence; });
                    if (pending == client.pending.end()) {
                        ++counters.disconnected;
                        return true;
                    }
                    client.outstanding -= pending->bytes;
                    client.pending.erase(pending);
                }
            }
            return false;
        });
    }
};
Publisher::Publisher(const std::filesystem::path &path, Stream info, std::size_t credits,
                     std::size_t limit)
    : impl_(std::make_unique<Impl>(path, std::move(info), credits, limit)) {
}
Publisher::~Publisher() = default;
void Publisher::poll() {
    std::lock_guard lock(impl_->mutex);
    impl_->poll();
}
void Publisher::publish(SharedBlock block, std::size_t offset, std::size_t size, std::uint64_t mono,
                        std::uint64_t utc) {
    if (!block.impl_ || !size || offset > block.impl_->capacity ||
        size > block.impl_->capacity - offset)
        throw std::invalid_argument("Invalid shared record slice");
    block.impl_->seal();
    std::lock_guard lock(impl_->mutex);
    auto &p = *impl_;
    p.poll();
    Header header;
    header.stream = p.identity;
    header.sequence = ++p.sequence;
    header.mono = mono;
    header.utc = utc;
    header.offset = offset;
    header.size = size;
    header.capacity = block.impl_->capacity;
    std::erase_if(p.clients, [&](auto &c) {
        if (c.pending.size() >= p.credits || block.impl_->capacity > p.limit - c.outstanding ||
            !send_record(c.fd.fd, block.impl_->read.fd, header)) {
            ++p.counters.disconnected;
            return true;
        }
        c.pending.push_back({header.sequence, block.impl_->capacity});
        c.outstanding += block.impl_->capacity;
        return false;
    });
    ++p.counters.published;
    p.counters.bytes += size;
}
void Publisher::publish(std::span<const std::uint8_t> bytes, std::uint64_t mono,
                        std::uint64_t utc) {
    SharedBlock block(bytes.size());
    std::memcpy(block.writable().data(), bytes.data(), bytes.size());
    publish(std::move(block), 0, bytes.size(), mono, utc);
}
PublisherStats Publisher::stats() const {
    std::lock_guard lock(impl_->mutex);
    auto stats = impl_->counters;
    stats.subscribers = impl_->clients.size();
    for (const auto &c : impl_->clients)
        stats.outstanding_bytes += c.outstanding;
    return stats;
}
struct Frame::Impl {
    std::shared_ptr<Connection> connection;
    Fd file;
    void *data = MAP_FAILED;
    Header header;
    ~Impl() {
        if (data != MAP_FAILED)
            ::munmap(data, header.capacity);
        if (connection && header.sequence)
            connection->ack(header.sequence);
    }
};
Frame::Frame(std::unique_ptr<Impl> value) : impl_(std::move(value)) {
    sequence = impl_->header.sequence;
    monotonic = impl_->header.mono;
    utc = impl_->header.utc;
    stream_id = impl_->header.stream;
}
Frame::~Frame() = default;
Frame::Frame(Frame &&) noexcept = default;
Frame &Frame::operator=(Frame &&) noexcept = default;
std::span<const std::uint8_t> Frame::bytes() const {
    return {static_cast<const std::uint8_t *>(impl_->data) + impl_->header.offset,
            std::size_t(impl_->header.size)};
}
struct Subscription::Impl {
    std::shared_ptr<Connection> connection;
    Stream stream;
    std::uint64_t identity{};
    explicit Impl(Fd fd) : connection(std::make_shared<Connection>(std::move(fd))) {}
};
Subscription::Subscription(const std::filesystem::path &path, int timeout_ms)
    : impl_(std::make_unique<Impl>(detail::connect(path, timeout_ms))) {
    auto hello = receive(timeout_ms);
    if (!hello || hello->sequence != 0)
        throw std::runtime_error("Missing stream schema handshake");
    const auto info = nlohmann::json::parse(hello->bytes());
    if (info.at("protocol") != 1)
        throw std::runtime_error("Unsupported stream protocol");
    impl_->stream = {info.at("topic"), info.at("schema_name"),
                     info.at("schema").get<std::vector<std::uint8_t>>(), info.at("metadata")};
    impl_->identity = hello->stream_id;
}
Subscription::~Subscription() = default;
const Stream &Subscription::stream() const {
    return impl_->stream;
}
std::unique_ptr<Frame> Subscription::receive(int timeout_ms) {
    std::lock_guard lock(impl_->connection->mutex);
    const auto fd = impl_->connection->fd.fd;
    if (fd < 0)
        throw std::runtime_error("Subscription closed");
    pollfd ready{fd, POLLIN, 0};
    if (::poll(&ready, 1, timeout_ms) == 0)
        return {};
    auto frame = std::make_unique<Frame::Impl>();
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> ancillary{};
    iovec iov{&frame->header, sizeof(Header)};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = ancillary.data();
    message.msg_controllen = ancillary.size();
    const auto n = ::recvmsg(fd, &message, 0);
    if (n < 0 && again())
        return {};
    if (n <= 0)
        throw std::runtime_error("Producer disconnected");
    unsigned descriptors = 0;
    for (auto *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            const auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (std::size_t i = 0; i < count; ++i) {
                int received;
                std::memcpy(&received, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                Fd candidate(received);
                if (++descriptors == 1)
                    frame->file = std::move(candidate);
            }
        }
    }
    if (descriptors != 1 || message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
        throw std::runtime_error("Invalid shared descriptor");
    check(::fcntl(frame->file.fd, F_SETFD, FD_CLOEXEC) == 0, "shared descriptor close-on-exec");
    auto received = std::size_t(n);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(100, timeout_ms));
    while (received < sizeof(Header)) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Truncated shared header");
        pollfd more{fd, POLLIN, 0};
        ::poll(&more, 1, 10);
        const auto count = ::recv(fd, reinterpret_cast<char *>(&frame->header) + received,
                                  sizeof(Header) - received, 0);
        if (count == 0 || (count < 0 && !again()))
            throw std::runtime_error("Truncated shared header");
        if (count > 0)
            received += std::size_t(count);
    }
    const auto &h = frame->header;
    if (h.magic != Header{}.magic || h.version != 1 || !h.capacity || h.capacity > max_record ||
        !h.size || h.offset > h.capacity || h.size > h.capacity - h.offset ||
        (impl_->identity && h.stream != impl_->identity))
        throw std::runtime_error("Invalid shared record header");
    struct stat status{};
    check(::fstat(frame->file.fd, &status) == 0, "inspect shared record");
    if (std::uint64_t(status.st_size) != h.capacity)
        throw std::runtime_error("Shared file size mismatch");
#ifdef __linux__
    const int seals = ::fcntl(frame->file.fd, F_GET_SEALS);
    constexpr int required = F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW;
    if (seals < 0 || (seals & required) != required)
        throw std::runtime_error("Unsealed shared record");
#endif
    frame->data = ::mmap(nullptr, h.capacity, PROT_READ, MAP_SHARED, frame->file.fd, 0);
    check(frame->data != MAP_FAILED, "map subscriber record");
    // mmap retains the object; the lease does not need an open descriptor.
    frame->file = Fd();
    frame->connection = impl_->connection;
    return std::unique_ptr<Frame>(new Frame(std::move(frame)));
}
} // namespace pl64
