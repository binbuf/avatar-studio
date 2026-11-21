#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../mediapipe/clip_io.h"
#include "avatar/capture/camera_source.h"
#include "avatar/core/clock.h"

#define NOMINMAX
#include <windows.h>

namespace {
// diagnostics-scan: allow -- structured CLI reports, not application logging.
std::ostream& report = std::cout;
// diagnostics-scan: allow -- CLI usage/errors; the isolated tool has no app Logger.
std::ostream& errors = std::cerr;
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        errors << "Usage: avatar_record_tracker_clip <seconds:1..20> <output.avclip>\n";
        return 2;
    }
    try {
        using namespace avatar;
        const auto seconds = std::stoul(argv[1]);
        if (!seconds || seconds > 20) {
            throw std::runtime_error("recording duration must be 1..20 seconds");
        }
        core::SteadyTimeSource time;
        core::FrameClock clock(time);
        capture::CameraConfig config;
        config.preferences = {{640, 480, 30, 1, capture::PixelFormat::nv12}};
        config.reconnect = false;
        config.restart_on_stall = false;
        auto backend = capture::create_platform_camera_backend();
        capture::CameraSource source(*backend, clock, config);
        if (!source.start()) {
            throw std::runtime_error("camera start failed");
        }
        std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open output clip");
        }
        out.write("AVCLIP1\n", 8);
        const auto start = clock.now_ns();
        uint32_t last_sequence = 0;
        uint64_t written = 0;
        while (clock.now_ns() - start < seconds * 1000000000ull) {
            (void)source.produce();
            auto lease = source.ring().try_acquire_latest();
            const auto* frame = lease ? &lease->frame() : nullptr;
            if (frame && (!written || frame->sequence != last_sequence)) {
                if (frame->color_space != capture::ColorSpace::bt601_limited ||
                    frame->format != capture::PixelFormat::nv12 ||
                    frame->strides[0] != frame->strides[1]) {
                    throw std::runtime_error("recorder requires equal-stride BT.601 limited NV12");
                }
                avatar_tracker_frame input{};
                input.timestamp_ns = frame->timestamp_ns;
                input.sequence = frame->sequence;
                input.width = frame->width;
                input.height = frame->height;
                input.format = AVATAR_TRACKER_PIXEL_FORMAT_NV12;
                input.stride = frame->strides[0];
                input.plane_count = 2;
                input.plane0 = frame->planes[0];
                input.plane1 = frame->planes[1];
                tracker_backend::write_clip_frame(out, input);
                last_sequence = frame->sequence;
                ++written;
            }
            Sleep(1);
        }
        source.stop();
        report << "recorded_frames=" << written << " duration_seconds=" << seconds << '\n';
        return written ? 0 : 1;
    } catch (const std::exception& error) {
        errors << error.what() << '\n';
        return 1;
    }
}
