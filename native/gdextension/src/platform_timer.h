#pragma once

namespace avatar::platform {

/// Holds 1 ms Windows timer resolution while the runtime is alive, including
/// headless runs where the display backend does not request it. Other
/// platforms need no timer-resolution override.
class FrameTimerResolution {
public:
    FrameTimerResolution();
    ~FrameTimerResolution();
    FrameTimerResolution(const FrameTimerResolution&) = delete;
    FrameTimerResolution& operator=(const FrameTimerResolution&) = delete;

private:
#if defined(_WIN32)
    bool acquired_ = false;
#endif
};

} // namespace avatar::platform
