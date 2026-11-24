#include "avatar_live_pipeline.h"

#include "avatar/capture/synthetic_face_source.h"
#include "avatar/capture/video_frame.h"
#include "avatar/tracking/scripted_tracker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <string>

// clang-format off
// psapi.h requires windows.h first; clang-format would reorder them.
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif
// clang-format on

namespace {

using avatar::capture::CameraConfig;
using avatar::capture::SyntheticFaceConfig;
using avatar::capture::SyntheticFaceSource;
using avatar::core::TimestampNs;
using avatar::tracking::CabiTracker;
using avatar::tracking::LandmarkMap;
using avatar::tracking::ScriptedTracker;
using avatar::tracking::ScriptedTrackerConfig;
using avatar::tracking::TrackerConfig;
using avatar::tracking::TrackerStats;
using avatar::tracking::TrackingResult;

/// Tracker seam used when no real backend could be loaded. It accepts frames and
/// produces nothing, so the HUD reports TRK-007 honestly instead of inventing a
/// face.
class NullTracker final : public avatar::tracking::ITrackingBackend {
public:
    bool initialize(const TrackerConfig&) override { return true; }
    void submit(const avatar::capture::VideoFrame&) override {}
    std::optional<TrackingResult> latest() override { return std::nullopt; }
    TrackerStats stats() const override { return {}; }
    void shutdown() override {}
};

godot::String camera_name(avatar::live::CameraState state) {
    switch (state) {
        case avatar::live::CameraState::ok:
            return "ok";
        case avatar::live::CameraState::lost:
            return "lost";
        case avatar::live::CameraState::busy:
            return "busy";
        case avatar::live::CameraState::denied:
            return "denied";
        case avatar::live::CameraState::none:
            return "none";
    }
    return "none";
}

godot::String face_name(avatar::live::FaceState state) {
    switch (state) {
        case avatar::live::FaceState::tracked:
            return "tracked";
        case avatar::live::FaceState::not_visible:
            return "not_visible";
        case avatar::live::FaceState::features_hidden:
            return "features_hidden";
        case avatar::live::FaceState::degraded:
            return "degraded";
        case avatar::live::FaceState::unavailable:
            return "unavailable";
    }
    return "unavailable";
}

double ns_to_ms(std::uint64_t ns) {
    return static_cast<double>(ns) / 1'000'000.0;
}

std::uint8_t clamp_byte(float value) {
    const int rounded = static_cast<int>(value + 0.5F);
    return static_cast<std::uint8_t>(std::clamp(rounded, 0, 255));
}

/// Converts one ring frame to an RGB8 Godot image for the development snapshot.
/// This runs only when the operator presses Snapshot, never on the frame path.
godot::Ref<godot::Image> frame_to_image(const avatar::capture::VideoFrame& frame) {
    const int width = static_cast<int>(frame.width);
    const int height = static_cast<int>(frame.height);
    if (width <= 0 || height <= 0) {
        return {};
    }
    const auto* plane0 = frame.plane(0);
    if (plane0 == nullptr) {
        return {};
    }
    godot::PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(width) * static_cast<int64_t>(height) * 3);
    auto* out = bytes.ptrw();
    const auto put = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)) *
                                  3u;
        out[index] = r;
        out[index + 1] = g;
        out[index + 2] = b;
    };

    if (frame.format == avatar::capture::PixelFormat::bgra8 ||
        frame.format == avatar::capture::PixelFormat::rgba8) {
        const bool bgra = frame.format == avatar::capture::PixelFormat::bgra8;
        const std::size_t stride = frame.stride(0);
        for (int y = 0; y < height; ++y) {
            const std::uint8_t* row = plane0 + static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < width; ++x) {
                const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4u;
                put(x, y, bgra ? pixel[2] : pixel[0], pixel[1], bgra ? pixel[0] : pixel[2]);
            }
        }
        return godot::Image::create_from_data(width, height, false, godot::Image::FORMAT_RGB8,
                                              bytes);
    }

    if (frame.format == avatar::capture::PixelFormat::nv12) {
        const auto* uv_plane = frame.plane(1);
        if (uv_plane == nullptr) {
            return {};
        }
        const auto space = frame.color_space;
        const bool full = space == avatar::capture::ColorSpace::bt601_full ||
                          space == avatar::capture::ColorSpace::bt709_full;
        const bool bt709 = space == avatar::capture::ColorSpace::bt709_full ||
                           space == avatar::capture::ColorSpace::bt709_limited;
        const std::size_t y_stride = frame.stride(0);
        const std::size_t uv_stride = frame.stride(1);
        for (int y = 0; y < height; ++y) {
            const std::uint8_t* y_row = plane0 + static_cast<std::size_t>(y) * y_stride;
            const std::uint8_t* uv_row = uv_plane + static_cast<std::size_t>(y / 2) * uv_stride;
            for (int x = 0; x < width; ++x) {
                const float luma = full ? static_cast<float>(y_row[x])
                                        : (static_cast<float>(y_row[x]) - 16.0F) * 1.164F;
                const std::uint8_t* uv = uv_row + static_cast<std::size_t>(x / 2) * 2u;
                const float u = static_cast<float>(uv[0]) - 128.0F;
                const float v = static_cast<float>(uv[1]) - 128.0F;
                float r = 0.0F;
                float g = 0.0F;
                float b = 0.0F;
                if (bt709) {
                    r = luma + 1.793F * v;
                    g = luma - 0.213F * u - 0.533F * v;
                    b = luma + 2.112F * u;
                } else {
                    r = luma + 1.596F * v;
                    g = luma - 0.391F * u - 0.813F * v;
                    b = luma + 2.018F * u;
                }
                put(x, y, clamp_byte(r), clamp_byte(g), clamp_byte(b));
            }
        }
        return godot::Image::create_from_data(width, height, false, godot::Image::FORMAT_RGB8,
                                              bytes);
    }
    return {};
}

