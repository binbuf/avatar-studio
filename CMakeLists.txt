cmake_minimum_required(VERSION 3.25)

project(avatar_studio LANGUAGES CXX)

option(AVATAR_TESTS "Build the native unit test suite" ON)
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
target_include_directories(avatar_json INTERFACE "${AVATAR_NLOHMANN_JSON_INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# Native core (framework-free; no Godot headers, no engine dependency)
# ---------------------------------------------------------------------------
add_library(avatar_core STATIC
    native/core/src/clock.cpp
    native/core/src/status.cpp
)
add_library(avatar::core ALIAS avatar_core)
target_include_directories(avatar_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/core/include"
)
target_compile_features(avatar_core PUBLIC cxx_std_20)
target_link_libraries(avatar_core PRIVATE avatar_json)
avatar_enable_warnings(avatar_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(AVATAR_TESTS)
    enable_testing()

    add_executable(avatar_core_tests tests/unit/test_main.cpp)
    target_link_libraries(avatar_core_tests PRIVATE avatar::core)
    target_compile_features(avatar_core_tests PRIVATE cxx_std_20)
    avatar_enable_warnings(avatar_core_tests)

    add_test(NAME avatar_core_unit COMMAND avatar_core_tests)
    set_tests_properties(avatar_core_unit PROPERTIES
        LABELS "fast;task_T01"
        TIMEOUT 30
    )
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
