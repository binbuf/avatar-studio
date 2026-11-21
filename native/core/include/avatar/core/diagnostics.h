#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "avatar/core/clock.h"

namespace avatar::core {

/// Stable support-code areas. The numeric value is the thousands digit of the
/// code: `CAP-001` is 1001, `TRK-001` is 2001, and so on.
enum class DiagnosticArea : std::uint8_t {
    unknown = 0,
    cap = 1,
    trk = 2,
    rig = 3,
    avt = 4,
    out = 5,
    vcam = 6,
    gpu = 7,
    cfg = 8,
    sec = 9,
};

/// Severity is separate from the code: the same condition may be a warning in
/// one build and an error in another. `Fatal` means the affected feature stops;
/// the application keeps running.
enum class Severity : std::uint8_t {
    info = 0,
    warning,
    error,
    fatal,
};

/// Stable diagnostic codes. Codes are permanent: adding one is additive, and a
/// retired code is never reused or renumbered.
///
/// The numeric values encode `<area><NNN>` so a code can be parsed and compared
/// without a string table. The registry table below is the metadata source of
/// truth; `docs/design/diagnostics.md` is generated from it from T06 onwards.
enum class DiagnosticCode : std::uint32_t {
    unknown = 0,

    // CAP - capture.
    cap_001 = 1001,
    cap_002 = 1002,
    cap_003 = 1003,
    cap_004 = 1004,
    cap_005 = 1005,
    cap_006 = 1006,
    cap_007 = 1007,
    cap_008 = 1008,

    // TRK - tracking.
    trk_001 = 2001,
    trk_002 = 2002,
    trk_003 = 2003,
    trk_004 = 2004,
    trk_005 = 2005,
    trk_006 = 2006,
    trk_007 = 2007,
    trk_008 = 2008,
    trk_009 = 2009,

    // RIG - face frame and calibration.
    rig_001 = 3001,
    rig_002 = 3002,
    rig_003 = 3003,
    rig_004 = 3004,
    rig_005 = 3005,
    rig_006 = 3006,

    // AVT - avatar packages and library.
    avt_101 = 4101,
    avt_102 = 4102,
    avt_103 = 4103,
    avt_104 = 4104,
    avt_105 = 4105,
    avt_106 = 4106,
    avt_107 = 4107,
    avt_108 = 4108,
    avt_109 = 4109,
    avt_110 = 4110,
    avt_111 = 4111,
    avt_112 = 4112,
    avt_113 = 4113,
    avt_114 = 4114,
    avt_115 = 4115,
    avt_116 = 4116,
    avt_117 = 4117,
    avt_120 = 4120,

    // OUT - output and transports.
    out_001 = 5001,
    out_002 = 5002,
    out_003 = 5003,
    out_004 = 5004,
    out_005 = 5005,
    out_006 = 5006,
    out_007 = 5007,

    // VCAM - OS virtual camera.
    vcam_001 = 6001,
    vcam_002 = 6002,
    vcam_003 = 6003,
    vcam_004 = 6004,
    vcam_005 = 6005,
    vcam_006 = 6006,
    vcam_007 = 6007,

    // GPU - renderer and readback.
    gpu_001 = 7001,
    gpu_002 = 7002,
    gpu_003 = 7003,
    gpu_004 = 7004,

    // CFG - settings, profiles, user data.
    cfg_001 = 8001,
    cfg_002 = 8002,
    cfg_003 = 8003,
    cfg_004 = 8004,
    cfg_005 = 8005,
    cfg_006 = 8006,

