# GDExtension adapter: C++ binding shell only, no business logic.
#
# godot-cpp is pinned and unpacked by scripts/bootstrap.* into .tools/godot-cpp.
# The root CMakeLists adds this directory only when that checkout is present, so
# the native core still configures and builds on a machine without Godot (T01).
#
# godot-cpp 10.x is versioned independently of Godot; GODOTCPP_API_VERSION pins
# the generated bindings to the engine version (4.7). See ADR-0012.

set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version targeted by godot-cpp" FORCE)
set(GODOTCPP_TARGET "template_release" CACHE STRING "godot-cpp target (template_release for shipped builds)" FORCE)
set(GODOTCPP_DISABLE_EXCEPTIONS ON CACHE BOOL "godot-cpp exception policy" FORCE)
set(GODOTCPP_ENABLE_TESTING OFF CACHE BOOL "godot-cpp integration tests" FORCE)

add_subdirectory("${AVATAR_GODOTCPP_DIR}" "${CMAKE_BINARY_DIR}/godot-cpp" EXCLUDE_FROM_ALL)

# godot-cpp's generated headers are not warning-clean under /W4 or -Wall -Wextra.
# Treat its include directories as system headers so the adapter can keep the
# project warning policy without inheriting third-party warnings.
get_target_property(AVATAR_GODOTCPP_INCLUDES godot-cpp INTERFACE_INCLUDE_DIRECTORIES)
if(AVATAR_GODOTCPP_INCLUDES)
    set_property(TARGET godot-cpp APPEND PROPERTY
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${AVATAR_GODOTCPP_INCLUDES})
endif()

add_library(avatar_gdextension SHARED
    src/avatar_model.cpp
    src/avatar_head.cpp
    src/avatar_runtime.cpp
    src/avatar_output_manager.cpp
    src/avatar_live_session.cpp
    src/global_hotkey.cpp
    src/avatar_camera_ring.cpp
    src/avatar_single_instance.cpp
    src/window_output.cpp
    src/platform_timer.cpp
    src/register_types.cpp
)
if(APPLE)
    # App Nap is an NSProcessInfo activity; the hook is Objective-C++.
    target_sources(avatar_gdextension PRIVATE src/platform_power.mm)
else()
    target_sources(avatar_gdextension PRIVATE src/platform_power.cpp)
endif()
# avatar_json is the pinned nlohmann/json header set used by the GLB preflight;
# the import path must never download or use a system copy.
target_link_libraries(avatar_gdextension PRIVATE godot::cpp avatar::core avatar::capture avatar::rig avatar::output avatar::live avatar::instance avatar_json)
target_include_directories(avatar_gdextension PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(avatar_gdextension PRIVATE AVATAR_RUNTIME_VERSION="0.1.0")
target_compile_features(avatar_gdextension PRIVATE cxx_std_20)
avatar_enable_warnings(avatar_gdextension)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
    set(AVATAR_GDE_ARCH "arm64")
else()
    set(AVATAR_GDE_ARCH "x86_64")
endif()

if(WIN32)
    target_link_libraries(avatar_gdextension PRIVATE winmm user32)
    set(AVATAR_GDE_OS "windows")
    set(AVATAR_GDE_EXT "dll")
elseif(APPLE)
    set(AVATAR_GDE_OS "macos")
    set(AVATAR_GDE_EXT "dylib")
else()
    set(AVATAR_GDE_OS "linux")
    set(AVATAR_GDE_EXT "so")
endif()

set(AVATAR_GDE_OUTPUT
    "${PROJECT_SOURCE_DIR}/app/bin/avatar_studio.${AVATAR_GDE_OS}.${AVATAR_GDE_ARCH}.${AVATAR_GDE_EXT}")

set_target_properties(avatar_gdextension PROPERTIES
    PREFIX ""
    OUTPUT_NAME "avatar_studio"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gdextension"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gdextension"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gdextension"
)

# The .gdextension file points both debug and release at this single RelWithDebInfo
# artifact. A debug-specific build can add a suffix later; T02 only needs a
# loadable library for the headless smoke test.
add_custom_command(TARGET avatar_gdextension POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_SOURCE_DIR}/app/bin"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:avatar_gdextension>"
        "${AVATAR_GDE_OUTPUT}"
    COMMENT "Copying GDExtension to app/bin"
    VERBATIM
)
