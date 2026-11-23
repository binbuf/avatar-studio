// Small command-line probe for the single-instance contract. It is used by
// app/tests/single_instance.gd and by manual checks; it links the same
// avatar::instance library as the application, so a test crosses a real process
// boundary without launching a second engine.
//
//   avatar_instance_probe --app-id ID --activate [--avatar PATH] [--show PATH]
//   avatar_instance_probe --app-id ID --acquire [--hold-ms N] [--ready-file P]
//
// Exit codes: 0 delivered/primary, 3 not running/secondary, 4 error.

#include "avatar/instance/activation_channel.h"
#include "avatar/instance/instance_identity.h"
#include "avatar/instance/instance_lock.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string app_id = "avatar-studio";
    bool new_instance = false;
    bool activate = false;
    bool acquire = false;
    std::string avatar;
    std::string show;
    std::string ready_file;
    int timeout_ms = 1500;
    int hold_ms = 0;
};

void print_json(bool ok, const std::string& command, const std::string& state,
                const std::string& detail, const avatar::instance::InstanceIdentity& identity,
                bool reclaimed) {
    std::string escaped_detail;
    for (const char value : detail) {
        if (value == '"' || value == '\\') {
            escaped_detail.push_back('\\');
        }
        escaped_detail.push_back(value);
    }
    // diagnostics-scan: allow (machine-readable probe output, not a user message)
    std::printf("{\"ok\":%s,\"command\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\","
                "\"reclaimed\":%s,\"pid\":%u,\"identity\":{\"lock\":\"%s\",\"activation\":\"%s\","
                "\"shared_memory\":\"%s\",\"new_instance\":%s,\"virtual_camera\":%s,"
                "\"output_registration\":%s}}\n",
                ok ? "true" : "false", command.c_str(), state.c_str(), escaped_detail.c_str(),
                reclaimed ? "true" : "false", identity.pid, identity.lock_name.c_str(),
                identity.activation_name.c_str(), identity.shared_memory_name.c_str(),
                identity.new_instance ? "true" : "false",
                identity.virtual_camera_enabled ? "true" : "false",
                identity.output_registration_enabled ? "true" : "false");
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto next = [&](std::string& target) {
            if (index + 1 >= argc) {
                return false;
            }
            target = argv[++index];
            return true;
        };
        if (argument == "--app-id") {
            if (!next(options.app_id)) {
                return false;
            }
        } else if (argument == "--new-instance") {
            options.new_instance = true;
        } else if (argument == "--activate") {
            options.activate = true;
        } else if (argument == "--acquire") {
            options.acquire = true;
        } else if (argument == "--avatar") {
            if (!next(options.avatar)) {
                return false;
            }
        } else if (argument == "--show") {
            if (!next(options.show)) {
                return false;
            }
        } else if (argument == "--ready-file") {
            if (!next(options.ready_file)) {
                return false;
            }
        } else if (argument == "--timeout-ms") {
            std::string value;
            if (!next(value)) {
                return false;
            }
            options.timeout_ms = std::atoi(value.c_str());
        } else if (argument == "--hold-ms") {
            std::string value;
            if (!next(value)) {
                return false;
            }
            options.hold_ms = std::atoi(value.c_str());
        } else {
            return false;
        }
    }
    return options.activate != options.acquire;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        // diagnostics-scan: allow (usage text for a developer CLI)
        std::fprintf(stderr,
                     "usage: avatar_instance_probe --app-id ID (--activate [--avatar P] [--show P] "
                     "| --acquire [--hold-ms N] [--ready-file P])\n");
        return 4;
    }
    const auto identity = avatar::instance::current_identity(options.app_id, options.new_instance);

    if (options.activate) {
        avatar::instance::ActivationMessage message;
        message.avatar_path = options.avatar;
        message.show_path = options.show;
        const auto result =
            avatar::instance::send_activation(identity, message, options.timeout_ms);
        const bool delivered = result.outcome == avatar::instance::ActivationDelivery::delivered;
        print_json(delivered, "activate", delivered ? "delivered" : "not_running", result.detail,
                   identity, false);
        if (delivered) {
            return 0;
        }
        return result.outcome == avatar::instance::ActivationDelivery::not_running ? 3 : 4;
    }

    avatar::instance::InstanceLock lock;
    const auto state = lock.acquire(identity);
    const bool primary = state == avatar::instance::LockState::primary;
    print_json(primary, "acquire",
               primary ? "primary"
                       : (state == avatar::instance::LockState::secondary ? "secondary" : "error"),
               lock.last_error(), identity, lock.reclaimed_stale());
    std::fflush(stdout);
    if (!primary) {
        return state == avatar::instance::LockState::secondary ? 3 : 4;
    }
    if (!options.ready_file.empty()) {
        std::ofstream ready(options.ready_file, std::ios::trunc);
        ready << "primary\n";
        ready.close();
    }
    if (options.hold_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options.hold_ms));
    }
    lock.release();
    return 0;
}
