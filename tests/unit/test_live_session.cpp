#include "test_registry.h"

#include "avatar/live/live_session.h"

#include <cmath>
#include <string>

namespace {

using avatar::core::DiagnosticCode;
using avatar::core::TimestampNs;
using avatar::live::CameraState;
using avatar::live::EditKind;
using avatar::live::FaceState;
using avatar::live::Hotkey;
using avatar::live::LiveSession;
using avatar::live::OutputState;
using avatar::live::PanicReason;

constexpr TimestampNs kSecond = 1'000'000'000;

} // namespace

TEST_CASE(live_session_starts_stops_and_tracks_elapsed_time) {
    LiveSession session;
    CHECK(!session.is_live());

    session.start(kSecond);
    CHECK(session.is_live());
    CHECK(std::abs(session.elapsed_seconds(3 * kSecond) - 2.0) < 1e-9);
    CHECK(session.metrics(3 * kSecond).live);

    session.stop(4 * kSecond);
    CHECK(!session.is_live());
    CHECK_EQ(session.elapsed_seconds(5 * kSecond), 0.0);
    const auto status = session.status();
    CHECK(!status.live);
    CHECK(status.blocks_output);
    CHECK_EQ(status.sentence, std::string_view("Live is stopped"));
}

TEST_CASE(live_session_reports_actual_fps_and_frame_time) {
    LiveSession session;
    session.start(0);
    for (int frame = 0; frame < 60; ++frame) {
        session.report_frame(static_cast<TimestampNs>(frame) * 16'666'667, 4.0);
    }
    const auto metrics = session.metrics(1'000'000'000);
    CHECK(metrics.fps > 55.0);
    CHECK(metrics.fps < 65.0);
    CHECK(metrics.frame_time_ms > 3.5);
    CHECK(metrics.frame_time_ms < 4.5);

    session.report_frame(1'016'666'667, 6.0);
    const auto smoothed = session.metrics(1'016'666'667);
    CHECK(smoothed.frame_time_ms > 4.0);
    CHECK(smoothed.frame_time_ms < 5.0);
}

TEST_CASE(live_status_prefers_panic_then_camera_then_face_then_output) {
    LiveSession session;
    session.start(0);
    CHECK(session.status().healthy);
    CHECK(session.status().code == DiagnosticCode::unknown);

    session.set_camera(CameraState::lost);
    auto status = session.status();
    CHECK(!status.healthy);
    CHECK(status.blocks_output);
    CHECK(status.code == DiagnosticCode::cap_001);
    CHECK_EQ(status.sentence, std::string_view("Camera disconnected"));
    CHECK(status.action.find("Reconnect") != std::string_view::npos);

    session.set_panic(true, PanicReason::button);
    status = session.status();
    CHECK(status.code == DiagnosticCode::out_007);
    CHECK_EQ(status.sentence, std::string_view("Output is hidden"));
    CHECK(status.action.find("Restore") != std::string_view::npos);
    session.set_panic(false, PanicReason::none);

    session.set_camera(CameraState::ok);
    session.set_face(FaceState::not_visible, 0.0F);
    status = session.status();
    CHECK(status.code == DiagnosticCode::trk_001);
    CHECK_EQ(status.sentence, std::string_view("Can't see your face"));
    CHECK(status.action.find("lighting") != std::string_view::npos);
    CHECK(status.blocks_output);

    session.set_face(FaceState::features_hidden, 0.7F);
    status = session.status();
    CHECK(status.code == DiagnosticCode::trk_008);
    CHECK_EQ(status.sentence, std::string_view("Can't see your mouth or both eyes"));
    CHECK(status.action.find("sunglasses") != std::string_view::npos);
    CHECK(status.blocks_output);

    session.set_face(FaceState::degraded, 0.35F);
    status = session.status();
    CHECK(status.code == DiagnosticCode::trk_004);
    CHECK_EQ(status.sentence, std::string_view("Face tracking is struggling"));
    CHECK(status.action.find("resolution") != std::string_view::npos);
    CHECK(!status.blocks_output);

    session.set_face(FaceState::tracked, 0.9F);
    session.set_output_state(OutputState::blanked);
    status = session.status();
    CHECK(status.code == DiagnosticCode::out_007);
    CHECK_EQ(status.sentence, std::string_view("The output is blank"));
    CHECK(status.blocks_output);

    session.set_output_state(OutputState::live);
    session.set_tracker_available(false);
    status = session.status();
    CHECK(status.code == DiagnosticCode::trk_007);
    CHECK_EQ(status.sentence, std::string_view("Face tracking isn't running"));
}

TEST_CASE(live_panic_toggles_with_the_button_and_hotkey_and_restores) {
    LiveSession session;
    session.start(0);

    CHECK(session.toggle_panic(PanicReason::button));
    CHECK(session.is_panic());
    CHECK(session.panic_reason() == PanicReason::button);
    CHECK(session.metrics(0).output == OutputState::hidden);
    CHECK_EQ(session.status().sentence, std::string_view("Output is hidden"));

    CHECK(!session.toggle_panic(PanicReason::hotkey));
    CHECK(!session.is_panic());
    CHECK(session.panic_reason() == PanicReason::none);
    CHECK(session.metrics(0).output == OutputState::blanked);
    CHECK(session.status().code == DiagnosticCode::out_007);

    CHECK(session.handle_hotkey(Hotkey::panic));
    CHECK(session.is_panic());
    CHECK(session.panic_reason() == PanicReason::hotkey);
    CHECK(session.handle_hotkey(Hotkey::panic));
    CHECK(!session.is_panic());
}

TEST_CASE(live_edit_guard_refuses_pipeline_changes_while_live) {
    LiveSession session;
    session.start(0);

    for (const EditKind kind : {EditKind::avatar, EditKind::calibration, EditKind::camera,
                                EditKind::resolution, EditKind::transport}) {
        const auto decision = session.request_edit(kind);
        CHECK(!decision.allowed);
        CHECK(decision.requires_stop);
        CHECK_EQ(decision.action, std::string("Stop and switch"));
        CHECK(decision.sentence.find("Stop live output") != std::string::npos);
    }

    for (const EditKind kind : {EditKind::outfit, EditKind::appearance}) {
        const auto decision = session.request_edit(kind);
        CHECK(decision.allowed);
        CHECK(!decision.requires_stop);
        CHECK(decision.sentence.find("safe while live") != std::string::npos);
    }

    session.stop(1 * kSecond);
    for (const EditKind kind :
         {EditKind::avatar, EditKind::calibration, EditKind::camera, EditKind::resolution,
          EditKind::transport, EditKind::outfit, EditKind::appearance}) {
        const auto decision = session.request_edit(kind);
        CHECK(decision.allowed);
        CHECK(!decision.requires_stop);
    }
}

TEST_CASE(live_hud_text_carries_every_required_number_and_state) {
    LiveSession session;
    session.start(0);
    session.report_frame(16'666'667, 4.25);
    session.set_face(FaceState::tracked, 0.87F);

    const std::string healthy = session.hud_text(2'000'000'000);
    CHECK(healthy.find("LIVE") != std::string::npos);
    CHECK(healthy.find("00:00:02") != std::string::npos);
    CHECK(healthy.find("fps") != std::string::npos);
    CHECK(healthy.find("frame") != std::string::npos);
    CHECK(healthy.find("tracking 87%") != std::string::npos);
    CHECK(healthy.find("face tracked") != std::string::npos);
    CHECK(healthy.find("output") != std::string::npos);

    session.set_face(FaceState::features_hidden, 0.7F);
    const std::string degraded = session.hud_text(2'000'000'000);
    CHECK(degraded.find("TRK-008") != std::string::npos);
    CHECK(degraded.find("Can't see your mouth or both eyes") != std::string::npos);
    CHECK(degraded.find("-> Remove sunglasses") != std::string::npos);
}

TEST_CASE(live_state_names_are_stable_strings) {
    CHECK_EQ(LiveSession::camera_state_name(CameraState::lost), std::string_view("lost"));
    CHECK_EQ(LiveSession::face_state_name(FaceState::features_hidden),
             std::string_view("eyes/mouth hidden"));
    CHECK_EQ(LiveSession::output_state_name(OutputState::hidden),
             std::string_view("hidden (panic)"));
    CHECK_EQ(LiveSession::panic_reason_name(PanicReason::global_hotkey),
             std::string_view("global_hotkey"));
    CHECK_EQ(LiveSession::edit_kind_name(EditKind::calibration), std::string_view("calibration"));
}
