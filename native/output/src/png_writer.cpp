#include "avatar/output/png_writer.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <vector>

namespace avatar::output {
namespace {

constexpr std::array<std::uint8_t, 8> kPngSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

[[nodiscard]] std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data,
                                         std::size_t size) noexcept {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0U ? 0xEDB88320U ^ (value >> 1U) : value >> 1U;
            }
            values[i] = value;
        }
        return values;
    }();
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc;
}

[[nodiscard]] std::uint32_t adler32(const std::uint8_t* data, std::size_t size) noexcept {
    constexpr std::uint32_t kMod = 65521U;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16U) | a;
}

void write_u32(std::ostream& stream, std::uint32_t value) {
    std::array<char, 4> bytes = {
        static_cast<char>((value >> 24U) & 0xFFU), static_cast<char>((value >> 16U) & 0xFFU),
        static_cast<char>((value >> 8U) & 0xFFU), static_cast<char>(value & 0xFFU)};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_chunk(std::ostream& stream, const char type[4], const std::uint8_t* data,
                 std::size_t size) {
    write_u32(stream, static_cast<std::uint32_t>(size));
    stream.write(type, 4);
    if (size > 0) {
        stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    std::uint32_t crc = 0xFFFFFFFFU;
    crc = crc32_update(crc, reinterpret_cast<const std::uint8_t*>(type), 4);
    crc = crc32_update(crc, data, size);
    write_u32(stream, crc ^ 0xFFFFFFFFU);
}

[[nodiscard]] std::vector<std::uint8_t> deflate_stored(const std::uint8_t* data, std::size_t size) {
    std::vector<std::uint8_t> output;
    output.reserve(size + size / 65535U * 5U + 16U);
    // zlib header: deflate, 32 KiB window, no preset dictionary, fastest level.
    output.push_back(0x78U);
    output.push_back(0x01U);
    std::size_t offset = 0;
    do {
        const std::size_t chunk = std::min<std::size_t>(65535U, size - offset);
        const bool final = offset + chunk == size;
        output.push_back(final ? 0x01U : 0x00U);
        output.push_back(static_cast<std::uint8_t>(chunk & 0xFFU));
        output.push_back(static_cast<std::uint8_t>((chunk >> 8U) & 0xFFU));
        const std::uint16_t inverse = static_cast<std::uint16_t>(~chunk);
        output.push_back(static_cast<std::uint8_t>(inverse & 0xFFU));
        output.push_back(static_cast<std::uint8_t>((inverse >> 8U) & 0xFFU));
        output.insert(output.end(), data + offset, data + offset + chunk);
        offset += chunk;
    } while (offset < size);
    const std::uint32_t checksum = adler32(data, size);
    output.push_back(static_cast<std::uint8_t>((checksum >> 24U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((checksum >> 16U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((checksum >> 8U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>(checksum & 0xFFU));
    return output;
}

} // namespace

bool write_png_rgba8(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                     std::span<const std::uint8_t> rgba, std::string& error) {
    if (width == 0 || height == 0) {
        error = "PNG dimensions must be positive";
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(width) * height * 4U;
    if (rgba.size() != expected) {
        error = "PNG pixel span does not match the dimensions";
        return false;
    }
    if (width > std::numeric_limits<std::uint32_t>::max() / 4U ||
        height > std::numeric_limits<std::uint32_t>::max() / (width * 4U + 1U)) {
        error = "PNG dimensions overflow the scanline buffer";
        return false;
    }

    std::vector<std::uint8_t> scanlines;
    scanlines.reserve(static_cast<std::size_t>(height) *
                      (1U + static_cast<std::size_t>(width) * 4U));
    for (std::uint32_t y = 0; y < height; ++y) {
        scanlines.push_back(0U); // filter type 0 (None)
        const std::uint8_t* row = rgba.data() + static_cast<std::size_t>(y) * width * 4U;
        scanlines.insert(scanlines.end(), row, row + static_cast<std::size_t>(width) * 4U);
    }
    const std::vector<std::uint8_t> compressed = deflate_stored(scanlines.data(), scanlines.size());

    std::array<std::uint8_t, 13> header{};
    header[0] = static_cast<std::uint8_t>((width >> 24U) & 0xFFU);
    header[1] = static_cast<std::uint8_t>((width >> 16U) & 0xFFU);
    header[2] = static_cast<std::uint8_t>((width >> 8U) & 0xFFU);
    header[3] = static_cast<std::uint8_t>(width & 0xFFU);
    header[4] = static_cast<std::uint8_t>((height >> 24U) & 0xFFU);
    header[5] = static_cast<std::uint8_t>((height >> 16U) & 0xFFU);
    header[6] = static_cast<std::uint8_t>((height >> 8U) & 0xFFU);
    header[7] = static_cast<std::uint8_t>(height & 0xFFU);
    header[8] = 8U;  // bit depth
    header[9] = 6U;  // colour type: truecolour with alpha
    header[10] = 0U; // compression
    header[11] = 0U; // filter method
    header[12] = 0U; // interlace

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot open PNG for writing: " + path.string();
        return false;
    }
    stream.write(reinterpret_cast<const char*>(kPngSignature.data()),
                 static_cast<std::streamsize>(kPngSignature.size()));
    write_chunk(stream, "IHDR", header.data(), header.size());
    write_chunk(stream, "IDAT", compressed.data(), compressed.size());
    write_chunk(stream, "IEND", nullptr, 0);
    stream.flush();
    if (!stream) {
        error = "failed while writing PNG: " + path.string();
        return false;
    }
    return true;
}

} // namespace avatar::output
