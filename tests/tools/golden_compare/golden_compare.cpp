#include "golden_compare.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace avatar::golden {

namespace {

class TokenReader {
public:
    explicit TokenReader(std::ifstream& stream) : stream_(stream) {}

    [[nodiscard]] bool next(std::string& token) {
        token.clear();
        int character = stream_.peek();
        while (character != std::char_traits<char>::eof()) {
            if (character == '#') {
                while (character != std::char_traits<char>::eof() && character != '\n') {
                    stream_.get();
                    character = stream_.peek();
                }
                continue;
            }
            if (std::isspace(character) != 0) {
                stream_.get();
                character = stream_.peek();
                continue;
            }
            break;
        }
        if (character == std::char_traits<char>::eof()) {
            return false;
        }
        while (character != std::char_traits<char>::eof() && std::isspace(character) == 0) {
            token.push_back(static_cast<char>(stream_.get()));
            character = stream_.peek();
        }
        return true;
    }

private:
    std::ifstream& stream_;
};

double luma_at(const Image& image, std::size_t pixel) noexcept {
    const std::size_t index = pixel * 3u;
    return 0.299 * static_cast<double>(image.rgb[index]) +
           0.587 * static_cast<double>(image.rgb[index + 1]) +
           0.114 * static_cast<double>(image.rgb[index + 2]);
}

std::string format_double(double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6) << value;
    return stream.str();
}

} // namespace

Image make_solid(std::uint32_t width, std::uint32_t height, std::uint8_t r, std::uint8_t g,
                 std::uint8_t b) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgb.resize(static_cast<std::size_t>(width) * height * 3u);
    for (std::size_t index = 0; index < image.rgb.size(); index += 3) {
        image.rgb[index] = r;
        image.rgb[index + 1] = g;
        image.rgb[index + 2] = b;
    }
    return image;
}

Image make_gradient(std::uint32_t width, std::uint32_t height) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgb.resize(static_cast<std::size_t>(width) * height * 3u);
    const double x_denominator = width > 1 ? static_cast<double>(width - 1) : 1.0;
    const double y_denominator = height > 1 ? static_cast<double>(height - 1) : 1.0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 3u;
            image.rgb[index] = static_cast<std::uint8_t>(std::lround(255.0 * x / x_denominator));
            image.rgb[index + 1] =
                static_cast<std::uint8_t>(std::lround(255.0 * y / y_denominator));
            image.rgb[index + 2] = static_cast<std::uint8_t>(
                std::lround(255.0 * (x + y) / (x_denominator + y_denominator)));
        }
    }
    return image;
}

bool load_ppm(const std::filesystem::path& path, Image& image, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "cannot open " + path.string();
        return false;
    }
    TokenReader reader(stream);
    std::string magic;
    std::string width;
    std::string height;
    std::string max_value;
    if (!reader.next(magic) || magic != "P6") {
        error = "not a binary PPM (P6) file: " + path.string();
        return false;
    }
    if (!reader.next(width) || !reader.next(height) || !reader.next(max_value)) {
        error = "truncated PPM header: " + path.string();
        return false;
    }
    const long parsed_width = std::strtol(width.c_str(), nullptr, 10);
    const long parsed_height = std::strtol(height.c_str(), nullptr, 10);
    const long parsed_max = std::strtol(max_value.c_str(), nullptr, 10);
    if (parsed_width <= 0 || parsed_height <= 0 || parsed_max != 255) {
        error = "unsupported PPM header values: " + path.string();
        return false;
    }

    int separator = stream.get();
    if (separator == '\r') {
        if (stream.peek() == '\n') {
            stream.get();
        }
    } else if (separator != '\n' && separator != ' ') {
        error = "malformed PPM header (missing whitespace after maxval): " + path.string();
        return false;
    }

    image.width = static_cast<std::uint32_t>(parsed_width);
    image.height = static_cast<std::uint32_t>(parsed_height);
    image.rgb.resize(static_cast<std::size_t>(image.width) * image.height * 3u);
    stream.read(reinterpret_cast<char*>(image.rgb.data()),
                static_cast<std::streamsize>(image.rgb.size()));
    if (static_cast<std::size_t>(stream.gcount()) != image.rgb.size()) {
        error = "truncated PPM pixel data: " + path.string();
        return false;
    }
    return true;
}

bool write_ppm(const std::filesystem::path& path, const Image& image, std::string& error) {
    if (!image.valid()) {
        error = "refusing to write an invalid image: " + path.string();
        return false;
    }
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        error = "cannot create " + path.string();
        return false;
    }
    stream << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    stream.write(reinterpret_cast<const char*>(image.rgb.data()),
                 static_cast<std::streamsize>(image.rgb.size()));
    if (!stream) {
        error = "failed while writing " + path.string();
        return false;
    }
    return true;
}

