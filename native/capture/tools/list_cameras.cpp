// Capture smoke command.
//
//   avatar_list_cameras --list-cameras [--modes]
//   avatar_list_cameras --capture <seconds> [--device <symbolic-link>]
//
// `--list-cameras` never opens a device, so it is safe to run while OBS or a
// meeting app owns the camera. `--capture` runs the real backend for a fixed
// duration and reports frame count, monotonic timestamps, the negotiated mode
// and the callback-to-ring-publish latency. It is the manual 60-second smoke
// from the T10 checklist.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "avatar/capture/camera_device.h"
#include "avatar/capture/camera_source.h"
#include "avatar/core/clock.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using avatar::capture::CameraCapabilities;
using avatar::capture::CameraDevice;
using avatar::capture::CameraEvent;
using avatar::capture::CameraMode;
using avatar::capture::CameraSource;
using avatar::capture::CameraConfig;
using avatar::capture::ColorSpace;
using avatar::capture::ICameraDeviceBackend;
using avatar::capture::PixelFormat;

// CLI output, not logging: the diagnostics scanner requires a code on every
// log call, and this is a program's report stream.
void write_line(std::FILE* stream, std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fwrite("\n", 1, 1, stream);
    std::fflush(stream);
}

void sleep_ms(unsigned milliseconds) {
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    usleep(static_cast<useconds_t>(milliseconds) * 1000u);
#endif
}

std::string format_fps(const CameraMode& mode) {
    const double fps = mode.fps();
    const int whole = static_cast<int>(fps);
    if (fps - static_cast<double>(whole) > 0.001) {
        return std::to_string(whole) + "." +
               std::to_string(static_cast<int>((fps - static_cast<double>(whole)) * 1000.0 + 0.5));
    }
    return std::to_string(whole);
}

std::string format_mode(const CameraMode& mode) {
    return std::to_string(mode.width) + "x" + std::to_string(mode.height) + "@" +
           format_fps(mode) + " " + avatar::capture::pixel_format_name(mode.format);
}

std::string format_us(std::uint64_t ns) {
    return std::to_string(ns / 1000u) + " us";
}

std::string_view color_space_name(ColorSpace space) {
    switch (space) {
        case ColorSpace::bt601_limited:
            return "bt601_limited";
        case ColorSpace::bt709_limited:
            return "bt709_limited";
        case ColorSpace::bt709_full:
            return "bt709_full";
        case ColorSpace::srgb:
            return "srgb";
    }
    return "unknown";
}

std::string format_event(const CameraEvent& event) {
    std::string text = avatar::core::format_diagnostic_code(event.code);
    if (event.code == avatar::core::DiagnosticCode::unknown) {
        text = "event";
    }
    text += ": ";
    text += event.detail;
    return text;
}

void print_usage() {
    write_line(stdout, "usage: avatar_list_cameras --list-cameras [--modes] [--device <id>]");
    write_line(stdout,
               "       avatar_list_cameras --capture <seconds> [--device <id>] "
               "[--mode WxH@FPS] [--format nv12|yuy2|bgra8]");
}

bool capture_smoke(ICameraDeviceBackend& backend, const std::string& device_id,
                   unsigned seconds, const CameraMode& forced_mode) {
    avatar::core::SteadyTimeSource frame_time;
    avatar::core::FrameClock clock(frame_time);
    avatar::core::SteadyTimeSource elapsed_time;

    CameraConfig config;
    config.device_id = device_id;
    if (forced_mode.width != 0 && forced_mode.height != 0) {
        config.preferences = {forced_mode};
    }
    CameraSource source(backend, clock, config);

    const avatar::core::Status status = source.start();
    for (const CameraEvent& event : source.take_events()) {
        write_line(stdout, format_event(event));
    }
    if (!status.ok()) {
        write_line(stderr, "capture failed: " + status.to_string());
        return false;
    }

    const CameraMode mode = source.negotiated_mode();
    write_line(stdout, "device: " + std::string(source.device_name()) + " (" +
                           std::string(source.device_id()) + ")");
    write_line(stdout, "negotiated: " + format_mode(mode) + " " +
                           std::string(color_space_name(source.color_space())));
    write_line(stdout, "running for " + std::to_string(seconds) + " s");

    const std::uint64_t start_ns = elapsed_time.now_ns();
    const std::uint64_t duration_ns = static_cast<std::uint64_t>(seconds) * 1'000'000'000ull;
    std::uint64_t delivered = 0;
    std::uint64_t last_timestamp = 0;
    bool monotonic = true;
    bool shape_ok = true;

    while (elapsed_time.now_ns() - start_ns < duration_ns) {
        if (source.produce()) {
            ++delivered;
            if (std::optional<avatar::capture::FrameLease> lease =
                    source.ring().try_acquire_latest()) {
                const avatar::capture::VideoFrame& frame = lease->frame();
                if (frame.timestamp_ns <= last_timestamp) {
                    monotonic = false;
                }
                last_timestamp = frame.timestamp_ns;
                const CameraMode current = source.negotiated_mode();
                if (frame.width != current.width || frame.height != current.height ||
                    frame.format != current.format) {
                    shape_ok = false;
                }
            }
        }
        for (const CameraEvent& event : source.take_events()) {
            write_line(stdout, format_event(event));
        }
        sleep_ms(2);
    }
    source.stop();

    const avatar::capture::CaptureLatencyHistogram latency = source.publish_latency();
    write_line(stdout, "frames delivered: " + std::to_string(delivered));
    write_line(stdout, "frames published: " + std::to_string(source.frames_published()));
    write_line(stdout, "ring dropped: " + std::to_string(source.ring().dropped()));
    write_line(stdout, "watchdog restarts: " + std::to_string(source.restarts()));
    write_line(stdout, "reconnects: " + std::to_string(source.reconnects()));
    write_line(stdout, "timestamp resets: " + std::to_string(source.timestamp_resets()));
    write_line(stdout, "publish latency p50=" + format_us(latency.p50_ns()) +
                           " p95=" + format_us(latency.p95_ns()) +
                           " max=" + format_us(latency.max_ns()) +
                           " samples=" + std::to_string(latency.samples()));
    write_line(stdout, std::string("timestamps monotonic: ") + (monotonic ? "yes" : "NO"));
    write_line(stdout, std::string("frame shape matches: ") + (shape_ok ? "yes" : "NO"));

    const bool passed = delivered > 0 && monotonic && shape_ok;
    write_line(stdout, passed ? "capture smoke: PASS" : "capture smoke: FAIL");
    return passed;
}

} // namespace

