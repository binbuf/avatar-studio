#include "avatar/capture/frame_ring.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace avatar::capture {

FrameLease::FrameLease(FrameLease&& other) noexcept
    : ring_(other.ring_), slot_(other.slot_), frame_(other.frame_) {
    other.ring_ = nullptr;
    other.slot_ = 0;
}

FrameLease& FrameLease::operator=(FrameLease&& other) noexcept {
    if (this != &other) {
        release();
        ring_ = other.ring_;
        slot_ = other.slot_;
        frame_ = other.frame_;
        other.ring_ = nullptr;
        other.slot_ = 0;
    }
    return *this;
}

FrameLease::~FrameLease() {
    release();
}

void FrameLease::release() noexcept {
    if (ring_ != nullptr) {
        ring_->release_slot(slot_);
        ring_ = nullptr;
        slot_ = 0;
        frame_ = VideoFrame{};
    }
}

FrameWrite::FrameWrite(FrameWrite&& other) noexcept
    : ring_(other.ring_),
      frame_(other.frame_),
      slot_(other.slot_),
      active_(other.active_),
      was_published_(other.was_published_) {
    other.ring_ = nullptr;
    other.frame_ = nullptr;
    other.slot_ = 0;
    other.active_ = false;
    other.was_published_ = false;
}

FrameWrite& FrameWrite::operator=(FrameWrite&& other) noexcept {
    if (this != &other) {
        if (active_ && ring_ != nullptr) {
            ring_->abandon(*this);
        }
        ring_ = other.ring_;
        frame_ = other.frame_;
        slot_ = other.slot_;
        active_ = other.active_;
        was_published_ = other.was_published_;
        other.ring_ = nullptr;
        other.frame_ = nullptr;
        other.slot_ = 0;
        other.active_ = false;
        other.was_published_ = false;
    }
    return *this;
}

FrameWrite::~FrameWrite() {
    if (active_ && ring_ != nullptr) {
        ring_->abandon(*this);
    }
}

std::uint8_t* FrameWrite::plane(std::size_t index) noexcept {
    if (frame_ == nullptr || index >= kMaxFramePlanes) {
        return nullptr;
    }
    return const_cast<std::uint8_t*>(frame_->planes[index]);
}

std::uint32_t FrameWrite::stride(std::size_t index) const noexcept {
    if (frame_ == nullptr || index >= kMaxFramePlanes) {
        return 0;
    }
    return frame_->strides[index];
}

FrameRing::FrameRing(std::uint32_t width,
                     std::uint32_t height,
                     PixelFormat format,
                     std::uint32_t capacity)
    : width_(width),
      height_(height),
      format_(format),
      capacity_(std::max<std::uint32_t>(capacity, 1)),
      slots_(std::max<std::uint32_t>(capacity, 1)) {
    if (!compute_plane_layout(format, width, height, plane_layouts_)) {
        // An invalid configuration leaves the ring empty. Callers validate the
        // format before construction; begin_write returns nullopt afterwards.
        capacity_ = 0;
        slots_.clear();
        return;
    }

    for (Slot& slot : slots_) {
        std::size_t total = 0;
        for (const PlaneLayout& layout : plane_layouts_) {
            total += layout.size;
        }
        slot.storage.assign(total, 0);
        std::size_t offset = 0;
        for (std::size_t plane = 0; plane < kMaxFramePlanes; ++plane) {
            slot.frame.width = width_;
            slot.frame.height = height_;
            slot.frame.format = format_;
            slot.frame.color_space = (format_ == PixelFormat::nv12 || format_ == PixelFormat::yuy2)
                                         ? ColorSpace::bt709_limited
                                         : ColorSpace::srgb;
            slot.frame.storage = FrameStorage::cpu;
            slot.frame.strides[plane] = plane_layouts_[plane].stride;
            slot.frame.planes[plane] = plane_layouts_[plane].size > 0
                                           ? slot.storage.data() + offset
                                           : nullptr;
            offset += plane_layouts_[plane].size;
        }
    }
}

FrameRing::~FrameRing() = default;

bool FrameRing::is_reusable(std::uint32_t index) const noexcept {
    if (index >= slots_.size()) {
        return false;
    }
    const Slot& slot = slots_[index];
    const SlotState state = slot.state.load(std::memory_order_acquire);
    if (state == SlotState::unpublished) {
        return true;
    }
    if (state != SlotState::published) {
        return false;
    }
    if (index == latest_index_.load(std::memory_order_acquire)) {
        return false;
    }
    return slot.pins.load(std::memory_order_acquire) == 0;
}

