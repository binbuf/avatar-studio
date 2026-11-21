#include "../unit/face_rig_fixture.h"

#include <chrono>
#include <cstdio>

int main() {
    avatar::test::RigFixture f;
    // Exclude source rendering, data generation, and allocation of the fixture.
    // Each timed call includes named joins, estimation, anchors and pose filtering.
    std::array<avatar::tracking::TrackingResult, 120> inputs;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        inputs[i] =
            f.result(1'000'000'000 + i * 16'666'667, 10 * std::sin(static_cast<float>(i) * 0.03f));
    }
    bool passed = true;
    for (const bool outlier : {false, true}) {
        avatar::rig::FaceRig rig;
        rig.configure(f.profile);
        double sum = 0;
        double checksum = 0;
        constexpr int count = 1200;
        for (int i = 0; i < count + 120; ++i) {
            auto input = inputs[static_cast<std::size_t>(i) % inputs.size()];
            input.source_timestamp_ns = 1'000'000'000 + static_cast<std::uint64_t>(i) * 16'666'667;
            if (outlier) {
                input.landmarks.points[*f.map.resolve("chin")].x += 0.2f;
            }
            const auto begin = std::chrono::steady_clock::now();
            const auto output = rig.update(input, input.source_timestamp_ns);
            const auto end = std::chrono::steady_clock::now();
            if (i >= 120) {
                sum += std::chrono::duration<double, std::milli>(end - begin).count();
                checksum += output.canonical_to_image[0];
                passed = passed && output.features_valid && !output.solve_failed;
            }
        }
        const double mean = sum / count;
        std::printf("FaceRig %s: %.6f ms/update (1200 samples, budget <0.100 ms), checksum=%.6f\n",
                    outlier ? "one-outlier" : "all-inlier", mean, checksum);
        passed = passed && mean < 0.1;
    }
    return passed ? 0 : 1;
}
