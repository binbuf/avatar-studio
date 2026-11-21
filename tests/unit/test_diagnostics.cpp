#include "test_registry.h"

#include "avatar/core/clock.h"
#include "avatar/core/diagnostics.h"
#include "avatar/core/status.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace {

using avatar::core::DiagnosticArea;
using avatar::core::DiagnosticCode;
using avatar::core::Error;
using avatar::core::FrameClock;
using avatar::core::LogField;
using avatar::core::LogRecord;
using avatar::core::Logger;
using avatar::core::LogSink;
using avatar::core::ManualTimeSource;
using avatar::core::Severity;
using avatar::core::Status;
using avatar::core::StatusCode;

struct CapturedLog {
    LogRecord record{};
    bool called = false;
};

void capture_sink(void* context, const LogRecord& record) noexcept {
    auto* captured = static_cast<CapturedLog*>(context);
    captured->record = record;
    captured->called = true;
}

[[nodiscard]] std::uint32_t area_bit(DiagnosticArea area) {
    return 1U << static_cast<std::uint32_t>(area);
}

} // namespace

TEST_CASE(diagnostic_registry_has_unique_codes_and_non_empty_keys) {
    CHECK_EQ(avatar::core::kDiagnosticRegistry.size(), std::size_t{69});
    CHECK_EQ(avatar::core::diagnostics().size(), std::size_t{69});

    std::set<std::uint32_t> codes;
    std::set<std::string_view> keys;
    for (const auto& info : avatar::core::kDiagnosticRegistry) {
        CHECK(codes.insert(static_cast<std::uint32_t>(info.code)).second);
        CHECK(!info.message_key.empty());
        CHECK(!info.action_key.empty());
        CHECK(info.message_key != info.action_key);
        CHECK(keys.insert(info.message_key).second);
        CHECK(keys.insert(info.action_key).second);
        CHECK(info.area == avatar::core::diagnostic_area(info.code));
        CHECK(avatar::core::diagnostic_number(info.code) >= 1);
        CHECK(avatar::core::find_diagnostic(info.code) == &info);
    }
}

TEST_CASE(diagnostic_registry_covers_every_area) {
    std::uint32_t covered = 0;
    for (const auto& info : avatar::core::kDiagnosticRegistry) {
        covered |= area_bit(info.area);
    }
    for (std::uint32_t value = 1; value <= 9; ++value) {
        CHECK((covered & area_bit(static_cast<DiagnosticArea>(value))) != 0);
    }
}

TEST_CASE(diagnostic_codes_format_and_parse_canonically) {
    CHECK_EQ(avatar::core::format_diagnostic_code(DiagnosticCode::cap_001),
             std::string("CAP-001"));
    CHECK_EQ(avatar::core::format_diagnostic_code(DiagnosticCode::trk_002),
             std::string("TRK-002"));
    CHECK_EQ(avatar::core::format_diagnostic_code(DiagnosticCode::avt_101),
             std::string("AVT-101"));
    CHECK_EQ(avatar::core::format_diagnostic_code(DiagnosticCode::vcam_007),
             std::string("VCAM-007"));
    CHECK_EQ(avatar::core::format_diagnostic_code(DiagnosticCode::unknown),
             std::string("UNKNOWN"));

    const auto parsed = avatar::core::parse_diagnostic_code("TRK-002");
    CHECK(parsed.has_value());
    CHECK(parsed.has_value() && *parsed == DiagnosticCode::trk_002);

    CHECK(!avatar::core::parse_diagnostic_code("TRK-999").has_value());
    CHECK(!avatar::core::parse_diagnostic_code("NOPE-001").has_value());
    CHECK(!avatar::core::parse_diagnostic_code("TRK-2").has_value());
    CHECK(!avatar::core::parse_diagnostic_code("TRK-002x").has_value());
    CHECK(!avatar::core::parse_diagnostic_code("").has_value());
}

TEST_CASE(diagnostic_severities_match_the_registry) {
    CHECK(avatar::core::diagnostic_severity(DiagnosticCode::cap_003) ==
          Severity::warning);
    CHECK(avatar::core::diagnostic_severity(DiagnosticCode::cap_008) ==
          Severity::info);
    CHECK(avatar::core::diagnostic_severity(DiagnosticCode::trk_007) ==
          Severity::fatal);
    CHECK(avatar::core::diagnostic_severity(DiagnosticCode::gpu_002) ==
          Severity::fatal);
    CHECK(avatar::core::diagnostic_severity(DiagnosticCode::avt_111) ==
          Severity::info);
    CHECK(avatar::core::diagnostic_severity(DiagnosticCode::sec_004) ==
          Severity::fatal);
}

