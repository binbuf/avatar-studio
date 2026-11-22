#include "avatar/output/null_output.h"

namespace avatar::output {

bool NullOutput::initialize(const OutputConfig& config) {
    if (!config.valid()) {
        initialized_ = false;
        return false;
    }
    config_ = config;
    stats_ = {};
    initialized_ = true;
    return true;
}

bool NullOutput::submit(const OutputFrame& frame) {
    if (!initialized_ || !frame.valid()) {
        ++stats_.dropped;
        return false;
    }
    ++stats_.submitted;
    ++stats_.accepted;
    stats_.last_sequence = frame.sequence;
    stats_.last_submit_ns = frame.render_ns;
    if (frame.content == FrameContent::blank) {
        ++stats_.blank_frames;
    } else {
        ++stats_.avatar_frames;
    }
    last_content_ = frame.content;
    last_alpha_ = frame.alpha;
    last_color_space_ = frame.color_space;
    last_width_ = frame.width;
    last_height_ = frame.height;
    return true;
}

void NullOutput::shutdown() {
    initialized_ = false;
}

} // namespace avatar::output
