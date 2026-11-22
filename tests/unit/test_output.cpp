#include "test_registry.h"

#include "avatar/output/file_output.h"
#include "avatar/output/frame_convert.h"
#include "avatar/output/null_output.h"
#include "avatar/output/output_gate.h"
#include "avatar/output/output_manager.h"
#include "avatar/output/video_output.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using avatar::core::DiagnosticCode;
using avatar::core::TimestampNs;
using avatar::output::AlphaMode;
using avatar::output::BlankCause;
using avatar::output::ColorSpace;
using avatar::output::FileOutput;
using avatar::output::FileOutputOptions;
using avatar::output::FrameContent;
using avatar::output::GateDecision;
using avatar::output::GateSignal;
using avatar::output::GateState;
using avatar::output::NullOutput;
using avatar::output::OutputConfig;
using avatar::output::OutputFrame;
using avatar::output::OutputGate;
using avatar::output::OutputManager;
using avatar::output::OutputStats;
using avatar::output::PixelFormat;

struct FrameBuffer {
    std::vector<std::uint8_t> pixels;
    OutputFrame frame;
};

FrameBuffer make_frame(std::uint32_t width, std::uint32_t height, std::array<std::uint8_t, 4> rgba,
                       AlphaMode alpha = AlphaMode::straight,
                       ColorSpace color_space = ColorSpace::srgb) {
    FrameBuffer buffer;
    buffer.pixels.resize(static_cast<std::size_t>(width) * height * 4U);
    for (std::size_t i = 0; i < buffer.pixels.size(); i += 4U) {
        buffer.pixels[i] = rgba[0];
        buffer.pixels[i + 1] = rgba[1];
        buffer.pixels[i + 2] = rgba[2];
        buffer.pixels[i + 3] = rgba[3];
    }
    buffer.frame.width = width;
    buffer.frame.height = height;
    buffer.frame.format = PixelFormat::rgba8;
    buffer.frame.alpha = alpha;
    buffer.frame.color_space = color_space;
    buffer.frame.content = FrameContent::avatar;
    buffer.frame.sequence = 1;
    buffer.frame.pixels = std::span<const std::uint8_t>(buffer.pixels.data(), buffer.pixels.size());
    return buffer;
}

struct CapturedDiagnostics {
    std::vector<DiagnosticCode> codes;
    std::vector<std::string> causes;
};

void capture_sink(void* context, const avatar::core::LogRecord& record) noexcept {
    auto* captured = static_cast<CapturedDiagnostics*>(context);
    captured->codes.push_back(record.code);
    std::string cause;
    for (std::uint8_t i = 0; i < record.field_count; ++i) {
        if (record.fields[i].key == "cause") {
            cause = std::string(record.fields[i].value);
        }
    }
    captured->causes.push_back(std::move(cause));
}

class RecordingOutput final : public avatar::output::IVideoOutput {
public:
    bool initialize(const OutputConfig& config) override {
        if (!config.valid()) {
            return false;
        }
        config_ = config;
        stats_ = {};
        initialized_ = true;
        return true;
    }

    bool submit(const OutputFrame& frame) override {
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
        last_pixels.assign(frame.pixels.begin(), frame.pixels.end());
        last_content = frame.content;
        last_alpha = frame.alpha;
        last_color_space = frame.color_space;
        last_sequence = frame.sequence;
        return true;
    }

    void shutdown() override { initialized_ = false; }

    [[nodiscard]] OutputStats stats() const override { return stats_; }
    [[nodiscard]] OutputConfig config() const override { return config_; }
    [[nodiscard]] bool is_initialized() const override { return initialized_; }
    [[nodiscard]] const char* name() const noexcept override { return "recording"; }

    std::vector<std::uint8_t> last_pixels;
    FrameContent last_content = FrameContent::avatar;
    AlphaMode last_alpha = AlphaMode::straight;
    ColorSpace last_color_space = ColorSpace::srgb;
    std::uint32_t last_sequence = 0;

private:
    OutputConfig config_{};
    OutputStats stats_{};
    bool initialized_ = false;
};

[[nodiscard]] std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? 0xEDB88320U ^ (crc >> 1U) : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