godot::Dictionary histogram_ms(const avatar::live::AppLatencyHistogram& histogram) {
    godot::Dictionary out;
    out["samples"] = static_cast<int64_t>(histogram.samples());
    out["p50"] = ns_to_ms(histogram.p50_ns());
    out["p95"] = ns_to_ms(histogram.p95_ns());
    out["p99"] = ns_to_ms(histogram.p99_ns());
    out["mean"] = ns_to_ms(static_cast<std::uint64_t>(histogram.mean_ns()));
    out["max"] = ns_to_ms(histogram.max_ns());
    out["min"] = ns_to_ms(histogram.min_ns());
    return out;
}

std::uint64_t process_cpu_ns() {
#if defined(_WIN32)
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0) {
        return 0;
    }
    auto to_ns = [](const FILETIME& time) -> std::uint64_t {
        ULARGE_INTEGER value{};
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        // FILETIME is 100 ns units.
        return static_cast<std::uint64_t>(value.QuadPart) * 100ull;
    };
    return to_ns(kernel) + to_ns(user);
#else
    std::FILE* file = std::fopen("/proc/self/stat", "r");
    if (file == nullptr) {
        return 0;
    }
    std::uint64_t utime = 0;
    std::uint64_t stime = 0;
    const int matched = std::fscanf(
        file, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime);
    std::fclose(file);
    if (matched != 2) {
        return 0;
    }
    const long ticks = sysconf(_SC_CLK_TCK);
    const std::uint64_t hz = ticks > 0 ? static_cast<std::uint64_t>(ticks) : 100ull;
    return (utime + stime) * 1'000'000'000ull / hz;
#endif
}

} // namespace

AvatarLivePipeline::AvatarLivePipeline() = default;

AvatarLivePipeline::~AvatarLivePipeline() {
    shutdown();
}