TEST_CASE(logger_emits_the_code_severity_timestamp_and_structured_fields) {
    ManualTimeSource source(1234);
    FrameClock clock(source);
    CapturedLog captured;
    Logger logger(clock, capture_sink, &captured);

    logger.log(DiagnosticCode::cap_002,
               {LogField{"device", "Cam Link"},
                LogField{"state", "busy"}});

    CHECK(captured.called);
    CHECK_EQ(captured.record.timestamp_ns, avatar::core::TimestampNs{1234});
    CHECK(captured.record.code == DiagnosticCode::cap_002);
    CHECK(captured.record.severity == Severity::error);
    CHECK_EQ(captured.record.field_count, std::uint8_t{2});
    CHECK_EQ(captured.record.fields[0].key, std::string_view("device"));
    CHECK_EQ(captured.record.fields[0].value, std::string_view("Cam Link"));
    CHECK_EQ(captured.record.fields[1].key, std::string_view("state"));
    CHECK_EQ(captured.record.fields[1].value, std::string_view("busy"));
    CHECK_EQ(logger.records_written(), std::uint64_t{1});
    CHECK_EQ(logger.fields_redacted(), std::uint64_t{0});
}

TEST_CASE(logger_redacts_forbidden_payload_keys) {
    CapturedLog captured;
    Logger logger(capture_sink, &captured);

    logger.log_at(42,
                  DiagnosticCode::trk_002,
                  {LogField{"landmarks", "478 points"},
                   LogField{"device", "Cam Link"}});

    CHECK(captured.called);
    CHECK_EQ(captured.record.timestamp_ns, avatar::core::TimestampNs{42});
    CHECK_EQ(captured.record.field_count, std::uint8_t{1});
    CHECK_EQ(captured.record.dropped_fields, std::uint8_t{1});
    CHECK_EQ(captured.record.fields[0].key, std::string_view("device"));
    CHECK_EQ(logger.fields_redacted(), std::uint64_t{1});
    CHECK(avatar::core::is_forbidden_log_key("landmarks"));
    CHECK(avatar::core::is_forbidden_log_key("frame_data"));
    CHECK(avatar::core::is_forbidden_log_key("left_landmarks"));
    CHECK(!avatar::core::is_forbidden_log_key("frame_id"));
}

TEST_CASE(logger_counts_fields_beyond_the_fixed_capacity) {
    CapturedLog captured;
    Logger logger(capture_sink, &captured);

    const std::initializer_list<LogField> fields = {
        LogField{"f0", "v0"}, LogField{"f1", "v1"}, LogField{"f2", "v2"},
        LogField{"f3", "v3"}, LogField{"f4", "v4"}, LogField{"f5", "v5"},
        LogField{"f6", "v6"}, LogField{"f7", "v7"}, LogField{"f8", "v8"},
        LogField{"f9", "v9"}};
    logger.log_at(0, DiagnosticCode::out_006, fields);

    CHECK(captured.called);
    CHECK_EQ(captured.record.field_count, std::uint8_t{8});
    CHECK_EQ(captured.record.dropped_fields, std::uint8_t{2});
    CHECK_EQ(logger.fields_overflowed(), std::uint64_t{2});
}

TEST_CASE(logger_without_a_sink_counts_the_record_as_dropped) {
    Logger logger(static_cast<LogSink>(nullptr));
    logger.log_at(0, DiagnosticCode::gpu_001);
    CHECK_EQ(logger.records_written(), std::uint64_t{0});
    CHECK_EQ(logger.records_dropped(), std::uint64_t{1});
}

TEST_CASE(error_carries_a_stable_code_message_and_optional_detail) {
    const Error error{DiagnosticCode::trk_002, "Face lost", "hold_ms=150"};
    CHECK_EQ(error.to_string(),
             std::string("TRK-002: Face lost (detail: hold_ms=150)"));

    const Error brief{DiagnosticCode::cap_008, "No camera", {}};
    CHECK_EQ(brief.to_string(), std::string("CAP-008: No camera"));
}

TEST_CASE(status_detail_is_optional_and_formatted) {
    const Status status = Status::error(StatusCode::internal_error,
                                        "ring publish failed",
                                        "capacity=4");
    CHECK(!status.ok());
    CHECK_EQ(status.detail(), std::string("capacity=4"));
    CHECK_EQ(status.to_string(),
             std::string("internal_error: ring publish failed (detail: capacity=4)"));

    const Status plain = Status::error(StatusCode::timeout, "late");
    CHECK(plain.detail().empty());
    CHECK_EQ(plain.to_string(), std::string("timeout: late"));
}
