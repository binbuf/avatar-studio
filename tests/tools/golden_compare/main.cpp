#include "golden_compare.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using avatar::golden::Image;
using avatar::golden::Options;
using avatar::golden::Result;

void print_usage() {
    std::cout << "golden_compare --actual <file.ppm> --baseline <file.ppm> [options]\n"
                 "  --artifacts <dir>   write <name>.actual/baseline/diff.ppm\n"
                 "  --name <stem>       artifact stem (default: golden)\n"
                 "  --ssim-min <value>  minimum SSIM (default 0.995)\n"
                 "  --max-delta <n>     maximum per-channel delta (default 8)\n"
                 "  --self-test <dir>   run the comparator self-test and exit\n"
                 "  --help              show this message\n";
}

bool parse_double(const std::string& text, double& value) {
    try {
        std::size_t consumed = 0;
        value = std::stod(text, &consumed);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

bool parse_int(const std::string& text, int& value) {
    try {
        std::size_t consumed = 0;
        value = std::stoi(text, &consumed);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

int run_self_test(const std::filesystem::path& directory) {
    std::error_code code;
    std::filesystem::remove_all(directory, code);
    std::filesystem::create_directories(directory, code);
    if (code) {
        std::cerr << "self-test: cannot create " << directory << ": " << code.message() << '\n';
        return 1;
    }

    int failures = 0;
    const auto expect = [&failures](bool condition, const std::string& what) {
        if (condition) {
            std::cout << "  ok: " << what << '\n';
        } else {
            std::cerr << "  FAIL: " << what << '\n';
            ++failures;
        }
    };

    const Image gradient = avatar::golden::make_gradient(64, 48);
    const Image identical = gradient;

    const Result identical_result = avatar::golden::compare(gradient, identical, Options{});
    expect(identical_result.pass, "identical images pass");
    expect(identical_result.metrics.ssim == 1.0, "identical SSIM is exactly 1.0");
    expect(identical_result.metrics.max_delta == 0, "identical max delta is 0");

    std::string error;
    const Image identical_diff = avatar::golden::make_diff(gradient, identical);
    expect(avatar::golden::write_artifacts(directory / "identical",
                                           "frame",
                                           gradient,
                                           identical,
                                           identical_diff,
                                           error),
           "identical artifacts written");
    expect(std::filesystem::exists(directory / "identical" / "frame.actual.ppm") &&
               std::filesystem::exists(directory / "identical" / "frame.baseline.ppm") &&
               std::filesystem::exists(directory / "identical" / "frame.diff.ppm"),
           "identical run writes all three artifacts");

    const Image black = avatar::golden::make_solid(64, 48, 0, 0, 0);
    const Image white = avatar::golden::make_solid(64, 48, 255, 255, 255);
    const Result different_result = avatar::golden::compare(black, white, Options{});
    expect(!different_result.pass, "visibly different images fail");
    expect(different_result.metrics.ssim < 0.995, "visibly different SSIM is below the threshold");
    expect(different_result.metrics.max_delta > 8, "visibly different max delta exceeds the guard");
    expect(avatar::golden::write_artifacts(directory / "different",
                                           "frame",
                                           black,
                                           white,
                                           avatar::golden::make_diff(black, white),
                                           error),
           "different artifacts written");
    expect(std::filesystem::exists(directory / "different" / "frame.actual.ppm") &&
               std::filesystem::exists(directory / "different" / "frame.baseline.ppm") &&
               std::filesystem::exists(directory / "different" / "frame.diff.ppm"),
           "different run writes all three artifacts");

    Image one_pixel = gradient;
    one_pixel.rgb[0] = static_cast<std::uint8_t>(std::min(255, static_cast<int>(one_pixel.rgb[0]) + 50));
    const Result guard_result = avatar::golden::compare(gradient, one_pixel, Options{});
    expect(!guard_result.pass, "max-delta guard rejects a single large pixel change");
    expect(guard_result.metrics.max_delta >= 50, "max-delta guard reports the local delta");

    Options strict;
    strict.ssim_min = 0.999999;
    const Result strict_result = avatar::golden::compare(gradient, identical, strict);
    expect(strict_result.pass, "identical images pass a strict SSIM threshold");

    expect(avatar::golden::write_ppm(directory / "roundtrip.ppm", gradient, error),
           "PPM round-trip write");
    Image loaded;
    expect(avatar::golden::load_ppm(directory / "roundtrip.ppm", loaded, error),
           "PPM round-trip read");
    expect(avatar::golden::compare(loaded, gradient, Options{}).pass,
           "PPM round-trip is pixel exact");

    if (failures == 0) {
        std::cout << "golden_compare self-test: PASS\n";
        return 0;
    }
    std::cerr << "golden_compare self-test: FAIL (" << failures << " check(s))\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> arguments(argv + 1, argv + argc);
    std::filesystem::path actual_path;
    std::filesystem::path baseline_path;
    std::filesystem::path artifacts;
    std::filesystem::path self_test_dir;
    std::string name = "golden";
    Options options;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        const auto next_value = [&arguments, &index](std::string& value) {
            if (index + 1 >= arguments.size()) {
                return false;
            }
            value = arguments[++index];
            return true;
        };
        std::string value;
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        }
        if (argument == "--actual") {
            if (!next_value(value)) {
                std::cerr << "golden_compare: --actual needs a value\n";
                return 2;
            }
            actual_path = value;
        } else if (argument == "--baseline") {
            if (!next_value(value)) {
                std::cerr << "golden_compare: --baseline needs a value\n";
                return 2;
            }
            baseline_path = value;
        } else if (argument == "--artifacts") {
            if (!next_value(value)) {
                std::cerr << "golden_compare: --artifacts needs a value\n";
                return 2;
            }
            artifacts = value;
        } else if (argument == "--name") {
            if (!next_value(value)) {
                std::cerr << "golden_compare: --name needs a value\n";
                return 2;
            }
            name = value;
        } else if (argument == "--ssim-min") {
            if (!next_value(value) || !parse_double(value, options.ssim_min)) {
                std::cerr << "golden_compare: --ssim-min needs a number\n";
                return 2;
            }
        } else if (argument == "--max-delta") {
            if (!next_value(value) || !parse_int(value, options.max_delta)) {
                std::cerr << "golden_compare: --max-delta needs an integer\n";
                return 2;
            }
        } else if (argument == "--self-test") {
            if (!next_value(value)) {
                std::cerr << "golden_compare: --self-test needs a directory\n";
                return 2;
            }
            self_test_dir = value;
        } else {
            std::cerr << "golden_compare: unknown argument '" << argument << "'\n";
            print_usage();
            return 2;
        }
    }

    if (!self_test_dir.empty()) {
        return run_self_test(self_test_dir);
    }
    if (actual_path.empty() || baseline_path.empty()) {
        std::cerr << "golden_compare: --actual and --baseline are required\n";
        print_usage();
        return 2;
    }

    Image actual;
    Image baseline;
    std::string error;
    if (!avatar::golden::load_ppm(actual_path, actual, error)) {
        std::cerr << "golden_compare: " << error << '\n';
        return 2;
    }
    if (!avatar::golden::load_ppm(baseline_path, baseline, error)) {
        std::cerr << "golden_compare: " << error << '\n';
        return 2;
    }

    const Result result = avatar::golden::compare(actual, baseline, options);
    std::cout << "golden_compare: actual=" << actual_path.string()
              << " baseline=" << baseline_path.string() << '\n';
    std::cout << "  ssim=" << result.metrics.ssim
              << " max_delta=" << result.metrics.max_delta
              << " mean_delta=" << result.metrics.mean_delta
              << " differing_pixels=" << result.metrics.differing_pixels
              << '/' << result.metrics.total_pixels << '\n';

    if (!artifacts.empty()) {
        if (!avatar::golden::write_artifacts(artifacts,
                                             name,
                                             actual,
                                             baseline,
                                             avatar::golden::make_diff(actual, baseline),
                                             error)) {
            std::cerr << "golden_compare: " << error << '\n';
            return 2;
        }
        std::cout << "  artifacts: " << artifacts.string() << '\n';
    }

    if (result.pass) {
        std::cout << "result: PASS\n";
        return 0;
    }
    std::cout << "result: FAIL (" << result.reason << ")\n";
    return 1;
}
