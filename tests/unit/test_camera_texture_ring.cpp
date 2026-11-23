#include "face_rig_fixture.h"
#include "test_registry.h"

#include "avatar/capture/camera_texture_ring.h"
#include "avatar/capture/synthetic_face_source.h"
#include "avatar/capture/tracking_frame_gate.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
using avatar::capture::CameraTextureRing;
using avatar::capture::CameraTextureSlotInfo;
using avatar::capture::ColorSpace;
using avatar::capture::ICameraTextureUploader;
using avatar::capture::PixelFormat;
using avatar::capture::TrackingFrameGate;
using avatar::capture::VideoFrame;
using avatar::core::TimestampNs;

constexpr TimestampNs kMs = 1'000'000;
constexpr TimestampNs kStep = 16'666'667;

struct FakeUploader final : ICameraTextureUploader {
    struct Slot {
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        PixelFormat format = PixelFormat::bgra8;
        int configure_calls = 0;
        int upload_calls = 0;
        int invalidate_calls = 0;
    };

    std::array<Slot, 8> slots{};
    bool fail = false;

    bool configure(std::uint32_t slot, const VideoFrame& frame) override {
        if (fail || slot >= slots.size()) {
            return false;
        }
        slots[slot].width = frame.width;
        slots[slot].height = frame.height;
        slots[slot].format = frame.format;
        ++slots[slot].configure_calls;
        return true;
    }

    bool upload(std::uint32_t slot, const VideoFrame& frame) override {
        if (fail || slot >= slots.size()) {
            return false;
        }
        std::array<avatar::capture::PlaneLayout, avatar::capture::kMaxFramePlanes> layout{};
        if (!avatar::capture::compute_plane_layout(frame.format, frame.width, frame.height,
                                                   layout)) {
            return false;
        }
        auto& target = slots[slot].pixels;
        target.clear();
        for (std::size_t plane = 0; plane < avatar::capture::kMaxFramePlanes; ++plane) {
            if (layout[plane].size == 0) {
                continue;
            }
            const auto* source = frame.plane(plane);
            const std::size_t row_bytes =
                plane == 0
                    ? static_cast<std::size_t>(frame.width) *
                          (frame.format == PixelFormat::bgra8 || frame.format == PixelFormat::rgba8
                               ? 4u
                               : (frame.format == PixelFormat::yuy2 ? 2u : 1u))
                    : static_cast<std::size_t>(frame.width);
            for (std::uint32_t row = 0; row < frame.height / (plane == 0 ? 1u : 2u); ++row) {
                const auto* begin = source + static_cast<std::size_t>(frame.stride(plane)) * row;
                target.insert(target.end(), begin, begin + row_bytes);
            }
        }
        ++slots[slot].upload_calls;
        return true;
    }

    void invalidate(std::uint32_t slot) override {
        if (slot < slots.size()) {
            slots[slot] = Slot{};
            ++slots[slot].invalidate_calls;
        }
    }
};

struct OwnedFrame {
    std::vector<std::uint8_t> pixels;
    VideoFrame frame;
};

OwnedFrame make_bgra_frame(std::uint32_t width, std::uint32_t height, TimestampNs timestamp,
                           std::uint32_t sequence, float marker_x, float marker_y) {
    OwnedFrame owned;
    owned.pixels.assign(static_cast<std::size_t>(width) * height * 4u, 0);
    for (std::size_t i = 0; i < owned.pixels.size(); i += 4) {
        owned.pixels[i + 0] = 12;  // B
        owned.pixels[i + 1] = 12;  // G
        owned.pixels[i + 2] = 12;  // R
        owned.pixels[i + 3] = 255; // A
    }
    const int cx = static_cast<int>(std::lround(marker_x));
    const int cy = static_cast<int>(std::lround(marker_y));
    for (int y = cy - 3; y <= cy + 3; ++y) {
        if (y < 0 || y >= static_cast<int>(height)) {
            continue;
        }
        for (int x = cx - 3; x <= cx + 3; ++x) {
            if (x < 0 || x >= static_cast<int>(width)) {
                continue;
            }
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4u;
            owned.pixels[offset + 0] = 255;
            owned.pixels[offset + 1] = 255;
            owned.pixels[offset + 2] = 255;
        }
    }
    owned.frame.timestamp_ns = timestamp;
    owned.frame.arrival_ns = timestamp;
    owned.frame.sequence = sequence;
    owned.frame.width = width;
    owned.frame.height = height;
    owned.frame.format = PixelFormat::bgra8;
    owned.frame.color_space = ColorSpace::srgb;
    owned.frame.storage = avatar::capture::FrameStorage::cpu;
    owned.frame.strides[0] = width * 4u;
    owned.frame.planes[0] = owned.pixels.data();
    return owned;
}

