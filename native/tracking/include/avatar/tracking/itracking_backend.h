#pragma once

#include <cstdint>
#include <optional>

#include "avatar/capture/video_frame.h"
#include "avatar/tracking/tracking_result.h"

namespace avatar::tracking {

/// Generic tracker configuration. It deliberately says nothing about a vendor:
/// a backend may use the requested resolution, ignore it, or reconfigure when
/// the capture format changes.
struct TrackerConfig {
    std::uint32_t input_width = 0;  ///< 0 means "the backend chooses"
    std::uint32_t input_height = 0; ///< 0 means "the backend chooses"
    std::uint32_t target_fps = 30;
    bool allow_input_resize = true;
};

/// The app-side tracker seam (docs/design/tracking.md#contract).
///
/// The application depends on this interface, never on a tracker vendor header.
/// A real backend is a separate shared library behind the C ABI in
/// `avatar/tracker/tracker_c_api.h`; `ScriptedTracker` implements both this
/// interface and that ABI so the rig, renderer and contract tests can run
/// before inference exists.
///
/// Contract:
/// - `submit` never waits for inference and never throws. If the backend is
///   busy, the frame is dropped inside the backend, not queued by the caller.
/// - `latest` returns the newest complete result or nothing. There is no
///   history, and `source_timestamp_ns` is the input frame's timestamp.
/// - `stats` is a snapshot; it is safe to call while the backend is running.
class ITrackingBackend {
public:
    virtual ~ITrackingBackend() = default;

    virtual bool initialize(const TrackerConfig& config) = 0;
    virtual void submit(const capture::VideoFrame& frame) = 0;
    virtual std::optional<TrackingResult> latest() = 0;
    [[nodiscard]] virtual TrackerStats stats() const = 0;
    virtual void shutdown() = 0;
};

} // namespace avatar::tracking