Image make_diff(const Image& actual, const Image& baseline) {
    if (!actual.valid() || !baseline.valid() || actual.width != baseline.width ||
        actual.height != baseline.height) {
        return Image{};
    }
    Image diff;
    diff.width = actual.width;
    diff.height = actual.height;
    diff.rgb.resize(actual.rgb.size());
    for (std::size_t index = 0; index < actual.rgb.size(); index += 3) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int delta = std::abs(static_cast<int>(actual.rgb[index + channel]) -
                                       static_cast<int>(baseline.rgb[index + channel]));
            const int amplified = std::min(255, delta * 8);
            diff.rgb[index + channel] = channel == 0 ? static_cast<std::uint8_t>(amplified)
                                                     : static_cast<std::uint8_t>(amplified / 3);
        }
    }
    return diff;
}

Metrics measure(const Image& actual, const Image& baseline) {
    Metrics metrics;
    if (!actual.valid() || !baseline.valid() || actual.width != baseline.width ||
        actual.height != baseline.height) {
        return metrics;
    }
    const std::uint32_t width = actual.width;
    const std::uint32_t height = actual.height;
    metrics.total_pixels = static_cast<std::uint64_t>(width) * height;

    std::uint64_t delta_sum = 0;
    for (std::size_t index = 0; index < actual.rgb.size(); index += 3) {
        bool differs = false;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int delta = std::abs(static_cast<int>(actual.rgb[index + channel]) -
                                       static_cast<int>(baseline.rgb[index + channel]));
            metrics.max_delta = std::max(metrics.max_delta, delta);
            delta_sum += static_cast<std::uint64_t>(delta);
            if (delta != 0) {
                differs = true;
            }
        }
        if (differs) {
            ++metrics.differing_pixels;
        }
    }
    metrics.mean_delta =
        static_cast<double>(delta_sum) / static_cast<double>(metrics.total_pixels * 3u);

    constexpr int kWindow = 8;
    constexpr double kL = 255.0;
    constexpr double kC1 = (0.01 * kL) * (0.01 * kL);
    constexpr double kC2 = (0.03 * kL) * (0.03 * kL);

    double ssim_sum = 0.0;
    std::uint64_t window_count = 0;
    for (std::uint32_t y0 = 0; y0 < height; y0 += kWindow) {
        for (std::uint32_t x0 = 0; x0 < width; x0 += kWindow) {
            const std::uint32_t x1 = std::min(x0 + kWindow, width);
            const std::uint32_t y1 = std::min(y0 + kWindow, height);
            const double count = static_cast<double>((x1 - x0) * (y1 - y0));
            double sum_x = 0.0;
            double sum_y = 0.0;
            double sum_xx = 0.0;
            double sum_yy = 0.0;
            double sum_xy = 0.0;
            for (std::uint32_t y = y0; y < y1; ++y) {
                for (std::uint32_t x = x0; x < x1; ++x) {
                    const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
                    const double a = luma_at(actual, pixel);
                    const double b = luma_at(baseline, pixel);
                    sum_x += a;
                    sum_y += b;
                    sum_xx += a * a;
                    sum_yy += b * b;
                    sum_xy += a * b;
                }
            }
            const double mean_x = sum_x / count;
            const double mean_y = sum_y / count;
            double variance_x = sum_xx / count - mean_x * mean_x;
            double variance_y = sum_yy / count - mean_y * mean_y;
            double covariance = sum_xy / count - mean_x * mean_y;
            variance_x = std::max(variance_x, 0.0);
            variance_y = std::max(variance_y, 0.0);
            const double numerator = (2.0 * mean_x * mean_y + kC1) * (2.0 * covariance + kC2);
            const double denominator =
                (mean_x * mean_x + mean_y * mean_y + kC1) * (variance_x + variance_y + kC2);
            ssim_sum += denominator > 0.0 ? numerator / denominator : 1.0;
            ++window_count;
        }
    }
    metrics.ssim = window_count > 0 ? ssim_sum / static_cast<double>(window_count) : 0.0;
    return metrics;
}

Result compare(const Image& actual, const Image& baseline, const Options& options) {
    Result result;
    if (!actual.valid() || !baseline.valid()) {
        result.reason = "one or both images are invalid";
        return result;
    }
    if (actual.width != baseline.width || actual.height != baseline.height) {
        result.reason = "image dimensions differ";
        return result;
    }
    result.metrics = measure(actual, baseline);
    if (result.metrics.ssim < options.ssim_min) {
        result.reason =
            "SSIM " + format_double(result.metrics.ssim) + " < " + format_double(options.ssim_min);
        return result;
    }
    if (result.metrics.max_delta > options.max_delta) {
        result.reason = "max delta " + std::to_string(result.metrics.max_delta) + " > " +
                        std::to_string(options.max_delta);
        return result;
    }
    result.pass = true;
    result.reason = "pass";
    return result;
}

bool write_artifacts(const std::filesystem::path& directory, const std::string& name,
                     const Image& actual, const Image& baseline, const Image& diff,
                     std::string& error) {
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    if (code) {
        error = "cannot create artifact directory " + directory.string() + ": " + code.message();
        return false;
    }
    const std::string stem = name.empty() ? "golden" : name;
    if (!write_ppm(directory / (stem + ".actual.ppm"), actual, error)) {
        return false;
    }
    if (!write_ppm(directory / (stem + ".baseline.ppm"), baseline, error)) {
        return false;
    }
    if (!write_ppm(directory / (stem + ".diff.ppm"), diff, error)) {
        return false;
    }
    return true;
}

} // namespace avatar::golden
