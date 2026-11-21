#include "avatar/core/latest_slot.h"
#include "avatar/core/spsc_ring.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

using avatar::core::LatestSlot;
using avatar::core::SpscRing;

constexpr std::uint64_t kIterations = 1'000'000;
constexpr std::size_t kRingCapacity = 1024;

struct Pose {
    std::uint64_t timestamp_ns = 0;
    std::uint32_t sequence = 0;
    float confidence = 0.0F;
};

[[nodiscard]] double ns_per_iteration(std::chrono::steady_clock::duration elapsed) noexcept {
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<double>(nanoseconds) / static_cast<double>(kIterations);
}

void bench_ring() {
    SpscRing<std::uint32_t, kRingCapacity> ring;
    for (std::size_t value = 0; value < kRingCapacity; ++value) {
        (void)ring.try_push(static_cast<std::uint32_t>(value));
    }

    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t iteration = 0; iteration < kIterations; ++iteration) {
        std::uint32_t value = 0;
        if (ring.try_pop(value)) {
            checksum += value;
        }
        (void)ring.try_push(static_cast<std::uint32_t>(iteration));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    std::printf("bench: spsc_ring pop+push  %.2f ns/pair (%.2f ns/op) checksum=%llu\n",
                ns_per_iteration(elapsed), ns_per_iteration(elapsed) / 2.0,
                static_cast<unsigned long long>(checksum));
}

void bench_latest_slot() {
    LatestSlot<Pose, 3> slot;
    (void)slot.publish(Pose{0, 0, 0.0F});

    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t iteration = 0; iteration < kIterations; ++iteration) {
        Pose published;
        published.timestamp_ns = iteration;
        published.sequence = static_cast<std::uint32_t>(iteration);
        published.confidence = 1.0F;
        (void)slot.publish(published);

        Pose read;
        if (slot.read(read)) {
            checksum += read.sequence;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    std::printf("bench: latest_slot publish+read %.2f ns/pair (%.2f ns/op) checksum=%llu\n",
                ns_per_iteration(elapsed), ns_per_iteration(elapsed) / 2.0,
                static_cast<unsigned long long>(checksum));
}

} // namespace

int main() {
    bench_ring();
    bench_latest_slot();
    return 0;
}