bool AvatarLivePipeline::initialize(const godot::Dictionary& config) {
    shutdown();
    source_kind_ = config.get("source", godot::String("camera"));
    camera_device_ = config.get("camera", godot::String(""));
    const godot::String tracker_path =
        godot::String(config.get("tracker", godot::String(""))).strip_edges();
    const int64_t tracker_input =
        static_cast<int64_t>(config.get("tracker_input", static_cast<int64_t>(0)));
    tracker_input_ = tracker_input;
    const int64_t fps = static_cast<int64_t>(config.get("fps", static_cast<int64_t>(60)));

    if (source_kind_ == "synthetic") {
        SyntheticFaceConfig synthetic;
        synthetic.width = 1280;
        synthetic.height = 720;
        synthetic.format = avatar::capture::PixelFormat::bgra8;
        synthetic_source_ = new SyntheticFaceSource(clock_, synthetic);
        source_.reset(synthetic_source_);
    } else if (source_kind_ == "still") {
        still_path_ = godot::String(config.get("still_image", godot::String(""))).strip_edges();
        if (still_path_.is_empty()) {
            last_error_ = "still source requires still_image";
            return false;
        }
        godot::Ref<godot::Image> image;
        image.instantiate();
        const godot::Error load_error = image->load(still_path_);
        if (load_error != godot::OK || image->is_empty()) {
            last_error_ = "still image not found or unreadable: " + still_path_;
            return false;
        }
        image->convert(godot::Image::FORMAT_RGB8);
        const int width = image->get_width();
        const int height = image->get_height();
        if (width <= 0 || height <= 0) {
            last_error_ = "still image has no dimensions: " + still_path_;
            return false;
        }
        const godot::PackedByteArray bytes = image->get_data();
        avatar::capture::StillImageConfig still;
        still.width = static_cast<std::uint32_t>(width);
        still.height = static_cast<std::uint32_t>(height);
        still.format = avatar::capture::PixelFormat::bgra8;
        still.fps = static_cast<std::uint32_t>(std::max<int64_t>(fps, 1));
        still.rgb.assign(bytes.ptr(), bytes.ptr() + bytes.size());
        still_source_ = new avatar::capture::StillImageSource(clock_, std::move(still));
        source_.reset(still_source_);
    } else {
        source_kind_ = "camera";
        backend_ = avatar::capture::create_platform_camera_backend();
        if (backend_ == nullptr) {
            last_error_ = "no platform camera backend";
            return false;
        }
        CameraConfig camera;
        camera.device_id = camera_device_.utf8().get_data();
        // The T24 ring uploads NV12 and BGRA/RGBA only. Keep the platform
        // preference order but drop YUY2/MJPEG so a throughput step-down cannot
        // land on a format the decal cannot sample.
        const int64_t request_width =
            static_cast<int64_t>(config.get("capture_width", static_cast<int64_t>(0)));
        const int64_t request_height =
            static_cast<int64_t>(config.get("capture_height", static_cast<int64_t>(0)));
        const int64_t request_fps =
            static_cast<int64_t>(config.get("capture_fps", static_cast<int64_t>(0)));
        capture_request_ = "auto";
        if (request_width > 0 && request_height > 0 && request_fps > 0) {
            capture_request_ = godot::String(std::to_string(request_width).c_str()) + "x" +
                               godot::String(std::to_string(request_height).c_str()) + "@" +
                               godot::String(std::to_string(request_fps).c_str());
            // Exact requested mode first, then the standard fallback list. The
            // camera source reports the negotiated mode, so a request the C920
            // does not advertise is visible rather than silently replaced.
            for (const avatar::capture::PixelFormat format :
                 {avatar::capture::PixelFormat::nv12, avatar::capture::PixelFormat::bgra8}) {
                avatar::capture::CameraMode mode;
                mode.width = static_cast<std::uint32_t>(request_width);
                mode.height = static_cast<std::uint32_t>(request_height);
                mode.fps_numerator = static_cast<std::uint32_t>(request_fps);
                mode.fps_denominator = 1;
                mode.format = format;
                camera.preferences.push_back(mode);
            }
        }
        for (const auto& mode : avatar::capture::default_camera_preferences()) {
            if (mode.format == avatar::capture::PixelFormat::nv12 ||
                mode.format == avatar::capture::PixelFormat::bgra8) {
                camera.preferences.push_back(mode);
            }
        }
        camera_source_ = new avatar::capture::CameraSource(*backend_, clock_, camera);
        source_.reset(camera_source_);
    }

    if (tracker_path == "scripted") {
        ScriptedTrackerConfig scripted;
        scripted.scenario = avatar::tracking::ScriptedScenario::valid_tracking;
        scripted.frame_count = 600;
        scripted.frame_interval_ns =
            fps > 0 ? 1'000'000'000ull / static_cast<uint64_t>(fps) : 16'666'667ull;
        tracker_ = std::make_unique<ScriptedTracker>(scripted);
        tracker_kind_ = "scripted";
        tracker_available_ = true;
    } else if (!tracker_path.is_empty()) {
        auto cabi = std::make_unique<CabiTracker>(tracker_path.utf8().get_data());
        std::string error;
        TrackerConfig tracker_config;
        tracker_config.input_width =
            static_cast<std::uint32_t>(std::max<int64_t>(tracker_input, 0));
        tracker_config.input_height = tracker_config.input_width;
        tracker_config.target_fps = static_cast<std::uint32_t>(std::max<int64_t>(fps, 1));
        if (!cabi->load(error) || !cabi->initialize(tracker_config)) {
            last_error_ = godot::String(error.empty() ? cabi->last_error().c_str() : error.c_str());
            tracker_ = std::make_unique<NullTracker>();
            tracker_kind_ = "none";
            tracker_available_ = false;
        } else {
            cabi_tracker_ = cabi.get();
            tracker_ = std::move(cabi);
            tracker_kind_ = "cabi";
            tracker_available_ = true;
        }
    } else {
        tracker_ = std::make_unique<NullTracker>();
        tracker_kind_ = "none";
        tracker_available_ = false;
    }

    rig_ = std::make_unique<avatar::rig::FaceRig>(LandmarkMap::mediapipe(), 16.0F / 9.0F);
    rig_->configure({});
    pipeline_ = std::make_unique<avatar::live::LivePipeline>(*source_, *tracker_, *rig_);
    pipeline_->set_tracker_available(tracker_available_);
    initialized_ = true;
    return true;
}

void AvatarLivePipeline::shutdown() {
    stop();
    pipeline_.reset();
    rig_.reset();
    tracker_.reset();
    source_.reset();
    backend_.reset();
    camera_source_ = nullptr;
    synthetic_source_ = nullptr;
    still_source_ = nullptr;
    still_path_ = godot::String();
    cabi_tracker_ = nullptr;
    initialized_ = false;
    tracker_available_ = false;
    frozen_ = false;
    frozen_image_ = godot::Ref<godot::Image>();
    selected_slot_ = -1;
    preview_slot_ = -1;
    preview_pinned_ = false;
    landmarks_ = godot::PackedVector2Array();
    face_box_ = godot::Rect2();
    face_box_initialized_ = false;
    landmark_count_ = 0;
    published_timestamp_ = 0;
    publish_count_ = 0;
    latest_timestamp_ = 0;
    latest_arrival_ = 0;
}

bool AvatarLivePipeline::is_initialized() const {
    return initialized_;
}

bool AvatarLivePipeline::start() {
    if (!initialized_ || source_ == nullptr) {
        last_error_ = "pipeline is not initialized";
        return false;
    }
    const avatar::core::Status status = source_->start();
    if (!status.ok()) {
        last_error_ = godot::String(status.to_string().c_str());
        running_ = false;
        return false;
    }
    running_ = true;
    last_error_ = godot::String();
    return true;
}

void AvatarLivePipeline::stop() {
    if (source_ != nullptr) {
        source_->stop();
    }
    release_selection();
    running_ = false;
}