std::array<double, 2> marker_centroid(const FakeUploader::Slot& slot) {
    double sum_x = 0, sum_y = 0;
    std::uint64_t count = 0;
    for (std::uint32_t y = 0; y < slot.height; ++y) {
        for (std::uint32_t x = 0; x < slot.width; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * slot.width + static_cast<std::size_t>(x)) * 4u;
            if (offset + 3 >= slot.pixels.size()) {
                continue;
            }
            if (slot.pixels[offset] > 200 && slot.pixels[offset + 1] > 200 &&
                slot.pixels[offset + 2] > 200) {
                sum_x += static_cast<double>(x) + 0.5;
                sum_y += static_cast<double>(y) + 0.5;
                ++count;
            }
        }
    }
    if (count == 0) {
        return {0.0, 0.0};
    }
    return {sum_x / static_cast<double>(count), sum_y / static_cast<double>(count)};
}

VideoFrame nv12_frame(std::vector<std::uint8_t>& y_plane, std::vector<std::uint8_t>& uv_plane,
                      std::uint32_t width, std::uint32_t height, TimestampNs timestamp,
                      std::uint32_t sequence) {
    y_plane.assign(static_cast<std::size_t>(width) * height, 16);
    uv_plane.assign(static_cast<std::size_t>(width) * height / 2u, 128);
    VideoFrame frame;
    frame.timestamp_ns = timestamp;
    frame.arrival_ns = timestamp;
    frame.sequence = sequence;
    frame.width = width;
    frame.height = height;
    frame.format = PixelFormat::nv12;
    frame.color_space = ColorSpace::bt601_limited;
    frame.storage = avatar::capture::FrameStorage::cpu;
    frame.strides[0] = width;
    frame.strides[1] = width;
    frame.planes[0] = y_plane.data();
    frame.planes[1] = uv_plane.data();
    return frame;
}

} // namespace

TEST_CASE(camera_texture_ring_selects_the_exact_source_timestamp) {
    FakeUploader uploader;
    CameraTextureRing ring(uploader);
    std::vector<std::uint8_t> y0, uv0, y1, uv1;
    auto first = nv12_frame(y0, uv0, 64, 48, 100 * kMs, 1);
    auto second = nv12_frame(y1, uv1, 64, 48, 200 * kMs, 2);
    CHECK(ring.publish(first).has_value());
    CHECK(ring.publish(second).has_value());

    const auto older = ring.select(100 * kMs, 200 * kMs);
    CHECK(older.has_value());
    if (older) {
        const auto* info = ring.slot(*older);
        CHECK(info != nullptr);
        if (info != nullptr) {
            CHECK_EQ(info->sequence, 1u);
            CHECK_EQ(info->timestamp_ns, 100 * kMs);
            CHECK(info->format == PixelFormat::nv12);
        }
        ring.release(*older);
    }
    // Exact pairing never falls back to different pixels.
    CHECK(!ring.select(150 * kMs, 300 * kMs).has_value());
    CHECK(!ring.select(400 * kMs, 300 * kMs).has_value());
    CHECK(!ring.select(100 * kMs, 100 * kMs + 150 * kMs + 1).has_value());

    const auto tracking = ring.select_for_tracking(220 * kMs);
    CHECK(tracking.has_value());
    if (tracking) {
        CHECK_EQ(ring.slot(*tracking)->sequence, 2u);
        ring.release(*tracking);
    }
    const auto stats = ring.stats();
    CHECK_EQ(stats.published, 2u);
    CHECK_EQ(stats.selection_misses, 3u);
}

TEST_CASE(camera_texture_ring_wraps_and_drops_for_a_slow_consumer) {
    FakeUploader uploader;
    CameraTextureRing ring(uploader);
    std::vector<std::vector<std::uint8_t>> y_planes(8), uv_planes(8);
    for (std::uint32_t i = 0; i < 8; ++i) {
        auto frame = nv12_frame(y_planes[i], uv_planes[i], 64, 48, (i + 1) * 10 * kMs, i + 1);
        CHECK(ring.publish(frame).has_value());
    }
    CHECK_EQ(ring.valid_count(), 4u);
    CHECK(ring.stats().wraps >= 2u);
    CHECK(!ring.select(10 * kMs, 100 * kMs).has_value()); // overwritten

    // Pin all four slots, then prove the producer drops instead of growing.
    std::array<std::uint32_t, 4> pinned{};
    std::size_t pinned_count = 0;
    for (std::uint32_t i = 0; i < 8 && pinned_count < 4; ++i) {
        const auto selected = ring.select((i + 1) * 10 * kMs, 100 * kMs);
        if (selected) {
            pinned[pinned_count++] = *selected;
        }
    }
    CHECK_EQ(pinned_count, 4u);
    const auto before = ring.stats();
    std::vector<std::uint8_t> y_new, uv_new;
    auto dropped = nv12_frame(y_new, uv_new, 64, 48, 90 * kMs, 9);
    CHECK(!ring.publish(dropped).has_value());
    CHECK_EQ(ring.stats().dropped, before.dropped + 1);
    CHECK_EQ(ring.valid_count(), 4u);
    for (auto index : pinned) {
        CHECK(ring.slot(index)->valid);
    }
    ring.release(pinned[0]);
    CHECK(ring.publish(dropped).has_value());
}