std::optional<FrameWrite> FrameRing::begin_write() noexcept {
    if (capacity_ == 0) {
        return std::nullopt;
    }

    // Prefer a slot that has never been published; otherwise the oldest
    // published slot that is not the current latest and is not pinned.
    std::uint32_t best = kNoSlot;
    std::uint32_t best_sequence = std::numeric_limits<std::uint32_t>::max();
    bool best_unpublished = false;
    for (std::uint32_t index = 0; index < capacity_; ++index) {
        Slot& slot = slots_[index];
        const SlotState state = slot.state.load(std::memory_order_acquire);
        if (state == SlotState::unpublished) {
            best = index;
            best_unpublished = true;
            break;
        }
        if (state != SlotState::published) {
            continue;
        }
        if (index == latest_index_.load(std::memory_order_acquire)) {
            continue;
        }
        if (slot.pins.load(std::memory_order_acquire) != 0) {
            continue;
        }
        const std::uint32_t sequence = slot.published_sequence.load(std::memory_order_acquire);
        if (best == kNoSlot || sequence < best_sequence) {
            best = index;
            best_sequence = sequence;
        }
    }

    if (best == kNoSlot) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    Slot& slot = slots_[best];
    SlotState expected = best_unpublished ? SlotState::unpublished : SlotState::published;
    if (!slot.state.compare_exchange_strong(expected, SlotState::writing,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        // Another producer (or a stale view) changed the slot; retry once by
        // simply reporting no slot this frame. Single-producer callers never
        // take this path.
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    if (!best_unpublished && slot.pins.load(std::memory_order_acquire) != 0) {
        // A consumer pinned this slot between the scan and the CAS. Put it back
        // and let the next produce() pick another slot.
        slot.state.store(SlotState::published, std::memory_order_release);
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    FrameWrite write;
    write.ring_ = this;
    write.frame_ = &slot.frame;
    write.slot_ = best;
    write.active_ = true;
    write.was_published_ = !best_unpublished;
    return write;
}

bool FrameRing::publish(FrameWrite& write,
                        TimestampNs timestamp_ns,
                        TimestampNs arrival_ns,
                        bool mirrored) noexcept {
    if (write.ring_ != this || !write.active_ || write.slot_ >= capacity_) {
        return false;
    }
    Slot& slot = slots_[write.slot_];
    const std::uint32_t sequence = sequence_counter_.fetch_add(1, std::memory_order_relaxed) + 1;

    slot.frame.timestamp_ns = timestamp_ns;
    slot.frame.arrival_ns = arrival_ns;
    slot.frame.sequence = sequence;
    slot.frame.mirrored = mirrored;

    slot.published_ns.store(timestamp_ns, std::memory_order_relaxed);
    slot.published_sequence.store(sequence, std::memory_order_relaxed);
    slot.state.store(SlotState::published, std::memory_order_release);
    latest_index_.store(write.slot_, std::memory_order_release);
    published_count_.fetch_add(1, std::memory_order_relaxed);

    write.active_ = false;
    write.ring_ = nullptr;
    write.frame_ = nullptr;
    return true;
}

void FrameRing::abandon(FrameWrite& write) noexcept {
    if (write.ring_ != this || !write.active_ || write.slot_ >= capacity_) {
        return;
    }
    Slot& slot = slots_[write.slot_];
    slot.state.store(write.was_published_ ? SlotState::published : SlotState::unpublished,
                     std::memory_order_release);
    write.active_ = false;
    write.ring_ = nullptr;
    write.frame_ = nullptr;
}

std::optional<FrameLease> FrameRing::try_acquire_latest() noexcept {
    if (capacity_ == 0) {
        return std::nullopt;
    }

    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t index = latest_index_.load(std::memory_order_acquire);
        if (index >= capacity_) {
            return std::nullopt;
        }
        Slot& slot = slots_[index];
        if (slot.state.load(std::memory_order_acquire) != SlotState::published) {
            continue;
        }
        slot.pins.fetch_add(1, std::memory_order_acq_rel);
        if (slot.state.load(std::memory_order_acquire) != SlotState::published) {
            slot.pins.fetch_sub(1, std::memory_order_release);
            continue;
        }

        FrameLease lease;
        lease.ring_ = this;
        lease.slot_ = index;
        lease.frame_ = slot.frame;
        return lease;
    }
    return std::nullopt;
}

void FrameRing::release_slot(std::uint32_t index) noexcept {
    if (index < slots_.size()) {
        slots_[index].pins.fetch_sub(1, std::memory_order_release);
    }
}

void FrameRing::reset() noexcept {
    for (Slot& slot : slots_) {
        slot.pins.store(0, std::memory_order_relaxed);
        slot.state.store(SlotState::unpublished, std::memory_order_relaxed);
        slot.published_ns.store(0, std::memory_order_relaxed);
        slot.published_sequence.store(0, std::memory_order_relaxed);
    }
    latest_index_.store(kNoSlot, std::memory_order_release);
    sequence_counter_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    published_count_.store(0, std::memory_order_relaxed);
}

TimestampNs FrameRing::latest_timestamp() const noexcept {
    const std::uint32_t index = latest_index_.load(std::memory_order_acquire);
    if (index >= slots_.size()) {
        return 0;
    }
    return slots_[index].published_ns.load(std::memory_order_relaxed);
}

std::uint32_t FrameRing::latest_sequence() const noexcept {
    const std::uint32_t index = latest_index_.load(std::memory_order_acquire);
    if (index >= slots_.size()) {
        return 0;
    }
    return slots_[index].published_sequence.load(std::memory_order_relaxed);
}

} // namespace avatar::capture
