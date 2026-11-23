#include "avatar/instance/instance_lock.h"

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
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace avatar::instance {
namespace {

#if defined(_WIN32)
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

bool marker_exists(const std::string& path) {
    const std::wstring wide = widen(path);
    if (wide.empty()) {
        return false;
    }
    return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void write_marker(const std::string& path, std::uint32_t pid) {
    const std::wstring wide = widen(path);
    if (wide.empty()) {
        return;
    }
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const std::string text = "pid=" + std::to_string(pid) + "\r\n";
    DWORD written = 0;
    (void)WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

void remove_marker(const std::string& path) {
    const std::wstring wide = widen(path);
    if (!wide.empty()) {
        (void)DeleteFileW(wide.c_str());
    }
}
#else
std::string errno_text(int error) {
    return std::string(std::strerror(error));
}
#endif

} // namespace

InstanceLock::~InstanceLock() {
    release();
}

InstanceLock::InstanceLock(InstanceLock&& other) noexcept {
    *this = std::move(other);
}

InstanceLock& InstanceLock::operator=(InstanceLock&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
#if defined(_WIN32)
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    state_ = other.state_;
    reclaimed_stale_ = other.reclaimed_stale_;
    last_error_ = std::move(other.last_error_);
    marker_path_ = std::move(other.marker_path_);
    other.state_ = LockState::none;
    other.reclaimed_stale_ = false;
    other.last_error_.clear();
    other.marker_path_.clear();
    return *this;
}

LockState InstanceLock::acquire(const InstanceIdentity& identity) {
    release();
    last_error_.clear();
    reclaimed_stale_ = false;

#if defined(_WIN32)
    const std::wstring name = widen(identity.lock_name);
    if (name.empty()) {
        state_ = LockState::error;
        last_error_ = "empty lock name";
        return state_;
    }
    // A marker left behind means the previous owner died without a clean
    // release. The mutex is authoritative: if it exists, the owner is alive.
    const bool stale_marker = marker_exists(identity.state_path);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
    if (mutex == nullptr) {
        state_ = LockState::error;
        last_error_ = "CreateMutex failed: " + std::to_string(GetLastError());
        return state_;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        state_ = LockState::secondary;
        return state_;
    }
    handle_ = mutex;
    state_ = LockState::primary;
    reclaimed_stale_ = stale_marker;
    marker_path_ = identity.state_path;
    write_marker(identity.state_path, identity.pid);
    return state_;
#else
    const int fd = ::open(identity.lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        state_ = LockState::error;
        last_error_ = "open lock file failed: " + errno_text(errno);
        return state_;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        ::close(fd);
        if (error == EWOULDBLOCK) {
            state_ = LockState::secondary;
            return state_;
        }
        state_ = LockState::error;
        last_error_ = "flock failed: " + errno_text(error);
        return state_;
    }
    // We hold the lock, so any pid left in the file belongs to a dead owner.
    std::string previous;
    char buffer[128] = {};
    const ssize_t read = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
    if (read > 0) {
        previous.assign(buffer, static_cast<std::size_t>(read));
    }
    reclaimed_stale_ = previous.find("pid=") != std::string::npos;
    (void)::ftruncate(fd, 0);
    (void)::lseek(fd, 0, SEEK_SET);
    const std::string text = "pid=" + std::to_string(identity.pid) + "\n";
    const ssize_t written = ::write(fd, text.data(), text.size());
    (void)written;
    fd_ = fd;
    state_ = LockState::primary;
    return state_;
#endif
}

void InstanceLock::release() noexcept {
#if defined(_WIN32)
    if (state_ == LockState::primary && !marker_path_.empty()) {
        remove_marker(marker_path_);
    }
    if (handle_ != nullptr) {
        // The marker is diagnostic only; deleting it before closing the mutex
        // keeps a clean exit distinguishable from a crash.
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    marker_path_.clear();
#else
    if (fd_ >= 0) {
        // Truncate rather than unlink: unlinking while another process may be
        // opening the same path creates a two-owner race. An empty file plus a
        // released flock means "clean exit".
        (void)::ftruncate(fd_, 0);
        (void)::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
    }
#endif
    state_ = LockState::none;
    reclaimed_stale_ = false;
}

} // namespace avatar::instance
