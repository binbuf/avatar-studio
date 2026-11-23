#include "avatar_camera_ring.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace {

using avatar::capture::CameraTextureSlotInfo;
using avatar::capture::ColorSpace;
using avatar::capture::PixelFormat;
using avatar::capture::VideoFrame;

constexpr std::uint32_t kDefaultCapacity = 4;
constexpr std::uint32_t kMaxCapacity = 8;

bool parse_color_space(const godot::String& value, ColorSpace& space) {
    if (value == godot::String("bt601_limited")) {
        space = ColorSpace::bt601_limited;
        return true;
    }
    if (value == godot::String("bt601_full")) {
        space = ColorSpace::bt601_full;
        return true;
    }
    if (value == godot::String("bt709_limited")) {
        space = ColorSpace::bt709_limited;
        return true;
    }
    if (value == godot::String("bt709_full")) {
        space = ColorSpace::bt709_full;
        return true;
    }
    if (value == godot::String("srgb")) {
        space = ColorSpace::srgb;
        return true;
    }
    return false;
}

const char* color_space_name(ColorSpace space) {
    switch (space) {
        case ColorSpace::bt601_limited:
            return "bt601_limited";
        case ColorSpace::bt601_full:
            return "bt601_full";
        case ColorSpace::bt709_limited:
            return "bt709_limited";
        case ColorSpace::bt709_full:
            return "bt709_full";
        case ColorSpace::srgb:
            return "srgb";
    }
    return "srgb";
}

int matrix_for(ColorSpace space) {
    return space == ColorSpace::bt709_limited || space == ColorSpace::bt709_full ? 1 : 0;
}

int range_for(ColorSpace space) {
    return space == ColorSpace::bt601_limited || space == ColorSpace::bt709_limited ? 0 : 1;
}

bool is_yuv(ColorSpace space) {
    return space != ColorSpace::srgb;
}

std::uint32_t clamp_dimension(int64_t value) {
    return value > 0 && value <= static_cast<int64_t>(std::numeric_limits<std::uint32_t>::max())
               ? static_cast<std::uint32_t>(value)
               : 0;
}

godot::PackedByteArray pack_rows(const VideoFrame& frame, std::size_t plane, std::uint32_t width,
                                 std::uint32_t height, std::size_t bytes_per_pixel) {
    godot::PackedByteArray data;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;
    data.resize(static_cast<int64_t>(row_bytes * height));
    auto* destination = data.ptrw();
    const auto* source = frame.plane(plane);
    const std::size_t source_stride = frame.stride(plane);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) * row_bytes,
                    source + static_cast<std::size_t>(row) * source_stride, row_bytes);
    }
    return data;
}

godot::Ref<godot::Image> make_image(const VideoFrame& frame, std::size_t plane, std::uint32_t width,
                                    std::uint32_t height, godot::Image::Format format,
                                    std::size_t bytes_per_pixel) {
    return godot::Image::create_from_data(static_cast<int32_t>(width), static_cast<int32_t>(height),
                                          false, format,
                                          pack_rows(frame, plane, width, height, bytes_per_pixel));
}

} // namespace

class AvatarCameraRing::GodotUploader final : public avatar::capture::ICameraTextureUploader {
public:
    struct Slot {
        godot::Ref<godot::ImageTexture> rgb;
        godot::Ref<godot::ImageTexture> y;
        godot::Ref<godot::ImageTexture> uv;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        PixelFormat format = PixelFormat::bgra8;
    };

    [[nodiscard]] const Slot& slot(std::uint32_t index) const { return slots_[index]; }

    bool configure(std::uint32_t index, const VideoFrame& frame) override {
        if (index >= slots_.size()) {
            return false;
        }
        Slot& slot = slots_[index];
        slot.width = frame.width;
        slot.height = frame.height;
        slot.format = frame.format;
        slot.rgb = godot::Ref<godot::ImageTexture>();
        slot.y = godot::Ref<godot::ImageTexture>();
        slot.uv = godot::Ref<godot::ImageTexture>();

        if (frame.format == PixelFormat::nv12) {
            auto y = godot::Image::create(static_cast<int32_t>(frame.width),
                                          static_cast<int32_t>(frame.height), false,
                                          godot::Image::FORMAT_R8);
            auto uv = godot::Image::create(static_cast<int32_t>(frame.width / 2u),
                                           static_cast<int32_t>(frame.height / 2u), false,
                                           godot::Image::FORMAT_RG8);
            if (y.is_null() || uv.is_null()) {
                return false;
            }
            slot.y = godot::ImageTexture::create_from_image(y);
            slot.uv = godot::ImageTexture::create_from_image(uv);
            return slot.y.is_valid() && slot.uv.is_valid();
        }
        if (frame.format == PixelFormat::bgra8 || frame.format == PixelFormat::rgba8) {
            // Godot has no BGRA8 Image format. Upload the bytes unchanged as
            // RGBA8 and let the decal shader swap R/B from the slot's format.
            auto image = godot::Image::create(static_cast<int32_t>(frame.width),
                                              static_cast<int32_t>(frame.height), false,
                                              godot::Image::FORMAT_RGBA8);
            if (image.is_null()) {
                return false;
            }
            slot.rgb = godot::ImageTexture::create_from_image(image);
            return slot.rgb.is_valid();
        }
        // Packed YUY2 and MJPEG need a different shader path; the ring reports
        // the frame as dropped rather than guessing a conversion.
        return false;
    }