bool AvatarLivePipeline::is_running() const {
    return running_;
}

void AvatarLivePipeline::set_camera_ring(godot::Object* ring) {
    AvatarCameraRing* adapter = godot::Object::cast_to<AvatarCameraRing>(ring);
    ring_ = godot::Ref<AvatarCameraRing>(adapter);
}

void AvatarLivePipeline::set_preview_enabled(bool enabled) {
    preview_enabled_ = enabled;
    if (!enabled) {
        if (preview_pinned_ && ring_.is_valid()) {
            ring_->release(preview_slot_);
        }
        preview_pinned_ = false;
        preview_slot_ = -1;
    }
}

bool AvatarLivePipeline::is_preview_enabled() const {
    return preview_enabled_;
}

void AvatarLivePipeline::set_frozen(bool frozen) {
    if (frozen_ == frozen) {
        return;
    }
    frozen_ = frozen;
    if (pipeline_ != nullptr) {
        pipeline_->set_frozen(frozen);
    }
    if (frozen) {
        frozen_image_ = godot::Ref<godot::Image>();
        if (source_ != nullptr) {
            // Capture exactly the frame the operator was looking at. The source
            // ring keeps producing after this; the copy is independent, so the
            // held image cannot drift.
            const auto lease = source_->ring().try_acquire_latest();
            if (lease) {
                frozen_image_ = frame_to_image(lease->frame());
            }
        }
        // If the camera pane was disabled when the freeze started, take one
        // pin now so the Setup pane can still show the still frame.
        if (preview_enabled_ && preview_slot_ < 0 && ring_.is_valid()) {
            const godot::Dictionary newest =
                ring_->select_for_tracking(static_cast<int64_t>(time_.now_ns()));
            if (static_cast<bool>(newest.get("valid", false))) {
                preview_slot_ = static_cast<std::int64_t>(newest.get("slot", -1));
                preview_pinned_ = preview_slot_ >= 0;
            }
        }
    } else {
        frozen_image_ = godot::Ref<godot::Image>();
    }
}

bool AvatarLivePipeline::is_frozen() const {
    return frozen_;
}

godot::Ref<godot::Image> AvatarLivePipeline::get_snapshot_image() const {
    if (frozen_ && frozen_image_.is_valid()) {
        return frozen_image_;
    }
    if (source_ == nullptr) {
        return {};
    }
    const auto lease = source_->ring().try_acquire_latest();
    if (!lease) {
        return {};
    }
    return frame_to_image(lease->frame());
}

bool AvatarLivePipeline::save_snapshot(const godot::String& path) const {
    const godot::Ref<godot::Image> image = get_snapshot_image();
    if (!image.is_valid() || image->is_empty()) {
        return false;
    }
    return image->save_png(path) == godot::OK;
}

godot::PackedVector2Array AvatarLivePipeline::get_landmarks() const {
    return landmarks_;
}

godot::Rect2 AvatarLivePipeline::get_face_box() const {
    return face_box_;
}

void AvatarLivePipeline::release_selection() {
    if (ring_.is_valid() && selected_slot_ >= 0) {
        ring_->release(selected_slot_);
    }
    selected_slot_ = -1;
}

