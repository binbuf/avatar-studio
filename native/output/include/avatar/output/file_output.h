#pragma once

#include "avatar/output/video_output.h"

#include <filesystem>
#include <string>

namespace avatar::output {

struct FileOutputOptions {
    std::filesystem::path directory;
    std::string prefix = "frame";
    bool create_directory = true;
};

/// Writes a PNG sequence, one file per submitted frame. The file name is
/// `<prefix>-<sequence:06>.png`. This is the deterministic transport used by
/// the output invariant tests and by manual capture; it is not a live stream.
class FileOutput final : public IVideoOutput {
public:
    explicit FileOutput(FileOutputOptions options = {});

    bool initialize(const OutputConfig& config) override;
    bool submit(const OutputFrame& frame) override;
    void shutdown() override;

    [[nodiscard]] OutputStats stats() const override { return stats_; }
    [[nodiscard]] OutputConfig config() const override { return config_; }
    [[nodiscard]] bool is_initialized() const override { return initialized_; }
    [[nodiscard]] const char* name() const noexcept override { return "file"; }

    [[nodiscard]] const FileOutputOptions& options() const noexcept { return options_; }
    [[nodiscard]] const std::filesystem::path& last_path() const noexcept { return last_path_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    FileOutputOptions options_{};
    OutputConfig config_{};
    OutputStats stats_{};
    std::filesystem::path last_path_{};
    std::string last_error_{};
    bool initialized_ = false;
};

} // namespace avatar::output
