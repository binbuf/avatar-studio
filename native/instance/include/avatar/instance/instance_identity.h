#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace avatar::instance {

/// Names and policy for one application instance. The normal application uses
/// `new_instance = false`; `--new-instance` appends the process id so a test
/// instance cannot collide with the running application or with another test
/// instance.
///
/// The lock name and the activation name are derived from the same per-user
/// scope, so two users on one machine never share either object. The shared
/// memory name is reserved for the T53 cross-process frame transport; it is
/// distinct for a new instance by construction.
struct InstanceIdentity {
    std::string app_id;
    std::string scope;           // stable per-user key, hex
    std::string base_name;       // scope + app id (+ new-instance pid suffix)
    std::string lock_name;       // Windows mutex name or POSIX lock file path
    std::string activation_name; // Windows named pipe or POSIX Unix socket path
    std::string shared_memory_name;
    std::string state_path; // diagnostic pid marker (never authoritative)
    std::uint32_t pid = 0;
    bool new_instance = false;
    bool single_instance = true;
    bool virtual_camera_enabled = true;
    bool output_registration_enabled = true;
};

/// Lowercase-safe identifier: alphanumeric, '-', '_' and '.' only, at most 40
/// characters. Empty input becomes "app".
[[nodiscard]] std::string sanitize_app_id(std::string_view app_id);

/// Stable per-user key for this host. It is not a secret and is never logged as
/// an identity: it only keeps two users' OS objects apart.
[[nodiscard]] std::string user_scope_key();

/// Builds the identity for an explicit pid. Tests use this to model a second
/// process without spawning one.
[[nodiscard]] InstanceIdentity make_identity(std::string_view app_id, bool new_instance,
                                             std::uint32_t pid);

/// Builds the identity for the current process.
[[nodiscard]] InstanceIdentity current_identity(std::string_view app_id, bool new_instance);

} // namespace avatar::instance
