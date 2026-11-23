#include "avatar/instance/activation_channel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace avatar::instance {
namespace {

constexpr std::uint32_t kMaxPayload = 64U * 1024U;
#if defined(_WIN32)
constexpr int kPollIntervalMs = 5;
#endif

using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
[[maybe_unused]] int remaining_ms(Clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return left.count() > 0 ? static_cast<int>(left.count()) : 0;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                              size);
    return out;
}

bool wait_for_bytes(HANDLE pipe, DWORD wanted, int timeout_ms, const std::atomic<bool>& stop) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == 0) {
            return false;
        }
        if (available >= wanted) {
            return true;
        }
        if (stop.load(std::memory_order_relaxed)) {
            return false;
        }
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
}

bool read_exact(HANDLE pipe, void* data, DWORD size, int timeout_ms,
                const std::atomic<bool>& stop) {
    if (!wait_for_bytes(pipe, size, timeout_ms, stop)) {
        return false;
    }
    DWORD total = 0;
    while (total < size) {
        DWORD read = 0;
        if (ReadFile(pipe, static_cast<char*>(data) + total, size - total, &read, nullptr) == 0 ||
            read == 0) {
            return false;
        }
        total += read;
    }
    return true;
}

bool write_exact(HANDLE pipe, const void* data, DWORD size) {
    DWORD total = 0;
    while (total < size) {
        DWORD written = 0;
        if (WriteFile(pipe, static_cast<const char*>(data) + total, size - total, &written,
                      nullptr) == 0 ||
            written == 0) {
            return false;
        }
        total += written;
    }
    return true;
}

bool overlapped_write(HANDLE pipe, const void* data, DWORD size, int timeout_ms) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(pipe, data, size, &written, &overlapped);
    if (ok == 0 && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(overlapped.hEvent, static_cast<DWORD>(std::max(1, timeout_ms))) ==
            WAIT_OBJECT_0) {
            ok = GetOverlappedResult(pipe, &overlapped, &written, FALSE);
        } else {
            (void)CancelIoEx(pipe, &overlapped);
            ok = 0;
        }
    }
    CloseHandle(overlapped.hEvent);
    return ok != 0 && written == size;
}

bool overlapped_read(HANDLE pipe, void* data, DWORD size, int timeout_ms) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        return false;
    }
    DWORD read = 0;
    BOOL ok = ReadFile(pipe, data, size, &read, &overlapped);
    if (ok == 0 && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(overlapped.hEvent, static_cast<DWORD>(std::max(1, timeout_ms))) ==
            WAIT_OBJECT_0) {
            ok = GetOverlappedResult(pipe, &overlapped, &read, FALSE);
        } else {
            (void)CancelIoEx(pipe, &overlapped);
            ok = 0;
        }
    }
    CloseHandle(overlapped.hEvent);
    return ok != 0 && read == size;
}
#endif

} // namespace

struct ActivationServer::Impl {
    std::string name;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> received{0};
    mutable std::mutex mutex;
    std::vector<ActivationMessage> queue;

#if defined(_WIN32)
    HANDLE pipe = INVALID_HANDLE_VALUE;
#else
    int listen_fd = -1;
#endif
};

std::string ActivationMessage::to_json() const {
    nlohmann::json value;
    value["version"] = 1;
    value["command"] = command.empty() ? std::string("activate") : command;
    if (!avatar_path.empty()) {
        value["avatar"] = avatar_path;
    }
    if (!show_path.empty()) {
        value["show"] = show_path;
    }
    return value.dump();
}

std::optional<ActivationMessage> ActivationMessage::from_json(std::string_view json) {
    if (json.empty() || json.size() > kMaxPayload) {
        return std::nullopt;
    }
    nlohmann::json value = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        return std::nullopt;
    }
    const auto version = value.find("version");
    if (version == value.end() || !version->is_number_integer() || version->get<int>() != 1) {
        return std::nullopt;
    }
    ActivationMessage message;
    const auto command = value.find("command");
    if (command != value.end()) {
        if (!command->is_string()) {
            return std::nullopt;
        }
        message.command = command->get<std::string>();
    }
    if (message.command != "activate") {
        return std::nullopt;
    }
    const auto avatar = value.find("avatar");
    if (avatar != value.end()) {
        if (!avatar->is_string()) {
            return std::nullopt;
        }
        message.avatar_path = avatar->get<std::string>();
    }
    const auto show = value.find("show");
    if (show != value.end()) {
        if (!show->is_string()) {
            return std::nullopt;
        }
        message.show_path = show->get<std::string>();
    }
    if (message.avatar_path.size() > kMaxPayload || message.show_path.size() > kMaxPayload) {
        return std::nullopt;
    }
    return message;
}

ActivationServer::ActivationServer() : impl_(std::make_unique<Impl>()) {}

