#include "test_registry.h"

#include "avatar/core/latency_histogram.h"
#include "avatar/tracking/tracking_result.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

using avatar::core::LatencyHistogram;

} // namespace

TEST_CASE(latency_histogram_is_empty_and_resettable) {
    LatencyHistogram<> histogram;
    CHECK(histogram.empty());
    CHECK(histogram.samples() == 0);
    CHECK(histogram.p50_ns() == 0);
    CHECK(histogram.p95_ns() == 0);
    CHECK(histogram.mean_ns() == 0.0);

    histogram.add(1'000);
    histogram.add(2'000);
    CHECK(histogram.samples() == 2);
    CHECK(histogram.min_ns() == 1'000);
    CHECK(histogram.max_ns() == 2'000);

    histogram.reset();
    CHECK(histogram.empty());
    CHECK(histogram.max_ns() == 0);
}

TEST_CASE(latency_histogram_percentiles_track_the_samples) {
    LatencyHistogram<> histogram;
    for (std::uint64_t milliseconds = 1; milliseconds <= 100; ++milliseconds) {
        histogram.add(milliseconds * 1'000'000);
    }

    CHECK(histogram.samples() == 100);
    CHECK(histogram.min_ns() == 1'000'000);
    CHECK(histogram.max_ns() == 100'000'000);
    // 100 microsecond buckets: a percentile is the bucket midpoint.
    CHECK(histogram.p50_ns() >= 49'900'000);
    CHECK(histogram.p50_ns() <= 50'100'000);
    CHECK(histogram.p95_ns() >= 94'900'000);
    CHECK(histogram.p95_ns() <= 95'100'000);
    CHECK(histogram.p99_ns() >= 98'900'000);
    CHECK(histogram.p99_ns() <= 99'100'000);
    CHECK(histogram.percentile_ns(0) == 1'000'000);
    CHECK(histogram.percentile_ns(100) == 100'000'000);
    CHECK(histogram.mean_ns() > 50'000'000.0);
    CHECK(histogram.mean_ns() < 51'000'000.0);
}

TEST_CASE(latency_histogram_has_a_saturating_overflow_bucket) {
    LatencyHistogram<> histogram;
    histogram.add(500'000'000); // 500 ms, beyond the 102.4 ms window
    CHECK(histogram.samples() == 1);
    CHECK(histogram.max_ns() == 500'000'000);
    CHECK(histogram.p50_ns() == 500'000'000);
    CHECK(histogram.bucket_count(LatencyHistogram<>::kBucketCount - 1) == 1);
}

TEST_CASE(latency_histogram_bucket_count_saturates_without_losing_the_sample) {
    LatencyHistogram<4, 100, std::uint8_t> histogram;
    for (int index = 0; index < 300; ++index) {
        histogram.add(0);
    }
    CHECK(histogram.samples() == 300);
    CHECK(histogram.bucket_count(0) == 255);
    CHECK(histogram.p50_ns() == 50);
}

TEST_CASE(confidence_distribution_bins_and_mean) {
    avatar::tracking::ConfidenceDistribution distribution;
    distribution.add(0.0f);
    distribution.add(0.5f);
    distribution.add(1.0f);
    distribution.add(-1.0f);
    distribution.add(2.0f);
    distribution.add(std::numeric_limits<float>::quiet_NaN());

    CHECK(distribution.samples == 6);
    CHECK(distribution.bucket(0) == 3);
    CHECK(distribution.bucket(5) == 1);
    CHECK(distribution.bucket(9) == 2);
    CHECK(distribution.bucket(99) == 0);
    CHECK(distribution.mean() > 0.41);
    CHECK(distribution.mean() < 0.43);
}
