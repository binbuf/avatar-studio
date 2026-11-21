#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "avatar/capture/icamera_source.h"
#include "avatar/capture/y4m_reader.h"

namespace avatar::capture {

struct ReplaySourceConfig {
    std::filesystem::path y4m_path;
    /// Optional. When empty, the sidecar is `<y4m-stem>.json` next to the clip.
    std::filesystem::path sidecar_path;
    PixelFormat format = PixelFormat::bgra8;
};

/// Replays a `.y4m` clip with a sidecar JSON pose/landmark track.
///
/// The format is documented in docs/design/testing-and-quality.md. Frames are
/// emitted in file order; `timestamp_ns` comes from the sidecar (the source is
/// frame-exact) and `arrival_ns` from the injected FrameClock. A sidecar
/// timestamp that is not newer than the previous frame is advanced by 1 ns so
/// the published timeline is strictly monotonic.
class ReplaySource final : public ICameraSource {
public:
    ReplaySource(core::FrameClock& clock, ReplaySourceConfig config);

    [[nodiscard]] core::Status start() override;
    void stop() override;
    [[nodiscard]] FrameRing& ring() noexcept override;
    [[nodiscard]] bool produce() override;
    [[nodiscard]] SourceInfo info() const noexcept override;
    [[nodiscard]] std::optional<FaceGroundTruth> latest_ground_truth() const override;

    [[nodiscard]] std::uint32_t frame_count() const noexcept {
        return static_cast<std::uint32_t>(truth_.size());
    }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    [[nodiscard]] bool load_sidecar();
    [[nodiscard]] bool convert(VideoFrame& frame) const;

    core::FrameClock& clock_;
    ReplaySourceConfig config_;
    std::unique_ptr<FrameRing> ring_;
    Y4mReader reader_;
    std::vector<std::uint8_t> y_;
    std::vector<std::uint8_t> u_;
    std::vector<std::uint8_t> v_;
    std::vector<FaceGroundTruth> truth_;
    ColorSpace color_space_ = ColorSpace::bt601_limited;
    std::size_t next_frame_ = 0;
    TimestampNs last_timestamp_ = 0;
    bool started_ = false;
    std::optional<FaceGroundTruth> latest_truth_{};
    std::string last_error_;
};

} // namespace avatar::capture
