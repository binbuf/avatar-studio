#include "avatar/capture/video_frame.h"

namespace avatar::capture {

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
}

void hash_u32(std::uint64_t& hash, std::uint32_t value) noexcept {
    for (int shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<std::uint8_t>((value >> shift) & 0xFFu);
        hash *= kFnvPrime;
    }
}

} // namespace

bool is_supported_pixel_format(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::nv12:
        case PixelFormat::yuy2:
        case PixelFormat::bgra8:
        case PixelFormat::rgba8:
            return true;
        case PixelFormat::mjpeg:
            return false;
    }
    return false;
}

const char* pixel_format_name(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::nv12:
            return "nv12";
        case PixelFormat::yuy2:
            return "yuy2";
        case PixelFormat::mjpeg:
            return "mjpeg";
        case PixelFormat::bgra8:
            return "bgra8";
        case PixelFormat::rgba8:
            return "rgba8";
    }
    return "unknown";
}

bool pixel_format_has_alpha(PixelFormat format) noexcept {
    return format == PixelFormat::bgra8 || format == PixelFormat::rgba8;
}

bool is_planar_yuv(PixelFormat format) noexcept {
    return format == PixelFormat::nv12;
}

bool compute_plane_layout(PixelFormat format, std::uint32_t width, std::uint32_t height,
                          std::array<PlaneLayout, kMaxFramePlanes>& planes) noexcept {
    planes = {};
    if (width == 0 || height == 0) {
        return false;
    }

    switch (format) {
        case PixelFormat::bgra8:
        case PixelFormat::rgba8:
            planes[0].stride = width * 4u;
            planes[0].size = static_cast<std::size_t>(planes[0].stride) * height;
            return true;
        case PixelFormat::nv12:
            if ((width % 2u) != 0 || (height % 2u) != 0) {
                return false;
            }
            planes[0].stride = width;
            planes[0].size = static_cast<std::size_t>(width) * height;
            planes[1].stride = width;
            planes[1].size = static_cast<std::size_t>(width) * (height / 2u);
            return true;
        case PixelFormat::yuy2:
            if ((width % 2u) != 0) {
                return false;
            }
            planes[0].stride = width * 2u;
            planes[0].size = static_cast<std::size_t>(planes[0].stride) * height;
            return true;
        case PixelFormat::mjpeg:
            return false;
    }
    return false;
}

std::uint64_t frame_content_hash(const VideoFrame& frame) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_u32(hash, frame.width);
    hash_u32(hash, frame.height);
    hash_u32(hash, static_cast<std::uint32_t>(frame.format));
    hash_u32(hash, static_cast<std::uint32_t>(frame.color_space));
    hash_u32(hash, frame.mirrored ? 1u : 0u);
    hash_u32(hash, static_cast<std::uint32_t>(frame.storage));

    std::array<PlaneLayout, kMaxFramePlanes> layout{};
    if (!compute_plane_layout(frame.format, frame.width, frame.height, layout)) {
        return hash;
    }

    for (std::size_t plane = 0; plane < kMaxFramePlanes; ++plane) {
        if (layout[plane].size == 0) {
            continue;
        }
        hash_u32(hash, frame.strides[plane]);
        if (frame.planes[plane] != nullptr) {
            hash_bytes(hash, frame.planes[plane], layout[plane].size);
        }
    }
    return hash;
}

} // namespace avatar::capture