    bool upload(std::uint32_t index, const VideoFrame& frame) override {
        if (index >= slots_.size()) {
            return false;
        }
        Slot& slot = slots_[index];
        if (slot.width != frame.width || slot.height != frame.height ||
            slot.format != frame.format) {
            if (!configure(index, frame)) {
                return false;
            }
        }
        if (frame.format == PixelFormat::nv12) {
            auto y = make_image(frame, 0, frame.width, frame.height, godot::Image::FORMAT_R8, 1);
            auto uv = make_image(frame, 1, frame.width / 2u, frame.height / 2u,
                                 godot::Image::FORMAT_RG8, 2);
            if (y.is_null() || uv.is_null() || slot.y.is_null() || slot.uv.is_null()) {
                return false;
            }
            slot.y->update(y);
            slot.uv->update(uv);
            return true;
        }
        if (frame.format == PixelFormat::bgra8 || frame.format == PixelFormat::rgba8) {
            auto image =
                make_image(frame, 0, frame.width, frame.height, godot::Image::FORMAT_RGBA8, 4);
            if (image.is_null() || slot.rgb.is_null()) {
                return false;
            }
            slot.rgb->update(image);
            return true;
        }
        return false;
    }

    void invalidate(std::uint32_t index) override {
        if (index < slots_.size()) {
            slots_[index] = Slot{};
        }
    }

private:
    std::array<Slot, kMaxCapacity> slots_{};
};

AvatarCameraRing::AvatarCameraRing() {
    uploader_ = std::make_unique<GodotUploader>();
    ring_ = std::make_unique<avatar::capture::CameraTextureRing>(*uploader_, kDefaultCapacity);
}

AvatarCameraRing::~AvatarCameraRing() = default;

bool AvatarCameraRing::initialize(int64_t capacity) {
    if (capacity < 1 || capacity > static_cast<int64_t>(kMaxCapacity)) {
        set_error("capacity must be between 1 and 8");
        return false;
    }
    // Destroy the ring before its uploader: the ring's destructor invalidates
    // slots through the uploader reference.
    ring_.reset();
    uploader_ = std::make_unique<GodotUploader>();
    ring_ = std::make_unique<avatar::capture::CameraTextureRing>(
        *uploader_, static_cast<std::uint32_t>(capacity));
    last_error_ = godot::String();
    return true;
}

bool AvatarCameraRing::publish_nv12(const godot::PackedByteArray& y_plane,
                                    const godot::PackedByteArray& uv_plane, int64_t width,
                                    int64_t height, int64_t timestamp_ns, int64_t sequence,
                                    const godot::String& color_space) {
    const std::uint32_t w = clamp_dimension(width);
    const std::uint32_t h = clamp_dimension(height);
    if (w == 0 || h == 0 || (w % 2u) != 0 || (h % 2u) != 0) {
        set_error("nv12 dimensions must be positive and even");
        return false;
    }
    if (y_plane.size() < static_cast<int64_t>(w) * h ||
        uv_plane.size() < static_cast<int64_t>(w) * h / 2) {
        set_error("nv12 planes are smaller than width*height and width*height/2");
        return false;
    }
    ColorSpace space = ColorSpace::bt601_limited;
    if (!parse_color_space(color_space, space) || space == ColorSpace::srgb) {
        set_error(
            "nv12 colour space must be bt601_limited, bt601_full, bt709_limited or bt709_full");
        return false;
    }
    VideoFrame frame;
    frame.timestamp_ns = static_cast<avatar::core::TimestampNs>(timestamp_ns);
    frame.arrival_ns = frame.timestamp_ns;
    frame.sequence = static_cast<std::uint32_t>(std::max<int64_t>(sequence, 0));
    frame.width = w;
    frame.height = h;
    frame.format = PixelFormat::nv12;
    frame.color_space = space;
    frame.storage = avatar::capture::FrameStorage::cpu;
    frame.strides[0] = w;
    frame.strides[1] = w;
    frame.planes[0] = y_plane.ptr();
    frame.planes[1] = uv_plane.ptr();
    if (!ring_->publish(frame)) {
        set_error("nv12 frame dropped (slow consumer, format change or upload failure)");
        return false;
    }
    last_error_ = godot::String();
    return true;
}

