#include "avatar/core/status.h"

#include <utility>

namespace avatar::core {

std::string_view status_code_name(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::ok:
            return "ok";
        case StatusCode::cancelled:
            return "cancelled";
        case StatusCode::invalid_argument:
            return "invalid_argument";
        case StatusCode::not_found:
            return "not_found";
        case StatusCode::unsupported:
            return "unsupported";
        case StatusCode::timeout:
            return "timeout";
        case StatusCode::internal_error:
            return "internal_error";
    }
    return "unknown";
}

Status::Status(StatusCode code, std::string message, std::string detail) : code_(code) {
    if (code_ != StatusCode::ok) {
        message_ = std::move(message);
        detail_ = std::move(detail);
    }
}

Status Status::success() noexcept {
    return Status{};
}

Status Status::error(StatusCode code, std::string message, std::string detail) {
    return Status{code, std::move(message), std::move(detail)};
}

std::string Status::to_string() const {
    if (code_ == StatusCode::ok) {
        return std::string(status_code_name(code_));
    }
    std::string result(status_code_name(code_));
    if (!message_.empty()) {
        result += ": ";
        result += message_;
    }
    if (!detail_.empty()) {
        result += " (detail: ";
        result += detail_;
        result += ')';
    }
    return result;
}

} // namespace avatar::core
