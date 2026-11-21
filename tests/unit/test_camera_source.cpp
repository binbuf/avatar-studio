// Capture state-machine tests.
//
// The platform seam is a fake ICameraDeviceBackend, so negotiation, timestamp
// mapping, the watchdog, the 2-second reconnect poll, suspend/resume and the
// CAP mapping all run without a camera on every platform. The Windows backend
// is exercised by the --list-cameras smoke and the manual 60-second checklist.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "avatar/capture/camera_device.h"
#include "avatar/capture/camera_source.h"
#include "avatar/core/clock.h"
#include "avatar/core/diagnostics.h"
#include "test_registry.h"

namespace {

using namespace avatar;
using capture::CameraBackendCallbacks;
using capture::CameraBackendErrorKind;
using capture::CameraCapabilities;
using capture::CameraConfig;
using capture::CameraDevice;
using capture::CameraEvent;
using capture::CameraEventKind;
using capture::CameraFailure;
using capture::CameraFailureReason;
using capture::CameraMode;
using capture::CameraSample;
using capture::CameraSource;
using capture::CameraState;
using capture::ColorSpace;
using capture::FrameLease;
using capture::ICameraDeviceBackend;
using capture::PixelFormat;

constexpr core::TimestampNs kStartNs = 1'000'000'000ull;
constexpr core::TimestampNs kFrameIntervalNs = 16'666'667ull;

CameraMode make_mode(std::uint32_t width, std::uint32_t height, std::uint32_t fps,
                     PixelFormat format) {
    CameraMode mode;
    mode.width = width;
    mode.height = height;
    mode.fps_numerator = fps;
    mode.fps_denominator = 1;
    mode.format = format;
    return mode;
}

std::uint32_t packed_stride(const CameraMode& mode) {
    switch (mode.format) {
        case PixelFormat::nv12:
            return mode.width;
        case PixelFormat::yuy2:
            return mode.width * 2u;
        case PixelFormat::bgra8:
        case PixelFormat::rgba8:
            return mode.width * 4u;
        case PixelFormat::mjpeg:
            return 0;
    }
    return 0;
}

std::size_t packed_size(const CameraMode& mode) {
    const std::size_t luma = static_cast<std::size_t>(packed_stride(mode)) * mode.height;
    if (mode.format == PixelFormat::nv12) {
        return luma + luma / 2u;
    }
    return luma;
}

class FakeCameraBackend final : public ICameraDeviceBackend {
public:
    std::vector<CameraDevice> devices{{"fake://camera-0", "Fake Camera"}};
    CameraCapabilities advertised{};
    CameraFailureReason open_failure = CameraFailureReason::none;
    std::vector<CameraMode> rejected_modes{};
    std::vector<CameraMode> open_requests{};
    CameraMode mode{};
    ColorSpace space = ColorSpace::bt709_limited;
    CameraBackendCallbacks callbacks{};
    bool is_running = false;
    int start_calls = 0;
    int stop_calls = 0;
    int open_calls = 0;

    core::Status enumerate(std::vector<CameraDevice>& out) override {
        out = devices;
        return core::Status::success();
    }

    CameraFailure capabilities(std::string_view device_id, CameraCapabilities& out) override {
        (void)device_id;
        out = advertised;
        return CameraFailure{};
    }

    CameraFailure open(std::string_view device_id, const CameraMode& requested) override {
        (void)device_id;
        ++open_calls;
        open_requests.push_back(requested);
        if (open_failure != CameraFailureReason::none) {
            CameraFailure failure;
            failure.reason = open_failure;
            failure.code = core::StatusCode::internal_error;
            failure.message = "fake open failure";
            return failure;
        }
        const bool rejected =
            std::any_of(rejected_modes.begin(), rejected_modes.end(),
                        [&requested](const CameraMode& candidate) {
                            return capture::camera_mode_matches(candidate, requested);
                        });
        if (rejected) {
            CameraFailure failure;
            failure.reason = CameraFailureReason::invalid_format;
            failure.code = core::StatusCode::unsupported;
            failure.message = "fake format rejection";
            return failure;
        }
        mode = requested;
        return CameraFailure{};
    }

    CameraFailure start(const CameraBackendCallbacks& provided) override {
        callbacks = provided;
        is_running = true;
        ++start_calls;
        return CameraFailure{};
    }

