#include "avatar/core/diagnostics.h"

#include <array>
#include <cstdio>

namespace avatar::core {
namespace {

constexpr std::array<std::string_view, 15> kForbiddenLogKeys = {
    "frame",        "frame_data",       "frame_pixels",  "pixels",       "pixel_data",
    "image",        "image_data",       "landmark",      "landmarks",    "landmark_values",
    "face_profile", "profile_contents", "file_contents", "window_title", "raw_payload",
};

[[nodiscard]] constexpr DiagnosticArea area_from_prefix(std::string_view prefix) noexcept {
    for (std::uint32_t value = 1; value <= 9; ++value) {
        const auto area = static_cast<DiagnosticArea>(value);
        if (area_prefix(area) == prefix) {
            return area;
        }
    }
    return DiagnosticArea::unknown;
}

} // namespace

std::span<const DiagnosticInfo> diagnostics() noexcept {
    return std::span<const DiagnosticInfo>(kDiagnosticRegistry.data(), kDiagnosticRegistry.size());
}

std::string format_diagnostic_code(DiagnosticCode code) {
    if (code == DiagnosticCode::unknown) {
        return "UNKNOWN";
    }
    const std::string_view prefix = area_prefix(diagnostic_area(code));
    std::array<char, 16> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.*s-%03u", static_cast<int>(prefix.size()),
                  prefix.data(), diagnostic_number(code));
    return std::string(buffer.data());
}

std::optional<DiagnosticCode> parse_diagnostic_code(std::string_view text) noexcept {
    const std::size_t dash = text.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view prefix = text.substr(0, dash);
    const std::string_view digits = text.substr(dash + 1);
    if (digits.size() != 3) {
        return std::nullopt;
    }
    std::uint32_t number = 0;
    for (char character : digits) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        number = number * 10U + static_cast<std::uint32_t>(character - '0');
    }
    const DiagnosticArea area = area_from_prefix(prefix);
    if (area == DiagnosticArea::unknown) {
        return std::nullopt;
    }
    const auto code =
        static_cast<DiagnosticCode>(static_cast<std::uint32_t>(area) * 1000U + number);
    if (find_diagnostic(code) == nullptr) {
        return std::nullopt;
    }
    return code;
}

std::string Error::to_string() const {
    std::string result = format_diagnostic_code(code);
    if (!message.empty()) {
        result += ": ";
        result += message;
    }
    if (!detail.empty()) {
        result += " (detail: ";
        result += detail;
        result += ')';
    }
    return result;
}

bool is_forbidden_log_key(std::string_view key) noexcept {
    for (std::string_view forbidden : kForbiddenLogKeys) {
        if (key == forbidden) {
            return true;
        }
    }
    // Catch prefixed/suffixed variants such as `left_landmarks` or
    // `frame_pixels` without banning useful metadata keys such as `frame_id`
    // or `frame_time`.
    constexpr std::array<std::string_view, 6> kForbiddenFragments = {
        "landmark", "pixel", "image", "frame_data", "face_profile", "raw_payload",
    };
    for (std::string_view fragment : kForbiddenFragments) {
        if (key.find(fragment) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

Logger::Logger(LogSink sink, void* context) noexcept : sink_(sink), context_(context) {}

Logger::Logger(FrameClock& clock, LogSink sink, void* context) noexcept
    : clock_(&clock), sink_(sink), context_(context) {}

void Logger::log(DiagnosticCode code, std::initializer_list<LogField> fields) noexcept {
    log_at(clock_ != nullptr ? clock_->now_ns() : 0, code, fields);
}

void Logger::log_at(TimestampNs timestamp_ns, DiagnosticCode code,
                    std::initializer_list<LogField> fields) noexcept {
    LogRecord record;
    record.timestamp_ns = timestamp_ns;
    record.code = code;
    record.area = diagnostic_area(code);
    record.severity = diagnostic_severity(code);

    for (const LogField& field : fields) {
        if (is_forbidden_log_key(field.key)) {
            fields_redacted_.fetch_add(1, std::memory_order_relaxed);
            ++record.dropped_fields;
            continue;
        }
        if (record.field_count >= kMaxLogFields) {
            fields_overflowed_.fetch_add(1, std::memory_order_relaxed);
            ++record.dropped_fields;
            continue;
        }
        record.fields[record.field_count] = field;
        ++record.field_count;
    }

    if (sink_ == nullptr) {
        records_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    sink_(context_, record);
    records_written_.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Logger::records_written() const noexcept {
    return records_written_.load(std::memory_order_relaxed);
}

std::uint64_t Logger::records_dropped() const noexcept {
    return records_dropped_.load(std::memory_order_relaxed);
}

std::uint64_t Logger::fields_redacted() const noexcept {
    return fields_redacted_.load(std::memory_order_relaxed);
}

std::uint64_t Logger::fields_overflowed() const noexcept {
    return fields_overflowed_.load(std::memory_order_relaxed);
}

} // namespace avatar::core
