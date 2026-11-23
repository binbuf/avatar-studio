#include "avatar/capture/camera_texture_ring.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace avatar::capture {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(Clock::time_point start) {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

bool frame_planes_valid(const VideoFrame& frame) noexcept {
    if (frame.width == 0 || frame.height == 0 || !is_supported_pixel_format(frame.format)) {
        return false;
    }
    if (frame.storage != FrameStorage::cpu) {
        return false;
    }
    std::array<PlaneLayout, kMaxFramePlanes> layout{};
    if (!compute_plane_layout(frame.format, frame.width, frame.height, layout)) {
        return false;
    }
    for (std::size_t plane = 0; plane < kMaxFramePlanes; ++plane) {
        if (layout[plane].size > 0 && frame.planes[plane] == nullptr) {
            return false;
        }
    }
    return true;
}

} // namespace

CameraTextureRing::CameraTextureRing(ICameraTextureUploader& uploader, std::uint32_t capacity,
                                     TimestampNs max_pair_age_ns)
    : uploader_(uploader), capacity_(std::clamp<std::uint32_t>(capacity, 1, kMaxCapacity)),
      max_pair_age_ns_(max_pair_age_ns) {}

CameraTextureRing::~CameraTextureRing() {
    invalidate_all();
}

std::optional<std::uint32_t> CameraTextureRing::publish(const VideoFrame& frame) {
    if (!frame_planes_valid(frame)) {
        ++rejected_;
        return std::nullopt;
    }

    const bool changed = has_format_ && (frame.format != format_ || frame.width != width_ ||
                                         frame.height != height_);
    if (changed) {
        // Never invalidate a texture the render thread is still sampling. The
        // new-format frame is dropped; the next publish after release applies
        // the change. This is the format-change equivalent of drop-oldest.
        if (any_pinned()) {
            ++format_change_deferred_;
            ++dropped_;
            return std::nullopt;
        }
        apply_format_change(frame);
    } else if (!has_format_) {
        format_ = frame.format;
        width_ = frame.width;
        height_ = frame.height;
        has_format_ = true;
        generation_ = 1;
    }

    const auto slot_index = choose_free_slot();
    if (!slot_index) {
        ++dropped_;
        return std::nullopt;
    }
    Slot& slot = slots_[*slot_index];
    // Claim the slot before touching it. A concurrent select skips a writing
    // slot; re-check the pin because select may have won the race between the
    // candidate scan and this claim.
    slot.writing.store(true, std::memory_order_release);
    if (slot.pinned.load(std::memory_order_acquire)) {
        slot.writing.store(false, std::memory_order_release);
        ++dropped_;
        return std::nullopt;
    }
    if (slot.info.valid) {
        ++wraps_;
    }

    if (!slot.configured) {
        if (!uploader_.configure(*slot_index, frame)) {
            uploader_.invalidate(*slot_index);
            slot.configured = false;
            slot.info = CameraTextureSlotInfo{};
            slot.writing.store(false, std::memory_order_release);
            ++upload_failures_;
            ++dropped_;
            return std::nullopt;
        }
        slot.configured = true;
    }

    const auto start = Clock::now();
    if (!uploader_.upload(*slot_index, frame)) {
        uploader_.invalidate(*slot_index);
        slot.configured = false;
        slot.info = CameraTextureSlotInfo{};
        slot.writing.store(false, std::memory_order_release);
        ++upload_failures_;
        ++dropped_;
        return std::nullopt;
    }
    upload_histogram_.add(elapsed_ns(start));

    slot.info.valid = true;
    slot.info.pinned = false;
    slot.pinned.store(false, std::memory_order_release);
    slot.info.timestamp_ns = frame.timestamp_ns;
    slot.info.arrival_ns = frame.arrival_ns;
    slot.info.sequence = frame.sequence;
    slot.info.width = frame.width;
    slot.info.height = frame.height;
    slot.info.format = frame.format;
    slot.info.color_space = frame.color_space;
    slot.info.generation = generation_;
    slot.writing.store(false, std::memory_order_release);
    ++published_;
    return slot_index;
}

std::optional<std::uint32_t> CameraTextureRing::select(TimestampNs source_timestamp_ns,
                                                       TimestampNs render_time_ns) {
    ++selections_;
    if (source_timestamp_ns > render_time_ns ||
        render_time_ns - source_timestamp_ns > max_pair_age_ns_) {
        ++selection_misses_;
        return std::nullopt;
    }
    std::optional<std::uint32_t> best;
    for (std::uint32_t index = 0; index < capacity_; ++index) {
        Slot& slot = slots_[index];
        if (!slot.info.valid || slot.writing.load(std::memory_order_acquire) ||
            slot.info.timestamp_ns != source_timestamp_ns) {
            continue;
        }
        if (!best || slot.info.sequence > slots_[*best].info.sequence) {
            best = index;
        }
    }
    if (!best) {
        ++selection_misses_;
        return std::nullopt;
    }
    Slot& slot = slots_[*best];
    slot.pinned.store(true, std::memory_order_release);
    slot.info.pinned = true;
    return best;
}