    void stop() noexcept override {
        is_running = false;
        ++stop_calls;
    }

    void close() noexcept override { stop(); }

    [[nodiscard]] bool running() const noexcept override { return is_running; }
    [[nodiscard]] CameraMode negotiated_mode() const noexcept override { return mode; }
    [[nodiscard]] ColorSpace color_space() const noexcept override { return space; }
    [[nodiscard]] std::string_view last_error() const noexcept override { return {}; }

    void deliver(std::uint64_t source_timestamp_ns, bool has_timestamp = true) {
        if (!is_running || callbacks.on_sample == nullptr) {
            return;
        }
        const std::size_t stride = packed_stride(mode);
        buffer_.assign(packed_size(mode), 0x80);
        CameraSample sample;
        sample.data = buffer_.data();
        sample.size = buffer_.size();
        sample.width = mode.width;
        sample.height = mode.height;
        sample.format = mode.format;
        sample.color_space = space;
        sample.strides[0] = static_cast<std::uint32_t>(stride);
        sample.plane_offsets[0] = 0;
        sample.plane_sizes[0] = static_cast<std::size_t>(stride) * mode.height;
        if (mode.format == PixelFormat::nv12) {
            sample.strides[1] = static_cast<std::uint32_t>(stride);
            sample.plane_offsets[1] = sample.plane_sizes[0];
            sample.plane_sizes[1] = sample.plane_sizes[0] / 2u;
        }
        sample.source_timestamp_ns = source_timestamp_ns;
        sample.has_source_timestamp = has_timestamp;
        callbacks.on_sample(callbacks.context, sample);
    }

    void report(CameraBackendErrorKind kind, std::int32_t hresult = 0) {
        if (callbacks.on_error != nullptr) {
            callbacks.on_error(callbacks.context, kind, hresult);
        }
    }

private:
    std::vector<std::uint8_t> buffer_{};
};

struct Fixture {
    explicit Fixture(CameraConfig config_in = CameraConfig{})
        : config(std::move(config_in)), source(backend, clock, config) {}

    FakeCameraBackend backend{};
    core::ManualTimeSource time{kStartNs};
    core::FrameClock clock{time};
    CameraConfig config{};
    CameraSource source;

    std::vector<CameraEvent> events() { return source.take_events(); }

    static bool has_event(const std::vector<CameraEvent>& events, CameraEventKind kind,
                          core::DiagnosticCode code) {
        return std::any_of(events.begin(), events.end(),
                           [kind, code](const CameraEvent& event) {
                               return event.kind == kind && event.code == code;
                           });
    }