godot::Dictionary AvatarLivePipeline::tick() {
    if (!running_ || pipeline_ == nullptr) {
        return get_state();
    }
    const TimestampNs now = time_.now_ns();
    // Release last tick's preview pin before the decal selects its paired slot.
    // Releasing after selection could unpin a slot the decal just selected when
    // the newest frame and the pose frame are the same. A frozen diagnostic
    // keeps both pins so the held frame stays visible.
    if (!frozen_) {
        if (preview_pinned_ && ring_.is_valid()) {
            ring_->release(preview_slot_);
        }
        preview_pinned_ = false;
        preview_slot_ = -1;
    }
    const avatar::live::PipelineTick tick = pipeline_->tick(
        now, map_camera(camera_source_ != nullptr ? camera_source_->state()
                                                  : avatar::capture::CameraState::streaming));

    // Publish the newest camera frame into the T24 ring. The decal samples the
    // slot whose timestamp matches the pose, not the newest pixels. A frozen
    // pipeline publishes nothing; the ring keeps the last frame it held.
    bool new_frame = false;
    if (!frozen_ && ring_.is_valid()) {
        const auto lease = source_->ring().try_acquire_latest();
        if (lease &&
            static_cast<std::int64_t>(lease->frame().timestamp_ns) != published_timestamp_) {
            published_timestamp_ = static_cast<std::int64_t>(lease->frame().timestamp_ns);
            latest_timestamp_ = lease->frame().timestamp_ns;
            latest_arrival_ = lease->frame().arrival_ns;
            if (ring_->publish_video_frame(lease->frame())) {
                ++publish_count_;
                pipeline_->note_camera_frame();
                new_frame = true;
            }
        }
    }

    godot::Dictionary out;
    out["camera"] = camera_name(map_camera(camera_source_ != nullptr
                                               ? camera_source_->state()
                                               : avatar::capture::CameraState::streaming));
    out["face"] = face_name(pipeline_->face_state(now));
    out["tracker_available"] = tracker_available_;
    out["confidence"] = static_cast<double>(pipeline_->confidence());
    out["submitted"] = tick.submitted;
    out["consumed"] = tick.consumed;
    out["new_frame"] = new_frame;
    out["have_output"] = tick.have_output;
    out["output_fresh"] = tick.output_fresh;
    out["frozen"] = frozen_;
    out["source"] = source_kind_;
    out["still_image"] = still_path_;
    out["source_timestamp_ns"] = static_cast<int64_t>(tick.output_timestamp_ns);

    godot::PackedFloat32Array head;
    head.resize(16);
    godot::PackedFloat32Array homography;
    homography.resize(9);
    for (int i = 0; i < 16; ++i) {
        head[i] = tick.output.head_from_camera[static_cast<std::size_t>(i)];
    }
    for (int i = 0; i < 9; ++i) {
        homography[i] = tick.output.canonical_to_image[static_cast<std::size_t>(i)];
    }
    out["head_from_camera"] = head;
    out["canonical_to_image"] = homography;
    godot::Array regions;
    for (const auto& region : tick.output.regions) {
        regions.append(godot::Vector2(region.u, region.v));
    }
    out["regions"] = regions;
    godot::Array rest_regions;
    for (const auto& region : tick.output.rest_regions) {
        rest_regions.append(godot::Vector2(region.u, region.v));
    }
    out["region_rest"] = rest_regions;
    out["features_valid"] = tick.output.features_valid;
    out["inlier_count"] = static_cast<int64_t>(tick.output.inlier_count);
    out["solve_failed"] = tick.output.solve_failed;

    if (tick.consumed) {
        update_landmark_cache(tick.output.head_from_camera);
    }
    out["landmark_count"] = static_cast<int64_t>(landmark_count_);
    out["landmark_timestamp_ns"] = static_cast<int64_t>(pipeline_->output().source_timestamp_ns);

    godot::Dictionary slot;
    bool paired = false;
    if (frozen_) {
        // Keep the slot pinned at freeze time so the camera pane and the decal
        // keep sampling the same still frame.
        if (ring_.is_valid() && selected_slot_ >= 0) {
            slot = ring_->get_slot_info(selected_slot_);
        }
        paired = static_cast<bool>(slot.get("valid", false));
    } else if (ring_.is_valid() && tick.output_timestamp_ns != 0 && tick.output.features_valid) {
        // Release last frame's pin before selecting this frame's slot. Holding
        // one slot forever would block a capture format change and starve the
        // ring; the decal samples the slot during this frame only.
        release_selection();
        slot = ring_->select(static_cast<int64_t>(tick.output_timestamp_ns),
                             static_cast<int64_t>(now));
        if (static_cast<bool>(slot.get("valid", false))) {
            selected_slot_ = static_cast<std::int64_t>(slot.get("slot", -1));
            pipeline_->note_paired_frame(tick.output_timestamp_ns,
                                         static_cast<TimestampNs>(slot.get("arrival_ns", 0)));
            paired = true;
        }
    } else {
        release_selection();
    }
    if (!frozen_ && !paired && latest_arrival_ != 0) {
        // A blank frame is still a transport submit. Measure its age from the
        // newest camera callback so the latency gate is never vacuous.
        pipeline_->note_paired_frame(latest_timestamp_, latest_arrival_);
    }
    out["camera_slot"] = slot;

    godot::Dictionary preview;
    if (preview_enabled_ && ring_.is_valid()) {
        if (frozen_) {
            if (preview_slot_ >= 0) {
                preview = ring_->get_slot_info(preview_slot_);
            }
        } else {
            if (paired) {
                // The decal already pinned the newest pose frame; share it
                // instead of taking a second pin on the same slot.
                preview_slot_ = selected_slot_;
            } else {
                const godot::Dictionary newest =
                    ring_->select_for_tracking(static_cast<int64_t>(now));
                if (static_cast<bool>(newest.get("valid", false))) {
                    preview_slot_ = static_cast<std::int64_t>(newest.get("slot", -1));
                    preview_pinned_ = preview_slot_ >= 0;
                }
            }
            if (preview_slot_ >= 0) {
                preview = ring_->get_slot_info(preview_slot_);
            }
        }
    }
    out["preview_slot"] = preview;
    return out;
}

