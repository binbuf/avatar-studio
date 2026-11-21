#include "test_registry.h"

#include "avatar/core/frame_pacer.h"

#include <cstdint>

namespace {

using avatar::core::FramePacer;
using avatar::core::PacingDecision;
using avatar::core::PacingMode;
using avatar::core::TimestampNs;

constexpr TimestampNs kInterval = avatar::core::kDefaultFrameIntervalNs;

} // namespace

TEST_CASE(frame_pacer_selects_vsync_only_for_integer_refresh_multiples) {
    CHECK(FramePacer::select_mode(0.0, 60.0) == PacingMode::deadline);
    CHECK(FramePacer::select_mode(-1.0, 60.0) == PacingMode::deadline);
    CHECK(FramePacer::select_mode(30.0, 60.0) == PacingMode::deadline);
    CHECK(FramePacer::select_mode(60.0, 60.0) == PacingMode::vsync);
    CHECK(FramePacer::select_mode(59.94, 60.0) == PacingMode::vsync);
    CHECK(FramePacer::select_mode(90.0, 60.0) == PacingMode::deadline);
    CHECK(FramePacer::select_mode(120.0, 60.0) == PacingMode::vsync);
    CHECK(FramePacer::select_mode(144.0, 60.0) == PacingMode::deadline);
    CHECK(FramePacer::select_mode(240.0, 60.0) == PacingMode::vsync);
}

TEST_CASE(frame_pacer_computes_the_target_interval) {
    CHECK_EQ(FramePacer::interval_for_fps(60.0), TimestampNs{16'666'667});
    CHECK_EQ(FramePacer::interval_for_fps(30.0), TimestampNs{33'333'333});
    CHECK_EQ(FramePacer::interval_for_fps(0.0), kInterval);
    CHECK_EQ(FramePacer::interval_for_fps(-1.0), kInterval);
}

TEST_CASE(frame_pacer_advances_an_absolute_deadline_grid) {
    FramePacer pacer(kInterval);
    pacer.reset(0);

    const PacingDecision first = pacer.begin_frame(0);
    CHECK_EQ(first.deadline_ns, TimestampNs{0});
    CHECK_EQ(first.lateness_ns, TimestampNs{0});
    CHECK_EQ(first.dropped, 0u);
    CHECK_EQ(pacer.next_deadline_ns(), kInterval);

    const PacingDecision second = pacer.begin_frame(kInterval);
    CHECK_EQ(second.deadline_ns, kInterval);
    CHECK_EQ(second.lateness_ns, TimestampNs{0});
    CHECK_EQ(second.dropped, 0u);
    CHECK_EQ(pacer.next_deadline_ns(), kInterval * 2);

    // A little late is still the current frame, not a dropped slot.
    const PacingDecision third = pacer.begin_frame(kInterval * 2 + 1'000'000);
    CHECK_EQ(third.deadline_ns, kInterval * 2);
    CHECK_EQ(third.lateness_ns, TimestampNs{1'000'000});
    CHECK_EQ(third.dropped, 0u);
    CHECK_EQ(pacer.next_deadline_ns(), kInterval * 3);
}

TEST_CASE(frame_pacer_drops_missed_slots_and_never_replays_them) {
    FramePacer pacer(kInterval);
    pacer.reset(0);
    (void)pacer.begin_frame(0);

    // 70 ms is past the 16.7, 33.3 and 50.0 ms deadlines. Three whole slots
    // are skipped, the frame due at 66.7 ms is rendered late, and the next
    // deadline is the next future slot (66.7 + 16.7), not a catch-up burst.
    const PacingDecision decision = pacer.begin_frame(70'000'000);
    CHECK_EQ(decision.dropped, 3u);
    CHECK_EQ(decision.deadline_ns, kInterval * 4);
    CHECK_EQ(decision.lateness_ns, TimestampNs{70'000'000} - kInterval * 4);
    CHECK_EQ(pacer.next_deadline_ns(), kInterval * 5);
    CHECK_EQ(pacer.dropped_total(), 3u);
    CHECK(pacer.next_deadline_ns() > 70'000'000);

    // A large stall skips every whole slot exactly once and keeps the grid
    // anchored to the original epoch.
    const TimestampNs before_stall = pacer.next_deadline_ns();
    const PacingDecision stalled = pacer.begin_frame(before_stall + kInterval * 5 + 1);
    CHECK_EQ(stalled.dropped, 5u);
    CHECK_EQ(stalled.deadline_ns, kInterval * 10);
    CHECK_EQ(pacer.next_deadline_ns(), kInterval * 11);
    CHECK_EQ(pacer.dropped_total(), 8u);
}

TEST_CASE(frame_pacer_reports_an_early_wait_without_advancing) {
    FramePacer pacer(kInterval);
    pacer.reset(0);
    (void)pacer.begin_frame(0);

    const PacingDecision early = pacer.begin_frame(kInterval / 2);
    CHECK_EQ(early.deadline_ns, kInterval);
    CHECK_EQ(early.sleep_ns, kInterval - kInterval / 2);
    CHECK_EQ(early.lateness_ns, TimestampNs{0});
    CHECK_EQ(early.dropped, 0u);
    CHECK_EQ(pacer.next_deadline_ns(), kInterval);
    CHECK_EQ(pacer.early_frames(), 1u);

    // Waiting for the reported deadline and calling again produces the frame
    // exactly once.
    const PacingDecision on_time = pacer.begin_frame(early.deadline_ns);
    CHECK_EQ(on_time.deadline_ns, kInterval);
    CHECK_EQ(on_time.dropped, 0u);
    CHECK_EQ(pacer.next_deadline_ns(), kInterval * 2);
}

TEST_CASE(frame_pacer_reset_restarts_the_epoch_and_counters) {
    FramePacer pacer(kInterval);
    pacer.reset(1'000);
    (void)pacer.begin_frame(1'000 + kInterval * 3);
    CHECK(pacer.dropped_total() > 0);

    pacer.reset(5'000'000);
    CHECK_EQ(pacer.next_deadline_ns(), TimestampNs{5'000'000});
    CHECK_EQ(pacer.dropped_total(), 0u);
    CHECK_EQ(pacer.late_frames(), 0u);
    CHECK_EQ(pacer.early_frames(), 0u);
}
