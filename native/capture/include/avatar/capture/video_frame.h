#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "avatar/core/clock.h"

namespace avatar::capture {

using core::TimestampNs;

/// Pixel formats the capture layer can describe.
///
/// The ring only owns fixed-size CPU formats. Compressed formats (MJPEG) need a
/// decoder and are rejected by FrameRing; a real backend that receives MJPEG
/// decodes before it publishes, or the ring grows a compressed variant later.
enum class PixelFormat : std::uint8_t {
    nv12 = 0,
    yuy2,
    mjpeg,
    bgra8,
    rgba8,
};

/// Colour interpretation of the planes. The consumer must not guess.
enum class ColorSpace : std::uint8_t {
    bt601_limited = 0,
    bt709_limited,
    bt709_full,
    srgb,
};

/// Where the pixels live.
enum class FrameStorage : std::uint8_t {
    cpu = 0,
    native,
};

inline constexpr std::size_t kMaxFramePlanes = 3;

struct PlaneLayout {
    std::uint32_t stride = 0;
    std::size_t size = 0;
};

[[nodiscard]] bool is_supported_pixel_format(PixelFormat format) noexcept;
[[nodiscard]] const char* pixel_format_name(PixelFormat format) noexcept;
[[nodiscard]] bool pixel_format_has_alpha(PixelFormat format) noexcept;
[[nodiscard]] bool is_planar_yuv(PixelFormat format) noexcept;

/// Fills the plane layout for a fixed-size CPU format. Returns false for an
/// unsupported format or a dimension that cannot be represented (odd sizes for
/// 4:2:0 / 4:2:2, zero dimensions).
[[nodiscard]] bool compute_plane_layout(PixelFormat format, std::uint32_t width,
                                        std::uint32_t height,
                                        std::array<PlaneLayout, kMaxFramePlanes>& planes) noexcept;

/// An immutable, non-owning view of one published frame.
///
/// Ownership rules (also documented in icamera_source.h):
/// - The producer owns the ring slots. It writes into a slot it has reserved and
///   then publishes it.
/// - A consumer pins a published slot for as long as it reads the pixels. While
///   pinned, the producer will not reuse that slot.
/// - `planes` point directly into the ring slot. Nothing in the pipeline copies
///   frame bytes implicitly; a copy is always an explicit operation.
/// - A VideoFrame value is only valid while the FrameLease that produced it is
///   alive. Copying the struct copies metadata and pointers, not pixels.
struct VideoFrame {
    TimestampNs timestamp_ns = 0;
    TimestampNs arrival_ns = 0;
    std::uint32_t sequence = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::bgra8;
    ColorSpace color_space = ColorSpace::srgb;
    bool mirrored = false;
    FrameStorage storage = FrameStorage::cpu;
    std::array<std::uint32_t, kMaxFramePlanes> strides{};
    std::array<const std::uint8_t*, kMaxFramePlanes> planes{};
    void* native_handle = nullptr;

    [[nodiscard]] const std::uint8_t* plane(std::size_t index) const noexcept {
        return index < kMaxFramePlanes ? planes[index] : nullptr;
    }
    [[nodiscard]] std::uint32_t stride(std::size_t index) const noexcept {
        return index < kMaxFramePlanes ? strides[index] : 0;
    }
};

/// FNV-1a 64 over the frame metadata and every plane byte.
///
/// Timestamp and sequence are deliberately excluded: the hash answers "are
/// these the same pixels and shape?", which is what a determinism test needs.
/// The byte order of the scalar fields is fixed explicitly, so the hash is
/// stable across little-endian hosts.
[[nodiscard]] std::uint64_t frame_content_hash(const VideoFrame& frame) noexcept;

} // namespace avatar::capture