void AvatarLivePipeline::update_landmark_cache(const std::array<float, 16>& head_from_camera) {
    const auto& landmarks = pipeline_->last_landmarks();
    const std::size_t count =
        std::min<std::size_t>(landmarks.count, avatar::tracking::LandmarkSet::kCapacity);
    landmark_count_ = static_cast<std::uint32_t>(count);
    landmarks_ = godot::PackedVector2Array();
    if (count == 0) {
        face_box_ = godot::Rect2();
        face_box_initialized_ = false;
        return;
    }
    landmarks_.resize(static_cast<int64_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const auto& point = landmarks.points[i];
        landmarks_[static_cast<int64_t>(i)] = godot::Vector2(point.x, point.y);
    }
    // A face box from the stable outline, not every landmark: the full set
    // includes the eyes and lips, whose motion would pump the box every frame.
    // The scripted/synthetic tracker uses the compact anatomical order instead
    // of the 478-point MediaPipe indices, so use that order for small sets.
    constexpr std::array<std::size_t, 12> kMediaPipeOutline = {10, 152, 127, 356, 234, 454,
                                                               50, 280, 33,  263, 61,  291};
    constexpr std::array<std::size_t, 14> kCompactOutline = {0,  1,  2,  3,  10, 11, 14,
                                                             15, 16, 17, 18, 19, 20, 21};
    const std::size_t* outline = kMediaPipeOutline.data();
    std::size_t outline_count = kMediaPipeOutline.size();
    if (count < 478) {
        outline = kCompactOutline.data();
        outline_count = kCompactOutline.size();
    }
    float min_x = 1.0F;
    float min_y = 1.0F;
    float max_x = 0.0F;
    float max_y = 0.0F;
    bool any = false;
    for (std::size_t i = 0; i < outline_count; ++i) {
        const std::size_t index = outline[i];
        if (index >= count) {
            continue;
        }
        const auto& point = landmarks.points[index];
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            continue;
        }
        min_x = std::min(min_x, point.x);
        min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x);
        max_y = std::max(max_y, point.y);
        any = true;
    }
    if (!any && count > 0) {
        // A backend with an unknown compact order: fall back to every point.
        for (std::size_t index = 0; index < count; ++index) {
            const auto& point = landmarks.points[index];
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                continue;
            }
            min_x = std::min(min_x, point.x);
            min_y = std::min(min_y, point.y);
            max_x = std::max(max_x, point.x);
            max_y = std::max(max_y, point.y);
            any = true;
        }
    }
    if (!any || max_x <= min_x || max_y <= min_y) {
        face_box_ = godot::Rect2();
        face_box_initialized_ = false;
        return;
    }
    const float margin = std::max(max_x - min_x, max_y - min_y) * 0.04F;
    min_x -= margin;
    min_y -= margin;
    max_x += margin;
    max_y += margin;
    // A profile view foreshortens the visible outline. Widen the box by
    // 1/cos(yaw) so it still covers the head, without letting a near-profile
    // solve explode it. `head_from_camera` is the inverse pose, so its (0,2)
    // term is -sin(yaw).
    const float yaw = std::asin(std::clamp(-head_from_camera[2], -1.0F, 1.0F));
    const float expansion = std::clamp(1.0F / std::max(std::cos(yaw), 0.55F), 1.0F, 1.8F);
    const float centre_x = (min_x + max_x) * 0.5F;
    const float width = (max_x - min_x) * expansion;
    const godot::Rect2 target(godot::Vector2(centre_x - width * 0.5F, min_y),
                              godot::Vector2(width, max_y - min_y));
    if (!face_box_initialized_) {
        face_box_ = target;
        face_box_initialized_ = true;
        return;
    }
    const godot::Vector2 old_centre(face_box_.position.x + face_box_.size.x * 0.5F,
                                    face_box_.position.y + face_box_.size.y * 0.5F);
    const godot::Vector2 new_centre(target.position.x + target.size.x * 0.5F,
                                    target.position.y + target.size.y * 0.5F);
    const bool jumped = old_centre.distance_to(new_centre) > 0.18F;
    const bool resized =
        target.size.x > face_box_.size.x * 1.6F || target.size.x < face_box_.size.x / 1.6F;
    if (jumped || resized) {
        face_box_ = target;
        return;
    }
    face_box_ = godot::Rect2(face_box_.position.lerp(target.position, 0.35F),
                             face_box_.size.lerp(target.size, 0.35F));
}

godot::Dictionary AvatarLivePipeline::get_state() const {
    godot::Dictionary out;
    const TimestampNs now = time_.now_ns();
    avatar::live::CameraState camera = avatar::live::CameraState::ok;
    if (camera_source_ != nullptr) {
        camera = map_camera(camera_source_->state());
    }
    out["camera"] = camera_name(camera);
    out["face"] = pipeline_ != nullptr ? face_name(pipeline_->face_state(now)) : "unavailable";
    out["tracker_available"] = tracker_available_;
    out["confidence"] = pipeline_ != nullptr ? static_cast<double>(pipeline_->confidence()) : 0.0;
    out["running"] = running_;
    out["initialized"] = initialized_;
    out["frozen"] = frozen_;
    out["source"] = source_kind_;
    out["still_image"] = still_path_;
    return out;
}

