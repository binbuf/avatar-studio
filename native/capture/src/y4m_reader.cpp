#include "avatar/capture/y4m_reader.h"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace avatar::capture {

namespace {

bool parse_u32(const std::string& text, std::uint32_t& value) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool is_chroma_420(const std::string& chroma) noexcept {
    return chroma == "420" || chroma == "420jpeg" || chroma == "420mpeg2" || chroma == "420paldv" ||
           chroma == "420p210";
}

bool is_chroma_422(const std::string& chroma) noexcept {
    return chroma == "422" || chroma == "422p210";
}

} // namespace

Y4mReader::~Y4mReader() = default;

core::Status Y4mReader::open(const std::filesystem::path& path) {
    if (stream_.is_open()) {
        stream_.close();
    }
    stream_.clear();
    stream_.open(path, std::ios::binary);
    if (!stream_) {
        return core::Status::error(core::StatusCode::not_found, "cannot open Y4M file",
                                   path.string());
    }

    std::string line;
    if (!read_line(line)) {
        return core::Status::error(core::StatusCode::invalid_argument, "Y4M header is missing");
    }
    if (line.rfind("YUV4MPEG2", 0) != 0) {
        return core::Status::error(core::StatusCode::invalid_argument, "not a YUV4MPEG2 stream");
    }

    Y4mHeader header;
    std::istringstream tokens(line.substr(9));
    std::string token;
    while (tokens >> token) {
        if (token.empty()) {
            continue;
        }
        const char tag = token[0];
        const std::string value = token.substr(1);
        switch (tag) {
            case 'W':
                if (!parse_u32(value, header.width)) {
                    return core::Status::error(core::StatusCode::invalid_argument,
                                               "invalid Y4M width");
                }
                break;
            case 'H':
                if (!parse_u32(value, header.height)) {
                    return core::Status::error(core::StatusCode::invalid_argument,
                                               "invalid Y4M height");
                }
                break;
            case 'F': {
                const std::size_t colon = value.find(':');
                const std::string numerator = value.substr(0, colon);
                const std::string denominator =
                    colon == std::string::npos ? "1" : value.substr(colon + 1);
                if (!parse_u32(numerator, header.fps_num) ||
                    !parse_u32(denominator, header.fps_den)) {
                    return core::Status::error(core::StatusCode::invalid_argument,
                                               "invalid Y4M frame rate");
                }
                break;
            }
            case 'I':
                header.interlaced = value != "p";
                break;
            case 'C':
                header.chroma = value;
                break;
            default:
                break;
        }
    }

    if (header.width == 0 || header.height == 0) {
        return core::Status::error(core::StatusCode::invalid_argument,
                                   "Y4M header has no frame size");
    }
    if (!is_chroma_420(header.chroma) && !is_chroma_422(header.chroma) && header.chroma != "444") {
        return core::Status::error(core::StatusCode::unsupported, "unsupported Y4M chroma",
                                   header.chroma);
    }
    if ((is_chroma_420(header.chroma) || is_chroma_422(header.chroma)) &&
        (header.width % 2u) != 0) {
        return core::Status::error(core::StatusCode::invalid_argument,
                                   "subsampled Y4M requires an even width");
    }
    if (is_chroma_420(header.chroma) && (header.height % 2u) != 0) {
        return core::Status::error(core::StatusCode::invalid_argument,
                                   "4:2:0 Y4M requires an even height");
    }

    header_ = header;
    frames_read_ = 0;
    eof_ = false;
    last_error_.clear();
    return core::Status::success();
}

bool Y4mReader::read_line(std::string& line) {
    line.clear();
    char character = 0;
    while (stream_.get(character)) {
        if (character == '\n') {
            return true;
        }
        line.push_back(character);
    }
    return !line.empty();
}

bool Y4mReader::read_bytes(std::vector<std::uint8_t>& buffer, std::size_t count) {
    buffer.resize(count);
    if (count == 0) {
        return true;
    }
    stream_.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count));
    return static_cast<std::size_t>(stream_.gcount()) == count;
}

Y4mReader::ReadResult Y4mReader::read_frame(std::vector<std::uint8_t>& y,
                                            std::vector<std::uint8_t>& u,
                                            std::vector<std::uint8_t>& v) {
    if (eof_ || !stream_.is_open()) {
        return ReadResult::end;
    }

    std::string line;
    if (!read_line(line)) {
        eof_ = true;
        return ReadResult::end;
    }
    if (line.empty()) {
        // A trailing newline after the last frame is not an error.
        eof_ = true;
        return ReadResult::end;
    }
    if (line.rfind("FRAME", 0) != 0) {
        last_error_ = "expected a FRAME header, got: " + line;
        return ReadResult::error;
    }

    const std::size_t width = header_.width;
    const std::size_t height = header_.height;
    const std::size_t luma_size = width * height;
    std::size_t chroma_width = width;
    std::size_t chroma_height = height;
    if (is_chroma_420(header_.chroma)) {
        chroma_width = width / 2u;
        chroma_height = height / 2u;
    } else if (is_chroma_422(header_.chroma)) {
        chroma_width = width / 2u;
    }
    const std::size_t chroma_size = chroma_width * chroma_height;

    if (!read_bytes(y, luma_size) || !read_bytes(u, chroma_size) || !read_bytes(v, chroma_size)) {
        last_error_ = "truncated Y4M frame";
        eof_ = true;
        return ReadResult::error;
    }

    ++frames_read_;
    return ReadResult::frame;
}

} // namespace avatar::capture
