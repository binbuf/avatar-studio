#include "backend.h"

#include <algorithm>
#include <chrono>

namespace avatar::tracker_backend {

Runtime::Runtime(const std::string& path, uint32_t size)
    : engine_(path), size_(size), worker_(&Runtime::run, this) {}

Runtime::~Runtime() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    worker_.join();
}

avatar_tracker_error Runtime::push(const avatar_tracker_frame& frame) {
    const auto error = validate_frame(frame);
    if (error != AVATAR_TRACKER_OK) {
        return error;
    }
    // The worker never holds this lock during preprocessing or inference.
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.submitted;
    if (failed_) {
        return AVATAR_TRACKER_ERROR_INTERNAL;
    }
    if (busy_) {
        ++metrics_.dropped;
        return AVATAR_TRACKER_OK;
    }
    input_.copy(frame);
    busy_ = true;
    changed_.notify_one();
    return AVATAR_TRACKER_OK;
}

avatar_tracker_error Runtime::poll(avatar_tracker_result& result) {
    result = {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) {
        return AVATAR_TRACKER_ERROR_INTERNAL;
    }
    if (!result_) {
        return AVATAR_TRACKER_NO_RESULT;
    }
    result = *result_;
    result_.reset();
    return AVATAR_TRACKER_OK;
}

bool Runtime::wait_idle(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !busy_; });
}

Metrics Runtime::metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

void Runtime::run() {
    for (;;) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return stopping_ || busy_; });
        if (stopping_) {
            return;
        }
        lock.unlock();
        try {
            const auto start = std::chrono::steady_clock::now();
            const auto prepared = prepare(input_.frame, size_);
            const auto inference_start = std::chrono::steady_clock::now();
            const auto ms = next_timestamp_ms(input_.frame.timestamp_ns, last_ms_);
            const bool adjusted = ms != static_cast<int64_t>(input_.frame.timestamp_ns / 1000000);
            auto result = engine_.infer(prepared, ms);
            const auto end = std::chrono::steady_clock::now();
            last_ms_ = ms;
            result.source_timestamp_ns = input_.frame.timestamp_ns;
            lock.lock();
            metrics_.timestamp_adjustments += adjusted ? 1 : 0;
            ++metrics_.produced;
            ++metrics_.confidence[std::min(size_t(result.confidence * 10), size_t(9))];
            // Bounded diagnostic reservoir. Counters cover the entire session;
            // samples cover its first 4096 results, including benchmark warm-up.
            if (metrics_.inference_ms.size() < 4096) {
                metrics_.inference_ms.push_back(
                    std::chrono::duration<double, std::milli>(end - inference_start).count());
                metrics_.preprocessing_ms.push_back(
                    std::chrono::duration<double, std::milli>(inference_start - start).count());
            }
            result_ = result;
        } catch (...) {
            if (!lock.owns_lock()) {
                lock.lock();
            }
            failed_ = true;
            result_.reset();
        }
        busy_ = false;
        lock.unlock();
        changed_.notify_all();
    }
}

} // namespace avatar::tracker_backend
