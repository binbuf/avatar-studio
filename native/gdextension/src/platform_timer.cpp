#include "platform_timer.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <timeapi.h>
#endif

namespace avatar::platform {

FrameTimerResolution::FrameTimerResolution() {
#if defined(_WIN32)
    // Without this, deadline sleeps can round up to the default ~15.6 ms
    // timer tick, alternating short frames with ~31 ms intervals at 60 Hz.
    acquired_ = timeBeginPeriod(1) == TIMERR_NOERROR;
#endif
}

FrameTimerResolution::~FrameTimerResolution() {
#if defined(_WIN32)
    if (acquired_) {
        (void)timeEndPeriod(1);
    }
#endif
}

} // namespace avatar::platform
