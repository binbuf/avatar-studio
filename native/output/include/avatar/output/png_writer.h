#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace avatar::output {

/// Writes a tightly packed RGBA8 image as a PNG with stored (uncompressed)
/// deflate blocks. This is intentionally dependency-free: FileOutput is a
/// diagnostic transport, and a PNG sequence is easier to inspect than a raw
/// dump. The bytes are a valid PNG that any decoder can open; the file is
/// larger than a compressed PNG by design.
[[nodiscard]] bool write_png_rgba8(const std::filesystem::path& path, std::uint32_t width,
                                   std::uint32_t height, std::span<const std::uint8_t> rgba,
                                   std::string& error);

} // namespace avatar::output
