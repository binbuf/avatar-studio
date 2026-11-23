#pragma once

#include "avatar/instance/instance_identity.h"

#include <string>

namespace avatar::instance {

enum class LockState {
    none = 0,
    primary,
    secondary,
    error,
};

/// Per-user single-instance lock. Windows uses a named mutex in the session's
/// `Local\` namespace; macOS and Linux use an advisory file lock (`flock`) on a
/// per-user runtime file. Both are released by the OS when the process dies, so
/// a crash never requires a reboot or a manual cleanup. The diagnostic state
/// file is not authoritative: it only lets the next launch report that it
/// reclaimed a stale marker.
class InstanceLock {
public:
    InstanceLock() = default;
    ~InstanceLock();

    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    InstanceLock(InstanceLock&& other) noexcept;
    InstanceLock& operator=(InstanceLock&& other) noexcept;

    /// Acquires the lock. `primary` owns the application; `secondary` means
    /// another process is running and must be activated instead.
    LockState acquire(const InstanceIdentity& identity);
    void release() noexcept;

    [[nodiscard]] LockState state() const noexcept { return state_; }
    [[nodiscard]] bool is_primary() const noexcept { return state_ == LockState::primary; }
    [[nodiscard]] bool reclaimed_stale() const noexcept { return reclaimed_stale_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
#if defined(_WIN32)
    void* handle_ = nullptr; // HANDLE
#else
    int fd_ = -1;
#endif
    LockState state_ = LockState::none;
    bool reclaimed_stale_ = false;
    std::string last_error_;
    std::string marker_path_;
};

} // namespace avatar::instance
