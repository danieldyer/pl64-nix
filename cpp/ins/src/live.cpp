#include "ins/acquisition.hpp"
#include "ins/config.hpp"
#include "ins/estimator.hpp"
#include "ins/recording.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

namespace ins {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
struct Fd {
    int value = -1;
    explicit Fd(int v = -1) : value(v) {}
    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd &operator=(Fd &&other) noexcept {
        if (value >= 0)
            ::close(value);
        value = std::exchange(other.value, -1);
        return *this;
    }
};
void checked(bool ok, const char *operation) {
    if (!ok)
        throw std::system_error(errno, std::generic_category(), operation);
}
void nonblocking(int fd) {
    checked(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) >= 0, "nonblocking descriptor");
    checked(fcntl(fd, F_SETFD, FD_CLOEXEC) >= 0, "close-on-exec descriptor");
}
Stamp now() {
    const auto before = std::chrono::steady_clock::now();
    const auto utc = std::chrono::system_clock::now();
    const auto after = std::chrono::steady_clock::now();
    return {std::chrono::duration_cast<Time>((before + (after - before) / 2).time_since_epoch()),
            std::chrono::duration_cast<Time>(utc.time_since_epoch()),
            {},
            std::chrono::duration<double>(after - before).count() / 2,
            0};
}
template <class T> class Queue {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<T> items_;
    bool closed_ = false;

  public:
    void put(T item) {
        std::lock_guard lock(mutex_);
        if (closed_)
            return;
        if (items_.size() >= 32768)
            throw std::runtime_error(
                "Navigation queue overflow; stopping to preserve explicit data-loss boundary");
        items_.push_back(std::move(item));
        ready_.notify_one();
    }
    std::optional<T> get() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || !items_.empty(); });
        if (items_.empty())
            return {};
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }
};
void baud(int fd, unsigned rate) {
    termios options{};
    checked(tcgetattr(fd, &options) == 0, "read serial settings");
    cfmakeraw(&options);
    options.c_cflag |= CLOCAL | CREAD;
    options.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS | HUPCL);
    speed_t speed = B9600;
    if (rate == 460800) {
#ifdef B460800
        speed = B460800;
#else
        throw std::runtime_error("Live IMU transport requires Linux 460800 baud support");
#endif
    }
    cfsetispeed(&options, speed);
    cfsetospeed(&options, speed);
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;
    checked(tcsetattr(fd, TCSANOW, &options) == 0, "set serial settings");
}
void transmit(int fd, std::span<const std::uint8_t> bytes) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!bytes.empty()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Sensor transmit timeout");
        const auto n = ::write(fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLOUT, 0};
            checked(poll(&p, 1, 500) > 0, "serial transmit timeout");
            continue;
        }
        checked(n > 0, "serial transmit");
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
}
using Log = std::function<void(Record)>;
std::vector<std::uint8_t> command(int fd, std::uint8_t code, std::optional<std::uint8_t> value,
                                  std::uint8_t reply, const Log &log) {
    const auto bytes = imu_command(code, value);
    Record tx;
    tx.kind = Kind::metadata;
    tx.source = Source::imu;
    tx.payload = bytes;
    tx.detail = "IMU command transmitted";
    auto send = [&] {
        tx.stamp = now();
        log(tx);
        transmit(fd, bytes);
    };
    send();
    Framer framer(Source::imu);
    std::vector<std::uint8_t> answer;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    auto next_probe = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < until && !interrupted) {
        // The baud-change ACK can precede UART readiness on the fitted IMU.
        // Probe readiness with the read-only link test inside the existing
        // deadline. Never retransmit configuration writes or calibrations.
        if (code == 0x1f && !value && std::chrono::steady_clock::now() >= next_probe) {
            send();
            next_probe = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        }
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 30) <= 0)
            continue;
        std::array<std::uint8_t, 4096> buffer{};
        const auto n = ::read(fd, buffer.data(), buffer.size());
        if (n <= 0)
            continue;
        Record r;
        r.source = Source::imu;
        r.stamp = now();
        r.payload.assign(buffer.begin(), buffer.begin() + n);
        framer.feed(r.payload, [&](auto frame) {
            if (frame[3] == reply)
                answer.assign(frame.begin(), frame.end());
        });
        log(std::move(r));
        if (!answer.empty())
            return answer;
    }
    throw std::runtime_error("IMU command timed out: " + std::to_string(code));
}
void acknowledge(const std::vector<std::uint8_t> &frame) {
    if (frame.size() != 6 || frame[4] != 0)
        throw std::runtime_error("IMU rejected configuration");
}
void configure_imu(int fd, const Log &log) {
    bool found = false;
    unsigned selected = 0;
    for (unsigned rate : {460800u, 9600u}) {
        baud(fd, rate);
        try {
            const auto identity = command(fd, 0x49, {}, 0x49, log);
            std::string text(identity.begin() + 4, identity.end() - 1);
            while (!text.empty() && text.back() == 0)
                text.pop_back();
            if (text != "IMU50-T-IAP-GD#AH100-V4-B8#24.09.20SV2.0")
                throw std::invalid_argument("Unexpected IMU identity: " + text);
            found = true;
            selected = rate;
            break;
        } catch (const std::invalid_argument &) {
            throw;
        } catch (const std::runtime_error &) {
            if (interrupted)
                throw;
        }
    }
    if (!found)
        throw std::runtime_error("Known IMU not responding at 460800 or 9600 baud");
    acknowledge(command(fd, 0x0c, 0, 0x8c, log));
    if (selected != 460800) {
        acknowledge(command(fd, 0x0b, 7, 0x8b, log));
        baud(fd, 460800);
        acknowledge(command(fd, 0x1f, {}, 0x1f, log));
    }
    // Read only: startup zero changes persist in the sensor and require hardware
    // validation. No zero-calibration or EEPROM-save command is issued here.
    const auto zero = command(fd, 0xf8, {}, 0xf8, log);
    Record h;
    h.kind = Kind::health;
    h.source = Source::imu;
    h.stamp = now();
    h.payload = zero;
    h.detail = "Startup gyro-zero setting (F8); estimated bias includes any startup subtraction";
    log(std::move(h));
    acknowledge(command(fd, 0x56, 3, 0x56, log));
    acknowledge(command(fd, 0x0c, 7, 0x8c, log));
}
Fd gpsd() {
    Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
    checked(fd.value >= 0, "gpsd socket");
    nonblocking(fd.value);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(2947);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS)
            return Fd();
        // A wedged local daemon must not block IMU acquisition indefinitely.
        pollfd pending{fd.value, POLLOUT, 0};
        int error = 0;
        socklen_t size = sizeof(error);
        if (::poll(&pending, 1, 20) <= 0 ||
            ::getsockopt(fd.value, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
            return Fd();
    }
    constexpr std::string_view watch = "?WATCH={\"enable\":true,\"json\":false,\"raw\":2};\n";
    transmit(fd.value, {reinterpret_cast<const std::uint8_t *>(watch.data()), watch.size()});
    return fd;
}
// A transport fault ends this acquisition session. systemd repeats configuration
// and BIT; no parser, clock mapping or filter posterior crosses that boundary.
struct GpsFault : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Cadence {
    Time last, window;
    unsigned count = 0;
    double minimum, maximum;
    Cadence(Time start, double low, double high)
        : last(start), window(start), minimum(low), maximum(high) {}
    bool sample(Time time) {
        last = time;
        // Startup latency is not a sample interval. The independent stale()
        // deadline still covers the wait for the first decoded packet.
        if (!count)
            window = time;
        ++count;
        const double elapsed = seconds(time - window);
        if (elapsed < 2)
            return true;
        const double rate = (count - 1) / elapsed;
        window = time;
        count = 1;
        return rate >= minimum && rate <= maximum;
    }
    bool stale(Time time, double limit) const { return seconds(time - last) > limit; }
};
class Publisher {
    Fd fd_;
    std::filesystem::path path_;
    std::vector<sockaddr_un> clients_;