godot::Dictionary AvatarLivePipeline::get_stats() {
    godot::Dictionary out;
    if (pipeline_ != nullptr) {
        const auto& counters = pipeline_->counters();
        godot::Dictionary counts;
        counts["camera_frames"] = static_cast<int64_t>(counters.camera_frames);
        counts["frames_submitted"] = static_cast<int64_t>(counters.frames_submitted);
        counts["results_consumed"] = static_cast<int64_t>(counters.results_consumed);
        counts["output_frames"] = static_cast<int64_t>(counters.output_frames);
        counts["submit_samples"] = static_cast<int64_t>(counters.submit_samples);
        out["counters"] = counts;
        out["app_latency_ms"] = histogram_ms(pipeline_->app_latency());
    }
    godot::Dictionary tracker;
    tracker["kind"] = tracker_kind_;
    tracker["available"] = tracker_available_;
    if (tracker_ != nullptr) {
        const TrackerStats stats = tracker_->stats();
        tracker["submitted"] = static_cast<int64_t>(stats.submitted);
        tracker["produced"] = static_cast<int64_t>(stats.produced);
        tracker["dropped"] = static_cast<int64_t>(stats.dropped);
        tracker["inference_p50_ms"] = ns_to_ms(stats.inference_p50_ns());
        tracker["inference_p95_ms"] = ns_to_ms(stats.inference_p95_ns());
    }
    out["tracker"] = tracker;
    godot::Dictionary camera;
    camera["kind"] = source_kind_;
    if (camera_source_ != nullptr) {
        camera["state"] =
            godot::String(avatar::capture::camera_state_name(camera_source_->state()).data());
        camera["device"] = godot::String(camera_source_->device_name().data());
        camera["frames_published"] = static_cast<int64_t>(camera_source_->frames_published());
        camera["restarts"] = static_cast<int64_t>(camera_source_->restarts());
        camera["reconnects"] = static_cast<int64_t>(camera_source_->reconnects());
        camera["timestamp_resets"] = static_cast<int64_t>(camera_source_->timestamp_resets());
    } else if (still_source_ != nullptr) {
        camera["state"] = "streaming";
        camera["device"] = still_path_;
        camera["frames_published"] = static_cast<int64_t>(still_source_->frames_published());
        camera["restarts"] = 0;
        camera["reconnects"] = 0;
        camera["timestamp_resets"] = 0;
    }
    out["camera"] = camera;
    if (ring_.is_valid()) {
        out["ring"] = ring_->get_stats();
    }
    out["publish_count"] = static_cast<int64_t>(publish_count_);
    out["rss_bytes"] = get_rss_bytes();
    out["cpu_percent_one_core"] = get_cpu_percent_one_core();
    return out;
}

godot::Dictionary AvatarLivePipeline::get_config() const {
    godot::Dictionary out;
    out["source"] = source_kind_;
    out["tracker"] = tracker_kind_;
    out["tracker_available"] = tracker_available_;
    out["tracker_input"] = tracker_input_;
    out["capture_request"] = capture_request_;
    out["camera"] = camera_device_;
    out["still_image"] = still_path_;
    out["frozen"] = frozen_;
    if (camera_source_ != nullptr) {
        const auto mode = camera_source_->negotiated_mode();
        out["width"] = static_cast<int64_t>(mode.width);
        out["height"] = static_cast<int64_t>(mode.height);
        out["fps"] = mode.fps();
        out["format"] = godot::String(avatar::capture::pixel_format_name(mode.format));
    } else if (still_source_ != nullptr) {
        const auto info = still_source_->info();
        out["width"] = static_cast<int64_t>(info.width);
        out["height"] = static_cast<int64_t>(info.height);
        out["fps"] = 60.0;
        out["format"] = godot::String(avatar::capture::pixel_format_name(info.format));
    } else {
        out["width"] = static_cast<int64_t>(1280);
        out["height"] = static_cast<int64_t>(720);
        out["fps"] = 60.0;
        out["format"] = "bgra8";
    }
    return out;
}

void AvatarLivePipeline::record_transport_submit() {
    if (pipeline_ != nullptr) {
        pipeline_->record_transport_submit(time_.now_ns());
    }
}

void AvatarLivePipeline::set_tracker_available(bool available) {
    tracker_available_ = available;
    if (pipeline_ != nullptr) {
        pipeline_->set_tracker_available(available);
    }
}

void AvatarLivePipeline::reset_stats() {
    if (pipeline_ != nullptr) {
        pipeline_->reset_metrics();
    }
    publish_count_ = 0;
    last_cpu_sample_ns_ = 0;
    last_cpu_process_ns_ = 0;
    cpu_percent_one_core_ = 0.0;
}

godot::String AvatarLivePipeline::get_last_error() const {
    return last_error_;
}

std::int64_t AvatarLivePipeline::get_frames_published() const {
    return static_cast<std::int64_t>(publish_count_);
}

godot::Dictionary AvatarLivePipeline::get_build_info() const {
    godot::Dictionary out;
#ifdef AVATAR_BUILD_TYPE
    out["type"] = godot::String(AVATAR_BUILD_TYPE);
#else
    out["type"] = godot::String("unknown");
#endif
#ifdef AVATAR_GIT_COMMIT
    out["git_commit"] = godot::String(AVATAR_GIT_COMMIT);
#else
    out["git_commit"] = godot::String("unknown");
#endif
#ifdef AVATAR_GIT_DIRTY
    out["git_dirty"] = AVATAR_GIT_DIRTY != 0;
#else
    out["git_dirty"] = false;
#endif
#ifdef AVATAR_COMPILER
    out["compiler"] = godot::String(AVATAR_COMPILER);
#else
    out["compiler"] = godot::String("unknown");
#endif
    return out;
}

avatar::live::CameraState AvatarLivePipeline::map_camera(avatar::capture::CameraState state) {
    switch (state) {
        case avatar::capture::CameraState::streaming:
            return avatar::live::CameraState::ok;
        case avatar::capture::CameraState::device_lost:
        case avatar::capture::CameraState::stalled:
        case avatar::capture::CameraState::suspended:
            return avatar::live::CameraState::lost;
        case avatar::capture::CameraState::busy:
            return avatar::live::CameraState::busy;
        case avatar::capture::CameraState::permission_denied:
            return avatar::live::CameraState::denied;
        case avatar::capture::CameraState::closed:
        case avatar::capture::CameraState::opening:
            return avatar::live::CameraState::none;
    }
    return avatar::live::CameraState::none;
}