[[nodiscard]] bool read_stored_png(const std::filesystem::path& path, std::uint32_t& width,
                                   std::uint32_t& height, std::vector<std::uint8_t>& rgba,
                                   std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "cannot open PNG";
        return false;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                                    std::istreambuf_iterator<char>());
    const std::array<std::uint8_t, 8> signature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (bytes.size() < signature.size() ||
        !std::equal(signature.begin(), signature.end(), bytes.begin())) {
        error = "bad PNG signature";
        return false;
    }
    std::vector<std::uint8_t> idat;
    std::size_t offset = signature.size();
    bool saw_header = false;
    bool saw_end = false;
    while (offset + 12U <= bytes.size()) {
        const std::uint32_t length = read_u32(bytes, offset);
        const std::size_t type_offset = offset + 4U;
        const std::size_t data_offset = offset + 8U;
        if (data_offset + length + 4U > bytes.size()) {
            error = "truncated PNG chunk";
            return false;
        }
        const std::uint32_t expected = read_u32(bytes, data_offset + length);
        const std::uint32_t actual =
            crc32(bytes.data() + type_offset, static_cast<std::size_t>(length) + 4U);
        if (expected != actual) {
            error = "PNG chunk CRC mismatch";
            return false;
        }
        const std::string type(reinterpret_cast<const char*>(bytes.data() + type_offset), 4);
        if (type == "IHDR") {
            if (length != 13U) {
                error = "bad IHDR length";
                return false;
            }
            width = read_u32(bytes, data_offset);
            height = read_u32(bytes, data_offset + 4U);
            if (bytes[data_offset + 8U] != 8U || bytes[data_offset + 9U] != 6U) {
                error = "PNG is not RGBA8";
                return false;
            }
            saw_header = true;
        } else if (type == "IDAT") {
            idat.insert(idat.end(), bytes.begin() + static_cast<std::ptrdiff_t>(data_offset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + length));
        } else if (type == "IEND") {
            saw_end = true;
        }
        offset = data_offset + length + 4U;
    }
    if (!saw_header || !saw_end || idat.size() < 6U) {
        error = "missing PNG header, data or end";
        return false;
    }

    // The writer uses stored deflate blocks. Decode those and verify Adler-32.
    if (idat[0] != 0x78U || idat[1] != 0x01U) {
        error = "unexpected zlib header";
        return false;
    }
    std::vector<std::uint8_t> raw;
    std::size_t cursor = 2U;
    bool final = false;
    while (!final && cursor + 5U <= idat.size()) {
        const std::uint8_t header = idat[cursor++];
        final = (header & 0x01U) != 0U;
        if (((header >> 1U) & 0x03U) != 0U) {
            error = "PNG uses a non-stored deflate block";
            return false;
        }
        const std::uint16_t length =
            static_cast<std::uint16_t>(idat[cursor] | (idat[cursor + 1] << 8U));
        const std::uint16_t inverse =
            static_cast<std::uint16_t>(idat[cursor + 2] | (idat[cursor + 3] << 8U));
        cursor += 4U;
        if (static_cast<std::uint16_t>(~length) != inverse || cursor + length > idat.size()) {
            error = "bad stored deflate block";
            return false;
        }
        raw.insert(raw.end(), idat.begin() + static_cast<std::ptrdiff_t>(cursor),
                   idat.begin() + static_cast<std::ptrdiff_t>(cursor + length));
        cursor += length;
    }
    if (!final || cursor + 4U != idat.size()) {
        error = "bad zlib stream end";
        return false;
    }
    const std::uint32_t expected_adler = read_u32(idat, cursor);
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t value : raw) {
        a = (a + value) % 65521U;
        b = (b + a) % 65521U;
    }
    if (expected_adler != ((b << 16U) | a)) {
        error = "PNG Adler-32 mismatch";
        return false;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4U + 1U;
    if (raw.size() != row_bytes * height) {
        error = "PNG scanline size mismatch";
        return false;
    }
    rgba.resize(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t filter = raw[static_cast<std::size_t>(y) * row_bytes];
        if (filter != 0U) {
            error = "PNG uses an unexpected filter";
            return false;
        }
        std::copy(raw.begin() + static_cast<std::ptrdiff_t>(y * row_bytes + 1U),
                  raw.begin() + static_cast<std::ptrdiff_t>((y + 1U) * row_bytes),
                  rgba.begin() +
                      static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * width * 4U));
    }
    return true;
}

} // namespace

