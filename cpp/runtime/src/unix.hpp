#pragma once
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace pl64::detail {
struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(int value) : fd(value) {}
    ~Fd() {
        if (fd >= 0)
            ::close(fd);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept : fd(std::exchange(other.fd, -1)) {}
    Fd &operator=(Fd &&other) noexcept {
        if (this != &other) {
            if (fd >= 0)
                ::close(fd);
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }
};
inline void check(bool ok, const char *operation) {
    if (!ok)
        throw std::system_error(errno, std::generic_category(), operation);
}
inline bool again() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
inline void setup(int fd) {
    check(fd >= 0, "Unix socket");
    check(fcntl(fd, F_SETFD, FD_CLOEXEC) >= 0, "socket close-on-exec");
    check(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) >= 0, "nonblocking socket");
#ifdef SO_NOSIGPIPE
    int one = 1;
    check(setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == 0, "no SIGPIPE");
#endif
}
inline int send_flags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}
inline sockaddr_un address(const std::filesystem::path &path) {
    sockaddr_un value{};
    value.sun_family = AF_UNIX;
    const auto text = path.string();
    if (text.empty() || text.size() >= sizeof(value.sun_path))
        throw std::invalid_argument("Unix socket path too long");
    std::copy(text.begin(), text.end(), value.sun_path);
    return value;
}
struct Listener {
    Fd fd;
    std::filesystem::path path;
    explicit Listener(const std::filesystem::path &name) : path(name) {
        const auto addr = address(path);
        fd = Fd(::socket(AF_UNIX, SOCK_STREAM, 0));
        setup(fd.fd);
        // Never replace an active listener, including one owned by another user.
        if (std::filesystem::exists(path)) {
            if (!std::filesystem::is_socket(std::filesystem::symlink_status(path)))
                throw std::runtime_error("Socket path exists and is not a socket");
            Fd probe(::socket(AF_UNIX, SOCK_STREAM, 0));
            setup(probe.fd);
            if (::connect(probe.fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0 ||
                errno != ECONNREFUSED)
                throw std::runtime_error("Socket already in use");
            std::filesystem::remove(path);
        }
        check(::bind(fd.fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0,
              "bind Unix socket");
        check(::chmod(path.c_str(), 0660) == 0, "Unix socket permissions");
        check(::listen(fd.fd, 16) == 0, "listen Unix socket");
    }
    ~Listener() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};
inline Fd connect(const std::filesystem::path &path, int timeout_ms) {
    const auto addr = address(path);
    Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    setup(fd.fd);
    if (::connect(fd.fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0) {
        if (errno != EINPROGRESS)
            check(false, "connect Unix socket");
        pollfd poller{fd.fd, POLLOUT, 0};
        if (::poll(&poller, 1, timeout_ms) <= 0)
            throw std::runtime_error("Socket connect timed out");
        int error = 0;
        socklen_t size = sizeof(error);
        check(getsockopt(fd.fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0, "socket error");
        if (error)
            throw std::system_error(error, std::generic_category(), "connect Unix socket");
    }
    return fd;
}
} // namespace pl64::detail