    // SEC - security.
    sec_001 = 9001,
    sec_002 = 9002,
    sec_003 = 9003,
    sec_004 = 9004,
};

/// One row of the registry. Message and action keys are stable identifiers into
/// the application string table; they are never the user-facing sentence.
struct DiagnosticInfo {
    DiagnosticCode code;
    DiagnosticArea area;
    Severity severity;
    std::string_view message_key;
    std::string_view action_key;
};

/// The complete registry. Order follows the enum for easy review; lookups use
/// `find_diagnostic`, and a unit test asserts the values are unique.
inline constexpr std::array<DiagnosticInfo, 69> kDiagnosticRegistry = {{
    {DiagnosticCode::cap_001, DiagnosticArea::cap, Severity::error,
     "diag.cap.001.message", "diag.cap.001.action"},
    {DiagnosticCode::cap_002, DiagnosticArea::cap, Severity::error,
     "diag.cap.002.message", "diag.cap.002.action"},
    {DiagnosticCode::cap_003, DiagnosticArea::cap, Severity::warning,
     "diag.cap.003.message", "diag.cap.003.action"},
    {DiagnosticCode::cap_004, DiagnosticArea::cap, Severity::error,
     "diag.cap.004.message", "diag.cap.004.action"},
    {DiagnosticCode::cap_005, DiagnosticArea::cap, Severity::warning,
     "diag.cap.005.message", "diag.cap.005.action"},
    {DiagnosticCode::cap_006, DiagnosticArea::cap, Severity::error,
     "diag.cap.006.message", "diag.cap.006.action"},
    {DiagnosticCode::cap_007, DiagnosticArea::cap, Severity::warning,
     "diag.cap.007.message", "diag.cap.007.action"},
    {DiagnosticCode::cap_008, DiagnosticArea::cap, Severity::info,
     "diag.cap.008.message", "diag.cap.008.action"},

    {DiagnosticCode::trk_001, DiagnosticArea::trk, Severity::info,
     "diag.trk.001.message", "diag.trk.001.action"},
    {DiagnosticCode::trk_002, DiagnosticArea::trk, Severity::warning,
     "diag.trk.002.message", "diag.trk.002.action"},
    {DiagnosticCode::trk_003, DiagnosticArea::trk, Severity::error,
     "diag.trk.003.message", "diag.trk.003.action"},
    {DiagnosticCode::trk_004, DiagnosticArea::trk, Severity::warning,
     "diag.trk.004.message", "diag.trk.004.action"},
    {DiagnosticCode::trk_005, DiagnosticArea::trk, Severity::warning,
     "diag.trk.005.message", "diag.trk.005.action"},
    {DiagnosticCode::trk_006, DiagnosticArea::trk, Severity::error,
     "diag.trk.006.message", "diag.trk.006.action"},
    {DiagnosticCode::trk_007, DiagnosticArea::trk, Severity::fatal,
     "diag.trk.007.message", "diag.trk.007.action"},
    {DiagnosticCode::trk_008, DiagnosticArea::trk, Severity::warning,
     "diag.trk.008.message", "diag.trk.008.action"},
    {DiagnosticCode::trk_009, DiagnosticArea::trk, Severity::warning,
     "diag.trk.009.message", "diag.trk.009.action"},

    {DiagnosticCode::rig_001, DiagnosticArea::rig, Severity::warning,
     "diag.rig.001.message", "diag.rig.001.action"},
    {DiagnosticCode::rig_002, DiagnosticArea::rig, Severity::warning,
     "diag.rig.002.message", "diag.rig.002.action"},
    {DiagnosticCode::rig_003, DiagnosticArea::rig, Severity::warning,
     "diag.rig.003.message", "diag.rig.003.action"},
    {DiagnosticCode::rig_004, DiagnosticArea::rig, Severity::error,
     "diag.rig.004.message", "diag.rig.004.action"},
    {DiagnosticCode::rig_005, DiagnosticArea::rig, Severity::info,
     "diag.rig.005.message", "diag.rig.005.action"},
    {DiagnosticCode::rig_006, DiagnosticArea::rig, Severity::warning,
     "diag.rig.006.message", "diag.rig.006.action"},

    {DiagnosticCode::avt_101, DiagnosticArea::avt, Severity::error,
     "diag.avt.101.message", "diag.avt.101.action"},
    {DiagnosticCode::avt_102, DiagnosticArea::avt, Severity::error,
     "diag.avt.102.message", "diag.avt.102.action"},
    {DiagnosticCode::avt_103, DiagnosticArea::avt, Severity::error,
     "diag.avt.103.message", "diag.avt.103.action"},
    {DiagnosticCode::avt_104, DiagnosticArea::avt, Severity::error,
     "diag.avt.104.message", "diag.avt.104.action"},
    {DiagnosticCode::avt_105, DiagnosticArea::avt, Severity::error,
     "diag.avt.105.message", "diag.avt.105.action"},
    {DiagnosticCode::avt_106, DiagnosticArea::avt, Severity::error,
     "diag.avt.106.message", "diag.avt.106.action"},
    {DiagnosticCode::avt_107, DiagnosticArea::avt, Severity::error,
     "diag.avt.107.message", "diag.avt.107.action"},
    {DiagnosticCode::avt_108, DiagnosticArea::avt, Severity::warning,
     "diag.avt.108.message", "diag.avt.108.action"},
    {DiagnosticCode::avt_109, DiagnosticArea::avt, Severity::error,
     "diag.avt.109.message", "diag.avt.109.action"},
    {DiagnosticCode::avt_110, DiagnosticArea::avt, Severity::error,
     "diag.avt.110.message", "diag.avt.110.action"},
    {DiagnosticCode::avt_111, DiagnosticArea::avt, Severity::info,
     "diag.avt.111.message", "diag.avt.111.action"},
    {DiagnosticCode::avt_112, DiagnosticArea::avt, Severity::error,
     "diag.avt.112.message", "diag.avt.112.action"},
    {DiagnosticCode::avt_113, DiagnosticArea::avt, Severity::error,
     "diag.avt.113.message", "diag.avt.113.action"},
    {DiagnosticCode::avt_114, DiagnosticArea::avt, Severity::error,
     "diag.avt.114.message", "diag.avt.114.action"},
    {DiagnosticCode::avt_115, DiagnosticArea::avt, Severity::error,
     "diag.avt.115.message", "diag.avt.115.action"},
    {DiagnosticCode::avt_116, DiagnosticArea::avt, Severity::warning,
     "diag.avt.116.message", "diag.avt.116.action"},
    {DiagnosticCode::avt_117, DiagnosticArea::avt, Severity::error,
     "diag.avt.117.message", "diag.avt.117.action"},
    {DiagnosticCode::avt_120, DiagnosticArea::avt, Severity::error,
     "diag.avt.120.message", "diag.avt.120.action"},

    {DiagnosticCode::out_001, DiagnosticArea::out, Severity::error,
     "diag.out.001.message", "diag.out.001.action"},
    {DiagnosticCode::out_002, DiagnosticArea::out, Severity::warning,
     "diag.out.002.message", "diag.out.002.action"},
    {DiagnosticCode::out_003, DiagnosticArea::out, Severity::warning,
     "diag.out.003.message", "diag.out.003.action"},
    {DiagnosticCode::out_004, DiagnosticArea::out, Severity::error,
     "diag.out.004.message", "diag.out.004.action"},
    {DiagnosticCode::out_005, DiagnosticArea::out, Severity::warning,
     "diag.out.005.message", "diag.out.005.action"},
    {DiagnosticCode::out_006, DiagnosticArea::out, Severity::info,
     "diag.out.006.message", "diag.out.006.action"},
    {DiagnosticCode::out_007, DiagnosticArea::out, Severity::info,
     "diag.out.007.message", "diag.out.007.action"},

    {DiagnosticCode::vcam_001, DiagnosticArea::vcam, Severity::error,
     "diag.vcam.001.message", "diag.vcam.001.action"},
    {DiagnosticCode::vcam_002, DiagnosticArea::vcam, Severity::warning,
     "diag.vcam.002.message", "diag.vcam.002.action"},
    {DiagnosticCode::vcam_003, DiagnosticArea::vcam, Severity::warning,
     "diag.vcam.003.message", "diag.vcam.003.action"},
    {DiagnosticCode::vcam_004, DiagnosticArea::vcam, Severity::info,
     "diag.vcam.004.message", "diag.vcam.004.action"},
    {DiagnosticCode::vcam_005, DiagnosticArea::vcam, Severity::error,
     "diag.vcam.005.message", "diag.vcam.005.action"},
    {DiagnosticCode::vcam_006, DiagnosticArea::vcam, Severity::error,
     "diag.vcam.006.message", "diag.vcam.006.action"},
    {DiagnosticCode::vcam_007, DiagnosticArea::vcam, Severity::error,
     "diag.vcam.007.message", "diag.vcam.007.action"},

    {DiagnosticCode::gpu_001, DiagnosticArea::gpu, Severity::error,
     "diag.gpu.001.message", "diag.gpu.001.action"},
    {DiagnosticCode::gpu_002, DiagnosticArea::gpu, Severity::fatal,
     "diag.gpu.002.message", "diag.gpu.002.action"},
    {DiagnosticCode::gpu_003, DiagnosticArea::gpu, Severity::error,
     "diag.gpu.003.message", "diag.gpu.003.action"},
    {DiagnosticCode::gpu_004, DiagnosticArea::gpu, Severity::error,
     "diag.gpu.004.message", "diag.gpu.004.action"},

    {DiagnosticCode::cfg_001, DiagnosticArea::cfg, Severity::warning,
     "diag.cfg.001.message", "diag.cfg.001.action"},
    {DiagnosticCode::cfg_002, DiagnosticArea::cfg, Severity::error,
     "diag.cfg.002.message", "diag.cfg.002.action"},
    {DiagnosticCode::cfg_003, DiagnosticArea::cfg, Severity::error,
     "diag.cfg.003.message", "diag.cfg.003.action"},
    {DiagnosticCode::cfg_004, DiagnosticArea::cfg, Severity::error,
     "diag.cfg.004.message", "diag.cfg.004.action"},
    {DiagnosticCode::cfg_005, DiagnosticArea::cfg, Severity::warning,
     "diag.cfg.005.message", "diag.cfg.005.action"},
    {DiagnosticCode::cfg_006, DiagnosticArea::cfg, Severity::warning,
     "diag.cfg.006.message", "diag.cfg.006.action"},

    {DiagnosticCode::sec_001, DiagnosticArea::sec, Severity::fatal,
     "diag.sec.001.message", "diag.sec.001.action"},
    {DiagnosticCode::sec_002, DiagnosticArea::sec, Severity::warning,
     "diag.sec.002.message", "diag.sec.002.action"},
    {DiagnosticCode::sec_003, DiagnosticArea::sec, Severity::error,
     "diag.sec.003.message", "diag.sec.003.action"},
    {DiagnosticCode::sec_004, DiagnosticArea::sec, Severity::fatal,
     "diag.sec.004.message", "diag.sec.004.action"},
}};

[[nodiscard]] constexpr const DiagnosticInfo* find_diagnostic(
    DiagnosticCode code) noexcept {
    for (const DiagnosticInfo& info : kDiagnosticRegistry) {
        if (info.code == code) {
            return &info;
        }
    }
    return nullptr;
}

[[nodiscard]] inline Severity diagnostic_severity(DiagnosticCode code) noexcept {
    const DiagnosticInfo* info = find_diagnostic(code);
    return info != nullptr ? info->severity : Severity::info;
}

[[nodiscard]] constexpr DiagnosticArea diagnostic_area(DiagnosticCode code) noexcept {
    const auto value = static_cast<std::uint32_t>(code);
    const auto area = static_cast<std::uint32_t>(value / 1000U);
    if (area < 1U || area > 9U) {
        return DiagnosticArea::unknown;
    }
    return static_cast<DiagnosticArea>(area);
}

[[nodiscard]] constexpr std::uint32_t diagnostic_number(
    DiagnosticCode code) noexcept {
    return static_cast<std::uint32_t>(code) % 1000U;
}

[[nodiscard]] constexpr std::string_view area_prefix(
    DiagnosticArea area) noexcept {
    switch (area) {
        case DiagnosticArea::cap:
            return "CAP";
        case DiagnosticArea::trk:
            return "TRK";
        case DiagnosticArea::rig:
            return "RIG";
        case DiagnosticArea::avt:
            return "AVT";
        case DiagnosticArea::out:
            return "OUT";
        case DiagnosticArea::vcam:
            return "VCAM";
        case DiagnosticArea::gpu:
            return "GPU";
        case DiagnosticArea::cfg:
            return "CFG";
        case DiagnosticArea::sec:
            return "SEC";
        case DiagnosticArea::unknown:
            break;
    }
    return "???";
}

[[nodiscard]] constexpr std::string_view severity_name(
    Severity severity) noexcept {
    switch (severity) {
        case Severity::info:
            return "info";
        case Severity::warning:
            return "warning";
        case Severity::error:
            return "error";
        case Severity::fatal:
            return "fatal";
    }
    return "unknown";
}

[[nodiscard]] std::span<const DiagnosticInfo> diagnostics() noexcept;

/// Formats a code as `AREA-NNN` (`TRK-002`). Intended for logs, UI and support,
/// never for the data plane.
[[nodiscard]] std::string format_diagnostic_code(DiagnosticCode code);

/// Parses a canonical `AREA-NNN` string. Returns nullopt for an unknown area,
/// malformed text, or a code that is not in the registry.
[[nodiscard]] std::optional<DiagnosticCode> parse_diagnostic_code(
    std::string_view text) noexcept;

/// A user-visible error carrying a stable code plus a short message and
/// optional detail. No exception is thrown across the public core API.
struct Error {
    DiagnosticCode code = DiagnosticCode::unknown;
    std::string message;
    std::string detail;