TEST_CASE(output_frame_validates_size_and_pixels) {
    FrameBuffer buffer = make_frame(4, 3, {10, 20, 30, 255});
    CHECK(buffer.frame.valid());
    CHECK_EQ(buffer.frame.expected_bytes(), std::size_t{48});
    CHECK(buffer.frame.pixel_at(0, 0) != nullptr);
    CHECK_EQ(buffer.frame.pixel_at(0, 0)[0], std::uint8_t{10});
    CHECK_EQ(buffer.frame.pixel_at(0, 0)[1], std::uint8_t{20});
    CHECK_EQ(buffer.frame.pixel_at(0, 0)[2], std::uint8_t{30});
    CHECK_EQ(buffer.frame.pixel_at(0, 0)[3], std::uint8_t{255});
    CHECK(buffer.frame.pixel_at(4, 0) == nullptr);
    CHECK(buffer.frame.pixel_at(0, 3) == nullptr);

    OutputFrame invalid;
    CHECK(!invalid.valid());
    invalid.width = 4;
    invalid.height = 3;
    CHECK(!invalid.valid());
    invalid.pixels = std::span<const std::uint8_t>(buffer.pixels.data(), buffer.pixels.size());
    CHECK(invalid.valid());
}

TEST_CASE(null_output_accepts_avatar_and_blank_frames) {
    NullOutput output;
    OutputConfig config;
    config.width = 4;
    config.height = 4;
    config.alpha = AlphaMode::straight;
    CHECK(output.initialize(config));
    CHECK(output.is_initialized());

    FrameBuffer avatar = make_frame(4, 4, {1, 2, 3, 255});
    CHECK(output.submit(avatar.frame));
    CHECK_EQ(output.stats().submitted, std::uint64_t{1});
    CHECK_EQ(output.stats().avatar_frames, std::uint64_t{1});
    CHECK(output.last_content() == FrameContent::avatar);

    std::vector<std::uint8_t> blank_storage;
    OutputFrame blank = avatar::output::make_blank_frame(config, 10, 20, 2, blank_storage);
    CHECK(output.submit(blank));
    CHECK_EQ(output.stats().submitted, std::uint64_t{2});
    CHECK_EQ(output.stats().blank_frames, std::uint64_t{1});
    CHECK(output.last_content() == FrameContent::blank);
    CHECK_EQ(output.stats().last_sequence, std::uint32_t{2});
    CHECK(output.is_initialized());

    output.shutdown();
    CHECK(!output.is_initialized());
}

TEST_CASE(file_output_writes_a_round_trippable_png_sequence) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "avatar_t23_file_output";
    std::error_code code;
    std::filesystem::remove_all(directory, code);

    FileOutputOptions options;
    options.directory = directory;
    options.prefix = "gate";
    FileOutput output(options);
    OutputConfig config;
    config.width = 5;
    config.height = 3;
    config.alpha = AlphaMode::straight;
    CHECK(output.initialize(config));

    FrameBuffer first = make_frame(5, 3, {200, 100, 50, 255});
    first.frame.sequence = 1;
    CHECK(output.submit(first.frame));
    std::vector<std::uint8_t> blank_storage;
    OutputFrame blank = avatar::output::make_blank_frame(config, 1, 2, 2, blank_storage);
    CHECK(output.submit(blank));

    CHECK(std::filesystem::exists(directory / "gate-000001.png"));
    CHECK(std::filesystem::exists(directory / "gate-000002.png"));
    CHECK_EQ(output.stats().accepted, std::uint64_t{2});
    CHECK_EQ(output.stats().avatar_frames, std::uint64_t{1});
    CHECK_EQ(output.stats().blank_frames, std::uint64_t{1});

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
    std::string error;
    CHECK(read_stored_png(directory / "gate-000001.png", width, height, pixels, error));
    CHECK_EQ(width, std::uint32_t{5});
    CHECK_EQ(height, std::uint32_t{3});
    CHECK_EQ(pixels.size(), std::size_t{5 * 3 * 4});
    CHECK_EQ(pixels[0], std::uint8_t{200});
    CHECK_EQ(pixels[1], std::uint8_t{100});
    CHECK_EQ(pixels[2], std::uint8_t{50});
    CHECK_EQ(pixels[3], std::uint8_t{255});

    CHECK(read_stored_png(directory / "gate-000002.png", width, height, pixels, error));
    CHECK_EQ(pixels[0], std::uint8_t{0});
    CHECK_EQ(pixels[3], std::uint8_t{0});

    output.shutdown();
    std::filesystem::remove_all(directory, code);
}