TEST_CASE(camera_texture_ring_defers_a_format_change_while_a_slot_is_pinned) {
    FakeUploader uploader;
    CameraTextureRing ring(uploader);
    std::vector<std::uint8_t> y0, uv0;
    auto nv12 = nv12_frame(y0, uv0, 64, 48, 100 * kMs, 1);
    CHECK(ring.publish(nv12).has_value());
    const auto old = ring.select(100 * kMs, 200 * kMs);
    CHECK(old.has_value());
    const std::uint32_t generation = ring.generation();

    auto bgra = make_bgra_frame(32, 32, 200 * kMs, 2, 16, 16);
    CHECK(!ring.publish(bgra.frame).has_value());
    CHECK_EQ(ring.stats().format_change_deferred, 1u);
    CHECK_EQ(ring.generation(), generation);
    CHECK(ring.slot(*old)->valid);

    ring.release(*old);
    CHECK(ring.publish(bgra.frame).has_value());
    CHECK_EQ(ring.stats().format_changes, 1u);
    CHECK(ring.generation() > generation);
    CHECK(!ring.select(100 * kMs, 200 * kMs).has_value());
    const auto fresh = ring.select(200 * kMs, 200 * kMs);
    CHECK(fresh.has_value());
    if (fresh) {
        CHECK(ring.slot(*fresh)->format == PixelFormat::bgra8);
        CHECK_EQ(ring.slot(*fresh)->generation, ring.generation());
        ring.release(*fresh);
    }
}

TEST_CASE(camera_texture_ring_rejects_frames_it_cannot_upload) {
    FakeUploader uploader;
    CameraTextureRing ring(uploader);
    VideoFrame empty;
    CHECK(!ring.publish(empty).has_value());
    CHECK_EQ(ring.stats().rejected, 1u);

    std::vector<std::uint8_t> y, uv;
    auto odd = nv12_frame(y, uv, 63, 48, 100 * kMs, 1);
    CHECK(!ring.publish(odd).has_value());
    auto compressed = odd;
    compressed.format = PixelFormat::mjpeg;
    CHECK(!ring.publish(compressed).has_value());

    uploader.fail = true;
    auto good = nv12_frame(y, uv, 64, 48, 100 * kMs, 1);
    CHECK(!ring.publish(good).has_value());
    CHECK_EQ(ring.stats().upload_failures, 1u);
}

TEST_CASE(tracking_frame_gate_never_queues_while_inference_is_busy) {
    TrackingFrameGate gate(150 * kMs);
    CameraTextureSlotInfo info;
    info.valid = true;
    info.sequence = 1;
    info.timestamp_ns = 100 * kMs;
    CHECK(gate.offer(0, info, 100 * kMs));
    CHECK(gate.in_flight());
    CHECK(gate.in_flight_ticket().has_value());
    if (gate.in_flight_ticket()) {
        CHECK_EQ(gate.in_flight_ticket()->sequence, 1u);
    }
    for (std::uint32_t i = 2; i <= 5; ++i) {
        info.sequence = i;
        info.timestamp_ns = i * 10 * kMs;
        CHECK(!gate.offer(0, info, 100 * kMs));
    }
    CHECK_EQ(gate.submitted(), 1u);
    CHECK_EQ(gate.dropped_busy(), 4u);
    gate.complete();
    CHECK(!gate.in_flight());

    info.sequence = 6;
    info.timestamp_ns = 120 * kMs;
    CHECK(gate.offer(0, info, 120 * kMs));
    CHECK_EQ(gate.submitted(), 2u);
    gate.complete();

    info.sequence = 7;
    info.timestamp_ns = 120 * kMs;
    CHECK(!gate.offer(0, info, 120 * kMs));
    CHECK_EQ(gate.dropped_duplicate(), 1u);
    info.timestamp_ns = 1 * kMs;
    info.sequence = 8;
    CHECK(!gate.offer(0, info, 200 * kMs));
    CHECK_EQ(gate.dropped_stale(), 1u);
}