    [[nodiscard]] std::string to_string() const;
};

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxLogFields = 8;

struct LogField {
    std::string_view key;
    std::string_view value;
};

/// One structured log record. `fields` are string views owned by the caller for
/// the duration of the sink call; a sink that retains data must copy it.
struct LogRecord {
    TimestampNs timestamp_ns = 0;
    DiagnosticCode code = DiagnosticCode::unknown;
    DiagnosticArea area = DiagnosticArea::unknown;
    Severity severity = Severity::info;
    std::array<LogField, kMaxLogFields> fields{};
    std::uint8_t field_count = 0;
    std::uint8_t dropped_fields = 0;
};

using LogSink = void (*)(void* context, const LogRecord& record) noexcept;

/// True for keys that would carry frame data, landmark values, image contents
/// or other user-private payloads. The logger drops these fields and counts
/// them rather than relying on reviewer discipline.
[[nodiscard]] bool is_forbidden_log_key(std::string_view key) noexcept;

/// Emits a stable code plus structured fields. Logging is for state
/// transitions, not per-frame data; the logger is not on the data-plane fast
/// path and performs no allocation.
class Logger {
public:
    Logger(LogSink sink, void* context = nullptr) noexcept;
    Logger(FrameClock& clock, LogSink sink, void* context = nullptr) noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /// Timestamp is taken from the injected FrameClock, or 0 when the logger
    /// was constructed without one.
    void log(DiagnosticCode code,
             std::initializer_list<LogField> fields = {}) noexcept;

    void log_at(TimestampNs timestamp_ns,
                DiagnosticCode code,
                std::initializer_list<LogField> fields = {}) noexcept;

    [[nodiscard]] std::uint64_t records_written() const noexcept;
    [[nodiscard]] std::uint64_t records_dropped() const noexcept;
    [[nodiscard]] std::uint64_t fields_redacted() const noexcept;
    [[nodiscard]] std::uint64_t fields_overflowed() const noexcept;

private:
    FrameClock* clock_ = nullptr;
    LogSink sink_ = nullptr;
    void* context_ = nullptr;
    std::atomic<std::uint64_t> records_written_{0};
    std::atomic<std::uint64_t> records_dropped_{0};
    std::atomic<std::uint64_t> fields_redacted_{0};
    std::atomic<std::uint64_t> fields_overflowed_{0};
};

} // namespace avatar::core
