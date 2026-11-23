#include "test_registry.h"

#include "avatar/instance/activation_channel.h"
#include "avatar/instance/instance_identity.h"
#include "avatar/instance/instance_lock.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using avatar::instance::ActivationDelivery;
using avatar::instance::ActivationMessage;
using avatar::instance::ActivationServer;
using avatar::instance::InstanceIdentity;
using avatar::instance::InstanceLock;
using avatar::instance::LockState;
using avatar::instance::make_identity;

std::atomic<std::uint32_t> g_counter{0};

std::string unique_app_id() {
    const auto pid = static_cast<std::uint32_t>(
#if defined(_WIN32)
        GetCurrentProcessId()
#else
        ::getpid()
#endif
    );
    const auto counter = g_counter.fetch_add(1, std::memory_order_relaxed);
    return "t25a-" + std::to_string(pid) + "-" + std::to_string(counter);
}

std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

std::string module_directory() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {};
    const DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (size == 0) {
        return {};
    }
    std::filesystem::path path(std::wstring(buffer, size));
    return path.parent_path().string();
#else
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return {};
    }
    return path.parent_path().string();
#endif
}

std::string probe_path() {
#if defined(_WIN32)
    return (std::filesystem::path(module_directory()) / "avatar_instance_probe.exe").string();
#else
    return (std::filesystem::path(module_directory()) / "avatar_instance_probe").string();
#endif
}

bool wait_for_file(const std::filesystem::path& path, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::filesystem::exists(path);
}

struct ChildProcess {
#if defined(_WIN32)
    PROCESS_INFORMATION info{};
#else
    pid_t pid = -1;
#endif
    bool valid = false;
};

#if defined(_WIN32)
std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                              size);
    return out;
}

bool spawn_holder(const std::string& app_id, const std::filesystem::path& ready,
                  ChildProcess& child) {
    const std::string exe = probe_path();
    std::wstring command = L"\"" + widen(exe) + L"\" --app-id " + widen(app_id) +
                           L" --acquire --hold-ms 60000 --ready-file \"" + widen(ready.string()) +
                           L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    if (CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &child.info) == 0) {
        return false;
    }
    child.valid = true;
    return true;
}