bool AvatarCameraRing::publish_bgra(const godot::PackedByteArray& pixels, int64_t width,
                                    int64_t height, int64_t timestamp_ns, int64_t sequence) {
    const std::uint32_t w = clamp_dimension(width);
    const std::uint32_t h = clamp_dimension(height);
    if (w == 0 || h == 0) {
        set_error("bgra dimensions must be positive");
        return false;
    }
    if (pixels.size() < static_cast<int64_t>(w) * h * 4) {
        set_error("bgra pixels are smaller than width*height*4");
        return false;
    }
    VideoFrame frame;
    frame.timestamp_ns = static_cast<avatar::core::TimestampNs>(timestamp_ns);
    frame.arrival_ns = frame.timestamp_ns;
    frame.sequence = static_cast<std::uint32_t>(std::max<int64_t>(sequence, 0));
    frame.width = w;
    frame.height = h;
    frame.format = PixelFormat::bgra8;
    frame.color_space = ColorSpace::srgb;
    frame.storage = avatar::capture::FrameStorage::cpu;
    frame.strides[0] = w * 4u;
    frame.planes[0] = pixels.ptr();
    if (!ring_->publish(frame)) {
        set_error("bgra frame dropped (slow consumer, format change or upload failure)");
        return false;
    }
    last_error_ = godot::String();
    return true;
}

bool AvatarCameraRing::publish_video_frame(const avatar::capture::VideoFrame& frame) {
    if (ring_ == nullptr) {
        set_error("ring is not initialized");
        return false;
    }
    if (!ring_->publish(frame)) {
        set_error("frame dropped (slow consumer, format change or upload failure)");
        return false;
    }
    last_error_ = godot::String();
    return true;
}

godot::Dictionary AvatarCameraRing::select(int64_t source_timestamp_ns, int64_t render_time_ns) {
    godot::Dictionary result;
    if (ring_ == nullptr) {
        result["valid"] = false;
        return result;
    }
    const auto selected = ring_->select(static_cast<avatar::core::TimestampNs>(source_timestamp_ns),
                                        static_cast<avatar::core::TimestampNs>(render_time_ns));
    if (!selected) {
        result["valid"] = false;
        result["miss"] = true;
        return result;
    }
    const auto* info = ring_->slot(*selected);
    if (info == nullptr) {
        result["valid"] = false;
        return result;
    }
    return describe(*info, *selected);
}

godot::Dictionary AvatarCameraRing::select_for_tracking(int64_t render_time_ns) {
    godot::Dictionary result;
    if (ring_ == nullptr) {
        result["valid"] = false;
        return result;
    }
    const auto selected =
        ring_->select_for_tracking(static_cast<avatar::core::TimestampNs>(render_time_ns));
    if (!selected) {
        result["valid"] = false;
        result["miss"] = true;
        return result;
    }
    const auto* info = ring_->slot(*selected);
    if (info == nullptr) {
        result["valid"] = false;
        return result;
    }
    return describe(*info, *selected);
}

void AvatarCameraRing::release(int64_t slot) {
    if (ring_ != nullptr && slot >= 0) {
        ring_->release(static_cast<std::uint32_t>(slot));
    }
}

void AvatarCameraRing::release_selected() {
    if (ring_ != nullptr) {
        ring_->release_selected();
    }
}

void AvatarCameraRing::reset() {
    if (ring_ != nullptr) {
        ring_->reset();
    }
}

godot::Dictionary AvatarCameraRing::describe(const CameraTextureSlotInfo& info,
                                             std::uint32_t slot) const {
    godot::Dictionary result;
    result["valid"] = info.valid;
    result["slot"] = static_cast<int64_t>(slot);
    result["sequence"] = static_cast<int64_t>(info.sequence);
    result["timestamp_ns"] = static_cast<int64_t>(info.timestamp_ns);
    result["arrival_ns"] = static_cast<int64_t>(info.arrival_ns);
    result["width"] = static_cast<int64_t>(info.width);
    result["height"] = static_cast<int64_t>(info.height);
    result["format"] = godot::String(avatar::capture::pixel_format_name(info.format));
    result["color_space"] = godot::String(color_space_name(info.color_space));
    result["generation"] = static_cast<int64_t>(info.generation);
    result["is_yuv"] = is_yuv(info.color_space);
    result["matrix"] = matrix_for(info.color_space);
    result["range"] = range_for(info.color_space);
    result["bgra"] = info.format == PixelFormat::bgra8;
    if (uploader_ != nullptr && slot < kMaxCapacity) {
        const auto& textures = uploader_->slot(slot);
        if (info.format == PixelFormat::nv12) {
            result["y_texture"] = textures.y;
            result["uv_texture"] = textures.uv;
            result["rgb_texture"] = godot::Ref<godot::ImageTexture>();
        } else {
            result["rgb_texture"] = textures.rgb;
            result["y_texture"] = godot::Ref<godot::ImageTexture>();
            result["uv_texture"] = godot::Ref<godot::ImageTexture>();
        }
    }
    return result;
}

