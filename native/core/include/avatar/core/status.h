#pragma once

#include <string>
#include <string_view>

namespace avatar::core {

/// Stable, coarse error categories for the framework-free core.
///
/// User-facing diagnostics codes (e.g. `CAP-002`) belong to the diagnostics
/// registry; a StatusCode only classifies an operation result.
enum class StatusCode : int {
    ok = 0,
    cancelled,
    invalid_argument,
    not_found,
    unsupported,
    timeout,
    internal_error,
};

[[nodiscard]] std::string_view status_code_name(StatusCode code) noexcept;

/// A value-or-error result. The core avoids exceptions across subsystem and
/// language boundaries, so fallible functions return a Status (or a
/// StatusOr-style pair of Status and value).
class Status {
public:
    Status() noexcept = default;

    /// A Status constructed with StatusCode::ok drops the message; ok is ok.
    Status(StatusCode code, std::string message = {});

    [[nodiscard]] static Status success() noexcept;
    [[nodiscard]] static Status error(StatusCode code, std::string message = {});

    [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::ok; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] StatusCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// "ok" for success, otherwise "<code>: <message>" (or just "<code>").
    [[nodiscard]] std::string to_string() const;

private:
    StatusCode code_ = StatusCode::ok;
    std::string message_;
};

} // namespace avatar::core
