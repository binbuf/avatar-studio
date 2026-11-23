#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "avatar/capture/video_frame.h"
#include "avatar/core/latency_histogram.h"

namespace avatar::capture {

/// Metadata for one GPU-resident camera texture slot.
///
/// The slot owns the GPU texture resources; this struct only describes what the
/// last successful upload contained. `generation` changes on every camera
/// format/shape change, so a consumer can detect that a previously selected
/// texture belongs to the old stream.
struct CameraTextureSlotInfo {
    bool valid = false;
    bool pinned = false;
    TimestampNs timestamp_ns = 0;
    TimestampNs arrival_ns = 0;
    std::uint32_t sequence = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::bgra8;
    ColorSpace color_space = ColorSpace::srgb;
    std::uint32_t generation = 0;
};

/// GPU upload seam for the camera texture ring.
///
/// Implementations upload the frame's planes without converting colour space:
/// NV12 Y/UV become single/dual-channel textures, BGRA8/RGBA8 become an RGBA
/// texture. YUV -> RGB happens in the decal shader. `upload` is called on the
/// render thread, so it must not block on the capture thread.
class ICameraTextureUploader {
public:
    virtual ~ICameraTextureUploader() = default;

    /// Allocates or reconfigures the slot's texture resources for the frame's
    /// shape and format. Called on first use and after a format change.
    [[nodiscard]] virtual bool configure(std::uint32_t slot, const VideoFrame& frame) = 0;

    /// Uploads the frame's planes into the already configured slot.
    [[nodiscard]] virtual bool upload(std::uint32_t slot, const VideoFrame& frame) = 0;

    /// Releases the slot's texture resources.
    virtual void invalidate(std::uint32_t slot) = 0;
};

struct CameraTextureRingStats {
    std::uint64_t published = 0;
    std::uint64_t dropped = 0;
    std::uint64_t rejected = 0;
    std::uint64_t wraps = 0;
    std::uint64_t format_changes = 0;
    std::uint64_t format_change_deferred = 0;
    std::uint64_t upload_failures = 0;
    std::uint64_t selections = 0;
    std::uint64_t selection_misses = 0;
    std::uint64_t tracking_selections = 0;
    std::uint64_t tracking_misses = 0;
    std::uint64_t upload_samples = 0;
    std::uint64_t upload_total_ns = 0;
    std::uint64_t upload_max_ns = 0;
    std::uint64_t upload_p50_ns = 0;
    std::uint64_t upload_p95_ns = 0;
};

/// Four-slot, timestamped camera texture ring.
///
/// The ring is a correctness mechanism, not a cache. A published frame keeps
/// its source timestamp, arrival timestamp, sequence number and format. The
/// decal selects the slot whose timestamp exactly matches the tracking result's
/// `source_timestamp_ns`, so pose and pixels always come from the same instant.
/// Selection pins the slot until the render frame releases it; the producer
/// never overwrites a pinned slot. If every slot is pinned (a slow consumer),
/// the new frame is dropped and counted rather than blocking or growing a queue.
///
/// A format or shape change invalidates the old generation. The change is
/// deferred while any slot is pinned so a texture being sampled is never torn;
/// the new-format frame is dropped and the next publish after release applies
/// the change. Consumers must compare `generation` before sampling.
///
/// Threading: one producer thread may call `publish`; one render thread calls
/// `select`/`select_for_tracking`/`release`. The pin flags are atomic. Do not
/// call `reset` while either thread is active.
class CameraTextureRing {
public:
    static constexpr std::uint32_t kDefaultCapacity = 4;
    static constexpr std::uint32_t kMaxCapacity = 8;
    static constexpr TimestampNs kDefaultMaxPairAgeNs = 150'000'000;

    explicit CameraTextureRing(ICameraTextureUploader& uploader,
                               std::uint32_t capacity = kDefaultCapacity,
                               TimestampNs max_pair_age_ns = kDefaultMaxPairAgeNs);
    ~CameraTextureRing();

    CameraTextureRing(const CameraTextureRing&) = delete;
    CameraTextureRing& operator=(const CameraTextureRing&) = delete;
    CameraTextureRing(CameraTextureRing&&) = delete;
    CameraTextureRing& operator=(CameraTextureRing&&) = delete;

    /// Uploads one camera frame and publishes its metadata.
    ///
    /// Returns the slot index, or `nullopt` when the frame is rejected, the
    /// uploader fails, a format change is deferred by a pinned slot, or every
    /// slot is pinned. A dropped frame is counted; the ring never queues.
    [[nodiscard]] std::optional<std::uint32_t> publish(const VideoFrame& frame);

    /// Selects the slot whose source timestamp exactly equals
    /// `source_timestamp_ns`. Returns `nullopt` when no slot matches, when the
    /// timestamp is in the future relative to `render_time_ns`, or when the
    /// pair is older than `max_pair_age_ns`. A miss must make the decal blank;
    /// it must never fall back to different pixels.
    [[nodiscard]] std::optional<std::uint32_t> select(TimestampNs source_timestamp_ns,
                                                      TimestampNs render_time_ns);

    /// Selects the newest slot whose timestamp is at or before `render_time_ns`
    /// and no older than `max_pair_age_ns`. This is the frame the tracker
    /// should infer from; it pins the slot until the caller releases it.
    [[nodiscard]] std::optional<std::uint32_t> select_for_tracking(TimestampNs render_time_ns);

    /// Releases one pin. Safe to call with an invalid index.
    void release(std::uint32_t slot) noexcept;

    /// Releases every pin held by the render thread.
    void release_selected() noexcept;

    /// Drops every published slot and invalidates GPU resources. Only safe when
    /// no slot is pinned and no producer is writing.
    void reset();

    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint32_t valid_count() const noexcept;
    [[nodiscard]] bool has_published() const noexcept { return published_ > 0; }
    [[nodiscard]] const CameraTextureSlotInfo* slot(std::uint32_t index) const noexcept;
    [[nodiscard]] CameraTextureRingStats stats() const noexcept;

private:
    [[nodiscard]] bool any_pinned() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> choose_free_slot() noexcept;
    void apply_format_change(const VideoFrame& frame);
    void invalidate_all();

    struct Slot {
        CameraTextureSlotInfo info{};
        bool configured = false;
        std::atomic<bool> pinned{false};
        std::atomic<bool> writing{false};
    };

    ICameraTextureUploader& uploader_;
    std::uint32_t capacity_ = kDefaultCapacity;
    TimestampNs max_pair_age_ns_ = kDefaultMaxPairAgeNs;
    std::array<Slot, kMaxCapacity> slots_{};
    std::uint32_t generation_ = 0;
    bool has_format_ = false;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    PixelFormat format_ = PixelFormat::bgra8;
    std::uint64_t published_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t rejected_ = 0;
    std::uint64_t wraps_ = 0;
    std::uint64_t format_changes_ = 0;
    std::uint64_t format_change_deferred_ = 0;
    std::uint64_t upload_failures_ = 0;
    std::uint64_t selections_ = 0;
    std::uint64_t selection_misses_ = 0;
    std::uint64_t tracking_selections_ = 0;
    std::uint64_t tracking_misses_ = 0;
    core::LatencyHistogram<512, 10'000> upload_histogram_;
};

} // namespace avatar::capture
