#pragma once

#include "avatar/instance/instance_identity.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace avatar::instance {

/// One local activation request. The payload is JSON on a per-user named pipe
/// (Windows) or Unix socket (macOS/Linux); there is no network listener.
struct ActivationMessage {
    std::string command = "activate";
    std::string avatar_path;
    std::string show_path;

    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] static std::optional<ActivationMessage> from_json(std::string_view json);
};

enum class ActivationDelivery {
    delivered = 0,
    not_running,
    error,
};

struct ActivationResult {
    ActivationDelivery outcome = ActivationDelivery::error;
    std::string detail;
};

/// Server side of the activation channel. The thread only accepts, validates and
/// queues; the application thread calls `poll()` once per frame and owns every
/// window or scene action.
class ActivationServer {
public:
    ActivationServer();
    ~ActivationServer();

    ActivationServer(const ActivationServer&) = delete;
    ActivationServer& operator=(const ActivationServer&) = delete;
    ActivationServer(ActivationServer&&) = delete;
    ActivationServer& operator=(ActivationServer&&) = delete;

    /// Creates the OS object and starts the listener thread. Returns false and
    /// fills `error` when the channel cannot be created.
    bool start(const InstanceIdentity& identity, std::string& error);
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::vector<ActivationMessage> poll();
    [[nodiscard]] std::uint64_t received() const noexcept;

private:
    struct Impl;
    static void server_loop(Impl& impl);

    std::unique_ptr<Impl> impl_;
};

/// Client side used by a second launch (and by the test probe). `delivered`
/// means the running instance queued the message and acknowledged it, not just
/// that a socket accepted bytes.
[[nodiscard]] ActivationResult send_activation(const InstanceIdentity& identity,
                                               const ActivationMessage& message,
                                               int timeout_ms = 1500);

} // namespace avatar::instance