  public:
    explicit Publisher(std::filesystem::path path)
        : fd_(::socket(AF_UNIX, SOCK_DGRAM, 0)), path_(std::move(path)) {
        checked(fd_.value >= 0, "state socket");
        nonblocking(fd_.value);
        if (path_.string().size() >= sizeof(sockaddr_un::sun_path))
            throw std::invalid_argument("State socket path too long");
        if (std::filesystem::exists(path_)) {
            if (!std::filesystem::is_socket(std::filesystem::symlink_status(path_)))
                throw std::runtime_error("State path exists and is not a socket");
            std::filesystem::remove(path_);
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto name = path_.string();
        std::copy(name.begin(), name.end(), address.sun_path);
        checked(::bind(fd_.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
                "bind state socket");
        checked(::chmod(name.c_str(), 0660) == 0, "state socket permissions");
    }
    ~Publisher() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    void send(const Record &r) {
        for (;;) {
            std::array<char, 32> request{};
            sockaddr_un peer{};
            socklen_t size = sizeof(peer);
            const auto n = recvfrom(fd_.value, request.data(), request.size(), 0,
                                    reinterpret_cast<sockaddr *>(&peer), &size);
            if (n < 0)
                break;
            if (n == 9 && std::string_view(request.data(), 9) == "subscribe" && peer.sun_path[0] &&
                std::find(peer.sun_path, std::end(peer.sun_path), '\0') !=
                    std::end(peer.sun_path)) {
                std::erase_if(clients_, [&](const auto &c) {
                    return std::string_view(c.sun_path) == peer.sun_path;
                });
                if (clients_.size() < 16)
                    clients_.push_back(peer);
            }
        }
        const auto bytes = serialize(r);
        std::erase_if(clients_, [&](const auto &peer) {
            const auto n = sendto(fd_.value, bytes.data(), bytes.size(), 0,
                                  reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
            return n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS &&
                   errno != EINTR;
        });
    }
};
} // namespace

int live(const nlohmann::json &settings, const std::filesystem::path &schema) {
    const auto config = configuration(settings.value("navigation", nlohmann::json::object()));
    Estimator estimator(config);
    const auto service = settings.value("service", nlohmann::json::object());
    const auto directory =
        std::filesystem::path(service.value("log_directory", std::string("/var/lib/pl64-ins")));
    const double rotation = service.value("rotation_s", 900.);
    if (!std::isfinite(rotation) || rotation < 10)
        throw std::invalid_argument("Invalid log rotation");
    std::filesystem::create_directories(directory);
    Queue<Record> logs;
    std::atomic<bool> failed = false;
    std::mutex error_mutex;
    std::exception_ptr error;
    auto fail = [&] {
        std::lock_guard lock(error_mutex);
        if (!error)
            error = std::current_exception();
        failed = true;
    };
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    std::signal(SIGPIPE, SIG_IGN);
    interrupted = 0;
    std::jthread logger([&] {
        try {
            std::unique_ptr<Recorder> recorder;
            Time opened{};
            unsigned index = 0;
            while (auto r = logs.get()) {
                if (!recorder || seconds(r->stamp.arrival - opened) >= rotation) {
                    if (std::filesystem::space(directory).available < 256 * 1024 * 1024)
                        throw std::runtime_error("Recording disk has less than 256 MiB free");
                    if (recorder)
                        recorder->close();
                    opened = r->stamp.arrival;
                    recorder = std::make_unique<Recorder>(
                        directory / ("navigation-" + std::to_string(r->stamp.utc.count()) + "-" +
                                     std::to_string(index++) + ".mcap"),
                        schema);
                    Record meta;
                    meta.kind = Kind::metadata;
                    meta.source = Source::estimator;
                    meta.stamp = r->stamp;
                    meta.detail =
                        nlohmann::json{{"navigation", configuration_json(config)},
                                       {"schema_path", schema.string()},
                                       {"service", service},
                                       {"clock", "steady_clock + system_clock; timestamps are not "
                                                 "PPS-disciplined measurement times"}}
                            .dump();
                    recorder->write(meta);
                }
                recorder->write(*r);
            }
            if (recorder)
                recorder->close();
        } catch (...) {
            fail();
        }
    });
    Fd imu;
    std::unique_ptr<Publisher> publisher;
    try {
        imu = Fd(::open(service.value("imu_device", std::string("/dev/pl64com1")).c_str(),
                        O_RDWR | O_NOCTTY | O_NONBLOCK));
        checked(imu.value >= 0, "open IMU");
        checked(::flock(imu.value, LOCK_EX | LOCK_NB) == 0, "exclusive IMU access");
        nonblocking(imu.value);
        publisher = std::make_unique<Publisher>(
            service.value("state_socket", std::string("/run/pl64-ins/state.sock")));
        configure_imu(imu.value, [&](Record r) { logs.put(std::move(r)); });
        Acquisition acquisition(config);
        Cadence imu_health(now().arrival, 180, 220);
        std::optional<Cadence> gps_health;
        std::optional<std::uint32_t> gps_tow;
        acquisition.on_gnss_packet = [&](std::uint32_t tow, bool time_valid, const Stamp &stamp) {
            if (time_valid && tow >= 604800000)
                throw GpsFault("GNSS invalid time-of-week counter");
            if (!time_valid && gps_tow)
                throw GpsFault("GNSS lost established time; reconfiguring receiver");
            if (time_valid && gps_tow) {
                if (tow == *gps_tow)
                    return; // Repeated old packets do not keep the receiver alive.
                const auto step = (tow + 604800000 - *gps_tow) % 604800000;
                if (step > 1000)
                    throw GpsFault("GNSS epoch discontinuity; reconfiguring receiver");
            }
            if (time_valid)
                gps_tow = tow;
            if (!gps_health->sample(stamp.arrival))
                throw GpsFault("GNSS cadence outside 8-12 Hz; reconfiguring receiver");
        };
        unsigned state_count = 0;
        acquisition.on_discontinuity = [&](auto reason) { estimator.reset(std::move(reason)); };
        acquisition.on_record = [&](const auto &record) { logs.put(record); };
        acquisition.on_gnss = [&](const auto &fix) { estimator.gnss(fix); };
        acquisition.on_imu = [&](const ImuSample &sample) {
            if (sample.gap)
                throw std::runtime_error("IMU counter discontinuity; reconfiguring sensor");
            if (!imu_health.sample(sample.stamp.arrival))
                throw std::runtime_error("IMU cadence outside 180-220 Hz; reconfiguring sensor");
            estimator.imu(sample);
            auto state = state_record(estimator.estimate(), estimator.earth(), sample.stamp);
            publisher->send(state);
            if (state_count++ % 10 == 0)
                logs.put(std::move(state));
        };
        Fd gps;
        Time retry{}, last_report{};
        std::uint64_t sequence = 0;
        while (!interrupted && !failed.load()) {
            const auto stamp = now();
            if (gps.value < 0 && stamp.arrival >= retry) {
                gps = gpsd();
                if (gps.value >= 0)
                    gps_health.emplace(now().arrival, 8, 12);
                retry = stamp.arrival + std::chrono::seconds(2);
            }
            pollfd descriptors[] = {{imu.value, POLLIN, 0}, {gps.value, POLLIN, 0}};
            const int ready = ::poll(descriptors, 2, 100);
            if (ready < 0 && errno == EINTR)
                continue;
            checked(ready >= 0, "sensor poll");
            for (unsigned i = 0; i < 2; ++i)
                if (descriptors[i].revents) {
                    if (descriptors[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                        if (i)
                            throw GpsFault("gpsd disconnected; restarting sensor initialization");
                        throw std::runtime_error("IMU disconnected");
                    }
                    std::array<std::uint8_t, 8192> bytes{};
                    const auto n = ::read(descriptors[i].fd, bytes.data(), bytes.size());
                    if (n < 0 && (errno == EAGAIN || errno == EINTR))
                        continue;
                    if (n <= 0) {
                        if (i)
                            throw GpsFault("gpsd disconnected; restarting sensor initialization");
                        throw std::runtime_error("IMU read failed");
                    }
                    Record r;
                    r.source = i ? Source::gnss : Source::imu;
                    r.stamp = now();
                    r.stamp.sequence = sequence++;
                    r.payload.assign(bytes.begin(), bytes.begin() + n);
                    logs.put(r);
                    acquisition.accept(r);
                }
            const auto checked_at = now().arrival;
            if (imu_health.stale(checked_at, .25))
                throw std::runtime_error("IMU sample timeout; reconfiguring sensor");
            if (gps_health && gps_health->stale(checked_at, 3))
                throw GpsFault("GNSS NAV-PVT timeout; reconfiguring receiver");
            if (seconds(stamp.arrival - last_report) >= 5) {
                std::cerr << "navigation stage=" << stage_name(estimator.estimate().stage)
                          << " gnss_age_s=" << estimator.estimate().gnss_age_s
                          << " update_ms=" << estimator.estimate().update_ms
                          << " imu_bad_checksums=" << acquisition.imu_stats().bad_checksums << '\n';
                last_report = stamp.arrival;
            }
        }
    } catch (...) {
        fail();
    }
    // Explicitly invalidate the final published solution even when IMU input has
    // stopped. Consumers must also enforce their own message-age deadline.
    try {
        estimator.reset("Acquisition stopped; sensor initialization required");
        auto state = state_record(estimator.estimate(), nullptr, now());
        if (publisher)
            publisher->send(state);
        logs.put(std::move(state));
    } catch (...) {
        fail();
    }
    if (imu.value >= 0) {
        try {
            transmit(imu.value, imu_command(0x0c, 0));
        } catch (...) {
            fail();
        }
    }
    logs.close();
    logger.join();
    if (error) {
        try {
            std::rethrow_exception(error);
        } catch (const GpsFault &e) {
            std::cerr << e.what() << '\n';
            return 75; // Nix ExecStopPost requests gpsd reconfiguration, then Restart retries.
        }
    }
    return 0;
}
} // namespace ins