    static std::size_t count_event(const std::vector<CameraEvent>& events,
                                   core::DiagnosticCode code) {
        return static_cast<std::size_t>(std::count_if(
            events.begin(), events.end(),
            [code](const CameraEvent& event) { return event.code == code; }));
    }
};

void configure_single_mode(Fixture& fixture, const CameraMode& mode) {
    fixture.backend.advertised.modes = {mode};
}

// ---------------------------------------------------------------------------
// Enumeration, negotiation and open.
// ---------------------------------------------------------------------------

TEST_CASE(camera_enumerates_opens_and_streams_the_first_device) {
    Fixture fixture;
    configure_single_mode(fixture, make_mode(1280, 720, 60, PixelFormat::nv12));

    CHECK(!fixture.source.produce());
    CHECK(fixture.source.state() == CameraState::closed);

    const core::Status status = fixture.source.start();
    CHECK(status.ok());
    CHECK(fixture.source.state() == CameraState::streaming);
    CHECK(fixture.source.device_id() == "fake://camera-0");
    CHECK(fixture.source.device_name() == "Fake Camera");
    CHECK_EQ(fixture.source.negotiated_mode().width, 1280u);
    CHECK_EQ(fixture.source.negotiated_mode().height, 720u);
    CHECK(fixture.source.negotiated_mode().format == PixelFormat::nv12);
    CHECK_EQ(fixture.backend.open_calls, 1);
    CHECK_EQ(fixture.backend.start_calls, 1);
    CHECK(fixture.backend.is_running);

    const capture::SourceInfo info = fixture.source.info();
    CHECK_EQ(info.width, 1280u);
    CHECK_EQ(info.height, 720u);
    CHECK(info.format == PixelFormat::nv12);
    CHECK(!info.synthetic);
}

TEST_CASE(camera_steps_down_to_an_advertised_format_and_emits_cap_003) {
    Fixture fixture;
    fixture.backend.advertised.modes = {
        make_mode(1280, 720, 60, PixelFormat::yuy2),
        make_mode(1920, 1080, 30, PixelFormat::yuy2),
    };

    const core::Status status = fixture.source.start();
    CHECK(status.ok());
    CHECK(fixture.source.negotiated_mode().format == PixelFormat::yuy2);
    CHECK_EQ(fixture.source.negotiated_mode().width, 1280u);
    const std::vector<CameraEvent> events = fixture.events();
    CHECK(Fixture::has_event(events, CameraEventKind::format_rejected,
                             core::DiagnosticCode::cap_003));
}

TEST_CASE(camera_steps_down_after_the_backend_rejects_a_mode) {
    Fixture fixture;
    fixture.backend.advertised.modes = {
        make_mode(1280, 720, 60, PixelFormat::nv12),
        make_mode(1280, 720, 60, PixelFormat::yuy2),
    };
    fixture.backend.rejected_modes = {make_mode(1280, 720, 60, PixelFormat::nv12)};

    const core::Status status = fixture.source.start();
    CHECK(status.ok());
    CHECK(fixture.source.negotiated_mode().format == PixelFormat::yuy2);
    CHECK_EQ(fixture.backend.open_requests.size(), std::size_t{2});
    const std::vector<CameraEvent> events = fixture.events();
    CHECK(Fixture::count_event(events, core::DiagnosticCode::cap_003) >= 1);
}

TEST_CASE(camera_maps_busy_and_permission_failures) {
    Fixture busy;
    configure_single_mode(busy, make_mode(1280, 720, 60, PixelFormat::nv12));
    busy.backend.open_failure = CameraFailureReason::busy;
    const core::Status busy_status = busy.source.start();
    CHECK(!busy_status.ok());
    CHECK(busy.source.state() == CameraState::busy);
    CHECK(Fixture::has_event(busy.events(), CameraEventKind::device_busy,
                             core::DiagnosticCode::cap_002));

    Fixture denied;
    configure_single_mode(denied, make_mode(1280, 720, 60, PixelFormat::nv12));
    denied.backend.open_failure = CameraFailureReason::permission_denied;
    const core::Status denied_status = denied.source.start();
    CHECK(!denied_status.ok());
    CHECK(denied.source.state() == CameraState::permission_denied);
    CHECK(Fixture::has_event(denied.events(), CameraEventKind::permission_denied,
                             core::DiagnosticCode::cap_006));
}

TEST_CASE(camera_reports_no_camera_with_cap_008) {
    Fixture fixture;
    fixture.backend.devices.clear();
    const core::Status status = fixture.source.start();
    CHECK(!status.ok());
    CHECK(fixture.source.state() == CameraState::closed);
    CHECK(Fixture::has_event(fixture.events(), CameraEventKind::no_camera,
                             core::DiagnosticCode::cap_008));
}

// ---------------------------------------------------------------------------
// Timestamps.
// ---------------------------------------------------------------------------

TEST_CASE(camera_maps_source_timestamps_to_frame_clock_and_preserves_arrival) {
    Fixture fixture;
    configure_single_mode(fixture, make_mode(1280, 720, 60, PixelFormat::nv12));
    CHECK(fixture.source.start().ok());

    fixture.backend.deliver(0);
    CHECK(fixture.source.produce());
    std::optional<FrameLease> first = fixture.source.ring().try_acquire_latest();
    CHECK(first.has_value());
    if (first) {
        CHECK_EQ(first->frame().timestamp_ns, kStartNs);
        CHECK_EQ(first->frame().arrival_ns, kStartNs);
        CHECK_EQ(first->frame().sequence, 1u);
        CHECK(first->frame().format == PixelFormat::nv12);
        CHECK_EQ(first->frame().stride(0), 1280u);
        CHECK(first->frame().plane(0) != nullptr);
        CHECK(first->frame().plane(1) != nullptr);
    }
    CHECK(!fixture.source.produce());

    fixture.time.advance(kFrameIntervalNs);
    fixture.backend.deliver(kFrameIntervalNs);
    CHECK(fixture.source.produce());
    std::optional<FrameLease> second = fixture.source.ring().try_acquire_latest();
    CHECK(second.has_value());
    if (second) {
        CHECK_EQ(second->frame().timestamp_ns, kStartNs + kFrameIntervalNs);
        CHECK_EQ(second->frame().arrival_ns, kStartNs + kFrameIntervalNs);
    }
}

TEST_CASE(camera_remaps_backwards_timestamps_once_per_episode) {
    Fixture fixture;
    configure_single_mode(fixture, make_mode(1280, 720, 60, PixelFormat::nv12));
    CHECK(fixture.source.start().ok());

    fixture.time.advance(100'000'000);
    fixture.backend.deliver(100'000'000);
    CHECK(fixture.source.produce());

    fixture.time.advance(10'000'000);
    fixture.backend.deliver(50'000'000);
    CHECK(fixture.source.produce());
    const core::TimestampNs arrival = kStartNs + 110'000'000;
    std::optional<FrameLease> regressed = fixture.source.ring().try_acquire_latest();
    CHECK(regressed.has_value());
    if (regressed) {
        CHECK(regressed->frame().timestamp_ns > kStartNs + 100'000'000);
        CHECK_EQ(regressed->frame().arrival_ns, arrival);
    }
    std::vector<CameraEvent> events = fixture.events();
    CHECK_EQ(Fixture::count_event(events, core::DiagnosticCode::cap_005), std::size_t{1});
    CHECK_EQ(fixture.source.timestamp_resets(), 1u);

    // A second backwards sample in the same episode does not repeat the log.
    fixture.time.advance(10'000'000);
    fixture.backend.deliver(40'000'000);
    CHECK(fixture.source.produce());
    events = fixture.events();
    CHECK_EQ(Fixture::count_event(events, core::DiagnosticCode::cap_005), std::size_t{0});

    // A good sample ends the episode; a later regression is reported again.
    fixture.time.advance(10'000'000);
    fixture.backend.deliver(200'000'000);
    CHECK(fixture.source.produce());
    fixture.time.advance(10'000'000);
    fixture.backend.deliver(10'000'000);
    CHECK(fixture.source.produce());
    events = fixture.events();
    CHECK_EQ(Fixture::count_event(events, core::DiagnosticCode::cap_005), std::size_t{1});
    CHECK_EQ(fixture.source.timestamp_resets(), 2u);
}

// ---------------------------------------------------------------------------
// Watchdog, device loss and reconnect.
// ---------------------------------------------------------------------------

TEST_CASE(camera_watchdog_restarts_a_stalled_stream_once_then_reports) {
    CameraConfig config;
    config.startup_grace_ns = 0;
    config.watchdog_min_deadline_ns = 0;
    Fixture fixture{config};
    configure_single_mode(fixture, make_mode(1280, 720, 60, PixelFormat::nv12));
    CHECK(fixture.source.start().ok());
    fixture.backend.deliver(0);
    CHECK(fixture.source.produce());

    fixture.time.advance(200'000'000);
    CHECK(!fixture.source.produce());
    CHECK_EQ(fixture.source.restarts(), 1u);
    CHECK_EQ(fixture.backend.start_calls, 2);
    CHECK(fixture.source.state() == CameraState::streaming);
    std::vector<CameraEvent> events = fixture.events();
    CHECK(Fixture::has_event(events, CameraEventKind::stream_stalled,
                             core::DiagnosticCode::cap_004));
    CHECK(Fixture::has_event(events, CameraEventKind::stream_restarted,
                             core::DiagnosticCode::unknown));

    fixture.time.advance(200'000'000);
    CHECK(!fixture.source.produce());
    CHECK(fixture.source.state() == CameraState::stalled);
    events = fixture.events();
    CHECK(Fixture::has_event(events, CameraEventKind::stream_stalled,
                             core::DiagnosticCode::cap_004));
}

TEST_CASE(camera_demotes_a_mode_that_cannot_sustain_its_frame_rate) {
    FakeCameraBackend backend;
    backend.advertised.modes = {make_mode(1920, 1080, 30, PixelFormat::nv12),
                                make_mode(640, 480, 30, PixelFormat::nv12)};
    core::ManualTimeSource time{kStartNs};
    core::FrameClock clock{time};
    CameraConfig config;
    config.restart_on_stall = false;
    config.startup_grace_ns = 0;
    config.demote_after_ns = 1'000'000'000ull;
    CameraSource source(backend, clock, config);

    CHECK(source.start().ok());
    CHECK_EQ(source.negotiated_mode().width, 1920u);

    for (int index = 0; index < 5; ++index) {
        time.advance(300'000'000);
        backend.deliver(static_cast<std::uint64_t>(index) * 33'000'000);
        CHECK(source.produce());
    }

    CHECK_EQ(source.negotiated_mode().width, 640u);
    CHECK(source.state() == CameraState::streaming);
    const std::vector<CameraEvent> events = source.take_events();
    CHECK(Fixture::count_event(events, core::DiagnosticCode::cap_003) >= 1);
}

TEST_CASE(camera_reconnects_after_device_loss_on_the_two_second_poll) {
    Fixture fixture;
    configure_single_mode(fixture, make_mode(1280, 720, 60, PixelFormat::nv12));
    CHECK(fixture.source.start().ok());
    fixture.backend.deliver(0);
    CHECK(fixture.source.produce());

    fixture.backend.report(CameraBackendErrorKind::device_lost, -2147024891);
    fixture.time.advance(10'000'000);
    CHECK(!fixture.source.produce());
    CHECK(fixture.source.state() == CameraState::device_lost);
    std::vector<CameraEvent> events = fixture.events();
    CHECK(Fixture::has_event(events, CameraEventKind::device_lost,
                             core::DiagnosticCode::cap_001));
    CHECK_EQ(fixture.backend.start_calls, 1);

    fixture.time.advance(1'900'000'000);
    CHECK(!fixture.source.produce());
    CHECK(fixture.source.state() == CameraState::device_lost);
    CHECK_EQ(fixture.backend.start_calls, 1);

    fixture.time.advance(200'000'000);
    CHECK(!fixture.source.produce());
    CHECK(fixture.source.state() == CameraState::streaming);
    CHECK_EQ(fixture.source.reconnects(), 1u);
    CHECK_EQ(fixture.backend.start_calls, 2);
    events = fixture.events();
    CHECK(Fixture::has_event(events, CameraEventKind::device_restored,
                             core::DiagnosticCode::unknown));
}

TEST_CASE(camera_reports_a_failed_reopen_once_and_recovers_later) {
    Fixture fixture;
    configure_single_mode(fixture, make_mode(1280, 720, 60, PixelFormat::nv12));
    CHECK(fixture.source.start().ok());
    fixture.backend.deliver(0);
    CHECK(fixture.source.produce());

    fixture.backend.report(CameraBackendErrorKind::device_lost, 0);
    CHECK(!fixture.source.produce());
    fixture.backend.open_failure = CameraFailureReason::internal;

    fixture.time.advance(2'000'000'000);
    CHECK(!fixture.source.produce());
    CHECK(fixture.source.state() == CameraState::device_lost);
    std::vector<CameraEvent> events = fixture.events();
    CHECK_EQ(Fixture::count_event(events, core::DiagnosticCode::cap_007), std::size_t{1});

    fixture.time.advance(2'000'000'000);
    CHECK(!fixture.source.produce());
    events = fixture.events();
    CHECK_EQ(Fixture::count_event(events, core::DiagnosticCode::cap_007), std::size_t{0});

    fixture.backend.open_failure = CameraFailureReason::none;
    fixture.time.advance(2'000'000'000);
    CHECK(!fixture.source.produce());
    CHECK(fixture.source.state() == CameraState::streaming);
    CHECK_EQ(fixture.source.reconnects(), 1u);
}

TEST_CASE(camera_maps_a_mid_stream_preemption_to_cap_002_and_recovers) {
    Fixture fixture;
    configure_single_mode(fixture, make_mode(1280, 720, 30, PixelFormat::nv12));
    CHECK(fixture.source.start().ok());
    fixture.backend.deliver(0);
    CHECK(fixture.source.produce());

    fixture.backend.report(CameraBackendErrorKind::busy,
                           static_cast<std::int32_t>(0xC00D3704u));
    CHECK(!fixture.source.produce());
    CHECK(fixture.source.state() == CameraState::busy);
    CHECK(Fixture::has_event(fixture.events(), CameraEventKind::device_busy,
                             core::DiagnosticCode::cap_002));

    fixture.time.advance(2'000'000'000);
    CHECK(!fixture.source.produce());
    CHECK(fixture.source.state() == CameraState::streaming);
    CHECK_EQ(fixture.source.reconnects(), 1u);
    CHECK(Fixture::has_event(fixture.events(), CameraEventKind::device_restored,
                             core::DiagnosticCode::unknown));
}

TEST_CASE(camera_suspend_and_resume_reset_the_timestamp_base) {
    Fixture fixture;
    configure_single_mode(fixture, make_mode(1280, 720, 60, PixelFormat::nv12));
    CHECK(fixture.source.start().ok());
    fixture.backend.deliver(0);
    CHECK(fixture.source.produce());
    std::optional<FrameLease> lease = fixture.source.ring().try_acquire_latest();
    CHECK(lease.has_value());

    fixture.time.advance(5'000'000'000);
    fixture.source.suspend();
    CHECK(fixture.source.state() == CameraState::suspended);
    CHECK(!fixture.backend.is_running);
    // Published frames remain readable across a suspend.
    CHECK(lease.has_value());
    lease.reset();

    fixture.time.advance(1'000'000'000);
    CHECK(fixture.source.resume().ok());
    CHECK(fixture.source.state() == CameraState::streaming);
    CHECK(fixture.backend.is_running);

    fixture.backend.deliver(0);
    CHECK(fixture.source.produce());
    std::optional<FrameLease> resumed = fixture.source.ring().try_acquire_latest();
    CHECK(resumed.has_value());
    if (resumed) {
        CHECK_EQ(resumed->frame().timestamp_ns, fixture.time.now_ns());
        CHECK_EQ(resumed->frame().arrival_ns, fixture.time.now_ns());
    }
    const std::vector<CameraEvent> events = fixture.events();
    CHECK(Fixture::has_event(events, CameraEventKind::suspended, core::DiagnosticCode::unknown));
    CHECK(Fixture::has_event(events, CameraEventKind::resumed, core::DiagnosticCode::unknown));
}

// ---------------------------------------------------------------------------
// Ring ownership and instrumentation.
// ---------------------------------------------------------------------------

TEST_CASE(camera_drops_frames_when_every_ring_slot_is_pinned) {
    Fixture fixture;
    configure_single_mode(fixture, make_mode(64, 48, 30, PixelFormat::nv12));
    CHECK(fixture.source.start().ok());
    CHECK_EQ(fixture.source.ring().capacity(), 4u);

    std::vector<FrameLease> leases;
    for (int index = 0; index < 4; ++index) {
        fixture.time.advance(kFrameIntervalNs);
        fixture.backend.deliver(static_cast<std::uint64_t>(index) * kFrameIntervalNs);
        std::optional<FrameLease> lease = fixture.source.ring().try_acquire_latest();
        CHECK(lease.has_value());
        if (lease) {
            leases.push_back(std::move(*lease));
        }
    }
    CHECK_EQ(leases.size(), std::size_t{4});
    const std::uint64_t dropped_before = fixture.source.ring().dropped();

    fixture.time.advance(kFrameIntervalNs);
    fixture.backend.deliver(5 * kFrameIntervalNs);
    CHECK(fixture.source.ring().dropped() > dropped_before);

    leases.clear();
    fixture.time.advance(kFrameIntervalNs);
    fixture.backend.deliver(6 * kFrameIntervalNs);
    CHECK(fixture.source.produce());
}

TEST_CASE(camera_records_callback_to_publish_latency) {
    Fixture fixture;
    configure_single_mode(fixture, make_mode(320, 240, 30, PixelFormat::nv12));
    CHECK(fixture.source.start().ok());
    for (int index = 0; index < 10; ++index) {
        fixture.time.advance(kFrameIntervalNs);
        fixture.backend.deliver(static_cast<std::uint64_t>(index) * kFrameIntervalNs);
        CHECK(fixture.source.produce());
    }
    const capture::CaptureLatencyHistogram latency = fixture.source.publish_latency();
    CHECK_EQ(latency.samples(), 10u);
    CHECK(latency.p50_ns() <= latency.max_ns() + capture::CaptureLatencyHistogram::kBucketWidthNs);
    CHECK_EQ(fixture.source.frames_published(), 10u);
}

TEST_CASE(camera_reports_a_read_error_as_a_stalled_stream) {
    Fixture fixture;
    configure_single_mode(fixture, make_mode(1280, 720, 60, PixelFormat::nv12));
    CHECK(fixture.source.start().ok());
    fixture.backend.deliver(0);
    CHECK(fixture.source.produce());

    fixture.backend.report(CameraBackendErrorKind::read_error, 0);
    CHECK(!fixture.source.produce());
    CHECK(fixture.source.state() == CameraState::stalled);
    CHECK(Fixture::has_event(fixture.events(), CameraEventKind::read_error,
                             core::DiagnosticCode::cap_004));
}

// ---------------------------------------------------------------------------
// Pure helpers.
// ---------------------------------------------------------------------------

TEST_CASE(camera_default_preferences_follow_the_documented_order) {
    const std::vector<CameraMode> preferences = capture::default_camera_preferences();
    CHECK_EQ(preferences.size(), std::size_t{12});
    CHECK(preferences.front().format == PixelFormat::nv12);
    CHECK_EQ(preferences.front().width, 1280u);
    CHECK_EQ(preferences.front().height, 720u);
    CHECK_EQ(preferences.front().fps_numerator, 60u);
    CHECK(preferences[1].format == PixelFormat::nv12);
    CHECK_EQ(preferences[1].width, 1920u);
    CHECK_EQ(preferences[1].height, 1080u);
    CHECK_EQ(preferences[1].fps_numerator, 30u);
    CHECK(preferences[4].format == PixelFormat::yuy2);
    CHECK(preferences[8].format == PixelFormat::bgra8);
    const bool has_mjpeg =
        std::any_of(preferences.begin(), preferences.end(), [](const CameraMode& mode) {
            return mode.format == PixelFormat::mjpeg;
        });
    CHECK(!has_mjpeg);
}

TEST_CASE(camera_mode_selection_prefers_exact_modes_then_the_closest) {
    const std::vector<CameraMode> preferences = capture::default_camera_preferences();

    CameraCapabilities exact;
    exact.modes = {make_mode(1920, 1080, 30, PixelFormat::nv12),
                   make_mode(1280, 720, 60, PixelFormat::nv12)};
    const std::vector<CameraMode> selected = capture::select_camera_modes(exact, preferences);
    CHECK_EQ(selected.size(), std::size_t{2});
    CHECK_EQ(selected[0].width, 1280u);
    CHECK_EQ(selected[1].width, 1920u);

    CameraCapabilities closest;
    closest.modes = {make_mode(800, 600, 30, PixelFormat::yuy2)};
    const std::vector<CameraMode> fallback = capture::select_camera_modes(closest, preferences);
    CHECK_EQ(fallback.size(), std::size_t{1});
    CHECK_EQ(fallback.front().width, 800u);
    CHECK(fallback.front().format == PixelFormat::yuy2);

    CameraCapabilities compressed;
    compressed.modes = {make_mode(1920, 1080, 30, PixelFormat::mjpeg)};
    CHECK(capture::select_camera_modes(compressed, preferences).empty());
}

TEST_CASE(camera_hresult_classification_matches_the_documented_states) {
    CHECK(capture::classify_camera_hresult(static_cast<std::int32_t>(0x80070005u)) ==
          CameraFailureReason::permission_denied);
    CHECK(capture::classify_camera_hresult(static_cast<std::int32_t>(0x800700AAu)) ==
          CameraFailureReason::busy);
    CHECK(capture::classify_camera_hresult(static_cast<std::int32_t>(0x8007001Fu)) ==
          CameraFailureReason::busy);
    CHECK(capture::classify_camera_hresult(static_cast<std::int32_t>(0xC00D36B4u)) ==
          CameraFailureReason::invalid_format);
    CHECK(capture::classify_camera_hresult(static_cast<std::int32_t>(0xC00D3EA2u)) ==
          CameraFailureReason::device_lost);
    CHECK(capture::classify_camera_hresult(static_cast<std::int32_t>(0xC00D3704u)) ==
          CameraFailureReason::busy);
    CHECK(capture::classify_camera_hresult(static_cast<std::int32_t>(0x8007048Fu)) ==
          CameraFailureReason::device_gone);
    CHECK(capture::classify_camera_hresult(0) == CameraFailureReason::internal);
    CHECK(capture::camera_mode_matches(make_mode(1280, 720, 30, PixelFormat::nv12),
                                       make_mode(1280, 720, 30, PixelFormat::nv12)));
    CHECK(!capture::camera_mode_matches(make_mode(1280, 720, 30, PixelFormat::nv12),
                                        make_mode(1280, 720, 60, PixelFormat::nv12)));
}

} // namespace