std::int64_t AvatarLivePipeline::get_rss_bytes() const {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
        return 0;
    }
    return static_cast<std::int64_t>(counters.WorkingSetSize);
#else
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return 0;
    }
    long pages = 0;
    long resident = 0;
    const int matched = std::fscanf(file, "%ld %ld", &pages, &resident);
    std::fclose(file);
    if (matched != 2) {
        return 0;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    return static_cast<std::int64_t>(resident) * page_size;
#endif
}

double AvatarLivePipeline::get_cpu_percent_one_core() {
    update_process_sample();
    return cpu_percent_one_core_;
}

void AvatarLivePipeline::update_process_sample() {
    const std::uint64_t now = time_.now_ns();
    const std::uint64_t process = process_cpu_ns();
    if (last_cpu_sample_ns_ != 0 && now > last_cpu_sample_ns_ && process >= last_cpu_process_ns_) {
        const double wall = static_cast<double>(now - last_cpu_sample_ns_);
        const double cpu = static_cast<double>(process - last_cpu_process_ns_);
        cpu_percent_one_core_ = wall > 0.0 ? cpu / wall * 100.0 : 0.0;
    }
    last_cpu_sample_ns_ = now;
    last_cpu_process_ns_ = process;
}

void AvatarLivePipeline::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("initialize", "config"),
                                &AvatarLivePipeline::initialize);
    godot::ClassDB::bind_method(godot::D_METHOD("shutdown"), &AvatarLivePipeline::shutdown);
    godot::ClassDB::bind_method(godot::D_METHOD("is_initialized"),
                                &AvatarLivePipeline::is_initialized);
    godot::ClassDB::bind_method(godot::D_METHOD("start"), &AvatarLivePipeline::start);
    godot::ClassDB::bind_method(godot::D_METHOD("stop"), &AvatarLivePipeline::stop);
    godot::ClassDB::bind_method(godot::D_METHOD("is_running"), &AvatarLivePipeline::is_running);
    godot::ClassDB::bind_method(godot::D_METHOD("set_camera_ring", "ring"),
                                &AvatarLivePipeline::set_camera_ring);
    godot::ClassDB::bind_method(godot::D_METHOD("set_preview_enabled", "enabled"),
                                &AvatarLivePipeline::set_preview_enabled);
    godot::ClassDB::bind_method(godot::D_METHOD("is_preview_enabled"),
                                &AvatarLivePipeline::is_preview_enabled);
    godot::ClassDB::bind_method(godot::D_METHOD("set_frozen", "frozen"),
                                &AvatarLivePipeline::set_frozen);
    godot::ClassDB::bind_method(godot::D_METHOD("is_frozen"), &AvatarLivePipeline::is_frozen);
    godot::ClassDB::bind_method(godot::D_METHOD("get_snapshot_image"),
                                &AvatarLivePipeline::get_snapshot_image);
    godot::ClassDB::bind_method(godot::D_METHOD("save_snapshot", "path"),
                                &AvatarLivePipeline::save_snapshot);
    godot::ClassDB::bind_method(godot::D_METHOD("get_landmarks"),
                                &AvatarLivePipeline::get_landmarks);
    godot::ClassDB::bind_method(godot::D_METHOD("get_face_box"), &AvatarLivePipeline::get_face_box);
    godot::ClassDB::bind_method(godot::D_METHOD("tick"), &AvatarLivePipeline::tick);
    godot::ClassDB::bind_method(godot::D_METHOD("get_state"), &AvatarLivePipeline::get_state);
    godot::ClassDB::bind_method(godot::D_METHOD("get_stats"), &AvatarLivePipeline::get_stats);
    godot::ClassDB::bind_method(godot::D_METHOD("get_config"), &AvatarLivePipeline::get_config);
    godot::ClassDB::bind_method(godot::D_METHOD("get_build_info"),
                                &AvatarLivePipeline::get_build_info);
    godot::ClassDB::bind_method(godot::D_METHOD("record_transport_submit"),
                                &AvatarLivePipeline::record_transport_submit);
    godot::ClassDB::bind_method(godot::D_METHOD("set_tracker_available", "available"),
                                &AvatarLivePipeline::set_tracker_available);
    godot::ClassDB::bind_method(godot::D_METHOD("reset_stats"), &AvatarLivePipeline::reset_stats);
    godot::ClassDB::bind_method(godot::D_METHOD("get_last_error"),
                                &AvatarLivePipeline::get_last_error);
    godot::ClassDB::bind_method(godot::D_METHOD("get_rss_bytes"),
                                &AvatarLivePipeline::get_rss_bytes);
    godot::ClassDB::bind_method(godot::D_METHOD("get_cpu_percent_one_core"),
                                &AvatarLivePipeline::get_cpu_percent_one_core);
    godot::ClassDB::bind_method(godot::D_METHOD("get_frames_published"),
                                &AvatarLivePipeline::get_frames_published);
}