void kill_child(ChildProcess& child) {
    if (!child.valid) {
        return;
    }
    (void)TerminateProcess(child.info.hProcess, 1);
    (void)WaitForSingleObject(child.info.hProcess, 5000);
    CloseHandle(child.info.hThread);
    CloseHandle(child.info.hProcess);
    child.valid = false;
}
#else
bool spawn_holder(const std::string& app_id, const std::filesystem::path& ready,
                  ChildProcess& child) {
    const std::string exe = probe_path();
    const std::string ready_text = ready.string();
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        ::execl(exe.c_str(), exe.c_str(), "--app-id", app_id.c_str(), "--acquire", "--hold-ms",
                "60000", "--ready-file", ready_text.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    child.pid = pid;
    child.valid = true;
    return true;
}

void kill_child(ChildProcess& child) {
    if (!child.valid) {
        return;
    }
    (void)::kill(child.pid, SIGKILL);
    int status = 0;
    (void)::waitpid(child.pid, &status, 0);
    child.valid = false;
}
#endif

bool acquire_with_retry(InstanceLock& lock, const InstanceIdentity& identity, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (lock.acquire(identity) == LockState::primary) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::vector<ActivationMessage> poll_until(ActivationServer& server, std::size_t wanted,
                                          int timeout_ms) {
    std::vector<ActivationMessage> collected;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (collected.size() < wanted && std::chrono::steady_clock::now() < deadline) {
        auto batch = server.poll();
        for (auto& message : batch) {
            collected.push_back(std::move(message));
        }
        if (collected.size() < wanted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return collected;
}

} // namespace

TEST_CASE(identity_separates_users_and_new_instances) {
    const auto normal = make_identity("Avatar Studio", false, 100);
    const auto test = make_identity("Avatar Studio", true, 200);
    CHECK_EQ(normal.app_id, std::string("avatar-studio"));
    CHECK(!normal.new_instance);
    CHECK(normal.single_instance);
    CHECK(normal.virtual_camera_enabled);
    CHECK(normal.output_registration_enabled);

    CHECK(test.new_instance);
    CHECK(!test.single_instance);
    CHECK(!test.virtual_camera_enabled);
    CHECK(!test.output_registration_enabled);
    CHECK(normal.lock_name != test.lock_name);
    CHECK(normal.activation_name != test.activation_name);
    CHECK(normal.shared_memory_name != test.shared_memory_name);
    CHECK(test.lock_name.find("200") != std::string::npos);
    CHECK(!normal.scope.empty());
    CHECK_EQ(normal.scope.size(), std::size_t{16});
}

TEST_CASE(first_launch_acquires_and_second_is_secondary) {
    const auto identity = make_identity(unique_app_id(), false, 0);
    InstanceLock first;
    CHECK(first.acquire(identity) == LockState::primary);
    CHECK(first.is_primary());
    CHECK(!first.reclaimed_stale());

    InstanceLock second;
    CHECK(second.acquire(identity) == LockState::secondary);
    CHECK(!second.is_primary());

    first.release();
    InstanceLock third;
    CHECK(third.acquire(identity) == LockState::primary);
    third.release();
}

TEST_CASE(clean_release_leaves_no_stale_marker) {
    const auto identity = make_identity(unique_app_id(), false, 0);
    {
        InstanceLock lock;
        CHECK(lock.acquire(identity) == LockState::primary);
        CHECK(std::filesystem::exists(identity.state_path));
        lock.release();
    }
    CHECK(!std::filesystem::exists(identity.state_path) ||
          std::filesystem::file_size(identity.state_path) == 0);
    InstanceLock next;
    CHECK(next.acquire(identity) == LockState::primary);
    CHECK(!next.reclaimed_stale());
    next.release();
}

TEST_CASE(stale_marker_is_reclaimed_by_the_next_launch) {
    const auto identity = make_identity(unique_app_id(), false, 0);
    {
        std::ofstream marker(identity.state_path, std::ios::trunc);
        marker << "pid=999999\n";
    }
    CHECK(std::filesystem::exists(identity.state_path));
    InstanceLock lock;
    CHECK(lock.acquire(identity) == LockState::primary);
    CHECK(lock.reclaimed_stale());
    lock.release();
    CHECK(!std::filesystem::exists(identity.state_path) ||
          std::filesystem::file_size(identity.state_path) == 0);
}

TEST_CASE(activation_round_trip_carries_avatar_and_show) {
    const auto identity = make_identity(unique_app_id(), false, 0);
    ActivationServer server;
    std::string error;
    CHECK(server.start(identity, error));
    CHECK(server.running());

    ActivationMessage message;
    message.avatar_path = "C:/Users/test/Avatars/My Avatar.avatar";
    message.show_path = "C:/Users/test/Shows/stream.show";
    const auto result = avatar::instance::send_activation(identity, message, 2000);
    CHECK(result.outcome == ActivationDelivery::delivered);

    const auto received = poll_until(server, 1, 2000);
    CHECK_EQ(received.size(), std::size_t{1});
    if (!received.empty()) {
        CHECK_EQ(received[0].avatar_path, message.avatar_path);
        CHECK_EQ(received[0].show_path, message.show_path);
        CHECK_EQ(received[0].command, std::string("activate"));
    }
    CHECK_EQ(server.received(), std::uint64_t{1});
    server.stop();
    CHECK(!server.running());
}

TEST_CASE(activation_without_a_running_instance_is_not_running) {
    const auto identity = make_identity(unique_app_id(), false, 0);
    ActivationMessage message;
    const auto result = avatar::instance::send_activation(identity, message, 250);
    CHECK(result.outcome == ActivationDelivery::not_running);
}

TEST_CASE(second_launch_activates_the_first_and_exits) {
    const auto identity = make_identity(unique_app_id(), false, 0);
    InstanceLock primary;
    CHECK(primary.acquire(identity) == LockState::primary);
    ActivationServer server;
    std::string error;
    CHECK(server.start(identity, error));

    // The second launch follows the same order as the application: acquire,
    // see secondary, send, and do not start a second server.
    InstanceLock second;
    CHECK(second.acquire(identity) == LockState::secondary);
    ActivationMessage message;
    message.avatar_path = "second-launch.avatar";
    const auto result = avatar::instance::send_activation(identity, message, 2000);
    CHECK(result.outcome == ActivationDelivery::delivered);
    const auto received = poll_until(server, 1, 2000);
    CHECK_EQ(received.size(), std::size_t{1});
    if (!received.empty()) {
        CHECK_EQ(received[0].avatar_path, message.avatar_path);
    }
    server.stop();
    primary.release();
}

TEST_CASE(new_instance_is_isolated_from_the_primary) {
    const auto normal = make_identity(unique_app_id(), false, 0);
    InstanceLock primary;
    CHECK(primary.acquire(normal) == LockState::primary);

    const auto isolated = make_identity(normal.app_id, true, 4242);
    InstanceLock new_instance;
    CHECK(new_instance.acquire(isolated) == LockState::primary);
    CHECK(new_instance.is_primary());
    CHECK(!isolated.virtual_camera_enabled);
    CHECK(!isolated.output_registration_enabled);
    CHECK(isolated.lock_name != normal.lock_name);

    // The normal second launch still reaches the normal primary, not the test
    // instance.
    InstanceLock normal_second;
    CHECK(normal_second.acquire(normal) == LockState::secondary);

    new_instance.release();
    primary.release();
}

TEST_CASE(activation_payload_is_validated) {
    CHECK(!ActivationMessage::from_json("not json").has_value());
    CHECK(!ActivationMessage::from_json("{\"version\":2,\"command\":\"activate\"}").has_value());
    CHECK(!ActivationMessage::from_json("{\"version\":1,\"command\":\"quit\"}").has_value());
    CHECK(!ActivationMessage::from_json("{\"version\":1,\"avatar\":5}").has_value());

    ActivationMessage message;
    message.avatar_path = "a path with spaces/and-unicode-\xC3\xA9.avatar";
    message.show_path = "show.show";
    const auto parsed = ActivationMessage::from_json(message.to_json());
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK_EQ(parsed->avatar_path, message.avatar_path);
        CHECK_EQ(parsed->show_path, message.show_path);
    }
}

TEST_CASE(a_crashed_owner_is_reclaimed_without_a_reboot) {
    const auto app_id = unique_app_id();
    const auto identity = make_identity(app_id, false, 0);
    const auto ready = temp_path(app_id + "-ready.txt");
    std::error_code ignored;
    std::filesystem::remove(ready, ignored);

    ChildProcess child;
    CHECK(spawn_holder(app_id, ready, child));
    CHECK(wait_for_file(ready, 5000));
    if (!child.valid) {
        return;
    }

    InstanceLock while_running;
    CHECK(while_running.acquire(identity) == LockState::secondary);
    while_running.release();

    kill_child(child);
    std::filesystem::remove(ready, ignored);

    InstanceLock after_crash;
    CHECK(acquire_with_retry(after_crash, identity, 3000));
    CHECK(after_crash.is_primary());
    CHECK(after_crash.reclaimed_stale());
    after_crash.release();
}
