#include "avatar/output/file_output.h"

#include "avatar/output/png_writer.h"

#include <array>
#include <cstdio>
#include <system_error>
#include <utility>

namespace avatar::output {
namespace {

[[nodiscard]] std::string sequence_name(const std::string& prefix, std::uint32_t sequence) {
    std::array<char, 16> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%06u", sequence);
    return prefix + "-" + buffer.data() + ".png";
}

} // namespace

FileOutput::FileOutput(FileOutputOptions options) : options_(std::move(options)) {}

bool FileOutput::initialize(const OutputConfig& config) {
    if (!config.valid()) {
        last_error_ = "output config is invalid";
        initialized_ = false;
        return false;
    }
    if (options_.directory.empty()) {
        last_error_ = "file output directory is empty";
        initialized_ = false;
        return false;
    }
    if (options_.prefix.empty()) {
        last_error_ = "file output prefix is empty";
        initialized_ = false;
        return false;
    }
    if (options_.create_directory) {
        std::error_code code;
        std::filesystem::create_directories(options_.directory, code);
        if (code) {
            last_error_ = "cannot create output directory: " + code.message();
            initialized_ = false;
            return false;
        }
    }
    config_ = config;
    stats_ = {};
    last_path_.clear();
    last_error_.clear();
    initialized_ = true;
    return true;
}

bool FileOutput::submit(const OutputFrame& frame) {
    if (!initialized_) {
        last_error_ = "file output is not initialized";
        ++stats_.dropped;
        return false;
    }
    if (!frame.valid()) {
        last_error_ = "frame is invalid";
        ++stats_.dropped;
        return false;
    }
    if (frame.width != config_.width || frame.height != config_.height) {
        last_error_ = "frame size does not match the output config";
        ++stats_.dropped;
        return false;
    }
    const std::filesystem::path path =
        options_.directory / sequence_name(options_.prefix, frame.sequence);
    std::string error;
    if (!write_png_rgba8(path, frame.width, frame.height, frame.pixels, error)) {
        last_error_ = std::move(error);
        ++stats_.dropped;
        return false;
    }
    last_path_ = path;
    last_error_.clear();
    ++stats_.submitted;
    ++stats_.accepted;
    stats_.last_sequence = frame.sequence;
    stats_.last_submit_ns = frame.render_ns;
    if (frame.content == FrameContent::blank) {
        ++stats_.blank_frames;
    } else {
        ++stats_.avatar_frames;
    }
    return true;
}

void FileOutput::shutdown() {
    initialized_ = false;
}

} // namespace avatar::output
