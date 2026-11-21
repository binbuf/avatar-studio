#include "test_registry.h"

#include "golden_compare.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using avatar::golden::Image;
using avatar::golden::Options;
using avatar::golden::Result;

std::filesystem::path make_temp_dir() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "avatar_studio_golden_unit";
    std::error_code code;
    std::filesystem::remove_all(directory, code);
    std::filesystem::create_directories(directory, code);
    return directory;
}

} // namespace

TEST_CASE(golden_compare_identical_images_pass) {
    const Image image = avatar::golden::make_gradient(64, 48);
    const Result result = avatar::golden::compare(image, image, Options{});
    CHECK(result.pass);
    CHECK(result.metrics.ssim == 1.0);
    CHECK(result.metrics.max_delta == 0);
    CHECK(result.metrics.mean_delta == 0.0);
    CHECK(result.metrics.differing_pixels == 0);
    CHECK(result.metrics.total_pixels == 64u * 48u);
}

TEST_CASE(golden_compare_visibly_different_images_fail) {
    const Image black = avatar::golden::make_solid(64, 48, 0, 0, 0);
    const Image white = avatar::golden::make_solid(64, 48, 255, 255, 255);
    const Result result = avatar::golden::compare(black, white, Options{});
    CHECK(!result.pass);
    CHECK(result.metrics.ssim < 0.995);
    CHECK(result.metrics.max_delta == 255);
    CHECK(result.reason.find("SSIM") != std::string::npos);
}

TEST_CASE(golden_compare_max_delta_guard_fails_a_local_change) {
    Image actual = avatar::golden::make_gradient(64, 48);
    const Image baseline = actual;
    actual.rgb[0] = static_cast<std::uint8_t>(std::min(255, static_cast<int>(actual.rgb[0]) + 50));
    const Result result = avatar::golden::compare(actual, baseline, Options{});
    CHECK(!result.pass);
    CHECK(result.metrics.max_delta >= 50);
    CHECK(result.reason.find("max delta") != std::string::npos);
}

TEST_CASE(golden_compare_size_mismatch_fails) {
    const Image small = avatar::golden::make_solid(32, 32, 10, 20, 30);
    const Image wide = avatar::golden::make_solid(64, 32, 10, 20, 30);
    const Result result = avatar::golden::compare(small, wide, Options{});
    CHECK(!result.pass);
    CHECK(result.reason.find("dimensions") != std::string::npos);
}

TEST_CASE(golden_compare_writes_all_three_artifacts) {
    const std::filesystem::path directory = make_temp_dir() / "artifacts";
    const Image actual = avatar::golden::make_gradient(32, 24);
    const Image baseline = avatar::golden::make_solid(32, 24, 1, 2, 3);
    std::string error;
    CHECK(avatar::golden::write_artifacts(directory, "frame", actual, baseline,
                                          avatar::golden::make_diff(actual, baseline), error));
    CHECK(std::filesystem::exists(directory / "frame.actual.ppm"));
    CHECK(std::filesystem::exists(directory / "frame.baseline.ppm"));
    CHECK(std::filesystem::exists(directory / "frame.diff.ppm"));

    Image loaded;
    CHECK(avatar::golden::load_ppm(directory / "frame.actual.ppm", loaded, error));
    CHECK(loaded.width == 32);
    CHECK(loaded.height == 24);
    CHECK(loaded.rgb == actual.rgb);
}

TEST_CASE(golden_compare_ppm_roundtrip_is_exact) {
    const std::filesystem::path path = make_temp_dir() / "roundtrip.ppm";
    const Image original = avatar::golden::make_gradient(40, 30);
    std::string error;
    CHECK(avatar::golden::write_ppm(path, original, error));
    Image loaded;
    CHECK(avatar::golden::load_ppm(path, loaded, error));
    CHECK(loaded.width == original.width);
    CHECK(loaded.height == original.height);
    CHECK(loaded.rgb == original.rgb);
    CHECK(avatar::golden::compare(loaded, original, Options{}).pass);
}

TEST_CASE(golden_compare_rejects_a_malformed_ppm) {
    const std::filesystem::path path = make_temp_dir() / "bad.ppm";
    {
        std::ofstream output(path, std::ios::binary);
        output << "P6\nnot a number 12\n255\n";
    }
    Image image;
    std::string error;
    CHECK(!avatar::golden::load_ppm(path, image, error));
    CHECK(!error.empty());
}
