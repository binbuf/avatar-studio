#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "avatar/core/status.h"

namespace avatar::capture {

struct Y4mHeader {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps_num = 30;
    std::uint32_t fps_den = 1;
    bool interlaced = false;
    std::string chroma = "420jpeg";
};

/// Minimal YUV4MPEG2 reader for the replay fixture format.
///
/// Supported chroma: 420 (all variants), 422 and 444. The reader owns no
/// frames; it fills caller-provided planar buffers so the replay source can
/// convert directly into a ring slot. It never reads a clock.
class Y4mReader {
public:
    enum class ReadResult {
        frame,
        end,
        error,
    };

    Y4mReader() = default;
    ~Y4mReader();

    Y4mReader(const Y4mReader&) = delete;
    Y4mReader& operator=(const Y4mReader&) = delete;

    [[nodiscard]] core::Status open(const std::filesystem::path& path);
    [[nodiscard]] const Y4mHeader& header() const noexcept { return header_; }
    [[nodiscard]] ReadResult read_frame(std::vector<std::uint8_t>& y,
                                        std::vector<std::uint8_t>& u,
                                        std::vector<std::uint8_t>& v);
    [[nodiscard]] std::uint32_t frames_read() const noexcept { return frames_read_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    [[nodiscard]] bool read_line(std::string& line);
    [[nodiscard]] bool read_bytes(std::vector<std::uint8_t>& buffer, std::size_t count);

    std::ifstream stream_;
    Y4mHeader header_{};
    std::uint32_t frames_read_ = 0;
    bool eof_ = false;
    std::string last_error_;
};

} // namespace avatar::capture
