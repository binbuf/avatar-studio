#include "test_registry.h"

#include "avatar/core/clock.h"
#include "avatar/core/frame.h"
#include "avatar/core/status.h"

#include <cstdint>
#include <string>

namespace {

using avatar::core::FrameClock;
using avatar::core::FrameStamp;
using avatar::core::make_frame_stamp;
using avatar::core::Status;
using avatar::core::StatusCode;
using avatar::core::TimeSource;
using avatar::core::TimestampNs;

/// Deterministic time source: tests move time by hand.
class FakeTimeSource final : public TimeSource {
public:
    TimestampNs now = 0;

    [[nodiscard]] TimestampNs now_ns() const override { return now; }
};

} // namespace

TEST_CASE(clock_reads_the_injected_time_source) {
    FakeTimeSource source;
    source.now = 1'000'000;
    FrameClock clock(source);

    CHECK_EQ(clock.now_ns(), TimestampNs{1'000'000});

    source.now = 1'500'000;
    CHECK_EQ(clock.now_ns(), TimestampNs{1'500'000});
    CHECK_EQ(clock.last_ns(), TimestampNs{1'500'000});
}

TEST_CASE(clock_never_goes_backwards_when_the_source_does) {
    FakeTimeSource source;
    source.now = 2'000;
    FrameClock clock(source);
    CHECK_EQ(clock.now_ns(), TimestampNs{2'000});

    // A non-monotonic source must not leak through to the frame timeline.
    source.now = 1'000;
    CHECK_EQ(clock.now_ns(), TimestampNs{2'000});
    CHECK_EQ(clock.last_ns(), TimestampNs{2'000});

    source.now = 3'000;
    CHECK_EQ(clock.now_ns(), TimestampNs{3'000});
}

TEST_CASE(status_ok_formats_as_ok) {
    const Status status;
    CHECK(status.ok());
    CHECK(static_cast<bool>(status));
    CHECK(status.code() == StatusCode::ok);
    CHECK(status.message().empty());
    CHECK_EQ(status.to_string(), std::string("ok"));
}

TEST_CASE(status_error_formats_code_and_message) {
    const Status status = Status::error(StatusCode::invalid_argument, "frame is too small");
    CHECK(!status.ok());
    CHECK(status.code() == StatusCode::invalid_argument);
    CHECK_EQ(status.message(), std::string("frame is too small"));
    CHECK_EQ(status.to_string(), std::string("invalid_argument: frame is too small"));
}

TEST_CASE(status_error_without_message_formats_as_code_only) {
    const Status status = Status::error(StatusCode::timeout);
    CHECK(!status.ok());
    CHECK_EQ(status.to_string(), std::string("timeout"));
}

TEST_CASE(status_error_with_ok_code_normalises_to_ok) {
    const Status status = Status::error(StatusCode::ok, "ignored");
    CHECK(status.ok());
    CHECK(status.message().empty());
    CHECK_EQ(status.to_string(), std::string("ok"));
}

TEST_CASE(frame_stamp_orders_by_timestamp_then_sequence) {
    const FrameStamp first = make_frame_stamp(10, 1);
    const FrameStamp later = make_frame_stamp(11, 0);
    const FrameStamp same_time = make_frame_stamp(10, 2);

    CHECK(!avatar::core::is_newer(first, first));
    CHECK(avatar::core::is_newer(later, first));
    CHECK(avatar::core::is_newer(same_time, first));
    CHECK(!avatar::core::is_newer(first, later));
}

int main() {
    return avatar::test::run_all();
}
