#include "avatar/instance/instance_identity.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>

#include <cstdlib>
#endif

namespace avatar::instance {
namespace {

constexpr std::size_t kMaxAppIdLength = 40;

std::string fnv1a_hex(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : text) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int index = 15; index >= 0; --index) {
        out[static_cast<std::size_t>(index)] = kHex[hash & 0xFU];
        hash >>= 4U;
    }
    return out;
}

#if defined(_WIN32)
std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                              size, nullptr, nullptr);
    return out;
}

std::wstring user_name_wide() {
    wchar_t buffer[256] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (GetUserNameW(buffer, &size) == 0) {
        return {};
    }
    return std::wstring(buffer, size > 0 ? size - 1 : 0);
}

std::wstring temp_dir_wide() {
    wchar_t buffer[MAX_PATH + 1] = {};
    const DWORD size = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
    if (size == 0 || size >= std::size(buffer)) {
        return L".";
    }
    std::wstring path(buffer, size);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}
#else
std::string runtime_dir() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg != nullptr && xdg[0] == '/') {
        std::string candidate(xdg);
        if (!candidate.empty() && candidate.back() == '/') {
            candidate.pop_back();
        }
        // sun_path is 108 bytes on Linux and 104 on macOS; keep a margin.
        if (candidate.size() <= 80) {
            return candidate;
        }
    }
    return "/tmp";
}

std::string login_name() {
    const char* name = std::getenv("LOGNAME");
    if (name == nullptr || name[0] == '\0') {
        name = std::getenv("USER");
    }
    return name == nullptr ? std::string{} : std::string(name);
}
#endif

} // namespace

std::string sanitize_app_id(std::string_view app_id) {
    std::string out;
    out.reserve(std::min(app_id.size(), kMaxAppIdLength));
    for (const char raw : app_id) {
        const auto byte = static_cast<unsigned char>(raw);
        char value = static_cast<char>(std::tolower(byte));
        const bool allowed = (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
                             value == '-' || value == '_' || value == '.';
        out.push_back(allowed ? value : '-');
        if (out.size() >= kMaxAppIdLength) {
            break;
        }
    }
    while (!out.empty() && out.front() == '-') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? std::string("app") : out;
}

std::string user_scope_key() {
#if defined(_WIN32)
    const std::string user = wide_to_utf8(user_name_wide());
    return fnv1a_hex(user.empty() ? std::string("windows-user") : user);
#else
    const std::string user = login_name();
    const std::string source =
        std::to_string(static_cast<unsigned long long>(::getuid())) + ":" + user;
    return fnv1a_hex(source);
#endif
}

InstanceIdentity make_identity(std::string_view app_id, bool new_instance, std::uint32_t pid) {
    InstanceIdentity identity;
    identity.app_id = sanitize_app_id(app_id);
    identity.scope = user_scope_key();
    identity.pid = pid;
    identity.new_instance = new_instance;
    identity.single_instance = !new_instance;
    identity.virtual_camera_enabled = !new_instance;
    identity.output_registration_enabled = !new_instance;

    identity.base_name = "avatar-studio-" + identity.scope + "-" + identity.app_id;
    if (new_instance) {
        identity.base_name += "-new-" + std::to_string(pid);
    }
    identity.shared_memory_name = identity.base_name + ".frames";

#if defined(_WIN32)
    identity.lock_name = "Local\\AvatarStudio." + identity.base_name + ".lock";
    identity.activation_name = "\\\\.\\pipe\\" + identity.base_name + ".activate";
    identity.state_path = wide_to_utf8(temp_dir_wide()) + "\\" + identity.base_name + ".instance";
#else
    const std::string dir = runtime_dir();
    identity.lock_name = dir + "/" + identity.base_name + ".lock";
    identity.activation_name = dir + "/" + identity.base_name + ".sock";
    identity.state_path = identity.lock_name;
#endif
    return identity;
}

InstanceIdentity current_identity(std::string_view app_id, bool new_instance) {
#if defined(_WIN32)
    const auto pid = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    const auto pid = static_cast<std::uint32_t>(::getpid());
#endif
    return make_identity(app_id, new_instance, pid);
}

} // namespace avatar::instance
