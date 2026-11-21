cmake_minimum_required(VERSION 3.25)

project(avatar_studio LANGUAGES CXX)

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
)
add_library(avatar::capture ALIAS avatar_capture)
target_include_directories(avatar_capture
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/capture/include"
)
target_compile_features(avatar_capture PUBLIC cxx_std_20)
target_link_libraries(avatar_capture PUBLIC avatar::core PRIVATE avatar_json)
avatar_enable_warnings(avatar_capture)

add_library(avatar_tracking STATIC
    native/tracking/src/scripted_tracker.cpp
)
add_library(avatar::tracking ALIAS avatar_tracking)
target_include_directories(avatar_tracking
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/tracking/include"
)
target_compile_features(avatar_tracking PUBLIC cxx_std_20)
target_link_libraries(avatar_tracking PUBLIC avatar::core avatar::capture)
avatar_enable_warnings(avatar_tracking)

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