std::optional<std::uint32_t> CameraTextureRing::select_for_tracking(TimestampNs render_time_ns) {
    ++tracking_selections_;
    std::optional<std::uint32_t> best;
    TimestampNs best_timestamp = 0;
    for (std::uint32_t index = 0; index < capacity_; ++index) {
        const Slot& slot = slots_[index];
        if (!slot.info.valid || slot.writing.load(std::memory_order_acquire) ||
            slot.info.timestamp_ns > render_time_ns) {
            continue;
        }
        if (render_time_ns - slot.info.timestamp_ns > max_pair_age_ns_) {
            continue;
        }
        if (!best || slot.info.timestamp_ns > best_timestamp) {
            best = index;
            best_timestamp = slot.info.timestamp_ns;
        }
    }
    if (!best) {
        ++tracking_misses_;
        return std::nullopt;
    }
    Slot& slot = slots_[*best];
    slot.pinned.store(true, std::memory_order_release);
    slot.info.pinned = true;
    return best;
}

void CameraTextureRing::release(std::uint32_t slot_index) noexcept {
    if (slot_index >= capacity_) {
        return;
    }
    slots_[slot_index].pinned.store(false, std::memory_order_release);
    slots_[slot_index].info.pinned = false;
}

void CameraTextureRing::release_selected() noexcept {
    for (Slot& slot : slots_) {
        slot.pinned.store(false, std::memory_order_release);
        slot.info.pinned = false;
    }
}

void CameraTextureRing::reset() {
    invalidate_all();
    has_format_ = false;
    width_ = 0;
    height_ = 0;
    format_ = PixelFormat::bgra8;
    ++generation_;
}

std::uint32_t CameraTextureRing::valid_count() const noexcept {
    std::uint32_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.info.valid) {
            ++count;
        }
    }
    return count;
}

const CameraTextureSlotInfo* CameraTextureRing::slot(std::uint32_t index) const noexcept {
    return index < capacity_ ? &slots_[index].info : nullptr;
}

CameraTextureRingStats CameraTextureRing::stats() const noexcept {
    CameraTextureRingStats result;
    result.published = published_;
    result.dropped = dropped_;
    result.rejected = rejected_;
    result.wraps = wraps_;
    result.format_changes = format_changes_;
    result.format_change_deferred = format_change_deferred_;
    result.upload_failures = upload_failures_;
    result.selections = selections_;
    result.selection_misses = selection_misses_;
    result.tracking_selections = tracking_selections_;
    result.tracking_misses = tracking_misses_;
    result.upload_samples = upload_histogram_.samples();
    result.upload_total_ns = upload_histogram_.total_ns();
    result.upload_max_ns = upload_histogram_.max_ns();
    result.upload_p50_ns = upload_histogram_.p50_ns();
    result.upload_p95_ns = upload_histogram_.p95_ns();
    return result;
}

bool CameraTextureRing::any_pinned() const noexcept {
    for (const Slot& slot : slots_) {
        if (slot.pinned.load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

std::optional<std::uint32_t> CameraTextureRing::choose_free_slot() noexcept {
    for (std::uint32_t index = 0; index < capacity_; ++index) {
        if (!slots_[index].info.valid && !slots_[index].writing.load(std::memory_order_acquire)) {
            return index;
        }
    }
    std::optional<std::uint32_t> oldest;
    TimestampNs oldest_timestamp = std::numeric_limits<TimestampNs>::max();
    for (std::uint32_t index = 0; index < capacity_; ++index) {
        const Slot& slot = slots_[index];
        if (slot.pinned.load(std::memory_order_acquire) ||
            slot.writing.load(std::memory_order_acquire)) {
            continue;
        }
        if (!oldest || slot.info.timestamp_ns < oldest_timestamp) {
            oldest = index;
            oldest_timestamp = slot.info.timestamp_ns;
        }
    }
    return oldest;
}

void CameraTextureRing::apply_format_change(const VideoFrame& frame) {
    invalidate_all();
    format_ = frame.format;
    width_ = frame.width;
    height_ = frame.height;
    has_format_ = true;
    ++generation_;
    ++format_changes_;
}

void CameraTextureRing::invalidate_all() {
    for (std::uint32_t index = 0; index < capacity_; ++index) {
        Slot& slot = slots_[index];
        if (slot.configured) {
            uploader_.invalidate(index);
        }
        slot.configured = false;
        slot.pinned.store(false, std::memory_order_release);
        slot.writing.store(false, std::memory_order_release);
        slot.info = CameraTextureSlotInfo{};
    }
}

} // namespace avatar::capture