TEST_CASE(output_gate_holds_then_blanks_at_the_deadline) {
    OutputGate gate;
    GateSignal valid;
    valid.render_time_ns = 1'000'000'000;
    valid.source_timestamp_ns = 900'000'000;
    GateDecision decision = gate.evaluate(valid);
    CHECK(decision.state == GateState::avatar);
    CHECK(!decision.blanked);

    GateSignal dropped;
    dropped.render_time_ns = 1'100'000'000;
    dropped.features_valid = false;
    decision = gate.evaluate(dropped);
    CHECK(decision.state == GateState::held);
    CHECK(!decision.blanked);

    dropped.render_time_ns = 1'149'000'000;
    decision = gate.evaluate(dropped);
    CHECK(decision.state == GateState::held);

    dropped.render_time_ns = 1'150'000'000;
    decision = gate.evaluate(dropped);
    CHECK(decision.state == GateState::blank);
    CHECK(decision.blanked);
    CHECK(decision.cause == BlankCause::missing_features);
    CHECK(decision.diagnostic == DiagnosticCode::trk_008);
    CHECK(decision.transitioned);

    dropped.render_time_ns = 1'151'000'000;
    decision = gate.evaluate(dropped);
    CHECK(decision.state == GateState::blank);
    CHECK(!decision.transitioned);
}

