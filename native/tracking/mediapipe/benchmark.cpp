#include "backend.h"
#include "clip_io.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
// diagnostics-scan: allow -- structured CLI reports, not application logging.
std::ostream& report = std::cout;
// diagnostics-scan: allow -- CLI usage/errors; the isolated tool has no app Logger.
std::ostream& errors = std::cerr;
} // namespace

namespace {
double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        throw std::runtime_error("no inference samples");
    }
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(std::ceil(values.size() * p)) - 1];
}
void print_distribution(const char* name, const std::vector<double>& values) {
    report << '"' << name << "\":{\"p50\":" << percentile(values, 0.5)
           << ",\"p95\":" << percentile(values, 0.95) << ",\"samples\":" << values.size() << '}';
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--self-test") {
        try {
            using namespace avatar::tracker_backend;
            Runtime runtime(argv[2], 64);
            std::vector<uint8_t> pixels(64 * 64 * 4, 0);
            avatar_tracker_frame frame{};
            frame.width = frame.height = 64;
            frame.stride = 256;
            frame.format = AVATAR_TRACKER_PIXEL_FORMAT_RGBA8;
            frame.plane_count = 1;
            frame.plane0 = pixels.data();
            frame.plane0_size = pixels.size();
            for (uint32_t i = 0; i < 1000; ++i) {
                if (runtime.push(frame) != AVATAR_TRACKER_OK) {
                    throw std::runtime_error("burst submission failed");
                }
            }
            if (!runtime.wait_idle(10000)) {
                throw std::runtime_error("burst drain timed out");
            }
            const auto burst = runtime.metrics();
            if (!burst.dropped || burst.produced + burst.dropped != burst.submitted) {
                throw std::runtime_error("one-slot busy-drop accounting failed");
            }
            if (runtime.push(frame) != AVATAR_TRACKER_OK || !runtime.wait_idle(10000)) {
                throw std::runtime_error("duplicate timestamp inference failed");
            }
            avatar_tracker_result result{};
            if (runtime.poll(result) != AVATAR_TRACKER_OK || result.source_timestamp_ns != 0 ||
                result.valid || !runtime.metrics().timestamp_adjustments) {
                throw std::runtime_error("worker timestamp/source/no-face contract failed");
            }
            report << "worker-contract: PASS submitted=" << burst.submitted
                   << " produced=" << burst.produced << " busy_dropped=" << burst.dropped << '\n';
            return 0;
        } catch (const std::exception& error) {
            errors << error.what() << '\n';
            return 1;
        }
    }
    if (argc != 7) {
        errors << "Usage: avatar_tracker_bench <model.task> <clip.avclip> <192|256> <frames> "
                  "<pace-ms|0> <warmup>\n";
        return 2;
    }
    try {
        using namespace avatar::tracker_backend;
        const auto size = static_cast<uint32_t>(std::stoul(argv[3]));
        const auto count = static_cast<uint32_t>(std::stoul(argv[4]));
        const auto pace = static_cast<uint32_t>(std::stoul(argv[5]));
        const auto warmup = static_cast<uint32_t>(std::stoul(argv[6]));
        if ((size != 192 && size != 256) || !count || count + uint64_t(warmup) > 4096 ||
            pace > 1000) {
            throw std::runtime_error("invalid benchmark arguments");
        }
        std::ifstream file(argv[2], std::ios::binary);
        auto frames = read_clip(file);
        for (size_t i = 1; i < frames.size(); ++i) {
            if (frames[i].frame.timestamp_ns <= frames[i - 1].frame.timestamp_ns) {
                throw std::runtime_error("recorded clip timestamps are not strictly monotonic");
            }
        }
        const uint64_t span = frames.back().frame.timestamp_ns - frames.front().frame.timestamp_ns;
        const uint64_t clip_interval = frames.size() > 1 ? span / (frames.size() - 1) : 33333333;
        const uint64_t epoch = span + std::max(clip_interval, uint64_t(1000000));
        const auto frame_at = [&](uint64_t index) {
            auto frame = frames[index % frames.size()].frame;
            frame.timestamp_ns += (index / frames.size()) * epoch;
            return frame;
        };
        const auto initialization_start = std::chrono::steady_clock::now();
        Runtime runtime(argv[1], size);
        const double initialization_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                      initialization_start)
                .count();
        avatar_tracker_result result{};
        for (uint32_t i = 0; i < warmup; ++i) {
            if (runtime.push(frame_at(i)) != AVATAR_TRACKER_OK || !runtime.wait_idle(10000) ||
                runtime.poll(result) != AVATAR_TRACKER_OK) {
                throw std::runtime_error("warmup inference failed");
            }
        }
        const auto before = runtime.metrics();
        uint64_t observed = 0, valid = 0, features = 0, expressions = 0;
        double max_push_ms = 0;
        const auto consume = [&] {
            const auto status = runtime.poll(result);
            if (status == AVATAR_TRACKER_ERROR_INTERNAL) {
                throw std::runtime_error("inference failed");
            }
            if (status == AVATAR_TRACKER_OK) {
                ++observed;
                valid += result.valid;
                features += result.features_valid;
                if (result.valid && result.landmark_count != 478) {
                    throw std::runtime_error("real face did not have 478 landmarks");
                }
                if (result.valid) {
                    float norm = 0;
                    for (const auto q : result.pose.rotation) {
                        norm += q * q;
                    }
                    if (!std::isfinite(norm) || std::abs(norm - 1) > 0.001f) {
                        throw std::runtime_error("real face pose quaternion is not normalized");
                    }
                    float sum = 0;
                    for (const auto coefficient : result.blendshapes) {
                        if (!std::isfinite(coefficient) || coefficient < 0 || coefficient > 1) {
                            throw std::runtime_error("invalid blendshape coefficient");
                        }
                        sum += coefficient;
                    }
                    expressions += sum > 0 ? 1 : 0;
                }
            }
        };
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < count; ++i) {
            if (pace) {
                std::this_thread::sleep_until(start +
                                              std::chrono::milliseconds(uint64_t(i) * pace));
            }
            consume();
            const auto push_start = std::chrono::steady_clock::now();
            if (runtime.push(frame_at(uint64_t(warmup) + i)) != AVATAR_TRACKER_OK) {
                throw std::runtime_error("frame submission failed");
            }
            max_push_ms = std::max(max_push_ms, std::chrono::duration<double, std::milli>(
                                                    std::chrono::steady_clock::now() - push_start)
                                                    .count());
            if (!pace) {
                if (!runtime.wait_idle(10000)) {
                    throw std::runtime_error("inference timed out");
                }
                consume();
            }
        }
        if (!runtime.wait_idle(10000)) {
            throw std::runtime_error("drain timed out");
        }
        consume();
        const auto after = runtime.metrics();
        std::vector<double> inference(after.inference_ms.begin() + before.inference_ms.size(),
                                      after.inference_ms.end());
        std::vector<double> preprocessing(after.preprocessing_ms.begin() +
                                              before.preprocessing_ms.size(),
                                          after.preprocessing_ms.end());
        std::vector<double> pipeline(inference.size());
        for (size_t i = 0; i < pipeline.size(); ++i) {
            pipeline[i] = inference[i] + preprocessing[i];
        }
        report << std::setprecision(8)
               << "{\"schema\":\"avatar.tracker-spike.v1\",\"backend\":\"mediapipe-0.10.21-cpu\","
               << "\"capture_width\":" << frames[0].frame.width
               << ",\"capture_height\":" << frames[0].frame.height
               << ",\"capture_format\":" << frames[0].frame.format
               << ",\"clip_frames\":" << frames.size() << ",\"prepared_width\":" << size
               << ",\"prepared_height\":" << size
               << ",\"landmark_tensor_width\":256,\"landmark_tensor_height\":256,\"detector_tensor_"
                  "width\":128,"
               << "\"warmup_frames\":" << warmup << ",\"initialization_ms\":" << initialization_ms
               << ",\"pace_ms\":" << pace << ",\"submitted\":" << after.submitted - before.submitted
               << ",\"produced\":" << after.produced - before.produced
               << ",\"dropped_frames\":" << after.dropped - before.dropped
               << ",\"observed_results\":" << observed << ",\"valid_faces\":" << valid
               << ",\"features_valid\":" << features << ",\"max_push_ms\":" << max_push_ms
               << ",\"nonzero_blendshape_results\":" << expressions << ",\"timestamp_adjustments\":"
               << after.timestamp_adjustments - before.timestamp_adjustments << ',';
        print_distribution("inference_ms", inference);
        report << ',';
        print_distribution("preprocessing_ms", preprocessing);
        report << ',';
        print_distribution("pipeline_ms", pipeline);
        report << ",\"confidence_histogram\":[";
        for (size_t i = 0; i < 10; ++i) {
            report << (i ? "," : "") << after.confidence[i] - before.confidence[i];
        }
        report << "],\"confidence_kind\":\"fraction-of-landmarks-in-source-bounds-after-model-"
                  "thresholds\"}\n";
        return valid ? 0 : 1;
    } catch (const std::exception& error) {
        errors << error.what() << '\n';
        return 1;
    }
}
