#include "test_registry.h"

#include "avatar/capture/frame_ring.h"
#include "avatar/capture/video_frame.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {

using avatar::capture::frame_content_hash;
using avatar::capture::FrameRing;
using avatar::capture::PixelFormat;
using avatar::capture::VideoFrame;

void fill_plane(avatar::capture::FrameWrite& write, std::uint8_t seed) {
    std::uint8_t* plane = write.plane(0);
    const std::size_t size = static_cast<std::size_t>(write.stride(0)) * write.frame().height;
    for (std::size_t index = 0; index < size; ++index) {
        plane[index] = static_cast<std::uint8_t>((index + seed) & 0xFFu);
    }
}

} // namespace

TEST_CASE(frame_ring_publishes_and_pins_without_copying) {
    FrameRing ring(64, 48, PixelFormat::bgra8, 4);
    CHECK(ring.capacity() == 4);
    CHECK(!ring.has_published());
    CHECK(!ring.try_acquire_latest().has_value());

    auto write = ring.begin_write();
    CHECK(write.has_value());
    CHECK(write->active());
    CHECK(write->frame().width == 64);
    CHECK(write->frame().height == 48);
    CHECK(write->frame().format == PixelFormat::bgra8);
    std::uint8_t* pixels = write->plane(0);
    CHECK(pixels != nullptr);
    fill_plane(*write, 7);

    CHECK(ring.publish(*write, 100, 90, false));
    CHECK(!write->active());
    CHECK(ring.has_published());
    CHECK(ring.latest_timestamp() == 100);
    CHECK(ring.latest_sequence() == 1);
    CHECK(ring.published_count() == 1);

    auto lease = ring.try_acquire_latest();
    CHECK(lease.has_value());
    CHECK(lease->frame().timestamp_ns == 100);
    CHECK(lease->frame().arrival_ns == 90);
    CHECK(lease->frame().sequence == 1);
    CHECK(lease->frame().width == 64);
    CHECK(lease->frame().height == 48);
    // The lease points at the slot storage: no hidden copy.
    CHECK(lease->frame().plane(0) == pixels);
    CHECK(lease->frame().plane(0)[5] == static_cast<std::uint8_t>((5 + 7) & 0xFFu));
    lease->release();
    CHECK(!lease->valid());
}

TEST_CASE(frame_ring_does_not_overwrite_a_pinned_slot) {
    FrameRing ring(32, 32, PixelFormat::bgra8, 2);

    auto first = ring.begin_write();
    CHECK(first.has_value());
    fill_plane(*first, 1);
    CHECK(ring.publish(*first, 10, 10, false));
    auto first_lease = ring.try_acquire_latest();
    CHECK(first_lease.has_value());
    const std::uint8_t* first_pixels = first_lease->frame().plane(0);

    auto second = ring.begin_write();
    CHECK(second.has_value());
    fill_plane(*second, 2);
    CHECK(ring.publish(*second, 20, 20, false));
    auto second_lease = ring.try_acquire_latest();
    CHECK(second_lease.has_value());

    // Both slots are pinned and one is the latest, so the producer must drop.
    const std::uint64_t dropped_before = ring.dropped();
    auto blocked = ring.begin_write();
    CHECK(!blocked.has_value());
    CHECK(ring.dropped() == dropped_before + 1);

    // The held frame is still the original content.
    CHECK(first_lease->frame().plane(0) == first_pixels);
    CHECK(first_lease->frame().plane(0)[0] == static_cast<std::uint8_t>(1));

    // Releasing a pin makes a slot reusable again.
    first_lease->release();
    auto third = ring.begin_write();
    CHECK(third.has_value());
}

TEST_CASE(frame_ring_reset_clears_publication_state) {
    FrameRing ring(16, 16, PixelFormat::bgra8, 4);
    auto write = ring.begin_write();
    CHECK(write.has_value());
    CHECK(ring.publish(*write, 5, 5, false));
    CHECK(ring.has_published());

    ring.reset();
    CHECK(!ring.has_published());
    CHECK(!ring.try_acquire_latest().has_value());
    CHECK(ring.latest_timestamp() == 0);
    CHECK(ring.published_count() == 0);

    auto after = ring.begin_write();
    CHECK(after.has_value());
    CHECK(ring.publish(*after, 6, 6, false));
    CHECK(ring.latest_sequence() == 1);
}

TEST_CASE(frame_ring_rejects_an_unsupported_layout) {
    FrameRing odd_nv12(63, 47, PixelFormat::nv12);
    CHECK(odd_nv12.capacity() == 0);
    CHECK(!odd_nv12.begin_write().has_value());
    CHECK(!odd_nv12.try_acquire_latest().has_value());

    std::array<avatar::capture::PlaneLayout, avatar::capture::kMaxFramePlanes> layout{};
    CHECK(!avatar::capture::compute_plane_layout(PixelFormat::nv12, 63, 48, layout));
    CHECK(avatar::capture::compute_plane_layout(PixelFormat::nv12, 64, 48, layout));
    CHECK(layout[0].stride == 64);
    CHECK(layout[0].size == 64u * 48u);
    CHECK(layout[1].stride == 64);
    CHECK(layout[1].size == 64u * 24u);
    CHECK(avatar::capture::compute_plane_layout(PixelFormat::bgra8, 64, 48, layout));
    CHECK(layout[0].stride == 64u * 4u);
}

TEST_CASE(frame_content_hash_is_content_addressed) {
    FrameRing first(16, 16, PixelFormat::bgra8, 4);
    FrameRing second(16, 16, PixelFormat::bgra8, 4);

    auto a = first.begin_write();
    CHECK(a.has_value());
    fill_plane(*a, 3);
    CHECK(first.publish(*a, 100, 100, false));
    auto a_lease = first.try_acquire_latest();
    CHECK(a_lease.has_value());
    const std::uint64_t a_hash = frame_content_hash(a_lease->frame());

    auto b = second.begin_write();
    CHECK(b.has_value());
    fill_plane(*b, 3);
    CHECK(second.publish(*b, 900, 900, false));
    auto b_lease = second.try_acquire_latest();
    CHECK(b_lease.has_value());
    // Same pixels, different timestamp and sequence: the hash is equal.
    CHECK(frame_content_hash(b_lease->frame()) == a_hash);

    auto c = first.begin_write();
    CHECK(c.has_value());
    fill_plane(*c, 4);
    CHECK(first.publish(*c, 200, 200, false));
    auto c_lease = first.try_acquire_latest();
    CHECK(c_lease.has_value());
    CHECK(frame_content_hash(c_lease->frame()) != a_hash);
}

TEST_CASE(frame_ring_abandon_returns_the_slot) {
    FrameRing ring(8, 8, PixelFormat::bgra8, 1);
    {
        auto write = ring.begin_write();
        CHECK(write.has_value());
        write->plane(0)[0] = 42;
    }
    CHECK(!ring.has_published());
    auto again = ring.begin_write();
    CHECK(again.has_value());
}
