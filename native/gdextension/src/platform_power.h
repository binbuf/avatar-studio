#pragma once

namespace avatar::platform {

/// Keeps the OS from throttling the app while a consumer is connected.
///
/// On macOS this holds an `NSProcessInfo` user-initiated activity so App Nap
/// cannot suspend the render loop. On other platforms it is a no-op: Godot's
/// low-processor mode is already disabled by `AvatarRuntime`.
void disable_app_nap();

/// Releases the activity taken by `disable_app_nap()`. Safe to call when no
/// activity is held.
void enable_app_nap();

} // namespace avatar::platform
