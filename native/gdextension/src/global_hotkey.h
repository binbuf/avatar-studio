#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace avatar::platform {

/// Optional OS-level Ctrl/Cmd+Shift+H binding for Panic.
///
/// The in-app binding always exists. This class only reports whether the OS
/// allowed a global registration; a platform without a supported implementation
/// returns false and a sentence the UI can show. The listener never touches the
/// scene tree: it sets an atomic flag that the Live surface polls once a frame.
class GlobalHotkey {
public:
    GlobalHotkey();
    ~GlobalHotkey();

    GlobalHotkey(const GlobalHotkey&) = delete;
    GlobalHotkey& operator=(const GlobalHotkey&) = delete;

    /// Attempts registration and waits briefly for the OS answer. Idempotent.
    bool start();
    void stop();

    /// True once per OS hotkey press.
    bool consume_pressed() noexcept;

    [[nodiscard]] bool is_registered() const noexcept { return registered_.load(); }
    [[nodiscard]] std::string detail() const;

private:
    void run();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> registered_{false};
    std::atomic<bool> pressed_{false};
    std::atomic<std::uint32_t> thread_id_{0};
    mutable std::mutex mutex_;
    std::string detail_;
};

} // namespace avatar::platform
