cmake_minimum_required(VERSION 3.25)

project(avatar_studio LANGUAGES C CXX)

option(AVATAR_TESTS "Build the native unit test suite" ON)
option(AVATAR_BENCHMARKS "Build the native microbenchmark suite" ON)
option(AVATAR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(AVATAR_GODEXTENSION "Build the Godot GDExtension adapter when godot-cpp is present" ON)

# Single-config generators (Linux/macOS, Ninja) need an explicit default. Multi-config
# generators (Visual Studio, Xcode) select the configuration with --config instead.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(avatar_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
        if(AVATAR_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(AVATAR_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Pinned dependencies
#
# scripts/bootstrap.ps1 / scripts/bootstrap.sh populate .tools/ from pinned
# URLs and verify SHA256. CMake only consumes the local copy: it must never
# download anything at configure or build time.
# ---------------------------------------------------------------------------
set(AVATAR_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.tools"
    CACHE PATH "Directory populated by scripts/bootstrap.*")
set(AVATAR_NLOHMANN_JSON_VERSION "3.11.3" CACHE STRING "Pinned nlohmann/json version")
set(AVATAR_NLOHMANN_JSON_INCLUDE_DIR "${AVATAR_TOOLS_DIR}/nlohmann_json/include")
set(AVATAR_GODOTCPP_DIR "${AVATAR_TOOLS_DIR}/godot-cpp"
    CACHE PATH "Pinned godot-cpp checkout populated by scripts/bootstrap.*")

if(NOT EXISTS "${AVATAR_NLOHMANN_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
    message(FATAL_ERROR
        "Pinned dependency nlohmann/json ${AVATAR_NLOHMANN_JSON_VERSION} is missing from "
        "${AVATAR_NLOHMANN_JSON_INCLUDE_DIR}.\n"
        "Run scripts/bootstrap.ps1 (Windows) or scripts/bootstrap.sh (macOS/Linux) first. "
        "CMake never downloads dependencies.")
endif()

add_library(avatar_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS avatar_json)
# Third-party headers are consumed with SYSTEM so the project's /W4 /WX policy
# does not turn library-internal warnings into build failures.
target_include_directories(avatar_json SYSTEM INTERFACE "${AVATAR_NLOHMANN_JSON_INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# Build provenance
#
# The performance harness records the commit it was built from. The runtime
# lookup in avatar_bench is authoritative (it also detects a dirty tree); these
# definitions are the fallback for a binary built or run without git.
# ---------------------------------------------------------------------------
set(AVATAR_GIT_COMMIT "unknown")
set(AVATAR_GIT_DIRTY 0)
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE AVATAR_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT AVATAR_GIT_COMMIT)
        set(AVATAR_GIT_COMMIT "unknown")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE AVATAR_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(AVATAR_GIT_STATUS)
        set(AVATAR_GIT_DIRTY 1)
    endif()
endif()

# ---------------------------------------------------------------------------
# Native core (framework-free; no Godot headers, no engine dependency)
# ---------------------------------------------------------------------------
add_library(avatar_core STATIC
    native/core/src/clock.cpp
    native/core/src/status.cpp
    native/core/src/diagnostics.cpp
)
add_library(avatar::core ALIAS avatar_core)
target_include_directories(avatar_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/core/include"
)
target_compile_features(avatar_core PUBLIC cxx_std_20)
target_link_libraries(avatar_core PRIVATE avatar_json)
avatar_enable_warnings(avatar_core)

# ---------------------------------------------------------------------------
# Capture and tracking foundations
#
# Framework-free like the core: no Godot, no OS headers. The synthetic sources
# and the replay reader are the deterministic inputs every later component test
# uses, so they live in the application tree, not in tests/.
# ---------------------------------------------------------------------------
add_library(avatar_capture STATIC
    native/capture/src/video_frame.cpp
    native/capture/src/face_ground_truth.cpp
    native/capture/src/frame_ring.cpp
    native/capture/src/test_pattern_source.cpp
    native/capture/src/synthetic_face_source.cpp
    native/capture/src/y4m_reader.cpp
    native/capture/src/replay_source.cpp
    native/capture/src/camera_device.cpp
    native/capture/src/camera_source.cpp
    native/capture/src/null_camera_backend.cpp
)
# The Windows Media Foundation backend is the only platform implementation
# today. It is compiled in place of the stub so every other platform still
# builds the shared state machine and the --list-cameras smoke.
if(WIN32)
    target_sources(avatar_capture PRIVATE native/capture/windows/mf_capture.cpp)
endif()
add_library(avatar::capture ALIAS avatar_capture)
target_include_directories(avatar_capture
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/capture/include"
)
target_compile_features(avatar_capture PUBLIC cxx_std_20)
target_link_libraries(avatar_capture PUBLIC avatar::core PRIVATE avatar_json)
if(WIN32)
    # Media Foundation is an OS component: mfplat for the core API, mfreadwrite
    # for IMFSourceReader, mfuuid for the format/attribute GUIDs, ole32 for COM.
    target_link_libraries(avatar_capture PUBLIC mfplat mfreadwrite mfuuid mf ole32)
endif()
avatar_enable_warnings(avatar_capture)

# Capture smoke command: --list-cameras (never opens a device) and --capture
# <seconds> (the manual 60-second real-camera checklist).
add_executable(avatar_list_cameras native/capture/tools/list_cameras.cpp)
target_link_libraries(avatar_list_cameras PRIVATE avatar::capture)
target_compile_features(avatar_list_cameras PRIVATE cxx_std_20)
avatar_enable_warnings(avatar_list_cameras)

add_library(avatar_tracking STATIC
    native/tracking/src/scripted_tracker.cpp
    native/tracking/src/tracker_c_api.cpp
    native/tracking/src/landmark_map.cpp
    native/tracking/src/canonical_face_frame.cpp
    native/tracking/src/profile_correspondence.cpp
)
add_library(avatar::tracking ALIAS avatar_tracking)
target_include_directories(avatar_tracking
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/tracking/include"
)
target_compile_features(avatar_tracking PUBLIC cxx_std_20)
target_link_libraries(avatar_tracking PUBLIC avatar::core avatar::capture PRIVATE avatar_json)
avatar_enable_warnings(avatar_tracking)

# The vendor graph is built separately by scripts/build-tracker.ps1. The app
# and its CMake graph see only the T09 C ABI and an offline, pinned SDK.
if(WIN32)
    add_executable(avatar_record_tracker_clip native/tracking/tools/record_tracker_clip.cpp)
    target_include_directories(avatar_record_tracker_clip PRIVATE native/tracking/include)
    target_link_libraries(avatar_record_tracker_clip PRIVATE avatar::capture)
    avatar_enable_warnings(avatar_record_tracker_clip)
    set(AVATAR_TRACKER_SDK "${AVATAR_TOOLS_DIR}/tracker-sdk" CACHE PATH "Prebuilt tracker SDK")
    set(_avatar_tracker_default OFF)
    if(EXISTS "${AVATAR_TRACKER_SDK}/libavatartracker.dll")
        set(_avatar_tracker_default ON)
    endif()
    option(AVATAR_REAL_TRACKER "Use the separately built real tracker SDK" ${_avatar_tracker_default})
    if(AVATAR_REAL_TRACKER)
        foreach(_file libavatartracker.dll opencv_world3410.dll avatar_tracker_bench.exe tracker-models/face_landmarker.task)
            if(NOT EXISTS "${AVATAR_TRACKER_SDK}/${_file}")
                message(FATAL_ERROR "Tracker SDK missing ${_file}; run scripts/build-tracker.ps1")
            endif()
        endforeach()
        add_custom_target(avatar_tracker_artifacts ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/tracker-models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AVATAR_TRACKER_SDK}/libavatartracker.dll"
                "${AVATAR_TRACKER_SDK}/opencv_world3410.dll"
                "${AVATAR_TRACKER_SDK}/avatar_tracker_bench.exe"
                "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AVATAR_TRACKER_SDK}/tracker-models/face_landmarker.task"
                "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/tracker-models/face_landmarker.task"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/app/bin/tracker-models"
                "${CMAKE_CURRENT_SOURCE_DIR}/app/bin/tracker-licenses"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AVATAR_TRACKER_SDK}/libavatartracker.dll"
                "${AVATAR_TRACKER_SDK}/opencv_world3410.dll"
                "${CMAKE_CURRENT_SOURCE_DIR}/app/bin"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AVATAR_TRACKER_SDK}/tracker-models/face_landmarker.task"
                "${CMAKE_CURRENT_SOURCE_DIR}/app/bin/tracker-models/face_landmarker.task"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AVATAR_TRACKER_SDK}/LICENSE-mediapipe.txt"
                "${AVATAR_TRACKER_SDK}/LICENSE-opencv.txt"
                "${CMAKE_CURRENT_SOURCE_DIR}/native/licenses.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/app/bin/tracker-licenses"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(AVATAR_TESTS)
    enable_testing()

    add_executable(avatar_core_tests
        tests/unit/test_main.cpp
        tests/unit/test_spsc_ring.cpp
        tests/unit/test_latest_slot.cpp
        tests/unit/test_diagnostics.cpp
    )
    target_link_libraries(avatar_core_tests PRIVATE avatar::core)
    target_compile_features(avatar_core_tests PRIVATE cxx_std_20)
    avatar_enable_warnings(avatar_core_tests)

    add_test(NAME avatar_core_unit COMMAND avatar_core_tests)
    set_tests_properties(avatar_core_unit PROPERTIES
        LABELS "fast;task_T01;task_T03"
        TIMEOUT 30
    )

    # Component suite for the deterministic sources, the scripted tracker and
    # the golden comparator library. Kept separate from avatar_core_tests so the
    # T01 core target stays dependency-free.
    add_executable(avatar_capture_tests
        tests/unit/test_capture_main.cpp
        tests/unit/test_frame_ring.cpp
        tests/unit/test_test_pattern_source.cpp
        tests/unit/test_synthetic_face_source.cpp
        tests/unit/test_scripted_tracker.cpp
        tests/unit/test_replay_source.cpp
        tests/unit/test_golden_compare.cpp
        tests/tools/golden_compare/golden_compare.cpp
    )
    target_include_directories(avatar_capture_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/golden_compare"
    )
    target_compile_definitions(avatar_capture_tests PRIVATE
        AVATAR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    target_link_libraries(avatar_capture_tests PRIVATE
        avatar::core
        avatar::capture
        avatar::tracking
        avatar_json
    )
    target_compile_features(avatar_capture_tests PRIVATE cxx_std_20)
    avatar_enable_warnings(avatar_capture_tests)

    add_test(NAME avatar_capture_unit COMMAND avatar_capture_tests)
    set_tests_properties(avatar_capture_unit PROPERTIES
        LABELS "fast;task_T04"
        TIMEOUT 180
    )

    # Tracker contract suite (T09): the C ABI, the C++ interface, the scripted
    # backend and the shared latency histogram. The C compile target is built
    # separately because it must not see a C++ compiler or a C++ header.
    add_executable(avatar_tracking_tests
        tests/unit/test_tracking_main.cpp
        tests/unit/test_tracker_contract.cpp
        tests/unit/test_latency_histogram.cpp
        tests/unit/test_landmark_map.cpp
    )
    target_include_directories(avatar_tracking_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(avatar_tracking_tests PRIVATE
        avatar::core
        avatar::capture
        avatar::tracking
    )
    target_compile_features(avatar_tracking_tests PRIVATE cxx_std_20)
    avatar_enable_warnings(avatar_tracking_tests)

    add_test(NAME avatar_tracking_unit COMMAND avatar_tracking_tests)
    set_tests_properties(avatar_tracking_unit PROPERTIES
        LABELS "fast;task_T09;task_T12"
        TIMEOUT 180
    )

    add_executable(avatar_tracker_c_abi_compile tests/unit/tracker_c_api_c_compile.c)
    target_include_directories(avatar_tracker_c_abi_compile PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native/tracking/include"
    )
    set_target_properties(avatar_tracker_c_abi_compile PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    if(MSVC)
        target_compile_options(avatar_tracker_c_abi_compile PRIVATE /W4 /WX)
    else()
        target_compile_options(avatar_tracker_c_abi_compile PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()

    add_test(NAME avatar_tracker_c_abi_compile COMMAND avatar_tracker_c_abi_compile)
    set_tests_properties(avatar_tracker_c_abi_compile PROPERTIES
        LABELS "fast;task_T09"
        TIMEOUT 60
    )

    # Capture backend suite (T10): the platform-neutral CameraSource driven by
    # a fake ICameraDeviceBackend, plus the --list-cameras smoke. Neither needs
    # a camera, so both run in CI.
    add_executable(avatar_camera_tests
        tests/unit/test_camera_main.cpp
        tests/unit/test_camera_source.cpp
    )
    target_include_directories(avatar_camera_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(avatar_camera_tests PRIVATE avatar::capture)
    target_compile_features(avatar_camera_tests PRIVATE cxx_std_20)
    avatar_enable_warnings(avatar_camera_tests)

    add_test(NAME avatar_camera_unit COMMAND avatar_camera_tests)
    set_tests_properties(avatar_camera_unit PROPERTIES
        LABELS "fast;task_T10"
        TIMEOUT 120
    )

add_test(NAME avatar_list_cameras_smoke COMMAND avatar_list_cameras --list-cameras)
set_tests_properties(avatar_list_cameras_smoke PROPERTIES
        LABELS "fast;task_T10"
        TIMEOUT 60
)
if(WIN32 AND AVATAR_REAL_TRACKER)
    add_executable(avatar_real_tracker_tests
        tests/unit/test_real_tracker.cpp
        native/tracking/mediapipe/preprocessing.cpp
    )
    target_include_directories(avatar_real_tracker_tests PRIVATE
        tests/unit native/tracking/include
    )
    add_dependencies(avatar_real_tracker_tests avatar_tracker_artifacts)
    avatar_enable_warnings(avatar_real_tracker_tests)
    add_test(NAME avatar_real_tracker_contract COMMAND avatar_real_tracker_tests
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/libavatartracker.dll")
    add_test(NAME avatar_tracker_model_integrity COMMAND avatar_real_tracker_tests
        --reject-corrupt-model "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/libavatartracker.dll"
        "${CMAKE_CURRENT_BINARY_DIR}/tracker-corrupt-model")
    add_test(NAME avatar_tracker_worker_contract
        COMMAND "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/avatar_tracker_bench.exe" --self-test
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/tracker-models/face_landmarker.task")
    set_tests_properties(avatar_real_tracker_contract avatar_tracker_model_integrity avatar_tracker_worker_contract PROPERTIES
        LABELS "fast;task_T09;task_T11" TIMEOUT 120)
endif()
endif()

# ---------------------------------------------------------------------------
# Golden comparator command
#
# Standalone so later golden tasks can attach artifacts from CTest or a manual
# run. The --self-test mode is registered as a CTest test so the CLI exit-code
# contract (0 pass, 1 fail, 2 usage/IO) is exercised, not just the library.
# ---------------------------------------------------------------------------
if(AVATAR_TESTS)
    add_executable(golden_compare
        tests/tools/golden_compare/main.cpp
        tests/tools/golden_compare/golden_compare.cpp
    )
    target_include_directories(golden_compare PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/golden_compare"
    )
    target_compile_features(golden_compare PRIVATE cxx_std_20)
    avatar_enable_warnings(golden_compare)

    add_test(NAME golden_compare_cli
        COMMAND golden_compare --self-test "${CMAKE_CURRENT_BINARY_DIR}/golden_compare_selftest"
    )
    set_tests_properties(golden_compare_cli PROPERTIES
        LABELS "fast;task_T04"
        TIMEOUT 60
    )
endif()

# ---------------------------------------------------------------------------
# Licence and diagnostics gates (T06)
#
# Both are Node scripts so the same implementation runs in the local CI script
# and on every matrix runner. The licence check is a build gate: a third-party
# include without a native/licenses.json entry fails. The diagnostics scan is
# advisory until T41 wires the string table, but its self-test exercises the
# strict mode the future gate will use.
# ---------------------------------------------------------------------------
if(AVATAR_TESTS)
    find_program(AVATAR_NODE_EXECUTABLE NAMES node)
if(AVATAR_NODE_EXECUTABLE)
if(WIN32 AND AVATAR_REAL_TRACKER)
add_test(NAME tracker_sdk_integrity COMMAND "${AVATAR_NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-tracker-sdk.mjs" --self-test
    --sdk "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
set_tests_properties(tracker_sdk_integrity PROPERTIES LABELS "fast;task_T11" TIMEOUT 30)
endif()
add_test(NAME license_manifest_check
            COMMAND "${AVATAR_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-licenses.mjs"
                --root "${CMAKE_CURRENT_SOURCE_DIR}" --self-test
        )
        set_tests_properties(license_manifest_check PROPERTIES
            LABELS "fast;task_T06"
            TIMEOUT 120
        )

        add_test(NAME diagnostics_source_scan
            COMMAND "${AVATAR_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-diagnostics.mjs"
                --root "${CMAKE_CURRENT_SOURCE_DIR}" --self-test
        )
        set_tests_properties(diagnostics_source_scan PROPERTIES
            LABELS "fast;task_T06"
            TIMEOUT 120
        )
    else()
        message(STATUS
            "avatar: node not found; licence and diagnostics CTest checks are skipped")
    endif()
endif()

# ---------------------------------------------------------------------------
# M0 headless feasibility harness (T13)
#
# Frames in, TrackingResult out, measured without Godot. The short mode runs in
# CI; the soak mode is registered under the "perf" label so verify -Fast stays
# cheap and verify -Full owns the 30-minute run. The real backend is loaded
# dynamically through the T09 C ABI, so this target never links MediaPipe.
# ---------------------------------------------------------------------------
if(AVATAR_TESTS OR AVATAR_BENCHMARKS)
    add_executable(avatar_m0 tests/m0/m0.cpp)
    target_link_libraries(avatar_m0 PRIVATE
        avatar::core
        avatar::capture
        avatar::tracking
        avatar_json
    )
    target_compile_features(avatar_m0 PRIVATE cxx_std_20)
    target_compile_definitions(avatar_m0 PRIVATE
        AVATAR_M0_BUILD_TYPE="$<CONFIG>"
        AVATAR_M0_GIT_COMMIT="${AVATAR_GIT_COMMIT}"
        AVATAR_M0_GIT_DIRTY=${AVATAR_GIT_DIRTY}
        AVATAR_M0_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    )
    if(WIN32)
        # psapi: working-set/handle counters; winmm: 1 ms timer resolution for
        # the paced submission loop.
        target_link_libraries(avatar_m0 PRIVATE psapi winmm)
    endif()
    if(UNIX)
        target_link_libraries(avatar_m0 PRIVATE ${CMAKE_DL_LIBS})
    endif()
    avatar_enable_warnings(avatar_m0)

    if(AVATAR_TESTS)
        add_test(NAME avatar_m0_selftest COMMAND avatar_m0 --self-test)
        set_tests_properties(avatar_m0_selftest PROPERTIES
            LABELS "fast;task_T13"
            TIMEOUT 120
        )

        if(WIN32)
            # The short mode is the documented 2-minute CI run. It writes the
            # JSON and the human summary; tests/gates/T13.ps1 reads them.
            add_test(NAME avatar_m0_short COMMAND powershell
                -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/m0/run.ps1"
                -Mode short -NoBuild
                -BuildDir "${CMAKE_CURRENT_BINARY_DIR}"
                -Config "$<CONFIG>"
            )
            set_tests_properties(avatar_m0_short PROPERTIES
                LABELS "fast;task_T13"
                TIMEOUT 420
            )

            # Full soak: 30 minutes across 192 and 256. Registered under "perf"
            # so it runs only in verify -Full / nightly.
            add_test(NAME avatar_m0_soak COMMAND powershell
                -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/m0/run.ps1"
                -Mode full -NoBuild
                -BuildDir "${CMAKE_CURRENT_BINARY_DIR}"
                -Config "$<CONFIG>"
            )
            set_tests_properties(avatar_m0_soak PROPERTIES
                LABELS "perf;task_T13"
                TIMEOUT 3000
            )
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Microbenchmarks
#
# Numbers are reported, not gated: a tight nanosecond threshold on shared CI
# runners is noise, not a regression signal. T05 owns the real performance
# harness and thresholds.
# ---------------------------------------------------------------------------
if(AVATAR_BENCHMARKS)
    add_executable(avatar_core_bench tests/performance/bench_primitives.cpp)
    target_link_libraries(avatar_core_bench PRIVATE avatar::core)
    target_compile_features(avatar_core_bench PRIVATE cxx_std_20)
    avatar_enable_warnings(avatar_core_bench)

    if(AVATAR_TESTS)
        add_test(NAME avatar_core_bench COMMAND avatar_core_bench)
        set_tests_properties(avatar_core_bench PROPERTIES
            LABELS "bench;task_T03"
            TIMEOUT 120
        )
    endif()

    # -----------------------------------------------------------------------
    # Pipeline performance harness (T05)
    #
    # The self-test is part of the fast suite: it runs every scenario tiny and
    # validates the JSON contract without asserting a hardware-dependent
    # number. The real measurement carries the "perf" label, which verify
    # excludes from -Fast and includes in -Full.
    # -----------------------------------------------------------------------
    add_executable(avatar_bench tests/performance/avatar_bench.cpp)
    target_link_libraries(avatar_bench PRIVATE
        avatar::core
        avatar::capture
        avatar::tracking
        avatar_json
    )
    target_compile_features(avatar_bench PRIVATE cxx_std_20)
    target_compile_definitions(avatar_bench PRIVATE
        AVATAR_BENCH_BUILD_TYPE="$<CONFIG>"
        AVATAR_BENCH_GIT_COMMIT="${AVATAR_GIT_COMMIT}"
        AVATAR_BENCH_GIT_DIRTY=${AVATAR_GIT_DIRTY}
        AVATAR_BENCH_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    )
    if(WIN32)
        target_link_libraries(avatar_bench PRIVATE psapi pdh winmm)
    endif()
    avatar_enable_warnings(avatar_bench)

    if(AVATAR_TESTS)
        add_test(NAME avatar_bench_selftest COMMAND avatar_bench --self-test)
        set_tests_properties(avatar_bench_selftest PROPERTIES
            LABELS "fast;task_T05"
            TIMEOUT 120
        )

        add_test(NAME avatar_bench_perf
            COMMAND avatar_bench
                --scenario rig --frames 120 --warmup 10
                --width 1280 --height 720 --fps 60 --quiet
                --output "${CMAKE_CURRENT_BINARY_DIR}/perf/rig.json"
        )
        set_tests_properties(avatar_bench_perf PROPERTIES
            LABELS "perf;task_T05"
            TIMEOUT 300
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# GDExtension adapter (optional)
#
# The native core must remain buildable without Godot. When bootstrap has
# unpacked the pinned godot-cpp checkout, build the adapter and copy it into
# app/bin/ for the headless smoke test. Verification for T02 and later tasks
# treats a missing adapter as a failure; T01 only needs the native core.
# ---------------------------------------------------------------------------
if(AVATAR_GODEXTENSION)
    if(EXISTS "${AVATAR_GODOTCPP_DIR}/CMakeLists.txt")
        add_subdirectory(native/gdextension)
    else()
        message(STATUS
            "avatar: godot-cpp not found at ${AVATAR_GODOTCPP_DIR}; "
            "skipping the GDExtension adapter (run scripts/bootstrap.ps1|sh)")
    endif()
endif()