godot::Dictionary AvatarCameraRing::get_stats() const {
    godot::Dictionary result;
    if (ring_ == nullptr) {
        return result;
    }
    const auto stats = ring_->stats();
    result["published"] = static_cast<int64_t>(stats.published);
    result["dropped"] = static_cast<int64_t>(stats.dropped);
    result["rejected"] = static_cast<int64_t>(stats.rejected);
    result["wraps"] = static_cast<int64_t>(stats.wraps);
    result["format_changes"] = static_cast<int64_t>(stats.format_changes);
    result["format_change_deferred"] = static_cast<int64_t>(stats.format_change_deferred);
    result["upload_failures"] = static_cast<int64_t>(stats.upload_failures);
    result["selections"] = static_cast<int64_t>(stats.selections);
    result["selection_misses"] = static_cast<int64_t>(stats.selection_misses);
    result["tracking_selections"] = static_cast<int64_t>(stats.tracking_selections);
    result["tracking_misses"] = static_cast<int64_t>(stats.tracking_misses);
    result["upload_samples"] = static_cast<int64_t>(stats.upload_samples);
    result["upload_total_ns"] = static_cast<int64_t>(stats.upload_total_ns);
    result["upload_max_ns"] = static_cast<int64_t>(stats.upload_max_ns);
    result["upload_p50_ns"] = static_cast<int64_t>(stats.upload_p50_ns);
    result["upload_p95_ns"] = static_cast<int64_t>(stats.upload_p95_ns);
    return result;
}

godot::Dictionary AvatarCameraRing::get_slot_info(int64_t slot) const {
    godot::Dictionary result;
    if (ring_ == nullptr || slot < 0 || slot >= static_cast<int64_t>(ring_->capacity())) {
        result["valid"] = false;
        return result;
    }
    const auto* info = ring_->slot(static_cast<std::uint32_t>(slot));
    if (info == nullptr) {
        result["valid"] = false;
        return result;
    }
    return describe(*info, static_cast<std::uint32_t>(slot));
}

godot::String AvatarCameraRing::get_last_error() const {
    return last_error_;
}

int64_t AvatarCameraRing::get_capacity() const {
    return ring_ != nullptr ? static_cast<int64_t>(ring_->capacity()) : 0;
}

int64_t AvatarCameraRing::get_generation() const {
    return ring_ != nullptr ? static_cast<int64_t>(ring_->generation()) : 0;
}

void AvatarCameraRing::set_error(const godot::String& message) {
    last_error_ = message;
}

void AvatarCameraRing::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("initialize", "capacity"),
                                &AvatarCameraRing::initialize);
    godot::ClassDB::bind_method(godot::D_METHOD("publish_nv12", "y_plane", "uv_plane", "width",
                                                "height", "timestamp_ns", "sequence",
                                                "color_space"),
                                &AvatarCameraRing::publish_nv12);
    godot::ClassDB::bind_method(
        godot::D_METHOD("publish_bgra", "pixels", "width", "height", "timestamp_ns", "sequence"),
        &AvatarCameraRing::publish_bgra);
    godot::ClassDB::bind_method(godot::D_METHOD("select", "source_timestamp_ns", "render_time_ns"),
                                &AvatarCameraRing::select);
    godot::ClassDB::bind_method(godot::D_METHOD("select_for_tracking", "render_time_ns"),
                                &AvatarCameraRing::select_for_tracking);
    godot::ClassDB::bind_method(godot::D_METHOD("release", "slot"), &AvatarCameraRing::release);
    godot::ClassDB::bind_method(godot::D_METHOD("release_selected"),
                                &AvatarCameraRing::release_selected);
    godot::ClassDB::bind_method(godot::D_METHOD("reset"), &AvatarCameraRing::reset);
    godot::ClassDB::bind_method(godot::D_METHOD("get_stats"), &AvatarCameraRing::get_stats);
    godot::ClassDB::bind_method(godot::D_METHOD("get_slot_info", "slot"),
                                &AvatarCameraRing::get_slot_info);
    godot::ClassDB::bind_method(godot::D_METHOD("get_last_error"),
                                &AvatarCameraRing::get_last_error);
    godot::ClassDB::bind_method(godot::D_METHOD("get_capacity"), &AvatarCameraRing::get_capacity);
    godot::ClassDB::bind_method(godot::D_METHOD("get_generation"),
                                &AvatarCameraRing::get_generation);
}
