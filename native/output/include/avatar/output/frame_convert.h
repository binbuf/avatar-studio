#pragma once

#include "avatar/output/output_frame.h"
#include "avatar/output/video_output.h"

#include <cstdint>
#include <string>
#include <vector>

namespace avatar::output {

/// Convert one avatar frame to the transport's alpha mode and colour space.
///
/// `storage` owns the converted pixels and must outlive `out`. When the source
/// already matches the target, `out` aliases the source span and `storage` is
/// untouched. The conversion normalises to straight alpha first, converts RGB
/// between linear and sRGB, then applies the target alpha policy. Opaque
/// outputs composite over `config.background` in the target colour space.
[[nodiscard]] bool convert_to_output(const OutputFrame& source, const OutputConfig& config,
                                     std::vector<std::uint8_t>& storage, OutputFrame& out,
                                     std::string* error = nullptr);

/// Build a blank frame in the transport's terms: all-zero RGBA for a
/// non-opaque output, the configured opaque background otherwise.
[[nodiscard]] OutputFrame make_blank_frame(const OutputConfig& config,
                                           std::uint64_t source_timestamp_ns,
                                           std::uint64_t render_ns, std::uint32_t sequence,
                                           std::vector<std::uint8_t>& storage);

[[nodiscard]] std::uint8_t linear_to_srgb8(float linear) noexcept;
[[nodiscard]] float srgb8_to_linear(std::uint8_t srgb) noexcept;

} // namespace avatar::output
