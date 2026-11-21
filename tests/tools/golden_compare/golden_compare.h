#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace avatar::golden {

/// An 8-bit RGB image. The comparator's working format is deliberately simple:
/// golden artifacts are PPM (P6), which needs no third-party decoder and is
/// trivial to diff and inspect.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 &&
               rgb.size() == static_cast<std::size_t>(width) * height * 3u;
    }
};

struct Options {
    double ssim_min = 0.995;
    int max_delta = 8;
};

struct Metrics {
    double ssim = 0.0;
    int max_delta = 0;
    double mean_delta = 0.0;
    std::uint64_t differing_pixels = 0;
    std::uint64_t total_pixels = 0;
};

struct Result {
    bool pass = false;
    Metrics metrics{};
    std::string reason{};
};

[[nodiscard]] Image make_solid(std::uint32_t width,
                               std::uint32_t height,
                               std::uint8_t r,
                               std::uint8_t g,
                               std::uint8_t b);
[[nodiscard]] Image make_gradient(std::uint32_t width, std::uint32_t height);

[[nodiscard]] bool load_ppm(const std::filesystem::path& path, Image& image, std::string& error);
[[nodiscard]] bool write_ppm(const std::filesystem::path& path,
                             const Image& image,
                             std::string& error);

/// Per-pixel absolute difference, amplified and tinted red for visibility.
[[nodiscard]] Image make_diff(const Image& actual, const Image& baseline);

/// Global SSIM over luma with 8x8 windows, plus the maximum per-channel delta.
/// Returns an empty Metrics with ssim 0 when the images differ in size.
[[nodiscard]] Metrics measure(const Image& actual, const Image& baseline);

[[nodiscard]] Result compare(const Image& actual, const Image& baseline, const Options& options);

/// Writes `<name>.actual.ppm`, `<name>.baseline.ppm` and `<name>.diff.ppm` into
/// `directory`, creating it if necessary.
[[nodiscard]] bool write_artifacts(const std::filesystem::path& directory,
                                   const std::string& name,
                                   const Image& actual,
                                   const Image& baseline,
                                   const Image& diff,
                                   std::string& error);

} // namespace avatar::golden
