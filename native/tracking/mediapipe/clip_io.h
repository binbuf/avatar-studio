#pragma once

// Local, uncompressed measurement clips. This is a diagnostic format, not a
// user-facing recording feature. All integers use the Windows little-endian ABI.
#include "backend.h"

#include <cstring>
#include <istream>
#include <ostream>
#include <stdexcept>

namespace avatar::tracker_backend {
template <typename T>
inline void write_scalar(std::ostream& stream, T value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
template <typename T>
inline T read_scalar(std::istream& stream) {
    T value{};
    if (!stream.read(reinterpret_cast<char*>(&value), sizeof(value))) {
        throw std::runtime_error("truncated tracker clip header");
    }
    return value;
}
inline void write_clip_frame(std::ostream& out, const avatar_tracker_frame& frame) {
    write_scalar(out, frame.timestamp_ns);
    write_scalar(out, frame.sequence);
    write_scalar(out, frame.width);
    write_scalar(out, frame.height);
    write_scalar(out, frame.format);
    write_scalar(out, frame.stride);
    write_scalar(out, frame.plane_count);
    const auto size0 = static_cast<uint64_t>(frame.stride) * frame.height;
    const auto size1 = frame.plane_count == 2 ? size0 / 2 : 0;
    write_scalar(out, size0);
    write_scalar(out, size1);
    out.write(reinterpret_cast<const char*>(frame.plane0), static_cast<std::streamsize>(size0));
    if (size1) {
        out.write(reinterpret_cast<const char*>(frame.plane1), static_cast<std::streamsize>(size1));
    }
    if (!out) {
        throw std::runtime_error("tracker clip write failed");
    }
}
inline std::vector<OwnedFrame> read_clip(std::istream& in) {
    char magic[8]{};
    if (!in.read(magic, 8) || std::memcmp(magic, "AVCLIP1\n", 8)) {
        throw std::runtime_error("invalid tracker clip magic");
    }
    std::vector<OwnedFrame> frames;
    uint64_t total = 0;
    while (in.peek() != EOF) {
        OwnedFrame owned;
        auto& f = owned.frame;
        f.timestamp_ns = read_scalar<uint64_t>(in);
        f.sequence = read_scalar<uint32_t>(in);
        f.width = read_scalar<uint32_t>(in);
        f.height = read_scalar<uint32_t>(in);
        f.format = read_scalar<uint32_t>(in);
        f.stride = read_scalar<uint32_t>(in);
        f.plane_count = read_scalar<uint32_t>(in);
        const auto size0 = read_scalar<uint64_t>(in), size1 = read_scalar<uint64_t>(in);
        if (size0 > 64 * 1024 * 1024 || size1 > 32 * 1024 * 1024 ||
            total + size0 + size1 > 512 * 1024 * 1024 || frames.size() >= 1000) {
            throw std::runtime_error("tracker clip exceeds diagnostic limits");
        }
        total += size0 + size1;
        f.plane0_size = static_cast<size_t>(size0);
        f.plane1_size = static_cast<size_t>(size1);
        owned.bytes.resize(f.plane0_size + f.plane1_size);
        if (!in.read(reinterpret_cast<char*>(owned.bytes.data()),
                     static_cast<std::streamsize>(owned.bytes.size()))) {
            throw std::runtime_error("truncated tracker clip pixels");
        }
        owned.bind();
        if (validate_frame(f) != AVATAR_TRACKER_OK) {
            throw std::runtime_error("invalid frame in tracker clip");
        }
        frames.push_back(std::move(owned));
    }
    if (frames.empty()) {
        throw std::runtime_error("tracker clip has no frames");
    }
    return frames;
}
} // namespace avatar::tracker_backend
