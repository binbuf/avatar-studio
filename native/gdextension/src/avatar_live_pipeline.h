#pragma once

#include "avatar/capture/camera_source.h"
#include "avatar/capture/icamera_source.h"
#include "avatar/capture/synthetic_face_source.h"
#include "avatar/core/clock.h"
#include "avatar/live/live_pipeline.h"
#include "avatar/rig/face_rig.h"
#include "avatar/tracking/cabi_tracker.h"
#include "avatar/tracking/itracking_backend.h"
#include "avatar_camera_ring.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
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

    avatar::core::SteadyTimeSource time_{};
    avatar::core::FrameClock clock_{time_};

    std::unique_ptr<avatar::capture::ICameraDeviceBackend> backend_{};
    std::unique_ptr<avatar::capture::ICameraSource> source_{};
    std::unique_ptr<avatar::tracking::ITrackingBackend> tracker_{};
    std::unique_ptr<avatar::rig::FaceRig> rig_{};
    std::unique_ptr<avatar::live::LivePipeline> pipeline_{};
    avatar::capture::CameraSource* camera_source_ = nullptr;
    avatar::capture::SyntheticFaceSource* synthetic_source_ = nullptr;
    avatar::tracking::CabiTracker* cabi_tracker_ = nullptr;

    godot::Ref<AvatarCameraRing> ring_{};
    bool initialized_ = false;
    bool running_ = false;
    bool tracker_available_ = false;
    std::int64_t selected_slot_ = -1;
    std::int64_t published_timestamp_ = 0;
    std::uint64_t publish_count_ = 0;
    avatar::core::TimestampNs latest_timestamp_ = 0;
    avatar::core::TimestampNs latest_arrival_ = 0;

    godot::String source_kind_{"camera"};
    godot::String tracker_kind_{"none"};
    godot::String camera_device_{};
    godot::String last_error_{};

    std::uint64_t last_cpu_sample_ns_ = 0;
    std::uint64_t last_cpu_process_ns_ = 0;
    double cpu_percent_one_core_ = 0.0;
};