ActivationServer::~ActivationServer() {
    stop();
}

bool ActivationServer::start(const InstanceIdentity& identity, std::string& error) {
    stop();
    impl_ = std::make_unique<Impl>();
    impl_->name = identity.activation_name;
    impl_->stop.store(false, std::memory_order_relaxed);

#if defined(_WIN32)
    const std::wstring name = widen(identity.activation_name);
    if (name.empty()) {
        error = "empty activation channel name";
        return false;
    }
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        error = "CreateNamedPipe failed: " + std::to_string(GetLastError());
        return false;
    }
    impl_->pipe = pipe;
#else
    if (identity.activation_name.size() >= sizeof(sockaddr_un::sun_path)) {
        error = "activation socket path is too long";
        return false;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("socket failed: ") + std::strerror(errno);
        return false;
    }
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, identity.activation_name.c_str(),
                identity.activation_name.size());
    // The primary owns the lock, so a leftover socket from a crash is safe to
    // replace. A live primary would have made us secondary before this point.
    (void)::unlink(identity.activation_name.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = std::string("bind failed: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    (void)::chmod(identity.activation_name.c_str(), 0600);
    if (::listen(fd, 8) != 0) {
        error = std::string("listen failed: ") + std::strerror(errno);
        ::close(fd);
        (void)::unlink(identity.activation_name.c_str());
        return false;
    }
    impl_->listen_fd = fd;
#endif

    impl_->running.store(true, std::memory_order_relaxed);
#if defined(_WIN32)
    Impl* raw = impl_.get();
    impl_->thread = std::thread([raw] { ActivationServer::server_loop(*raw); });
#else
    Impl* raw = impl_.get();
    impl_->thread = std::thread([raw] {
        while (!raw->stop.load(std::memory_order_relaxed)) {
            pollfd descriptor = {};
            descriptor.fd = raw->listen_fd;
            descriptor.events = POLLIN;
            const int ready = ::poll(&descriptor, 1, 200);
            if (ready <= 0) {
                continue;
            }
            const int client = ::accept(raw->listen_fd, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            timeval timeout = {};
            timeout.tv_sec = 1;
            (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            (void)::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

            std::uint32_t length = 0;
            bool ok = true;
            std::size_t total = 0;
            while (total < sizeof(length)) {
                const ssize_t got = ::read(client, reinterpret_cast<char*>(&length) + total,
                                           sizeof(length) - total);
                if (got <= 0) {
                    ok = false;
                    break;
                }
                total += static_cast<std::size_t>(got);
            }
            if (ok && length > 0 && length <= kMaxPayload) {
                std::string payload(length, '\0');
                std::size_t read = 0;
                while (read < payload.size()) {
                    const ssize_t got =
                        ::read(client, payload.data() + read, payload.size() - read);
                    if (got <= 0) {
                        ok = false;
                        break;
                    }
                    read += static_cast<std::size_t>(got);
                }
                if (ok) {
                    if (auto message = ActivationMessage::from_json(payload)) {
                        {
                            std::lock_guard<std::mutex> lock(raw->mutex);
                            raw->queue.push_back(std::move(*message));
                        }
                        raw->received.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            const char ack = 1;
            const ssize_t ignored = ::write(client, &ack, 1);
            (void)ignored;
            ::close(client);
        }
    });
#endif
    return true;
}

void ActivationServer::stop() noexcept {
    if (!impl_) {
        return;
    }
    impl_->stop.store(true, std::memory_order_relaxed);
#if defined(_WIN32)
    if (impl_->pipe != INVALID_HANDLE_VALUE) {
        // Unblock ConnectNamedPipe. The dummy client is closed immediately; the
        // loop sees the stop flag before it reads anything.
        const std::wstring name = widen(impl_->name);
        if (!name.empty()) {
            HANDLE dummy = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                       OPEN_EXISTING, 0, nullptr);
            if (dummy != INVALID_HANDLE_VALUE) {
                CloseHandle(dummy);
            }
        }
    }
#endif
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
#if defined(_WIN32)
    if (impl_->pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->pipe);
        impl_->pipe = INVALID_HANDLE_VALUE;
    }
#else
    if (impl_->listen_fd >= 0) {
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (!impl_->name.empty()) {
        (void)::unlink(impl_->name.c_str());
    }
#endif
    impl_->running.store(false, std::memory_order_relaxed);
}

bool ActivationServer::running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_relaxed);
}

std::vector<ActivationMessage> ActivationServer::poll() {
    std::vector<ActivationMessage> out;
    if (!impl_) {
        return out;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    out.swap(impl_->queue);
    return out;
}

std::uint64_t ActivationServer::received() const noexcept {
    return impl_ ? impl_->received.load(std::memory_order_relaxed) : 0;
}

#if defined(_WIN32)
void ActivationServer::server_loop(Impl& impl) {
    while (!impl.stop.load(std::memory_order_relaxed)) {
        const BOOL connected = ConnectNamedPipe(impl.pipe, nullptr);
        if (connected == 0) {
            const DWORD error = GetLastError();
            if (error != ERROR_PIPE_CONNECTED) {
                if (impl.stop.load(std::memory_order_relaxed)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
        }
        if (impl.stop.load(std::memory_order_relaxed)) {
            (void)DisconnectNamedPipe(impl.pipe);
            break;
        }

        std::uint32_t length = 0;
        bool ok = read_exact(impl.pipe, &length, sizeof(length), 1000, impl.stop);
        if (ok && length > 0 && length <= kMaxPayload) {
            std::string payload(length, '\0');
            ok = read_exact(impl.pipe, payload.data(), length, 1000, impl.stop);
            if (ok) {
                if (auto message = ActivationMessage::from_json(payload)) {
                    {
                        std::lock_guard<std::mutex> lock(impl.mutex);
                        impl.queue.push_back(std::move(*message));
                    }
                    impl.received.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        const char ack = 1;
        (void)write_exact(impl.pipe, &ack, 1);
        (void)FlushFileBuffers(impl.pipe);
        (void)DisconnectNamedPipe(impl.pipe);
    }
}
#endif

ActivationResult send_activation(const InstanceIdentity& identity, const ActivationMessage& message,
                                 int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
    const std::string payload = message.to_json();
    if (payload.size() > kMaxPayload) {
        return {ActivationDelivery::error, "activation message is too large"};
    }
    const auto length = static_cast<std::uint32_t>(payload.size());

#if defined(_WIN32)
    const std::wstring name = widen(identity.activation_name);
    if (name.empty()) {
        return {ActivationDelivery::error, "empty activation channel name"};
    }
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (;;) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return {ActivationDelivery::not_running, "no running instance"};
        }
        if (error == ERROR_PIPE_BUSY) {
            const int left = remaining_ms(deadline);
            if (left <= 0 || WaitNamedPipeW(name.c_str(), static_cast<DWORD>(left)) == 0) {
                return {ActivationDelivery::not_running, "the running instance did not answer"};
            }
            continue;
        }
        return {ActivationDelivery::error,
                "opening the activation channel failed: " + std::to_string(error)};
    }

    const int write_timeout = std::max(1, remaining_ms(deadline));
    bool ok = overlapped_write(pipe, &length, sizeof(length), write_timeout) &&
              overlapped_write(pipe, payload.data(), length, write_timeout) &&
              FlushFileBuffers(pipe) != 0;
    char ack = 0;
    if (ok) {
        ok = overlapped_read(pipe, &ack, 1, std::max(1, remaining_ms(deadline))) && ack == 1;
    }
    CloseHandle(pipe);
    if (!ok) {
        return {ActivationDelivery::error, "the running instance did not acknowledge"};
    }
    return {ActivationDelivery::delivered, "activated"};
#else
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return {ActivationDelivery::error, std::string("socket failed: ") + std::strerror(errno)};
    }
    timeval timeout = {};
    timeout.tv_sec = std::max(1, timeout_ms) / 1000;
    timeout.tv_usec = (std::max(1, timeout_ms) % 1000) * 1000;
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (identity.activation_name.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return {ActivationDelivery::error, "activation socket path is too long"};
    }
    std::memcpy(address.sun_path, identity.activation_name.c_str(),
                identity.activation_name.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const int error = errno;
        ::close(fd);
        if (error == ENOENT || error == ECONNREFUSED) {
            return {ActivationDelivery::not_running, "no running instance"};
        }
        return {ActivationDelivery::error,
                std::string("connecting to the running instance failed: ") + std::strerror(error)};
    }

    bool ok = true;
    std::size_t total = 0;
    while (total < sizeof(length)) {
        const ssize_t sent =
            ::write(fd, reinterpret_cast<const char*>(&length) + total, sizeof(length) - total);
        if (sent <= 0) {
            ok = false;
            break;
        }
        total += static_cast<std::size_t>(sent);
    }
    total = 0;
    while (ok && total < payload.size()) {
        const ssize_t sent = ::write(fd, payload.data() + total, payload.size() - total);
        if (sent <= 0) {
            ok = false;
            break;
        }
        total += static_cast<std::size_t>(sent);
    }
    char ack = 0;
    if (ok) {
        ok = ::read(fd, &ack, 1) == 1 && ack == 1;
    }
    ::close(fd);
    if (!ok) {
        return {ActivationDelivery::error, "the running instance did not acknowledge"};
    }
    return {ActivationDelivery::delivered, "activated"};
#endif
}

} // namespace avatar::instance
