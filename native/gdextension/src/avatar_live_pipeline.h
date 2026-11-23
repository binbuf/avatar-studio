#pragma once

#include "avatar/capture/camera_source.h"
#include "avatar/capture/icamera_source.h"
#include "avatar/capture/still_image_source.h"
#include "avatar/capture/synthetic_face_source.h"
#include "avatar/core/clock.h"
#include "avatar/live/live_pipeline.h"
#include "avatar/rig/face_rig.h"
#include "avatar/tracking/cabi_tracker.h"
#include "avatar/tracking/itracking_backend.h"
#include "avatar_camera_ring.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>

/// GDScript-facing M1 live pipeline (T26).
///
/// Owns the platform camera source, the real C-ABI tracker (or the deterministic
/// scripted/synthetic pair), FaceRig and the timestamp-paired camera ring. It
/// exposes only plain values: status strings, the paired pose, the decal
/// homography/anchors and the measured app-side latency. The Live HUD keeps
/// owning the words and the output gate; this class does not render.
class AvatarLivePipeline : public godot::RefCounted {
    GDCLASS(AvatarLivePipeline, godot::RefCounted)

public:
    AvatarLivePipeline();
    ~AvatarLivePipeline() override;

    /// config keys: source ("camera" | "synthetic"), camera (device id),
    /// tracker (DLL path or "scripted"), tracker_input, fps.
    bool initialize(const godot::Dictionary& config);
    void shutdown();
    bool is_initialized() const;
    bool start();
    void stop();
    bool is_running() const;

    void set_camera_ring(godot::Object* ring);
    godot::Dictionary tick();
    godot::Dictionary get_state() const;
    godot::Dictionary get_stats();
    godot::Dictionary get_config() const;
    godot::Dictionary get_build_info() const;
    /// The Setup window asks for the newest camera frame even when no face is
    /// tracked, so the operator can recognize the camera feed. The pin is
    /// released and re-selected every tick; it never shares the decal's pin.
    void set_preview_enabled(bool enabled);
    bool is_preview_enabled() const;
    /// Development-only diagnostic freeze (T25c). `set_frozen(true)` stops
    /// publishing new camera frames and holds the last ring slot, tracker result
    /// and FaceRig pose; the 500 ms result timeout and 150 ms loss hold are
    /// suspended. `get_snapshot_image()` returns the frame captured at freeze
    /// time so the operator can reload it with `--live-source still`.
    void set_frozen(bool frozen);
    bool is_frozen() const;
    /// The captured frozen frame, or the newest source frame when not frozen.
    /// Returns null when no frame is available. The Setup window saves this as
    /// a PNG; it is never submitted to the OBS output.
    godot::Ref<godot::Image> get_snapshot_image() const;
    bool save_snapshot(const godot::String& path) const;
    /// Dense tracker landmarks in normalised image coordinates (T25c overlay).
    /// Cached once per tracker result, not rebuilt per render frame.
    godot::PackedVector2Array get_landmarks() const;
    /// Landmark bounds in normalised image coordinates; empty when there are
    /// no landmarks. The Setup camera pane draws this face box.
    godot::Rect2 get_face_box() const;
    void record_transport_submit();
    void set_tracker_available(bool available);
    void reset_stats();
    godot::String get_last_error() const;
    int64_t get_rss_bytes() const;
    double get_cpu_percent_one_core();
    int64_t get_frames_published() const;

protected:
    static void _bind_methods();

private:
    static avatar::live::CameraState map_camera(avatar::capture::CameraState state);
    void update_process_sample();
    void release_selection();
    void update_landmark_cache();

    avatar::core::SteadyTimeSource time_{};
    avatar::core::FrameClock clock_{time_};

    std::unique_ptr<avatar::capture::ICameraDeviceBackend> backend_{};
    std::unique_ptr<avatar::capture::ICameraSource> source_{};
    std::unique_ptr<avatar::tracking::ITrackingBackend> tracker_{};
    std::unique_ptr<avatar::rig::FaceRig> rig_{};
    std::unique_ptr<avatar::live::LivePipeline> pipeline_{};
    avatar::capture::CameraSource* camera_source_ = nullptr;
    avatar::capture::SyntheticFaceSource* synthetic_source_ = nullptr;
    avatar::capture::StillImageSource* still_source_ = nullptr;
    avatar::tracking::CabiTracker* cabi_tracker_ = nullptr;

    godot::Ref<AvatarCameraRing> ring_{};
    bool initialized_ = false;
    bool running_ = false;
    bool tracker_available_ = false;
    bool preview_enabled_ = false;
    bool frozen_ = false;
    godot::Ref<godot::Image> frozen_image_{};
    std::int64_t selected_slot_ = -1;
    std::int64_t preview_slot_ = -1;
    bool preview_pinned_ = false;
    std::int64_t published_timestamp_ = 0;
    std::uint64_t publish_count_ = 0;
    avatar::core::TimestampNs latest_timestamp_ = 0;
    avatar::core::TimestampNs latest_arrival_ = 0;

    // T25c diagnostic overlay data, rebuilt when a tracker result is consumed.
    godot::PackedVector2Array landmarks_{};
    godot::Rect2 face_box_{};
    std::uint32_t landmark_count_ = 0;

    godot::String source_kind_{"camera"};
    godot::String tracker_kind_{"none"};
    godot::String camera_device_{};
    godot::String still_path_{};
    godot::String capture_request_{"auto"};
    int64_t tracker_input_ = 0;
    godot::String last_error_{};

    std::uint64_t last_cpu_sample_ns_ = 0;
    std::uint64_t last_cpu_process_ns_ = 0;
    double cpu_percent_one_core_ = 0.0;
};