int main(int argc, char** argv) {
    bool list = false;
    bool show_modes = false;
    unsigned capture_seconds = 0;
    std::string device_id;
    CameraMode forced_mode{};
    PixelFormat forced_format = PixelFormat::nv12;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--list-cameras") {
            list = true;
        } else if (argument == "--modes") {
            show_modes = true;
        } else if (argument == "--capture") {
            if (index + 1 >= argc) {
                write_line(stderr, "--capture needs a duration in seconds");
                return 2;
            }
            capture_seconds = static_cast<unsigned>(std::stoul(argv[++index]));
        } else if (argument == "--mode") {
            if (index + 1 >= argc) {
                write_line(stderr, "--mode needs WxH@FPS, for example 640x480@30");
                return 2;
            }
            const std::string value = argv[++index];
            const std::size_t x = value.find('x');
            const std::size_t at = value.find('@');
            if (x == std::string::npos || at == std::string::npos || x > at) {
                write_line(stderr, "invalid --mode (expected WxH@FPS): " + value);
                return 2;
            }
            forced_mode.width = static_cast<std::uint32_t>(std::stoul(value.substr(0, x)));
            forced_mode.height =
                static_cast<std::uint32_t>(std::stoul(value.substr(x + 1, at - x - 1)));
            forced_mode.fps_numerator =
                static_cast<std::uint32_t>(std::stoul(value.substr(at + 1)));
            forced_mode.fps_denominator = 1;
        } else if (argument == "--format") {
            if (index + 1 >= argc) {
                write_line(stderr, "--format needs nv12, yuy2 or bgra8");
                return 2;
            }
            const std::string value = argv[++index];
            if (value == "nv12") {
                forced_format = PixelFormat::nv12;
            } else if (value == "yuy2") {
                forced_format = PixelFormat::yuy2;
            } else if (value == "bgra8") {
                forced_format = PixelFormat::bgra8;
            } else {
                write_line(stderr, "unknown --format: " + value);
                return 2;
            }
        } else if (argument == "--device") {
            if (index + 1 >= argc) {
                write_line(stderr, "--device needs a symbolic link");
                return 2;
            }
            device_id = argv[++index];
        } else if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        } else {
            write_line(stderr, "unknown argument: " + std::string(argument));
            print_usage();
            return 2;
        }
    }

    if (!list && capture_seconds == 0) {
        print_usage();
        return 2;
    }

    const std::unique_ptr<ICameraDeviceBackend> backend =
        avatar::capture::create_platform_camera_backend();
    std::vector<CameraDevice> devices;
    const avatar::core::Status status = backend->enumerate(devices);
    if (!status.ok()) {
        write_line(stderr, "enumeration failed: " + status.to_string());
        return 1;
    }

    if (list) {
        if (devices.empty()) {
            write_line(stdout, "no camera found (CAP-008)");
        } else {
            write_line(stdout, "capture devices: " + std::to_string(devices.size()));
        }
        for (std::size_t index = 0; index < devices.size(); ++index) {
            write_line(stdout, "  [" + std::to_string(index) + "] " + devices[index].name);
            write_line(stdout, "      id: " + devices[index].id);
            if (!show_modes) {
                continue;
            }
            CameraCapabilities capabilities;
            const avatar::capture::CameraFailure failure =
                backend->capabilities(devices[index].id, capabilities);
            if (!failure.ok()) {
                write_line(stdout, "      modes: unavailable (" +
                                       std::string(avatar::capture::camera_failure_reason_name(
                                           failure.reason)) +
                                       ")");
                continue;
            }
            for (const CameraMode& mode : capabilities.modes) {
                write_line(stdout, "      " + format_mode(mode));
            }
        }
    }

    if (capture_seconds > 0) {
        forced_mode.format = forced_format;
        return capture_smoke(*backend, device_id, capture_seconds, forced_mode) ? 0 : 1;
    }
    return 0;
}
