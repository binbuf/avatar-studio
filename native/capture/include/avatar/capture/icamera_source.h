#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "avatar/capture/face_ground_truth.h"
#include "avatar/capture/frame_ring.h"
#include "avatar/capture/video_frame.h"
#include "avatar/core/status.h"

namespace avatar::capture {

/// Static description of a source, used for diagnostics and test assertions.
struct SourceInfo {
    std::string_view name{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::bgra8;
    bool has_alpha = false;
    bool synthetic = false;
    bool ground_truth = false;
};

/// A camera-like frame producer.
///
/// Ownership contract (the reason this header exists):
///
/// 1. **The producer owns the ring slots.** `ring()` returns a FrameRing whose
///    storage is allocated once. The producer writes pixels directly into a
///    reserved slot; it never allocates per frame and never copies a frame it
///    has published.
/// 2. **Consumers pin and release.** A consumer calls
///    `ring().try_acquire_latest()` and holds the returned FrameLease while it
///    reads. The lease pins the slot; the producer will not overwrite a pinned
///    slot. `release()` (or the lease destructor) unpins it.
/// 3. **Frames are immutable once published.** A published VideoFrame's metadata
///    and bytes are not modified until the producer reuses the slot, which it
///    only does after the pin count reaches zero and the slot is no longer the
///    newest.
/// 4. **No hidden copies.** `VideoFrame::planes` point into the slot storage.
///    Copying the struct copies metadata and pointers; a pixel copy is always an
///    explicit operation performed by a consumer that needs its own storage.
/// 5. **A VideoFrame value is only valid while its FrameLease is alive.** Do not
///    stash the view beyond the lease, and do not keep a lease while blocking on
///    something slow: a producer with every slot pinned drops frames rather than
///    waiting.
///
/// `produce()` is called by the capture thread (or a test driver). It publishes
/// at most one frame and returns false when no frame is due, when the clock has
/// not advanced, or when every slot is pinned. It never blocks.
class ICameraSource {
public:
    virtual ~ICameraSource() = default;

    /// Prepares the source and resets its ring. Idempotent; safe to call again
    /// after stop(). Returns an error for a source that cannot be opened.
    [[nodiscard]] virtual core::Status start() = 0;

    /// Stops production. Frames already published remain readable until reset.
    virtual void stop() = 0;

    /// The ring this source produces into. Valid after a successful start().
    [[nodiscard]] virtual FrameRing& ring() noexcept = 0;

    /// Publishes the next frame if one is due. Never blocks.
    [[nodiscard]] virtual bool produce() = 0;

    [[nodiscard]] virtual SourceInfo info() const noexcept = 0;

    /// Ground truth for the most recently published frame, when the source has
    /// it (synthetic face and replay sources). The default is "no ground truth".
    [[nodiscard]] virtual std::optional<FaceGroundTruth> latest_ground_truth() const {
        return std::nullopt;
    }
};

} // namespace avatar::capture
