#pragma once

// Private implementation and measurement seam. Never included by the application.
#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "avatar/tracker/tracker_c_api.h"

namespace avatar::tracker_backend {

struct OwnedFrame {
    avatar_tracker_frame frame{};
    std::vector<uint8_t> bytes;
    void copy(const avatar_tracker_frame& input);
    void bind();
};

avatar_tracker_error validate_frame(const avatar_tracker_frame& frame);
avatar_tracker_error validate_config(const avatar_tracker_config& config);

struct Letterbox {
    float scale = 1;
    float pad_x = 0;
    float pad_y = 0;
    uint32_t size = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    avatar_tracker_landmark unproject(float x, float y, float z) const;
};

struct PreparedFrame {
    std::vector<uint8_t> rgb;
    Letterbox box;
};

PreparedFrame prepare(const avatar_tracker_frame& frame, uint32_t size);
bool features_usable(const avatar_tracker_result& result);
int64_t next_timestamp_ms(uint64_t source_ns, int64_t previous_ms);

class MediaPipeEngine {
public:
    explicit MediaPipeEngine(const std::string& model_path);
    ~MediaPipeEngine();
    avatar_tracker_result infer(const PreparedFrame& frame, int64_t timestamp_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct Metrics {
    uint64_t submitted = 0;
    uint64_t dropped = 0;
    uint64_t produced = 0;
    uint64_t timestamp_adjustments = 0;
    std::array<uint64_t, 10> confidence{};
    std::vector<double> inference_ms;
    std::vector<double> preprocessing_ms;
};

// One owned frame, no pending queue. Measurement methods are private C++ symbols;
// the DLL exports only the five functions declared in tracker_c_api.h.
class Runtime {
public:
    Runtime(const std::string& model_path, uint32_t input_size);
    ~Runtime();
    avatar_tracker_error push(const avatar_tracker_frame& frame);
    avatar_tracker_error poll(avatar_tracker_result& result);
    bool wait_idle(uint32_t timeout_ms);
    Metrics metrics() const;

private:
    void run();
    MediaPipeEngine engine_;
    uint32_t size_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = false;
    bool busy_ = false;
    bool failed_ = false;
    OwnedFrame input_;
    std::optional<avatar_tracker_result> result_;
    int64_t last_ms_ = -1;
    Metrics metrics_;
    std::thread worker_;
};

std::string bundled_model_path();

} // namespace avatar::tracker_backend
