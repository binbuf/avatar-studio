#include "avatar/capture/replay_source.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <fstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace avatar::capture {

namespace {

std::uint8_t clamp_channel(int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

ColorSpace parse_color_space(const std::string& value) noexcept {
    if (value == "bt709_limited") {
        return ColorSpace::bt709_limited;
    }
    if (value == "bt709_full") {
        return ColorSpace::bt709_full;
    }
    if (value == "srgb") {
        return ColorSpace::srgb;
    }
    return ColorSpace::bt601_limited;
}

struct ChromaGeometry {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

ChromaGeometry chroma_geometry(const Y4mHeader& header) noexcept {
    ChromaGeometry geometry;
    geometry.width = header.width;
    geometry.height = header.height;
    if (header.chroma == "420" || header.chroma == "420jpeg" || header.chroma == "420mpeg2" ||
        header.chroma == "420paldv" || header.chroma == "420p210") {
        geometry.width = header.width / 2u;
        geometry.height = header.height / 2u;
    } else if (header.chroma == "422" || header.chroma == "422p210") {
        geometry.width = header.width / 2u;
    }
    return geometry;
}

} // namespace

ReplaySource::ReplaySource(core::FrameClock& clock, ReplaySourceConfig config)
    : clock_(clock), config_(std::move(config)) {}

core::Status ReplaySource::start() {
    const core::Status opened = reader_.open(config_.y4m_path);
    if (!opened.ok()) {
        return opened;
    }
    if (config_.format != PixelFormat::bgra8 && config_.format != PixelFormat::rgba8 &&
        config_.format != PixelFormat::nv12) {
        return core::Status::error(core::StatusCode::unsupported,
                                   "replay output format is not supported");
    }
    if (config_.sidecar_path.empty()) {
        config_.sidecar_path = config_.y4m_path;
        config_.sidecar_path.replace_extension(".json");
    }
    if (!load_sidecar()) {
        return core::Status::error(core::StatusCode::invalid_argument,
                                   "cannot load the replay sidecar",
                                   last_error_);
    }

    ring_ = std::make_unique<FrameRing>(reader_.header().width,
                                        reader_.header().height,
                                        config_.format);
    next_frame_ = 0;
    last_timestamp_ = 0;
    latest_truth_.reset();
    started_ = true;
    return core::Status::success();
}

void ReplaySource::stop() {
    started_ = false;
}

FrameRing& ReplaySource::ring() noexcept {
    return *ring_;
}

SourceInfo ReplaySource::info() const noexcept {
    const Y4mHeader& header = reader_.header();
    return SourceInfo{"replay",
                      header.width,
                      header.height,
                      config_.format,
                      pixel_format_has_alpha(config_.format),
                      false,
                      true};
}

std::optional<FaceGroundTruth> ReplaySource::latest_ground_truth() const {
    return latest_truth_;
}

bool ReplaySource::load_sidecar() {
    truth_.clear();
    std::ifstream input(config_.sidecar_path, std::ios::binary);
    if (!input) {
        last_error_ = "cannot open " + config_.sidecar_path.string();
        return false;
    }

    nlohmann::json document;
    try {
        input >> document;
    } catch (const std::exception& error) {
        last_error_ = error.what();
        return false;
    }
    if (!document.is_object()) {
        last_error_ = "sidecar root is not an object";
        return false;
    }
    if (document.value("version", 0) != 1) {
        last_error_ = "unsupported sidecar version";
        return false;
    }
    const std::uint32_t width = document.value("width", 0u);
    const std::uint32_t height = document.value("height", 0u);
    if (width != reader_.header().width || height != reader_.header().height) {
        last_error_ = "sidecar dimensions do not match the Y4M header";
        return false;
    }
    color_space_ = parse_color_space(document.value("color_space", std::string("bt601_limited")));

    const auto frames = document.find("frames");
    if (frames == document.end() || !frames->is_array()) {
        last_error_ = "sidecar has no frames array";
        return false;
    }

    try {
        for (const nlohmann::json& frame : *frames) {
            FaceGroundTruth truth;
            truth.timestamp_ns = frame.value("timestamp_ns", TimestampNs{0});
            truth.features_valid = frame.value("features_valid", false);
            truth.barrel_k1 = frame.value("barrel_k1", 0.0f);

            const auto pose = frame.find("pose");
            if (pose != frame.end() && pose->is_array() && pose->size() == 16) {
                for (std::size_t index = 0; index < 16; ++index) {
                    truth.camera_from_head[index] = (*pose)[index].get<float>();
                }
            }

            const auto correspondences = frame.find("correspondences");
            if (correspondences != frame.end() && correspondences->is_array()) {
                for (const nlohmann::json& entry : *correspondences) {
                    const std::string name = entry.value("name", std::string{});
                    GroundTruthLandmark landmark = GroundTruthLandmark::count;
                    if (!parse_ground_truth_landmark(name, landmark)) {
                        continue; // Unknown names are ignored, per tracking.md.
                    }
                    if (truth.correspondence_count >= kGroundTruthLandmarkCount) {
                        break;
                    }
                    CanonicalCorrespondence& correspondence =
                        truth.correspondences[truth.correspondence_count++];
                    correspondence.id = landmark;
                    correspondence.name = ground_truth_landmark_name(landmark);
                    correspondence.u = entry.value("u", 0.0f);
                    correspondence.v = entry.value("v", 0.0f);
                    correspondence.x = entry.value("x", 0.0f);
                    correspondence.y = entry.value("y", 0.0f);
                    correspondence.ideal_x = entry.value("ideal_x", correspondence.x);
                    correspondence.ideal_y = entry.value("ideal_y", correspondence.y);
                }
            }

            if (!truth_.empty() && truth.timestamp_ns < truth_.back().timestamp_ns) {
                last_error_ = "sidecar frame timestamps are not monotonic";
                return false;
            }
            truth_.push_back(truth);
        }
    } catch (const std::exception& error) {
        last_error_ = error.what();
        return false;
    }

    return true;
}

bool ReplaySource::convert(VideoFrame& frame) const {
    const Y4mHeader& header = reader_.header();
    const std::uint32_t width = header.width;
    const std::uint32_t height = header.height;
    const ChromaGeometry geometry = chroma_geometry(header);
    const std::size_t luma_size = static_cast<std::size_t>(width) * height;
    const std::size_t chroma_size = static_cast<std::size_t>(geometry.width) * geometry.height;
    if (y_.size() < luma_size || u_.size() < chroma_size || v_.size() < chroma_size) {
        return false;
    }

    if (frame.format == PixelFormat::bgra8 || frame.format == PixelFormat::rgba8) {
        std::uint8_t* destination = const_cast<std::uint8_t*>(frame.planes[0]);
        const std::uint32_t stride = frame.strides[0];
        const bool rgba = frame.format == PixelFormat::rgba8;
        for (std::uint32_t y = 0; y < height; ++y) {
            std::uint8_t* row = destination + static_cast<std::size_t>(y) * stride;
            const std::size_t chroma_row =
                static_cast<std::size_t>(y * geometry.height / height) * geometry.width;
            for (std::uint32_t x = 0; x < width; ++x) {
                const int luma = static_cast<int>(y_[static_cast<std::size_t>(y) * width + x]) - 16;
                const std::size_t chroma_column =
                    static_cast<std::size_t>(x * geometry.width / width);
                const int cb = static_cast<int>(u_[chroma_row + chroma_column]) - 128;
                const int cr = static_cast<int>(v_[chroma_row + chroma_column]) - 128;
                const int c = std::max(0, luma);
                const int r = (298 * c + 409 * cr + 128) >> 8;
                const int g = (298 * c - 100 * cb - 208 * cr + 128) >> 8;
                const int b = (298 * c + 516 * cb + 128) >> 8;
                std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4u;
                if (rgba) {
                    pixel[0] = clamp_channel(r);
                    pixel[1] = clamp_channel(g);
                    pixel[2] = clamp_channel(b);
                } else {
                    pixel[0] = clamp_channel(b);
                    pixel[1] = clamp_channel(g);
                    pixel[2] = clamp_channel(r);
                }
                pixel[3] = 255;
            }
        }
        return true;
    }

    if (frame.format == PixelFormat::nv12) {
        std::uint8_t* y_plane = const_cast<std::uint8_t*>(frame.planes[0]);
        std::uint8_t* uv_plane = const_cast<std::uint8_t*>(frame.planes[1]);
        for (std::uint32_t y = 0; y < height; ++y) {
            std::copy_n(y_.data() + static_cast<std::size_t>(y) * width,
                        width,
                        y_plane + static_cast<std::size_t>(y) * frame.strides[0]);
        }
        for (std::uint32_t y = 0; y < height; y += 2) {
            std::uint8_t* row = uv_plane + static_cast<std::size_t>(y / 2u) * frame.strides[1];
            for (std::uint32_t x = 0; x < width; x += 2) {
                int cb = 0;
                int cr = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy) {
                    const std::size_t chroma_row =
                        static_cast<std::size_t>((y + dy) * geometry.height / height) * geometry.width;
                    for (std::uint32_t dx = 0; dx < 2; ++dx) {
                        const std::size_t chroma_column =
                            static_cast<std::size_t>((x + dx) * geometry.width / width);
                        cb += u_[chroma_row + chroma_column];
                        cr += v_[chroma_row + chroma_column];
                    }
                }
                row[x] = static_cast<std::uint8_t>((cb + 2) / 4);
                row[x + 1] = static_cast<std::uint8_t>((cr + 2) / 4);
            }
        }
        return true;
    }

    return false;
}

bool ReplaySource::produce() {
    if (!started_ || !ring_) {
        return false;
    }
    const Y4mReader::ReadResult result = reader_.read_frame(y_, u_, v_);
    if (result == Y4mReader::ReadResult::end) {
        return false;
    }
    if (result == Y4mReader::ReadResult::error) {
        last_error_ = reader_.last_error();
        return false;
    }

    const TimestampNs now = clock_.now_ns();
    TimestampNs timestamp = now;
    if (next_frame_ < truth_.size()) {
        timestamp = truth_[next_frame_].timestamp_ns;
    }
    if (next_frame_ > 0 && timestamp <= last_timestamp_) {
        timestamp = last_timestamp_ + 1;
    }

    auto write = ring_->begin_write();
    if (!write) {
        return false;
    }
    write->frame().color_space = color_space_;
    if (!convert(write->frame())) {
        last_error_ = "cannot convert the replay frame into the ring format";
        return false;
    }
    if (!ring_->publish(*write, timestamp, now, false)) {
        return false;
    }

    if (next_frame_ < truth_.size()) {
        FaceGroundTruth truth = truth_[next_frame_];
        truth.timestamp_ns = timestamp;
        truth.sequence = ring_->latest_sequence();
        latest_truth_ = truth;
    }
    last_timestamp_ = timestamp;
    ++next_frame_;
    return true;
}

} // namespace avatar::capture
