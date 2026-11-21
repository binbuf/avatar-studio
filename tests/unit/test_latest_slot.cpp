#include "test_registry.h"

#include "avatar/core/latest_slot.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace {

using avatar::core::LatestSlot;

struct Pose {
    std::uint32_t sequence = 0;
    std::uint32_t checksum = 0;
};

constexpr std::uint32_t kChecksumSalt = 0x9E3779B9u;
constexpr std::uint32_t kPoseCount = 200'000;

[[nodiscard]] Pose make_pose(std::uint32_t sequence) {
    return Pose{sequence, sequence ^ kChecksumSalt};
}

} // namespace

TEST_CASE(latest_slot_returns_false_before_the_first_publish) {
    LatestSlot<Pose> slot;
    Pose pose;
    CHECK(!slot.has_value());
    CHECK_EQ(slot.sequence(), std::uint64_t{0});
    CHECK(!slot.read(pose));
}

TEST_CASE(latest_slot_reads_the_newest_complete_value) {
    LatestSlot<Pose> slot;
    CHECK(slot.publish(make_pose(1)));
    CHECK(slot.has_value());

    Pose pose;
    CHECK(slot.read(pose));
    CHECK_EQ(pose.sequence, std::uint32_t{1});
    CHECK_EQ(pose.checksum, std::uint32_t{1} ^ kChecksumSalt);

    CHECK(slot.publish(make_pose(2)));
    CHECK(slot.read(pose));
    CHECK_EQ(pose.sequence, std::uint32_t{2});
    CHECK_EQ(slot.sequence(), std::uint64_t{2});
}

TEST_CASE(latest_slot_never_tears_a_value_under_concurrent_publish) {
    LatestSlot<Pose, 3> slot;

    std::atomic<bool> writer_done{false};
    std::atomic<std::uint32_t> torn_reads{0};
    std::atomic<std::uint32_t> publishes_dropped{0};
    std::uint32_t reads = 0;

    std::thread writer([&slot, &writer_done, &publishes_dropped] {
        for (std::uint32_t sequence = 0; sequence < kPoseCount; ++sequence) {
            const Pose pose = make_pose(sequence);
            while (!slot.publish(pose)) {
                publishes_dropped.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
        writer_done.store(true, std::memory_order_release);
    });

    std::thread reader([&slot, &writer_done, &torn_reads, &reads] {
        std::uint32_t local_reads = 0;
        std::uint32_t local_torn = 0;
        while (!writer_done.load(std::memory_order_acquire) || local_reads < kPoseCount) {
            Pose pose;
            if (slot.read(pose)) {
                ++local_reads;
                if (pose.checksum != (pose.sequence ^ kChecksumSalt)) {
                    ++local_torn;
                }
            } else {
                std::this_thread::yield();
            }
        }
        reads = local_reads;
        torn_reads.store(local_torn, std::memory_order_relaxed);
    });

    writer.join();
    reader.join();

    CHECK_EQ(torn_reads.load(std::memory_order_relaxed), std::uint32_t{0});
    CHECK(reads >= kPoseCount);
    CHECK(slot.sequence() >= kPoseCount);
}