TEST_CASE(face_rig_applies_newest_result_not_after_render_time) {
    avatar::test::RigFixture fixture;
    avatar::rig::FaceRig rig;
    rig.configure(fixture.profile);
    const auto t0 = 1'000'000'000;
    auto r0 = fixture.result(t0);
    auto r1 = fixture.result(t0 + kStep);
    auto r2 = fixture.result(t0 + 2 * kStep);

    rig.submit(r0);
    rig.submit(r1);
    rig.submit(r2);
    CHECK_EQ(rig.pending_count(), 3u);
    const auto middle = rig.sample(t0 + kStep);
    CHECK_EQ(middle.source_timestamp_ns, t0 + kStep);
    CHECK_EQ(rig.pending_count(), 1u); // r2 is still in the future
    const auto newest = rig.sample(t0 + 2 * kStep);
    CHECK_EQ(newest.source_timestamp_ns, t0 + 2 * kStep);
    CHECK_EQ(rig.pending_count(), 0u);

    // A result from the future waits; it is not applied early.
    auto future = fixture.result(t0 + 3 * kStep);
    rig.submit(future);
    const auto early = rig.sample(t0 + 2 * kStep + kStep / 2);
    CHECK_EQ(early.source_timestamp_ns, t0 + 2 * kStep);
    const auto later = rig.sample(t0 + 3 * kStep);
    CHECK_EQ(later.source_timestamp_ns, t0 + 3 * kStep);
}

TEST_CASE(timestamp_pairing_removes_fast_motion_sliding_and_naive_fails) {
    avatar::test::RigFixture fixture;
    avatar::rig::FaceRig rig;
    rig.configure(fixture.profile);
    FakeUploader uploader;
    CameraTextureRing ring(uploader, 4);

    constexpr std::uint32_t kWidth = 320;
    constexpr std::uint32_t kHeight = 180;
    constexpr int kFrames = 30;
    constexpr int kLag = 2;
    constexpr float kMetresPerFrame = 0.006f;
    const TimestampNs t0 = 1'000'000'000;

    std::vector<avatar::tracking::TrackingResult> results;
    std::vector<OwnedFrame> frames;
    results.reserve(kFrames);
    frames.reserve(kFrames);

    for (int i = 0; i < kFrames; ++i) {
        const float tx = kMetresPerFrame * static_cast<float>(i);
        results.push_back(fixture.result(t0 + static_cast<TimestampNs>(i) * kStep, 0, 0, 0, tx));

        auto config = fixture.config;
        config.path = {{0, {tx, 0, 0}, {0, 0, 0}}};
        const auto pose = avatar::capture::SyntheticFaceSource::evaluate_pose(config, 0);
        const auto marker = avatar::capture::SyntheticFaceSource::project_canonical(pose, 0, 0);
        const float marker_x = marker[0] * static_cast<float>(kWidth);
        const float marker_y = marker[1] * static_cast<float>(kHeight);
        frames.push_back(make_bgra_frame(kWidth, kHeight, t0 + static_cast<TimestampNs>(i) * kStep,
                                         static_cast<std::uint32_t>(i + 1), marker_x, marker_y));
        CHECK(ring.publish(frames.back().frame).has_value());
    }

    double paired_error = 0.0;
    double naive_error = 0.0;
    for (int j = kLag; j < kFrames; ++j) {
        rig.submit(results[static_cast<std::size_t>(j - kLag)]);
        const auto output = rig.sample(t0 + static_cast<TimestampNs>(j) * kStep);
        if (j != kFrames - 1) {
            continue;
        }
        CHECK_EQ(output.source_timestamp_ns, t0 + static_cast<TimestampNs>(j - kLag) * kStep);
        const auto predicted = avatar::test::rig_project(output.canonical_to_image, 0, 0);
        const double predicted_x = predicted[0] * static_cast<double>(kWidth);
        const double predicted_y = predicted[1] * static_cast<double>(kHeight);

        const auto paired =
            ring.select(output.source_timestamp_ns, t0 + static_cast<TimestampNs>(j) * kStep);
        CHECK(paired.has_value());
        if (paired) {
            const auto centroid = marker_centroid(uploader.slots[*paired]);
            paired_error = std::hypot(centroid[0] - predicted_x, centroid[1] - predicted_y);
            ring.release(*paired);
        }
        const auto naive = ring.select_for_tracking(t0 + static_cast<TimestampNs>(j) * kStep);
        CHECK(naive.has_value());
        if (naive) {
            const auto centroid = marker_centroid(uploader.slots[*naive]);
            naive_error = std::hypot(centroid[0] - predicted_x, centroid[1] - predicted_y);
            ring.release(*naive);
        }
    }
    std::printf("T24 sliding: paired=%.3f px naive=%.3f px\n", paired_error, naive_error);
    CHECK(paired_error < 1.0);
    CHECK(naive_error > 5.0);
    CHECK(naive_error > paired_error * 5.0);
}
