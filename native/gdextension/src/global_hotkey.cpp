#include "global_hotkey.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace avatar::platform {
#if defined(_WIN32)
namespace {

constexpr std::uint32_t kHotkeyId = 1;
constexpr std::uint32_t kStopMessage = 0x8000U + 1U;

} // namespace
#endif

GlobalHotkey::GlobalHotkey() = default;

GlobalHotkey::~GlobalHotkey() {
    stop();
}

bool GlobalHotkey::start() {
    if (thread_.joinable()) {
        return registered_.load();
    }
#if defined(_WIN32)
    ready_.store(false);
    registered_.store(false);
    thread_ = std::thread(&GlobalHotkey::run, this);
    for (int i = 0; i < 500 && !ready_.load(); ++i) {
        Sleep(1);
    }
    return registered_.load();
#else
    std::lock_guard<std::mutex> lock(mutex_);
    detail_ = "Global hotkey is unavailable on this platform; the in-app Ctrl/Cmd+Shift+H "
              "binding works.";
    registered_.store(false);
    return false;
#endif
}

void GlobalHotkey::stop() {
    if (!thread_.joinable()) {
        return;
    }
#if defined(_WIN32)
    const std::uint32_t id = thread_id_.load();
    if (id != 0) {
        PostThreadMessageW(static_cast<DWORD>(id), kStopMessage, 0, 0);
    }
#endif
    thread_.join();
    running_.store(false);
    thread_id_.store(0);
}

bool GlobalHotkey::consume_pressed() noexcept {
    return pressed_.exchange(false);
}

std::string GlobalHotkey::detail() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return detail_;
}

#if defined(_WIN32)
void GlobalHotkey::run() {
    running_.store(true);
    thread_id_.store(static_cast<std::uint32_t>(GetCurrentThreadId()));
    const BOOL ok = RegisterHotKey(nullptr, kHotkeyId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'H');
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registered_.store(ok != FALSE);
        detail_ = ok != FALSE
                      ? "Global hotkey registered: Ctrl+Shift+H."
                      : "Global hotkey unavailable: another app may own Ctrl+Shift+H; the in-app "
                        "Ctrl/Cmd+Shift+H binding works.";
    }
    ready_.store(true);
    if (ok == FALSE) {
        running_.store(false);
        return;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_HOTKEY && message.wParam == kHotkeyId) {
            pressed_.store(true);
            continue;
        }
        if (message.message == kStopMessage) {
            UnregisterHotKey(nullptr, kHotkeyId);
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    running_.store(false);
}
#else
void GlobalHotkey::run() {}
#endif

} // namespace avatar::platform