TEST_CASE(output_gate_never_extends_the_hold_on_invalid_results) {
    OutputGate gate;
    GateSignal valid;
    valid.render_time_ns = 1'000'000'000;
    CHECK(gate.evaluate(valid).state == GateState::avatar);

    GateSignal invalid;
    invalid.render_time_ns = 1'100'000'000;
    invalid.features_valid = false;
    CHECK(gate.evaluate(invalid).state == GateState::held);

    invalid.render_time_ns = 1'149'000'000;
    CHECK(gate.evaluate(invalid).state == GateState::held);

    invalid.render_time_ns = 1'150'000'000;
    const GateDecision blank = gate.evaluate(invalid);
    CHECK(blank.state == GateState::blank);
    CHECK_EQ(gate.last_accepted_render_ns(), TimestampNs{1'000'000'000});
}

TEST_CASE(output_gate_blanks_immediately_without_a_prior_valid_sample) {
    OutputGate gate;
    GateSignal invalid;
    invalid.render_time_ns = 5'000'000;
    invalid.result_valid = false;
    const GateDecision decision = gate.evaluate(invalid);
    CHECK(decision.state == GateState::blank);
    CHECK(decision.cause == BlankCause::face_lost);
    CHECK(decision.diagnostic == DiagnosticCode::trk_002);
    CHECK(decision.transitioned);
}

TEST_CASE(output_gate_resumes_without_blank_diagnostics) {
    OutputGate gate;
    GateSignal valid;
    valid.render_time_ns = 1'000'000'000;
    CHECK(gate.evaluate(valid).state == GateState::avatar);

    GateSignal missing;
    missing.render_time_ns = 1'100'000'000;
    missing.features_valid = false;
    CHECK(gate.evaluate(missing).state == GateState::held);

    valid.render_time_ns = 1'120'000'000;
    const GateDecision resumed = gate.evaluate(valid);
    CHECK(resumed.state == GateState::avatar);
    CHECK(resumed.transitioned);
    CHECK(!resumed.blanked);
    CHECK_EQ(gate.blank_transitions(), std::uint32_t{0});
    CHECK_EQ(gate.resume_transitions(), std::uint32_t{2});
}

TEST_CASE(output_manager_emits_each_transition_once_with_a_cause) {
    CapturedDiagnostics captured;
    OutputManager manager(&capture_sink, &captured);
    OutputConfig config;
    config.width = 4;
    config.height = 4;
    config.alpha = AlphaMode::straight;
    CHECK(manager.initialize(std::make_unique<NullOutput>(), config));

    FrameBuffer avatar = make_frame(4, 4, {10, 20, 30, 255});
    GateSignal signal;
    signal.render_time_ns = 1'000'000'000;
    signal.source_timestamp_ns = 900'000'000;
    CHECK(manager.submit(avatar.frame, signal).state == GateState::avatar);
    CHECK(captured.codes.empty());

    signal.render_time_ns = 1'100'000'000;
    signal.features_valid = false;
    CHECK(manager.submit(avatar.frame, signal).state == GateState::held);
    CHECK(captured.codes.empty());

    signal.render_time_ns = 1'150'000'000;
    CHECK(manager.submit(avatar.frame, signal).state == GateState::blank);
    CHECK_EQ(captured.codes.size(), std::size_t{2});
    CHECK(captured.codes[0] == DiagnosticCode::trk_008);
    CHECK(captured.codes[1] == DiagnosticCode::out_007);
    CHECK_EQ(captured.causes[0], std::string("missing_features"));
    CHECK_EQ(captured.causes[1], std::string("missing_features"));

    signal.render_time_ns = 1'160'000'000;
    CHECK(manager.submit(avatar.frame, signal).state == GateState::blank);
    CHECK_EQ(captured.codes.size(), std::size_t{2});

    signal.render_time_ns = 1'200'000'000;
    signal.result_valid = false;
    CHECK(manager.submit(avatar.frame, signal).state == GateState::blank);
    CHECK_EQ(captured.codes.size(), std::size_t{4});
    CHECK(captured.codes[2] == DiagnosticCode::trk_002);
    CHECK(captured.codes[3] == DiagnosticCode::out_007);
    CHECK_EQ(captured.causes[2], std::string("face_lost"));
    CHECK_EQ(captured.causes[3], std::string("face_lost"));

    CHECK(manager.is_output_alive());
    CHECK_EQ(manager.stats().blank_frames, std::uint64_t{3});
    CHECK_EQ(manager.stats().avatar_frames, std::uint64_t{2});
}

TEST_CASE(output_manager_is_transport_agnostic_for_null_and_file) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "avatar_t23_agnostic";
    std::error_code code;
    std::filesystem::remove_all(directory, code);

    CapturedDiagnostics null_diagnostics;
    CapturedDiagnostics file_diagnostics;
    OutputConfig config;
    config.width = 3;
    config.height = 2;
    config.alpha = AlphaMode::straight;

    OutputManager null_manager(&capture_sink, &null_diagnostics);
    CHECK(null_manager.initialize(std::make_unique<NullOutput>(), config));

    FileOutputOptions options;
    options.directory = directory;
    options.prefix = "agnostic";
    OutputManager file_manager(&capture_sink, &file_diagnostics);
    CHECK(file_manager.initialize(std::make_unique<FileOutput>(options), config));

    FrameBuffer avatar = make_frame(3, 2, {80, 90, 100, 255});
    GateSignal signal;
    signal.render_time_ns = 1'000'000'000;
    std::vector<GateState> states;
    for (int frame = 0; frame < 6; ++frame) {
        signal.render_time_ns = 1'000'000'000 + static_cast<TimestampNs>(frame) * 40'000'000;
        signal.features_valid = frame == 0;
        signal.result_valid = frame < 4;
        states.push_back(null_manager.submit(avatar.frame, signal).state);
        CHECK(file_manager.submit(avatar.frame, signal).state == states.back());
    }
    CHECK(states[0] == GateState::avatar);
    CHECK(states[1] == GateState::held);
    CHECK(states[2] == GateState::held);
    CHECK(states[3] == GateState::held);
    CHECK(states[4] == GateState::blank);
    CHECK(states[5] == GateState::blank);
    CHECK_EQ(null_manager.stats().submitted, file_manager.stats().submitted);
    CHECK_EQ(null_manager.stats().blank_frames, file_manager.stats().blank_frames);
    CHECK_EQ(null_diagnostics.codes.size(), file_diagnostics.codes.size());
    CHECK(null_diagnostics.codes == file_diagnostics.codes);

    file_manager.shutdown();
    std::filesystem::remove_all(directory, code);
}

TEST_CASE(output_manager_holds_the_current_frame_not_a_frozen_copy) {
    CapturedDiagnostics captured;
    OutputManager manager(&capture_sink, &captured);
    OutputConfig config;
    config.width = 2;
    config.height = 2;
    config.alpha = AlphaMode::straight;
    CHECK(manager.initialize(std::make_unique<RecordingOutput>(), config));
    auto* recording = static_cast<RecordingOutput*>(manager.output());

    FrameBuffer first = make_frame(2, 2, {10, 20, 30, 255});
    GateSignal signal;
    signal.render_time_ns = 1'000'000'000;
    CHECK(manager.submit(first.frame, signal).state == GateState::avatar);
    CHECK_EQ(recording->last_pixels[0], std::uint8_t{10});

    FrameBuffer second = make_frame(2, 2, {40, 50, 60, 255});
    signal.render_time_ns = 1'100'000'000;
    signal.features_valid = false;
    CHECK(manager.submit(second.frame, signal).state == GateState::held);
    CHECK(recording->last_content == FrameContent::avatar);
    CHECK_EQ(recording->last_pixels[0], std::uint8_t{40});
    CHECK_EQ(manager.stats().held_frames, std::uint64_t{1});

    signal.render_time_ns = 1'150'000'000;
    CHECK(manager.submit(second.frame, signal).state == GateState::blank);
    CHECK(recording->last_content == FrameContent::blank);
    CHECK_EQ(recording->last_pixels[0], std::uint8_t{0});
    CHECK_EQ(recording->last_pixels[3], std::uint8_t{0});
}

TEST_CASE(output_manager_blank_respects_alpha_and_background) {
    OutputConfig transparent;
    transparent.width = 2;
    transparent.height = 2;
    transparent.alpha = AlphaMode::straight;
    std::vector<std::uint8_t> storage;
    OutputFrame blank = avatar::output::make_blank_frame(transparent, 0, 0, 1, storage);
    CHECK(blank.valid());
    CHECK_EQ(blank.pixels[0], std::uint8_t{0});
    CHECK_EQ(blank.pixels[3], std::uint8_t{0});

    OutputConfig opaque;
    opaque.width = 2;
    opaque.height = 2;
    opaque.alpha = AlphaMode::opaque;
    opaque.background = {12, 34, 56, 255};
    blank = avatar::output::make_blank_frame(opaque, 0, 0, 2, storage);
    CHECK(blank.valid());
    CHECK_EQ(blank.pixels[0], std::uint8_t{12});
    CHECK_EQ(blank.pixels[1], std::uint8_t{34});
    CHECK_EQ(blank.pixels[2], std::uint8_t{56});
    CHECK_EQ(blank.pixels[3], std::uint8_t{255});
}

TEST_CASE(output_manager_converts_colour_space_and_alpha) {
    OutputConfig config;
    config.width = 1;
    config.height = 1;
    config.alpha = AlphaMode::opaque;
    config.color_space = ColorSpace::srgb;
    config.background = {0, 0, 0, 255};

    FrameBuffer linear =
        make_frame(1, 1, {128, 0, 0, 128}, AlphaMode::straight, ColorSpace::linear);
    std::vector<std::uint8_t> storage;
    OutputFrame converted;
    std::string error;
    CHECK(avatar::output::convert_to_output(linear.frame, config, storage, converted, &error));
    CHECK(converted.valid());
    // 0.5 linear over black at alpha 0.5 is 0.25 linear -> sRGB ~0.537.
    CHECK(std::abs(static_cast<int>(converted.pixels[0]) - 137) <= 2);
    CHECK_EQ(converted.pixels[3], std::uint8_t{255});

    config.alpha = AlphaMode::premultiplied;
    FrameBuffer straight = make_frame(1, 1, {200, 100, 50, 128});
    CHECK(avatar::output::convert_to_output(straight.frame, config, storage, converted, &error));
    CHECK_EQ(converted.pixels[0], std::uint8_t{100});
    CHECK_EQ(converted.pixels[1], std::uint8_t{50});
    CHECK_EQ(converted.pixels[2], std::uint8_t{25});
    CHECK_EQ(converted.pixels[3], std::uint8_t{128});
}
