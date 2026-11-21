#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace avatar::core {

/// Fixed-bucket latency histogram, in nanoseconds.
///
/// Bucket `i` covers `[i * BucketWidthNs, (i + 1) * BucketWidthNs)`. The last
/// bucket is the overflow bucket: every larger sample lands there, and
/// `max_ns()` still reports the exact maximum. Counts saturate at the counter
/// type's maximum, which is enough for a session-scale diagnostic and keeps the
/// object small, trivially copyable and usable in `TrackerStats`.
///
/// Percentiles use integer arithmetic and return the midpoint of the bucket that
/// contains the requested rank. The default 100 microsecond buckets resolve a
/// 10 ms inference time to within 0.5%, and cover 102.4 ms before the overflow
/// bucket. That is deliberately coarse: this is a diagnostic and a regression
/// signal, not a benchmark timer. A caller that needs a different range
/// instantiates the template with its own bucket count and width.
template <std::size_t BucketCount = 1024, std::uint64_t BucketWidthNs = 100'000,
          typename Count = std::uint32_t>
class LatencyHistogram {
    static_assert(BucketCount > 0, "a histogram needs at least one bucket");
    static_assert(BucketWidthNs > 0, "a histogram bucket must have a positive width");

public:
    static constexpr std::size_t kBucketCount = BucketCount;
    static constexpr std::uint64_t kBucketWidthNs = BucketWidthNs;
    static constexpr std::uint64_t kCapacityNs =
        static_cast<std::uint64_t>(BucketCount) * BucketWidthNs;

    /// Records one sample. The value is not clamped to the capacity: the
    /// overflow bucket holds it and the exact maximum is retained.
    void add(std::uint64_t ns) noexcept {
        const std::uint64_t raw_index = ns / kBucketWidthNs;
        const std::size_t index =
            raw_index < kBucketCount ? static_cast<std::size_t>(raw_index) : kBucketCount - 1;
        if (counts_[index] != std::numeric_limits<Count>::max()) {
            ++counts_[index];
        }
        if (samples_ == 0) {
            min_ns_ = ns;
            max_ns_ = ns;
        } else {
            if (ns < min_ns_) {
                min_ns_ = ns;
            }
            if (ns > max_ns_) {
                max_ns_ = ns;
            }
        }
        ++samples_;
        total_ns_ += ns;
    }

    void reset() noexcept {
        counts_.fill(Count{0});
        samples_ = 0;
        total_ns_ = 0;
        min_ns_ = 0;
        max_ns_ = 0;
    }

    [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }
    [[nodiscard]] std::uint64_t count() const noexcept { return samples_; }
    [[nodiscard]] bool empty() const noexcept { return samples_ == 0; }
    [[nodiscard]] std::uint64_t total_ns() const noexcept { return total_ns_; }
    [[nodiscard]] std::uint64_t min_ns() const noexcept { return samples_ == 0 ? 0 : min_ns_; }
    [[nodiscard]] std::uint64_t max_ns() const noexcept { return samples_ == 0 ? 0 : max_ns_; }

    [[nodiscard]] double mean_ns() const noexcept {
        return samples_ == 0 ? 0.0 : static_cast<double>(total_ns_) / static_cast<double>(samples_);
    }

    /// Returns the midpoint of the bucket containing the requested percentile.
    /// `percent` is clamped to 0..100. An empty histogram returns 0.
    [[nodiscard]] std::uint64_t percentile_ns(std::uint32_t percent) const noexcept {
        if (samples_ == 0) {
            return 0;
        }
        if (percent == 0) {
            return min_ns_;
        }
        if (percent >= 100) {
            return max_ns_;
        }
        const std::uint64_t target = (samples_ * percent + 99) / 100;
        std::uint64_t cumulative = 0;
        for (std::size_t index = 0; index < kBucketCount; ++index) {
            cumulative += counts_[index];
            if (cumulative >= target) {
                if (index + 1 == kBucketCount) {
                    return max_ns_;
                }
                return bucket_lower_ns(index) + kBucketWidthNs / 2;
            }
        }
        return max_ns_;
    }

    [[nodiscard]] std::uint64_t p50_ns() const noexcept { return percentile_ns(50); }
    [[nodiscard]] std::uint64_t p95_ns() const noexcept { return percentile_ns(95); }
    [[nodiscard]] std::uint64_t p99_ns() const noexcept { return percentile_ns(99); }

    [[nodiscard]] Count bucket_count(std::size_t index) const noexcept {
        return index < kBucketCount ? counts_[index] : Count{0};
    }

    [[nodiscard]] std::uint64_t bucket_lower_ns(std::size_t index) const noexcept {
        return static_cast<std::uint64_t>(index) * kBucketWidthNs;
    }

    /// Upper bound of a bucket. The overflow bucket reports the exact maximum.
    [[nodiscard]] std::uint64_t bucket_upper_ns(std::size_t index) const noexcept {
        if (index + 1 >= kBucketCount) {
            return max_ns();
        }
        return bucket_lower_ns(index + 1) - 1;
    }

private:
    std::array<Count, BucketCount> counts_{};
    std::uint64_t samples_ = 0;
    std::uint64_t total_ns_ = 0;
    std::uint64_t min_ns_ = 0;
    std::uint64_t max_ns_ = 0;
};

/// The histogram shape `TrackerStats` uses: 100 microsecond buckets over a
/// 102.4 ms window, which covers every plausible CPU inference result.
using TrackerLatencyHistogram = LatencyHistogram<>;

} // namespace avatar::core
