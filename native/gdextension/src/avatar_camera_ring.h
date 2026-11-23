#pragma once

#include "avatar/capture/camera_texture_ring.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <array>
#include <cstdint>
#include <memory>

/// GDScript-facing camera texture ring.
///
/// The native `CameraTextureRing` owns the slot policy, timestamps, sequence
/// numbers, format-change invalidation and drop accounting. This adapter only
/// uploads NV12/BGRA planes into Godot textures (no colour conversion) and
/// exposes the selected slot to the decal pass. YUV -> RGB happens in the
/// shader, driven by the selected slot's `matrix` and `range` fields.
class AvatarCameraRing : public godot::RefCounted {
    GDCLASS(AvatarCameraRing, godot::RefCounted)

public:
    AvatarCameraRing();
    ~AvatarCameraRing() override;

    bool initialize(int64_t capacity);
    bool publish_nv12(const godot::PackedByteArray& y_plane, const godot::PackedByteArray& uv_plane,
                      int64_t width, int64_t height, int64_t timestamp_ns, int64_t sequence,
                      const godot::String& color_space);
    bool publish_bgra(const godot::PackedByteArray& pixels, int64_t width, int64_t height,
                      int64_t timestamp_ns, int64_t sequence);

    /// C++-only publish path for the live pipeline: no PackedByteArray copy and
    /// no Variant marshalling on the per-frame path. Returns false and sets the
    /// error when the frame is dropped or the upload fails.
    bool publish_video_frame(const avatar::capture::VideoFrame& frame);

    godot::Dictionary select(int64_t source_timestamp_ns, int64_t render_time_ns);
    godot::Dictionary select_for_tracking(int64_t render_time_ns);
    void release(int64_t slot);
    void release_selected();
    void reset();

    godot::Dictionary get_stats() const;
    godot::Dictionary get_slot_info(int64_t slot) const;
    godot::String get_last_error() const;
    int64_t get_capacity() const;
    int64_t get_generation() const;

protected:
    static void _bind_methods();

private:
    class GodotUploader;
    void set_error(const godot::String& message);
    godot::Dictionary describe(const avatar::capture::CameraTextureSlotInfo& info,
                               std::uint32_t slot) const;

    std::unique_ptr<GodotUploader> uploader_;
    std::unique_ptr<avatar::capture::CameraTextureRing> ring_;
    godot::String last_error_;
};
