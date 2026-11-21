// avatar_m0 - headless M0 feasibility harness for Avatar Studio.
//
// Frames in, TrackingResult out, measured. The harness deliberately does not
// use Godot, the renderer, or the capture state machine: M0 asks whether the
// tracker can keep up on this machine before rendering exists.
//
// Sources:
//   pattern  deterministic BGRA test card (native/capture)
//   replay   an AVCLIP1 diagnostic clip (the T11 recorder format), looped
//
// Trackers:
//   scripted  ScriptedTracker (synthetic; labelled as such in the JSON)
//   real      the T09 five-function C ABI, loaded dynamically from
//             libavatartracker.dll
//
// The report is one JSON object (schema avatar_m0.result.v1). It records
// submit-to-result latency, source-timestamp-to-result frame age, dropped
// frames classified by reason, confidence bins, RSS/handle/CPU samples,
// thread CPU, a pose trace, an SVG pose chart and a short BMP clip for human
// review at T14.
//
// Usage: avatar_m0 --help

#include "avatar/capture/test_pattern_source.h"
#include "avatar/capture/video_frame.h"
#include "avatar/core/clock.h"
#include "avatar/core/latency_histogram.h"
#include "avatar/tracker/tracker_c_api.h"
#include "avatar/tracking/landmark_map.h"
#include "avatar/tracking/scripted_tracker.h"
#include "avatar/tracking/tracking_result.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <intrin.h>
#include <psapi.h>
#include <timeapi.h>
#include <tlhelp32.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <dirent.h>
#include <dlfcn.h>
#elif defined(__linux__)
#include <dirent.h>
#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef AVATAR_M0_BUILD_TYPE
#define AVATAR_M0_BUILD_TYPE "unknown"
#endif
#ifndef AVATAR_M0_GIT_COMMIT
#define AVATAR_M0_GIT_COMMIT "unknown"
#endif
#ifndef AVATAR_M0_GIT_DIRTY
#define AVATAR_M0_GIT_DIRTY 0
#endif
#ifndef AVATAR_M0_COMPILER
#define AVATAR_M0_COMPILER "unknown"
#endif

namespace {

using namespace avatar;

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr const char* kSchema = "avatar_m0.result.v1";
constexpr const char* kClipMagic = "AVCLIP1\n";
constexpr std::size_t kClipMagicSize = 8;
constexpr std::uint64_t kMaxClipBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxClipPlaneBytes = 64ull * 1024ull * 1024ull;
constexpr std::size_t kMaxClipFrames = 1000;
constexpr std::uint64_t kMaxFrameDimension = 8192;

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint64_t steady_now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] double ms_between(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

[[nodiscard]] double round_to(double value, int decimals) {
    const double factor = std::pow(10.0, static_cast<double>(decimals));
    return std::round(value * factor) / factor;
}

[[nodiscard]] std::string utc_now_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

[[nodiscard]] std::string path_text(const std::filesystem::path& path) {
    return path.string();
}

[[nodiscard]] std::string fnv1a64_hex(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : text) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

// ---------------------------------------------------------------------------
// Process and hardware counters
// ---------------------------------------------------------------------------

#if defined(_WIN32)

[[nodiscard]] std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

[[nodiscard]] std::string read_registry_string(const wchar_t* subkey, const wchar_t* name) {
    wchar_t buffer[512] = {};
    DWORD size = static_cast<DWORD>(sizeof(buffer));
    const LSTATUS status =
        RegGetValueW(HKEY_LOCAL_MACHINE, subkey, name, RRF_RT_REG_SZ, nullptr, buffer, &size);
    if (status != ERROR_SUCCESS) {
        return {};
    }
    return narrow(buffer);
}

[[nodiscard]] DWORD read_registry_dword(const wchar_t* subkey, const wchar_t* name,
                                        DWORD fallback) {
    DWORD value = fallback;
    DWORD size = static_cast<DWORD>(sizeof(value));
    const LSTATUS status =
        RegGetValueW(HKEY_LOCAL_MACHINE, subkey, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS ? value : fallback;
}

[[nodiscard]] std::string os_name() {
    constexpr const wchar_t* kCurrentVersion = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string product = read_registry_string(kCurrentVersion, L"ProductName");
    const std::string display = read_registry_string(kCurrentVersion, L"DisplayVersion");
    const std::string build_text = read_registry_string(kCurrentVersion, L"CurrentBuildNumber");
    DWORD build = 0;
    if (!build_text.empty()) {
        try {
            build = static_cast<DWORD>(std::stoul(build_text));
        } catch (const std::exception&) {
            build = 0;
        }
    }
    const DWORD ubr = read_registry_dword(kCurrentVersion, L"UBR", 0);
    if (build >= 22000) {
        const std::string from = "Windows 10";
        const std::string to = "Windows 11";
        const std::size_t position = product.find(from);
        if (position != std::string::npos) {
            product.replace(position, from.size(), to);
        }
    }
    if (product.empty()) {
        product = "Windows";
    }
    std::string result = product;
    if (!display.empty()) {
        result += " " + display;
    }
    if (build != 0) {
        result += " (build " + std::to_string(build);
        if (ubr != 0) {
            result += "." + std::to_string(ubr);
        }
        result += ")";
    }
    return result;
}

[[nodiscard]] std::string cpu_brand() {
#if defined(_M_X64) || defined(_M_IX86)
    std::array<int, 4> registers{};
    __cpuid(registers.data(), static_cast<int>(0x80000000));
    const unsigned maximum = static_cast<unsigned>(registers[0]);
    if (maximum < 0x80000004u) {
        return "unknown x86 CPU";
    }
    char brand[49] = {};
    for (int index = 0; index < 3; ++index) {
        __cpuid(registers.data(), static_cast<int>(0x80000002 + index));
        std::memcpy(brand + index * 16, registers.data(), 16);
    }
    std::string result(brand);
    while (!result.empty() && (result.back() == ' ' || result.back() == '\0')) {
        result.pop_back();
    }
    return result.empty() ? "unknown x86 CPU" : result;
#else
    return "unknown CPU";
#endif
}

[[nodiscard]] int physical_cores() {
    DWORD length = 0;
    (void)GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0) {
        return 0;
    }
    std::vector<char> buffer(length);
    auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first, &length)) {
        return 0;
    }
    int count = 0;
    DWORD offset = 0;
    while (offset < length) {
        auto* entry =
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore) {
            ++count;
        }
        if (entry->Size == 0) {
            break;
        }
        offset += entry->Size;
    }
    return count;
}

[[nodiscard]] int logical_cores() {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count != 0) {
        return static_cast<int>(count);
    }
    const unsigned fallback = std::thread::hardware_concurrency();
    return static_cast<int>(fallback == 0 ? 1 : fallback);
}

[[nodiscard]] std::uint64_t total_memory_bytes() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return 0;
    }
    return static_cast<std::uint64_t>(status.ullTotalPhys);
}

[[nodiscard]] std::string gpu_name() {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index) {
        if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0 &&
            (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) == 0) {
            const std::string name = narrow(device.DeviceString);
            if (!name.empty()) {
                return name;
            }
        }
    }
    return "unknown";
}

[[nodiscard]] std::uint64_t process_rss_bytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              static_cast<DWORD>(sizeof(counters)))) {
        return 0;
    }
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
}

[[nodiscard]] std::uint64_t process_cpu_ns() {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0;
    }
    const auto to_ns = [](const FILETIME& time) -> std::uint64_t {
        ULARGE_INTEGER value{};
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return static_cast<std::uint64_t>(value.QuadPart) * 100ull;
    };
    return to_ns(kernel) + to_ns(user);
}

[[nodiscard]] std::uint32_t process_handle_count() {
    DWORD count = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &count)) {
        return 0;
    }
    return static_cast<std::uint32_t>(count);
}

struct ThreadCpuSample {
    std::uint32_t thread_id = 0;
    double cpu_ms = 0.0;
};

[[nodiscard]] std::vector<ThreadCpuSample> collect_thread_cpu() {
    std::vector<ThreadCpuSample> samples;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return samples;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD process_id = GetCurrentProcessId();
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != process_id) {
                continue;
            }
            const HANDLE thread =
                OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread == nullptr) {
                continue;
            }
            FILETIME creation{};
            FILETIME exit{};
            FILETIME kernel{};
            FILETIME user{};
            if (GetThreadTimes(thread, &creation, &exit, &kernel, &user)) {
                ULARGE_INTEGER kernel_value{};
                kernel_value.LowPart = kernel.dwLowDateTime;
                kernel_value.HighPart = kernel.dwHighDateTime;
                ULARGE_INTEGER user_value{};
                user_value.LowPart = user.dwLowDateTime;
                user_value.HighPart = user.dwHighDateTime;
                const std::uint64_t total_100ns =
                    static_cast<std::uint64_t>(kernel_value.QuadPart) +
                    static_cast<std::uint64_t>(user_value.QuadPart);
                samples.push_back({static_cast<std::uint32_t>(entry.th32ThreadID),
                                   static_cast<double>(total_100ns) / 10'000.0});
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    std::sort(
        samples.begin(), samples.end(),
        [](const ThreadCpuSample& a, const ThreadCpuSample& b) { return a.cpu_ms > b.cpu_ms; });
    return samples;
}

#elif defined(__linux__)

[[nodiscard]] std::string os_name() {
    std::ifstream file("/etc/os-release");
    std::string line;
    while (std::getline(file, line)) {
        constexpr std::string_view prefix = "PRETTY_NAME=";
        if (line.rfind(prefix.data(), 0) == 0) {
            std::string value = line.substr(prefix.size());
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
    }
    return "Linux";
}

[[nodiscard]] std::string cpu_brand() {
    std::ifstream file("/proc/cpuinfo");
    std::string line;
    while (std::getline(file, line)) {
        constexpr std::string_view prefix = "model name";
        if (line.rfind(prefix.data(), 0) == 0) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') {
                    value.erase(value.begin());
                }
                return value;
            }
        }
    }
    return "unknown CPU";
}

[[nodiscard]] int physical_cores() {
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return static_cast<int>(count > 0 ? count : 1);
}

[[nodiscard]] int logical_cores() {
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return static_cast<int>(count > 0 ? count : 1);
}

[[nodiscard]] std::uint64_t total_memory_bytes() {
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || page_size <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
}

[[nodiscard]] std::string gpu_name() {
    return "unknown";
}

[[nodiscard]] std::uint64_t process_rss_bytes() {
    std::ifstream file("/proc/self/statm");
    std::uint64_t total_pages = 0;
    std::uint64_t resident_pages = 0;
    if (!(file >> total_pages >> resident_pages)) {
        return 0;
    }
    (void)total_pages;
    const long page_size = sysconf(_SC_PAGE_SIZE);
    return resident_pages * static_cast<std::uint64_t>(page_size > 0 ? page_size : 4096);
}

[[nodiscard]] std::uint64_t process_cpu_ns() {
    timespec value{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(value.tv_nsec);
}

[[nodiscard]] std::uint32_t process_handle_count() {
    DIR* directory = opendir("/proc/self/fd");
    if (directory == nullptr) {
        return 0;
    }
    std::uint32_t count = 0;
    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] != '.') {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

struct ThreadCpuSample {
    std::uint32_t thread_id = 0;
    double cpu_ms = 0.0;
};

[[nodiscard]] std::vector<ThreadCpuSample> collect_thread_cpu() {
    std::vector<ThreadCpuSample> samples;
    DIR* directory = opendir("/proc/self/task");
    if (directory == nullptr) {
        return samples;
    }
    const long ticks = sysconf(_SC_CLK_TCK);
    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        std::ifstream stat(std::string("/proc/self/task/") + entry->d_name + "/stat");
        std::string line;
        if (!std::getline(stat, line)) {
            continue;
        }
        std::istringstream fields(line);
        std::string token;
        int field = 0;
        std::uint64_t utime = 0;
        std::uint64_t stime = 0;
        try {
            while (fields >> token) {
                ++field;
                if (field == 14) {
                    utime = std::stoull(token);
                } else if (field == 15) {
                    stime = std::stoull(token);
                    break;
                }
            }
            samples.push_back({static_cast<std::uint32_t>(std::stoul(entry->d_name)),
                               static_cast<double>(utime + stime) * 1000.0 /
                                   static_cast<double>(ticks > 0 ? ticks : 100)});
        } catch (const std::exception&) {
            continue;
        }
    }
    closedir(directory);
    std::sort(
        samples.begin(), samples.end(),
        [](const ThreadCpuSample& a, const ThreadCpuSample& b) { return a.cpu_ms > b.cpu_ms; });
    return samples;
}

#elif defined(__APPLE__)

[[nodiscard]] std::string sysctl_string(const char* name) {
    std::size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
        return {};
    }
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
        return {};
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] std::uint64_t sysctl_u64(const char* name) {
    std::uint64_t value = 0;
    std::size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) {
        return 0;
    }
    return value;
}

[[nodiscard]] std::string os_name() {
    const std::string version = sysctl_string("kern.osproductversion");
    return version.empty() ? "macOS" : "macOS " + version;
}

[[nodiscard]] std::string cpu_brand() {
    std::string brand = sysctl_string("machdep.cpu.brand_string");
    return brand.empty() ? "unknown CPU" : brand;
}

[[nodiscard]] int physical_cores() {
    const std::uint64_t value = sysctl_u64("hw.physicalcpu");
    return static_cast<int>(value == 0 ? 1 : value);
}

[[nodiscard]] int logical_cores() {
    const std::uint64_t value = sysctl_u64("hw.logicalcpu");
    return static_cast<int>(value == 0 ? 1 : value);
}

[[nodiscard]] std::uint64_t total_memory_bytes() {
    return sysctl_u64("hw.memsize");
}

[[nodiscard]] std::string gpu_name() {
    return "unknown";
}

[[nodiscard]] std::uint64_t process_rss_bytes() {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::uint64_t>(info.resident_size);
}

[[nodiscard]] std::uint64_t process_cpu_ns() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    const auto to_ns = [](const timeval& value) -> std::uint64_t {
        return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ull +
               static_cast<std::uint64_t>(value.tv_usec) * 1000ull;
    };
    return to_ns(usage.ru_utime) + to_ns(usage.ru_stime);
}

[[nodiscard]] std::uint32_t process_handle_count() {
    return 0;
}

struct ThreadCpuSample {
    std::uint32_t thread_id = 0;
    double cpu_ms = 0.0;
};

[[nodiscard]] std::vector<ThreadCpuSample> collect_thread_cpu() {
    return {};
}

#else

[[nodiscard]] std::string os_name() {
    return "unknown";
}
[[nodiscard]] std::string cpu_brand() {
    return "unknown";
}
[[nodiscard]] int physical_cores() {
    return 1;
}
[[nodiscard]] int logical_cores() {
    return 1;
}
[[nodiscard]] std::uint64_t total_memory_bytes() {
    return 0;
}
[[nodiscard]] std::string gpu_name() {
    return "unknown";
}
[[nodiscard]] std::uint64_t process_rss_bytes() {
    return 0;
}
[[nodiscard]] std::uint64_t process_cpu_ns() {
    return 0;
}
[[nodiscard]] std::uint32_t process_handle_count() {
    return 0;
}

struct ThreadCpuSample {
    std::uint32_t thread_id = 0;
    double cpu_ms = 0.0;
};

[[nodiscard]] std::vector<ThreadCpuSample> collect_thread_cpu() {
    return {};
}

#endif

[[nodiscard]] std::string architecture() noexcept {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

struct HardwareInfo {
    std::string os;
    std::string arch;
    std::string cpu;
    int physical_cores = 0;
    int logical_cores = 0;
    std::uint64_t memory_bytes = 0;
    std::string gpu;
    std::string fingerprint;
    std::string tier;
};

[[nodiscard]] HardwareInfo collect_hardware() {
    HardwareInfo info;
    info.os = os_name();
    info.arch = architecture();
    info.cpu = cpu_brand();
    info.physical_cores = physical_cores();
    info.logical_cores = logical_cores();
    info.memory_bytes = total_memory_bytes();
    info.gpu = gpu_name();
    const std::string canonical = info.os + "|" + info.arch + "|" + info.cpu + "|" +
                                  std::to_string(info.physical_cores) + "|" +
                                  std::to_string(info.logical_cores) + "|" +
                                  std::to_string(info.memory_bytes) + "|" + info.gpu;
    info.fingerprint = "fnv1a64:" + fnv1a64_hex(canonical);
    // The design leaves the Reference/Minimum split to the gate. This is the
    // explicit heuristic the harness records; T14 may override it with the
    // hardware table. It never changes a measured value.
    if (info.logical_cores >= 16 && info.memory_bytes >= 32ull * 1024ull * 1024ull * 1024ull) {
        info.tier = "reference";
    } else {
        info.tier = "minimum";
    }
    return info;
}

[[nodiscard]] json hardware_json(const HardwareInfo& info) {
    return json{{"os", info.os},
                {"arch", info.arch},
                {"cpu", info.cpu},
                {"physical_cores", info.physical_cores},
                {"logical_cores", info.logical_cores},
                {"memory_bytes", info.memory_bytes},
                {"gpu", info.gpu},
                {"fingerprint", info.fingerprint},
                {"tier", info.tier}};
}

// ---------------------------------------------------------------------------
// Git and build metadata
// ---------------------------------------------------------------------------

struct GitInfo {
    std::string commit = AVATAR_M0_GIT_COMMIT;
    bool dirty = AVATAR_M0_GIT_DIRTY != 0;
};

[[nodiscard]] std::string run_command(const std::string& command) {
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return {};
    }
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

[[nodiscard]] std::string shell_quote(const std::string& value) {
    std::string quoted = "\"";
    for (const char character : value) {
        if (character == '"') {
            quoted += "\\\"";
        } else {
            quoted += character;
        }
    }
    quoted += "\"";
    return quoted;
}

[[nodiscard]] const GitInfo& git_info() {
    static const GitInfo info = [] {
        GitInfo result;
        std::filesystem::path directory = std::filesystem::current_path();
        std::filesystem::path root;
        for (int depth = 0; depth < 32; ++depth) {
            if (std::filesystem::exists(directory / ".git")) {
                root = directory;
                break;
            }
            if (!directory.has_parent_path() || directory.parent_path() == directory) {
                break;
            }
            directory = directory.parent_path();
        }
        if (!root.empty()) {
            const std::string prefix = "git -C " + shell_quote(path_text(root));
            const std::string commit = run_command(prefix + " rev-parse HEAD");
            if (!commit.empty()) {
                result.commit = commit;
                result.dirty = !run_command(prefix + " status --porcelain").empty();
            }
        }
        if (result.commit.empty()) {
            result.commit = "unknown";
        }
        return result;
    }();
    return info;
}

[[nodiscard]] json build_json() {
    const GitInfo& git = git_info();
    return json{{"type", AVATAR_M0_BUILD_TYPE},
                {"git_commit", git.commit},
                {"git_dirty", git.dirty},
                {"compiler", AVATAR_M0_COMPILER}};
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct Distribution {
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double mean = 0.0;
    double max = 0.0;
    double min = 0.0;
    std::size_t samples = 0;
};

[[nodiscard]] double percentile_sorted(const std::vector<double>& values, double percent) {
    if (values.empty()) {
        return 0.0;
    }
    if (percent <= 0.0) {
        return values.front();
    }
    if (percent >= 100.0) {
        return values.back();
    }
    const double rank = (percent / 100.0) * static_cast<double>(values.size());
    std::size_t index = static_cast<std::size_t>(std::ceil(rank));
    if (index == 0) {
        index = 1;
    }
    if (index > values.size()) {
        index = values.size();
    }
    return values[index - 1];
}

[[nodiscard]] Distribution summarize(std::vector<double> values) {
    Distribution result;
    result.samples = values.size();
    if (values.empty()) {
        return result;
    }
    std::sort(values.begin(), values.end());
    result.p50 = percentile_sorted(values, 50.0);
    result.p95 = percentile_sorted(values, 95.0);
    result.p99 = percentile_sorted(values, 99.0);
    result.min = values.front();
    result.max = values.back();
    double total = 0.0;
    for (const double value : values) {
        total += value;
    }
    result.mean = total / static_cast<double>(values.size());
    return result;
}

[[nodiscard]] json distribution_json(const Distribution& distribution) {
    return json{{"p50", round_to(distribution.p50, 4)}, {"p95", round_to(distribution.p95, 4)},
                {"p99", round_to(distribution.p99, 4)}, {"mean", round_to(distribution.mean, 4)},
                {"max", round_to(distribution.max, 4)}, {"min", round_to(distribution.min, 4)},
                {"samples", distribution.samples}};
}

// ---------------------------------------------------------------------------
// AVCLIP1 replay reader
// ---------------------------------------------------------------------------

struct ClipFrame {
    std::uint64_t timestamp_ns = 0;
    std::uint32_t sequence = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    std::uint32_t stride = 0;
    std::uint32_t plane_count = 0;
    std::uint64_t plane0_size = 0;
    std::uint64_t plane1_size = 0;
    std::vector<std::uint8_t> bytes;
};

template <typename T>
[[nodiscard]] bool read_scalar(std::istream& stream, T& value) {
    if (!stream.read(reinterpret_cast<char*>(&value), sizeof(value))) {
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<capture::PixelFormat> clip_pixel_format(std::uint32_t format) {
    switch (format) {
        case AVATAR_TRACKER_PIXEL_FORMAT_NV12:
            return capture::PixelFormat::nv12;
        case AVATAR_TRACKER_PIXEL_FORMAT_YUY2:
            return capture::PixelFormat::yuy2;
        case AVATAR_TRACKER_PIXEL_FORMAT_BGRA8:
            return capture::PixelFormat::bgra8;
        case AVATAR_TRACKER_PIXEL_FORMAT_RGBA8:
            return capture::PixelFormat::rgba8;
        default:
            return std::nullopt;
    }
}

class ReplayClip {
public:
    [[nodiscard]] bool load(const std::filesystem::path& path, std::string& error) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            error = "cannot open replay clip: " + path_text(path);
            return false;
        }
        char magic[kClipMagicSize] = {};
        if (!stream.read(magic, static_cast<std::streamsize>(kClipMagicSize)) ||
            std::memcmp(magic, kClipMagic, kClipMagicSize) != 0) {
            error = "replay clip magic is not AVCLIP1: " + path_text(path);
            return false;
        }
        std::uint64_t total = 0;
        while (stream.peek() != std::char_traits<char>::eof()) {
            ClipFrame frame;
            if (!read_scalar(stream, frame.timestamp_ns) || !read_scalar(stream, frame.sequence) ||
                !read_scalar(stream, frame.width) || !read_scalar(stream, frame.height) ||
                !read_scalar(stream, frame.format) || !read_scalar(stream, frame.stride) ||
                !read_scalar(stream, frame.plane_count) ||
                !read_scalar(stream, frame.plane0_size) ||
                !read_scalar(stream, frame.plane1_size)) {
                error = "truncated replay clip header";
                return false;
            }
            if (frames_.size() >= kMaxClipFrames) {
                error = "replay clip has too many frames";
                return false;
            }
            if (frame.width == 0 || frame.height == 0 || frame.width > kMaxFrameDimension ||
                frame.height > kMaxFrameDimension) {
                error = "replay clip frame dimensions are invalid";
                return false;
            }
            if (frame.plane0_size > kMaxClipPlaneBytes || frame.plane1_size > kMaxClipPlaneBytes ||
                total + frame.plane0_size + frame.plane1_size > kMaxClipBytes) {
                error = "replay clip exceeds the diagnostic size limit";
                return false;
            }
            const auto format = clip_pixel_format(frame.format);
            if (!format.has_value()) {
                error = "replay clip uses an unsupported pixel format";
                return false;
            }
            const std::uint32_t expected_planes = *format == capture::PixelFormat::nv12 ? 2u : 1u;
            if (frame.plane_count != expected_planes) {
                error = "replay clip plane count does not match its pixel format";
                return false;
            }
            std::array<capture::PlaneLayout, capture::kMaxFramePlanes> layout{};
            if (!capture::compute_plane_layout(*format, frame.width, frame.height, layout)) {
                error = "replay clip frame layout is invalid";
                return false;
            }
            if (frame.plane0_size < layout[0].size ||
                (expected_planes == 2 && frame.plane1_size < layout[1].size)) {
                error = "replay clip plane sizes are smaller than the frame layout";
                return false;
            }
            total += frame.plane0_size + frame.plane1_size;
            frame.bytes.resize(static_cast<std::size_t>(frame.plane0_size + frame.plane1_size));
            if (!stream.read(reinterpret_cast<char*>(frame.bytes.data()),
                             static_cast<std::streamsize>(frame.bytes.size()))) {
                error = "truncated replay clip pixels";
                return false;
            }
            frames_.push_back(std::move(frame));
        }
        if (frames_.empty()) {
            error = "replay clip has no frames";
            return false;
        }
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return frames_.size(); }

    void fill(std::size_t index, std::uint64_t timestamp_ns, capture::VideoFrame& frame) const {
        const ClipFrame& source = frames_[index % frames_.size()];
        frame = {};
        frame.timestamp_ns = timestamp_ns;
        frame.arrival_ns = timestamp_ns;
        frame.sequence = static_cast<std::uint32_t>(index & 0xFFFFFFFFu);
        frame.width = source.width;
        frame.height = source.height;
        frame.format = *clip_pixel_format(source.format);
        frame.color_space = frame.format == capture::PixelFormat::bgra8 ||
                                    frame.format == capture::PixelFormat::rgba8
                                ? capture::ColorSpace::srgb
                                : capture::ColorSpace::bt601_limited;
        frame.storage = capture::FrameStorage::cpu;
        frame.strides[0] = source.stride;
        frame.planes[0] = source.bytes.data();
        if (source.plane_count >= 2) {
            frame.strides[1] = source.stride;
            frame.planes[1] = source.bytes.data() + source.plane0_size;
        }
    }

private:
    std::vector<ClipFrame> frames_;
};

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

class PatternSource {
public:
    PatternSource(std::uint32_t width, std::uint32_t height)
        : time_(1), clock_(time_), source_(clock_, make_config(width, height)) {}

    [[nodiscard]] bool start(std::string& error) {
        const core::Status status = source_.start();
        if (!status.ok()) {
            error = "test pattern source failed to start: " + status.to_string();
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<capture::FrameLease> acquire(std::uint64_t timestamp_ns) {
        time_.set(timestamp_ns);
        (void)source_.produce();
        return source_.ring().try_acquire_latest();
    }

    [[nodiscard]] capture::SourceInfo info() const noexcept { return source_.info(); }

private:
    [[nodiscard]] static capture::TestPatternConfig make_config(std::uint32_t width,
                                                                std::uint32_t height) {
        capture::TestPatternConfig config;
        config.width = width;
        config.height = height;
        config.format = capture::PixelFormat::bgra8;
        config.colour_bars = true;
        config.moving_marker = true;
        config.timestamp_burnin = true;
        config.alpha_checkerboard = true;
        return config;
    }

    core::ManualTimeSource time_;
    core::FrameClock clock_;
    capture::TestPatternSource source_;
};

// ---------------------------------------------------------------------------
// Trackers
// ---------------------------------------------------------------------------

class TrackerBackend {
public:
    virtual ~TrackerBackend() = default;
    virtual bool initialize(std::uint32_t input_size, std::string& error) = 0;
    virtual bool submit(const capture::VideoFrame& frame, std::string& error) = 0;
    virtual std::optional<tracking::TrackingResult> poll() = 0;
    virtual void shutdown() = 0;
    [[nodiscard]] virtual bool synthetic() const = 0;
    [[nodiscard]] virtual std::string kind() const = 0;
};

class ScriptedBackend final : public TrackerBackend {
public:
    bool initialize(std::uint32_t input_size, std::string& error) override {
        (void)input_size;
        tracking::ScriptedTrackerConfig config;
        config.scenario = tracking::ScriptedScenario::valid_tracking;
        // Bounded script: a long soak rewinds instead of building one result
        // per submitted frame (which would be hundreds of megabytes).
        config.frame_count = 600;
        config.frame_interval_ns = 33'333'333ull;
        tracker_.configure(config);
        (void)error;
        return true;
    }

    bool submit(const capture::VideoFrame& frame, std::string& error) override {
        if (tracker_.cursor() >= tracker_.script().size()) {
            tracker_.reset();
        }
        tracker_.submit(frame);
        pending_ = tracker_.latest();
        if (!pending_.has_value()) {
            error = "scripted tracker produced no result";
            return false;
        }
        (void)error;
        return true;
    }

    std::optional<tracking::TrackingResult> poll() override {
        std::optional<tracking::TrackingResult> result = std::move(pending_);
        pending_.reset();
        return result;
    }

    void shutdown() override { pending_.reset(); }

    [[nodiscard]] bool synthetic() const override { return true; }
    [[nodiscard]] std::string kind() const override { return "scripted"; }

private:
    tracking::ScriptedTracker tracker_;
    std::optional<tracking::TrackingResult> pending_;
};

class CabiBackend final : public TrackerBackend {
public:
    ~CabiBackend() override { shutdown(); }

    [[nodiscard]] bool load(const std::filesystem::path& path, std::string& error) {
        const std::filesystem::path absolute = std::filesystem::absolute(path);
#if defined(_WIN32)
        module_ =
            LoadLibraryExW(absolute.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module_ == nullptr) {
            error = "cannot load tracker DLL '" + path_text(absolute) + "' (error " +
                    std::to_string(GetLastError()) + ")";
            return false;
        }
        if (!bind_windows("avatar_tracker_create", create_) ||
            !bind_windows("avatar_tracker_configure", configure_) ||
            !bind_windows("avatar_tracker_push_frame", push_) ||
            !bind_windows("avatar_tracker_poll_result", poll_) ||
            !bind_windows("avatar_tracker_destroy", destroy_)) {
            error = "tracker DLL is missing one of the five T09 exports";
            shutdown();
            return false;
        }
#else
        module_ = dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (module_ == nullptr) {
            error = std::string("cannot load tracker library: ") + dlerror();
            return false;
        }
        if (!bind_posix("avatar_tracker_create", create_) ||
            !bind_posix("avatar_tracker_configure", configure_) ||
            !bind_posix("avatar_tracker_push_frame", push_) ||
            !bind_posix("avatar_tracker_poll_result", poll_) ||
            !bind_posix("avatar_tracker_destroy", destroy_)) {
            error = "tracker library is missing one of the five T09 exports";
            shutdown();
            return false;
        }
#endif
        dll_path_ = path_text(absolute);
        return true;
    }

    bool initialize(std::uint32_t input_size, std::string& error) override {
        if (create_ == nullptr) {
            error = "tracker DLL is not loaded";
            return false;
        }
        avatar_tracker_config config = AVATAR_TRACKER_CONFIG_INIT;
        config.input_width = input_size;
        config.input_height = input_size;
        avatar_tracker_error status = AVATAR_TRACKER_OK;
        handle_ = create_(&config, &status);
        if (handle_ == nullptr) {
            error = "avatar_tracker_create failed with status " +
                    std::to_string(static_cast<int>(status));
            return false;
        }
        return true;
    }

    bool submit(const capture::VideoFrame& frame, std::string& error) override {
        if (handle_ == nullptr) {
            error = "tracker is not initialized";
            return false;
        }
        avatar_tracker_frame abi{};
        abi.timestamp_ns = frame.timestamp_ns;
        abi.arrival_ns = frame.arrival_ns;
        abi.sequence = frame.sequence;
        abi.width = frame.width;
        abi.height = frame.height;
        abi.format = static_cast<std::uint32_t>(frame.format);
        abi.stride = frame.strides[0];
        std::array<capture::PlaneLayout, capture::kMaxFramePlanes> layout{};
        if (!capture::compute_plane_layout(frame.format, frame.width, frame.height, layout)) {
            error = "frame layout is invalid";
            return false;
        }
        const std::array<const std::uint8_t*, capture::kMaxFramePlanes> planes = {
            frame.planes[0], frame.planes[1], frame.planes[2]};
        for (std::size_t index = 0; index < capture::kMaxFramePlanes; ++index) {
            if (planes[index] == nullptr || layout[index].size == 0) {
                continue;
            }
            ++abi.plane_count;
        }
        abi.plane0 = planes[0];
        abi.plane1 = planes[1];
        abi.plane2 = planes[2];
        abi.plane0_size = planes[0] != nullptr ? layout[0].size : 0;
        abi.plane1_size = planes[1] != nullptr ? layout[1].size : 0;
        abi.plane2_size = planes[2] != nullptr ? layout[2].size : 0;
        const avatar_tracker_error status = push_(handle_, &abi);
        if (status != AVATAR_TRACKER_OK) {
            error = "avatar_tracker_push_frame failed with status " +
                    std::to_string(static_cast<int>(status));
            return false;
        }
        return true;
    }

    std::optional<tracking::TrackingResult> poll() override {
        if (handle_ == nullptr || poll_ == nullptr) {
            return std::nullopt;
        }
        avatar_tracker_result abi{};
        const avatar_tracker_error status = poll_(handle_, &abi);
        if (status == AVATAR_TRACKER_NO_RESULT) {
            return std::nullopt;
        }
        if (status != AVATAR_TRACKER_OK) {
            last_error_ = "avatar_tracker_poll_result failed with status " +
                          std::to_string(static_cast<int>(status));
            return std::nullopt;
        }
        tracking::TrackingResult result;
        result.source_timestamp_ns = abi.source_timestamp_ns;
        result.valid = abi.valid != 0;
        result.features_valid = abi.features_valid != 0;
        result.confidence = abi.confidence;
        for (std::size_t index = 0; index < 4; ++index) {
            result.pose.rotation[index] = abi.pose.rotation[index];
        }
        for (std::size_t index = 0; index < 3; ++index) {
            result.pose.translation[index] = abi.pose.translation[index];
        }
        const std::size_t count =
            std::min<std::size_t>(abi.landmark_count, tracking::LandmarkSet::kCapacity);
        result.landmarks.count = count;
        for (std::size_t index = 0; index < count; ++index) {
            result.landmarks.points[index].x = abi.landmarks[index].x;
            result.landmarks.points[index].y = abi.landmarks[index].y;
            result.landmarks.points[index].z = abi.landmarks[index].z;
        }
        for (std::size_t index = 0; index < tracking::BlendshapeSet::kCount; ++index) {
            result.blendshapes.coefficients[index] = abi.blendshapes[index];
        }
        return result;
    }

    void shutdown() override {
        if (destroy_ != nullptr && handle_ != nullptr) {
            destroy_(handle_);
            handle_ = nullptr;
        }
#if defined(_WIN32)
        if (module_ != nullptr) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
#else
        if (module_ != nullptr) {
            dlclose(module_);
            module_ = nullptr;
        }
#endif
    }

    [[nodiscard]] bool synthetic() const override { return false; }
    [[nodiscard]] std::string kind() const override { return "real"; }
    [[nodiscard]] const std::string& dll_path() const noexcept { return dll_path_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
#if defined(_WIN32)
    template <typename T>
    [[nodiscard]] bool bind_windows(const char* name, T& out) {
        const FARPROC address = GetProcAddress(module_, name);
        if (address == nullptr) {
            return false;
        }
        static_assert(sizeof(out) == sizeof(address));
        std::memcpy(&out, &address, sizeof(out));
        return true;
    }
#else
    template <typename T>
    [[nodiscard]] bool bind_posix(const char* name, T& out) {
        void* address = dlsym(module_, name);
        if (address == nullptr) {
            return false;
        }
        static_assert(sizeof(out) == sizeof(address));
        std::memcpy(&out, &address, sizeof(out));
        return true;
    }
#endif

#if defined(_WIN32)
    HMODULE module_ = nullptr;
#else
    void* module_ = nullptr;
#endif
    using CreateFn = avatar_tracker_handle* (*)(const avatar_tracker_config*,
                                                avatar_tracker_error*);
    using ConfigureFn = avatar_tracker_error (*)(avatar_tracker_handle*,
                                                 const avatar_tracker_config*);
    using PushFn = avatar_tracker_error (*)(avatar_tracker_handle*, const avatar_tracker_frame*);
    using PollFn = avatar_tracker_error (*)(avatar_tracker_handle*, avatar_tracker_result*);
    using DestroyFn = void (*)(avatar_tracker_handle*);
    CreateFn create_ = nullptr;
    ConfigureFn configure_ = nullptr;
    PushFn push_ = nullptr;
    PollFn poll_ = nullptr;
    DestroyFn destroy_ = nullptr;
    avatar_tracker_handle* handle_ = nullptr;
    std::string dll_path_;
    std::string last_error_;
};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

enum class SourceKind { pattern, replay };
enum class TrackerKind { scripted, real };

struct Options {
    SourceKind source = SourceKind::pattern;
    TrackerKind tracker = TrackerKind::scripted;
    std::string replay_path;
    std::string tracker_dll;
    std::uint32_t input_size = 192;
    double duration_s = 60.0;
    double warmup_s = 5.0;
    double rss_interval_s = 5.0;
    int fps = 30;
    std::uint32_t pattern_width = 640;
    std::uint32_t pattern_height = 480;
    int clip_frames = 48;
    std::string output;
    std::string pose_trace;
    std::string clip_dir;
    bool strict = false;
    bool quiet = false;
};

[[nodiscard]] const char* source_name(SourceKind kind) noexcept {
    return kind == SourceKind::replay ? "replay" : "pattern";
}

// ---------------------------------------------------------------------------
// Clip and pose artifacts
// ---------------------------------------------------------------------------

struct PosePoint {
    double t_s = 0.0;
    double yaw_deg = 0.0;
    double pitch_deg = 0.0;
    double roll_deg = 0.0;
    bool valid = false;
};

struct ClipCandidate {
    std::uint64_t timestamp_ns = 0;
    capture::VideoFrame frame{};
    std::vector<std::uint8_t> plane0;
    std::vector<std::uint8_t> plane1;
    std::uint32_t plane0_size = 0;
    std::uint32_t plane1_size = 0;
    bool has_result = false;
    bool valid = false;
    bool features_valid = false;
    float confidence = 0.0f;
    tracking::LandmarkSet landmarks{};
};

[[nodiscard]] std::array<double, 3> euler_deg(const std::array<float, 4>& q) {
    const double x = q[0];
    const double y = q[1];
    const double z = q[2];
    const double w = q[3];
    const double sinp = 2.0 * (w * x - y * z);
    const double pitch =
        std::abs(sinp) >= 1.0 ? std::copysign(3.14159265358979323846 / 2.0, sinp) : std::asin(sinp);
    const double yaw = std::atan2(2.0 * (w * y + x * z), 1.0 - 2.0 * (y * y + z * z));
    const double roll = std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (x * x + y * y));
    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    return {yaw * kRadToDeg, pitch * kRadToDeg, roll * kRadToDeg};
}

[[nodiscard]] double quaternion_delta_deg(const std::array<float, 4>& a,
                                          const std::array<float, 4>& b) {
    double dot = 0.0;
    for (std::size_t index = 0; index < 4; ++index) {
        dot += static_cast<double>(a[index]) * static_cast<double>(b[index]);
    }
    dot = std::min(1.0, std::max(-1.0, std::abs(dot)));
    return 2.0 * std::acos(dot) * (180.0 / 3.14159265358979323846);
}

[[nodiscard]] std::uint8_t clamp_byte(double value) {
    if (value <= 0.0) {
        return 0;
    }
    if (value >= 255.0) {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}

void bgra_to_rgb(const std::uint8_t* source, std::uint32_t stride, std::uint32_t width,
                 std::uint32_t height, std::vector<std::uint8_t>& rgb) {
    rgb.resize(static_cast<std::size_t>(width) * height * 3);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = source + static_cast<std::size_t>(y) * stride;
        std::uint8_t* out = rgb.data() + static_cast<std::size_t>(y) * width * 3;
        for (std::uint32_t x = 0; x < width; ++x) {
            out[x * 3 + 0] = row[x * 4 + 2];
            out[x * 3 + 1] = row[x * 4 + 1];
            out[x * 3 + 2] = row[x * 4 + 0];
        }
    }
}

void rgba_to_rgb(const std::uint8_t* source, std::uint32_t stride, std::uint32_t width,
                 std::uint32_t height, std::vector<std::uint8_t>& rgb) {
    rgb.resize(static_cast<std::size_t>(width) * height * 3);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = source + static_cast<std::size_t>(y) * stride;
        std::uint8_t* out = rgb.data() + static_cast<std::size_t>(y) * width * 3;
        for (std::uint32_t x = 0; x < width; ++x) {
            out[x * 3 + 0] = row[x * 4 + 0];
            out[x * 3 + 1] = row[x * 4 + 1];
            out[x * 3 + 2] = row[x * 4 + 2];
        }
    }
}

void nv12_to_rgb(const std::uint8_t* y_plane, std::uint32_t y_stride, const std::uint8_t* uv_plane,
                 std::uint32_t uv_stride, std::uint32_t width, std::uint32_t height,
                 std::vector<std::uint8_t>& rgb) {
    rgb.resize(static_cast<std::size_t>(width) * height * 3);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* y_row = y_plane + static_cast<std::size_t>(y) * y_stride;
        const std::uint8_t* uv_row = uv_plane + static_cast<std::size_t>(y / 2u) * uv_stride;
        std::uint8_t* out = rgb.data() + static_cast<std::size_t>(y) * width * 3;
        for (std::uint32_t x = 0; x < width; ++x) {
            const double c = static_cast<double>(y_row[x]) - 16.0;
            const double d = static_cast<double>(uv_row[(x / 2u) * 2u]) - 128.0;
            const double e = static_cast<double>(uv_row[(x / 2u) * 2u + 1u]) - 128.0;
            out[x * 3 + 0] = clamp_byte((298.0 * c + 409.0 * e + 128.0) / 256.0);
            out[x * 3 + 1] = clamp_byte((298.0 * c - 100.0 * d - 208.0 * e + 128.0) / 256.0);
            out[x * 3 + 2] = clamp_byte((298.0 * c + 516.0 * d + 128.0) / 256.0);
        }
    }
}

void yuy2_to_rgb(const std::uint8_t* source, std::uint32_t stride, std::uint32_t width,
                 std::uint32_t height, std::vector<std::uint8_t>& rgb) {
    rgb.resize(static_cast<std::size_t>(width) * height * 3);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = source + static_cast<std::size_t>(y) * stride;
        std::uint8_t* out = rgb.data() + static_cast<std::size_t>(y) * width * 3;
        for (std::uint32_t x = 0; x < width; x += 2) {
            const double c0 = static_cast<double>(row[x * 2 + 0]) - 16.0;
            const double d = static_cast<double>(row[x * 2 + 1]) - 128.0;
            const double c1 = static_cast<double>(row[x * 2 + 2]) - 16.0;
            const double e = static_cast<double>(row[x * 2 + 3]) - 128.0;
            out[x * 3 + 0] = clamp_byte((298.0 * c0 + 409.0 * e + 128.0) / 256.0);
            out[x * 3 + 1] = clamp_byte((298.0 * c0 - 100.0 * d - 208.0 * e + 128.0) / 256.0);
            out[x * 3 + 2] = clamp_byte((298.0 * c0 + 516.0 * d + 128.0) / 256.0);
            if (x + 1 < width) {
                out[(x + 1) * 3 + 0] = clamp_byte((298.0 * c1 + 409.0 * e + 128.0) / 256.0);
                out[(x + 1) * 3 + 1] =
                    clamp_byte((298.0 * c1 - 100.0 * d - 208.0 * e + 128.0) / 256.0);
                out[(x + 1) * 3 + 2] = clamp_byte((298.0 * c1 + 516.0 * d + 128.0) / 256.0);
            }
        }
    }
}

[[nodiscard]] bool frame_to_rgb(const capture::VideoFrame& frame, std::vector<std::uint8_t>& rgb) {
    const std::uint8_t* plane0 = frame.plane(0);
    if (plane0 == nullptr) {
        return false;
    }
    switch (frame.format) {
        case capture::PixelFormat::bgra8:
            bgra_to_rgb(plane0, frame.stride(0), frame.width, frame.height, rgb);
            return true;
        case capture::PixelFormat::rgba8:
            rgba_to_rgb(plane0, frame.stride(0), frame.width, frame.height, rgb);
            return true;
        case capture::PixelFormat::nv12: {
            const std::uint8_t* plane1 = frame.plane(1);
            if (plane1 == nullptr) {
                return false;
            }
            nv12_to_rgb(plane0, frame.stride(0), plane1, frame.stride(1), frame.width, frame.height,
                        rgb);
            return true;
        }
        case capture::PixelFormat::yuy2:
            yuy2_to_rgb(plane0, frame.stride(0), frame.width, frame.height, rgb);
            return true;
        case capture::PixelFormat::mjpeg:
            return false;
    }
    return false;
}

void downscale_rgb(const std::vector<std::uint8_t>& source, std::uint32_t source_width,
                   std::uint32_t source_height, std::uint32_t target_width,
                   std::uint32_t target_height, std::vector<std::uint8_t>& target) {
    target.resize(static_cast<std::size_t>(target_width) * target_height * 3);
    for (std::uint32_t y = 0; y < target_height; ++y) {
        const std::uint32_t source_y =
            std::min(source_height - 1, (y * source_height) / target_height);
        for (std::uint32_t x = 0; x < target_width; ++x) {
            const std::uint32_t source_x =
                std::min(source_width - 1, (x * source_width) / target_width);
            const std::size_t source_index =
                (static_cast<std::size_t>(source_y) * source_width + source_x) * 3;
            const std::size_t target_index = (static_cast<std::size_t>(y) * target_width + x) * 3;
            target[target_index + 0] = source[source_index + 0];
            target[target_index + 1] = source[source_index + 1];
            target[target_index + 2] = source[source_index + 2];
        }
    }
}

void draw_dot(std::vector<std::uint8_t>& rgb, std::uint32_t width, std::uint32_t height,
              float normalized_x, float normalized_y, std::uint8_t r, std::uint8_t g,
              std::uint8_t b) {
    if (!(normalized_x >= 0.0f && normalized_x <= 1.0f && normalized_y >= 0.0f &&
          normalized_y <= 1.0f)) {
        return;
    }
    const int centre_x = static_cast<int>(normalized_x * static_cast<float>(width - 1));
    const int centre_y = static_cast<int>(normalized_y * static_cast<float>(height - 1));
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int x = centre_x + dx;
            const int y = centre_y + dy;
            if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
                continue;
            }
            const std::size_t index =
                (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 3;
            rgb[index + 0] = r;
            rgb[index + 1] = g;
            rgb[index + 2] = b;
        }
    }
}

struct OverlayLandmark {
    const char* name = nullptr;
    std::size_t index = 0;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

[[nodiscard]] std::vector<OverlayLandmark> overlay_landmarks(bool real_tracker) {
    if (real_tracker) {
        const tracking::LandmarkMap map = tracking::LandmarkMap::mediapipe();
        const std::array<const char*, 10> names = {
            "eye_outer_r",   "eye_inner_r", "iris_center_r",  "eye_outer_l",    "eye_inner_l",
            "iris_center_l", "nose_tip",    "mouth_corner_r", "mouth_corner_l", "chin"};
        std::vector<OverlayLandmark> result;
        for (const char* name : names) {
            const auto index = map.resolve(name);
            if (!index.has_value()) {
                continue;
            }
            const bool eye = std::string_view(name).find("eye") != std::string_view::npos ||
                             std::string_view(name).find("iris") != std::string_view::npos;
            const bool mouth = std::string_view(name).find("mouth") != std::string_view::npos;
            result.push_back(
                {name, *index, eye ? static_cast<std::uint8_t>(0) : static_cast<std::uint8_t>(255),
                 eye ? static_cast<std::uint8_t>(255) : static_cast<std::uint8_t>(80),
                 mouth ? static_cast<std::uint8_t>(255) : static_cast<std::uint8_t>(0)});
        }
        return result;
    }
    using ScriptedIndex = tracking::ScriptedTracker::LandmarkIndex;
    return {
        {"eye_outer_r", static_cast<std::size_t>(ScriptedIndex::eye_outer_r), 0, 255, 0},
        {"eye_inner_r", static_cast<std::size_t>(ScriptedIndex::eye_inner_r), 0, 255, 0},
        {"iris_center_r", static_cast<std::size_t>(ScriptedIndex::iris_center_r), 0, 255, 0},
        {"eye_outer_l", static_cast<std::size_t>(ScriptedIndex::eye_outer_l), 0, 255, 0},
        {"eye_inner_l", static_cast<std::size_t>(ScriptedIndex::eye_inner_l), 0, 255, 0},
        {"iris_center_l", static_cast<std::size_t>(ScriptedIndex::iris_center_l), 0, 255, 0},
        {"nose_tip", static_cast<std::size_t>(ScriptedIndex::nose_tip), 255, 80, 0},
        {"mouth_corner_r", static_cast<std::size_t>(ScriptedIndex::mouth_corner_r), 255, 0, 0},
        {"mouth_corner_l", static_cast<std::size_t>(ScriptedIndex::mouth_corner_l), 255, 0, 0},
        {"chin", static_cast<std::size_t>(ScriptedIndex::chin), 255, 255, 0},
    };
}

[[nodiscard]] bool write_bmp(const std::filesystem::path& path, std::uint32_t width,
                             std::uint32_t height, const std::vector<std::uint8_t>& rgb) {
    if (width == 0 || height == 0 || rgb.size() < static_cast<std::size_t>(width) * height * 3) {
        return false;
    }
    const std::uint32_t row_bytes = width * 3;
    const std::uint32_t padding = (4u - (row_bytes % 4u)) % 4u;
    const std::uint32_t image_bytes = (row_bytes + padding) * height;
    const std::uint32_t file_bytes = 14u + 40u + image_bytes;
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    const auto write_u16 = [&out](std::uint16_t value) {
        out.put(static_cast<char>(value & 0xFFu));
        out.put(static_cast<char>((value >> 8) & 0xFFu));
    };
    const auto write_u32 = [&out](std::uint32_t value) {
        out.put(static_cast<char>(value & 0xFFu));
        out.put(static_cast<char>((value >> 8) & 0xFFu));
        out.put(static_cast<char>((value >> 16) & 0xFFu));
        out.put(static_cast<char>((value >> 24) & 0xFFu));
    };
    out.put('B');
    out.put('M');
    write_u32(file_bytes);
    write_u16(0);
    write_u16(0);
    write_u32(14u + 40u);
    write_u32(40u);
    write_u32(width);
    write_u32(height);
    write_u16(1);
    write_u16(24);
    write_u32(0);
    write_u32(image_bytes);
    write_u32(2835);
    write_u32(2835);
    write_u32(0);
    write_u32(0);
    const std::array<char, 3> zero = {0, 0, 0};
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t source_row = height - 1u - row;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(source_row) * width + x) * 3;
            out.put(static_cast<char>(rgb[index + 2]));
            out.put(static_cast<char>(rgb[index + 1]));
            out.put(static_cast<char>(rgb[index + 0]));
        }
        for (std::uint32_t pad = 0; pad < padding; ++pad) {
            out.put(zero[0]);
        }
    }
    return static_cast<bool>(out);
}

[[nodiscard]] bool write_pose_svg(const std::filesystem::path& path,
                                  const std::vector<PosePoint>& points) {
    if (points.empty()) {
        return false;
    }
    constexpr double kWidth = 1000.0;
    constexpr double kHeight = 360.0;
    constexpr double kMargin = 44.0;
    const double plot_width = kWidth - 2.0 * kMargin;
    const double plot_height = kHeight - 2.0 * kMargin;
    double t_min = points.front().t_s;
    double t_max = points.back().t_s;
    if (!(t_max > t_min)) {
        t_max = t_min + 1.0;
    }
    double angle_min = std::numeric_limits<double>::max();
    double angle_max = std::numeric_limits<double>::lowest();
    for (const PosePoint& point : points) {
        if (!point.valid) {
            continue;
        }
        angle_min = std::min({angle_min, point.yaw_deg, point.pitch_deg, point.roll_deg});
        angle_max = std::max({angle_max, point.yaw_deg, point.pitch_deg, point.roll_deg});
    }
    if (!(angle_max > angle_min)) {
        angle_min = -1.0;
        angle_max = 1.0;
    }
    const auto x_at = [&](double t) {
        return kMargin + ((t - t_min) / (t_max - t_min)) * plot_width;
    };
    const auto y_at = [&](double angle) {
        return kMargin + ((angle_max - angle) / (angle_max - angle_min)) * plot_height;
    };
    const auto polyline = [&](int channel) {
        std::ostringstream out;
        bool started = false;
        for (const PosePoint& point : points) {
            if (!point.valid) {
                continue;
            }
            const double angle =
                channel == 0 ? point.yaw_deg : (channel == 1 ? point.pitch_deg : point.roll_deg);
            out << (started ? " " : "") << std::fixed << std::setprecision(2) << x_at(point.t_s)
                << "," << y_at(angle);
            started = true;
        }
        return out.str();
    };
    std::ofstream svg(path);
    if (!svg) {
        return false;
    }
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << static_cast<int>(kWidth)
        << "\" height=\"" << static_cast<int>(kHeight) << "\">\n";
    svg << "<rect width=\"100%\" height=\"100%\" fill=\"#101418\"/>\n";
    svg << "<rect x=\"" << kMargin << "\" y=\"" << kMargin << "\" width=\"" << plot_width
        << "\" height=\"" << plot_height << "\" fill=\"none\" stroke=\"#39424a\"/>\n";
    svg << "<text x=\"" << kMargin
        << "\" y=\"24\" fill=\"#e6edf3\" font-family=\"sans-serif\" "
           "font-size=\"14\">M0 pose trace (degrees) - yaw red, pitch green, roll blue</text>\n";
    svg << "<text x=\"" << kMargin << "\" y=\"" << kHeight - 12
        << "\" fill=\"#8b949e\" font-family=\"sans-serif\" font-size=\"11\">" << std::fixed
        << std::setprecision(1) << t_min << " s to " << t_max << " s; angle range " << angle_min
        << " to " << angle_max << " deg</text>\n";
    const std::array<const char*, 3> colours = {"#ff5c5c", "#5cd67a", "#5c9bff"};
    for (int channel = 0; channel < 3; ++channel) {
        svg << "<polyline fill=\"none\" stroke=\"" << colours[static_cast<std::size_t>(channel)]
            << "\" stroke-width=\"1.2\" points=\"" << polyline(channel) << "\"/>\n";
    }
    svg << "</svg>\n";
    return static_cast<bool>(svg);
}

// ---------------------------------------------------------------------------
// Resource sampling
// ---------------------------------------------------------------------------

struct ResourceSample {
    double t_s = 0.0;
    std::uint64_t rss_bytes = 0;
    std::uint32_t handle_count = 0;
    double cpu_percent_one_core = 0.0;
};

[[nodiscard]] json resource_samples_json(const std::vector<ResourceSample>& samples) {
    json array = json::array();
    for (const ResourceSample& sample : samples) {
        array.push_back(json{{"t_s", round_to(sample.t_s, 3)},
                             {"rss_bytes", sample.rss_bytes},
                             {"handle_count", sample.handle_count},
                             {"cpu_percent_one_core", round_to(sample.cpu_percent_one_core, 3)}});
    }
    return array;
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

struct PendingFrame {
    std::uint64_t timestamp_ns = 0;
    Clock::time_point submit_wall{};
};

struct RunCounters {
    std::uint64_t push_attempts = 0;
    std::uint64_t submitted = 0;
    std::uint64_t submit_errors = 0;
    std::uint64_t resolved = 0;
    std::uint64_t valid = 0;
    std::uint64_t features_valid = 0;
    std::uint64_t no_face = 0;
    std::uint64_t source_missed = 0;
    std::uint64_t unmatched_results = 0;
    std::uint64_t superseded = 0;
    std::uint64_t stalled = 0;
};

struct RunResult {
    json report;
    bool leak_passed = true;
};

[[nodiscard]] bool validate_distribution(const json& value, const std::string& name,
                                         std::string& error) {
    if (!value.is_object()) {
        error = name + " is not an object";
        return false;
    }
    for (const char* key : {"p50", "p95", "p99", "mean", "max", "min"}) {
        if (!value.contains(key) || !value[key].is_number()) {
            error = name + "." + key + " is missing or not a number";
            return false;
        }
        if (value[key].get<double>() < 0.0) {
            error = name + "." + key + " is negative";
            return false;
        }
    }
    if (!value.contains("samples") || !value["samples"].is_number_unsigned()) {
        error = name + ".samples is missing or not an unsigned integer";
        return false;
    }
    if (!(value["p50"].get<double>() <= value["p95"].get<double>() &&
          value["p95"].get<double>() <= value["p99"].get<double>())) {
        error = name + " percentiles are not ordered p50 <= p95 <= p99";
        return false;
    }
    return true;
}

[[nodiscard]] bool validate_result(const json& result, std::string& error) {
    if (!result.is_object()) {
        error = "result is not a JSON object";
        return false;
    }
    if (result.value("schema", "") != kSchema) {
        error = "schema is not " + std::string(kSchema);
        return false;
    }
    if (!result.contains("mode") || !result["mode"].is_string()) {
        error = "mode is missing";
        return false;
    }
    if (!result.contains("hardware") || !result["hardware"].is_object() ||
        result["hardware"].value("fingerprint", "").empty()) {
        error = "hardware fingerprint is missing";
        return false;
    }
    if (!result.contains("build") || !result["build"].is_object() ||
        result["build"].value("type", "").empty()) {
        error = "build is missing";
        return false;
    }
    if (!result.contains("config") || !result["config"].is_object() ||
        !result["config"].contains("input_size")) {
        error = "config.input_size is missing";
        return false;
    }
    if (!result.contains("source") || !result["source"].is_object() ||
        !result["source"].contains("kind")) {
        error = "source is missing";
        return false;
    }
    if (!result.contains("tracker") || !result["tracker"].is_object() ||
        !result["tracker"].contains("kind")) {
        error = "tracker is missing";
        return false;
    }
    if (!result.contains("metrics") || !result["metrics"].is_object()) {
        error = "metrics is missing";
        return false;
    }
    const json& metrics = result["metrics"];
    if (!validate_distribution(metrics.value("inference_ms", json{}), "metrics.inference_ms",
                               error)) {
        return false;
    }
    if (!validate_distribution(metrics.value("frame_age_ms", json{}), "metrics.frame_age_ms",
                               error)) {
        return false;
    }
    if (!metrics.contains("frames") || !metrics["frames"].is_object()) {
        error = "metrics.frames is missing";
        return false;
    }
    for (const char* key : {"submitted", "resolved", "valid", "features_valid", "no_face",
                            "source_missed", "timestamp_mismatch"}) {
        if (!metrics["frames"].contains(key) || !metrics["frames"][key].is_number_unsigned()) {
            error = std::string("metrics.frames.") + key + " is missing";
            return false;
        }
    }
    if (!metrics.contains("dropped") || !metrics["dropped"].is_object() ||
        !metrics["dropped"].contains("reasons") || !metrics["dropped"]["reasons"].is_object()) {
        error = "metrics.dropped.reasons is missing";
        return false;
    }
    for (const char* key : {"superseded", "stalled", "submit_error", "source_missed"}) {
        if (!metrics["dropped"]["reasons"].contains(key)) {
            error = std::string("metrics.dropped.reasons.") + key + " is missing";
            return false;
        }
    }
    if (!metrics.contains("confidence") || !metrics["confidence"].is_object() ||
        !metrics["confidence"].contains("bins") || !metrics["confidence"]["bins"].is_array() ||
        metrics["confidence"]["bins"].size() != tracking::ConfidenceDistribution::kBucketCount) {
        error = "metrics.confidence.bins is missing or has the wrong size";
        return false;
    }
    if (!metrics.contains("rss") || !metrics["rss"].is_object() ||
        !metrics["rss"].contains("samples") || !metrics["rss"]["samples"].is_array() ||
        metrics["rss"]["samples"].empty()) {
        error = "metrics.rss.samples is missing or empty";
        return false;
    }
    if (!metrics.contains("leak") || !metrics["leak"].is_object() ||
        !metrics["leak"].contains("passed") || !metrics["leak"]["passed"].is_boolean()) {
        error = "metrics.leak.passed is missing";
        return false;
    }
    if (!metrics.contains("cpu") || !metrics["cpu"].is_object() ||
        !metrics["cpu"].contains("thread_cpu") || !metrics["cpu"]["thread_cpu"].is_array()) {
        error = "metrics.cpu.thread_cpu is missing";
        return false;
    }
    if (!metrics.contains("pose_stability") || !metrics["pose_stability"].is_object()) {
        error = "metrics.pose_stability is missing";
        return false;
    }
    if (!result.contains("artifacts") || !result["artifacts"].is_object()) {
        error = "artifacts is missing";
        return false;
    }
    return true;
}

[[nodiscard]] RunResult run_harness(const Options& options, const HardwareInfo& hardware,
                                    const std::string& mode) {
    if (options.fps < 1) {
        throw std::runtime_error("--fps must be at least 1");
    }
    if (!(options.duration_s > 0.0)) {
        throw std::runtime_error("--duration-seconds must be positive");
    }
    if (options.warmup_s < 0.0) {
        throw std::runtime_error("--warmup-seconds must be non-negative");
    }
    if (!(options.rss_interval_s > 0.0)) {
        throw std::runtime_error("--rss-interval-seconds must be positive");
    }
    if (options.input_size != 192 && options.input_size != 256) {
        throw std::runtime_error("--input-size must be 192 or 256");
    }
    if (options.pattern_width < 2 || options.pattern_height < 2) {
        throw std::runtime_error("--pattern-width/--pattern-height must be at least 2");
    }
    if (options.source == SourceKind::replay && options.replay_path.empty()) {
        throw std::runtime_error("--source replay requires --replay PATH");
    }
    if (options.tracker == TrackerKind::real && options.tracker_dll.empty()) {
        throw std::runtime_error("--tracker real requires --tracker-dll PATH");
    }

    ReplayClip clip;
    PatternSource pattern(options.pattern_width, options.pattern_height);
    std::string error;

    if (options.source == SourceKind::replay) {
        if (!clip.load(options.replay_path, error)) {
            throw std::runtime_error(error);
        }
    } else if (!pattern.start(error)) {
        throw std::runtime_error(error);
    }

    std::unique_ptr<TrackerBackend> tracker;
    CabiBackend* real = nullptr;
    if (options.tracker == TrackerKind::real) {
        auto backend = std::make_unique<CabiBackend>();
        if (!backend->load(options.tracker_dll, error) ||
            !backend->initialize(options.input_size, error)) {
            throw std::runtime_error(error);
        }
        real = backend.get();
        tracker = std::move(backend);
    } else {
        auto backend = std::make_unique<ScriptedBackend>();
        if (!backend->initialize(options.input_size, error)) {
            throw std::runtime_error(error);
        }
        tracker = std::move(backend);
    }

    std::ofstream trace;
    if (!options.pose_trace.empty()) {
        const std::filesystem::path path(options.pose_trace);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        trace.open(path, std::ios::binary);
        if (!trace) {
            throw std::runtime_error("cannot write pose trace: " + options.pose_trace);
        }
        trace
            << "frame,t_s,source_timestamp_ns,valid,features_valid,confidence,qx,qy,qz,qw,tx,ty,tz,"
               "yaw_deg,pitch_deg,roll_deg\n";
    }

    const auto period = std::chrono::nanoseconds(1'000'000'000ll / options.fps);
    const auto duration_ns = static_cast<std::uint64_t>(options.duration_s * 1'000'000'000.0);
    const auto warmup_ns = static_cast<std::uint64_t>(options.warmup_s * 1'000'000'000.0);
    const auto rss_interval_ns =
        static_cast<std::uint64_t>(options.rss_interval_s * 1'000'000'000.0);

    RunCounters counters;
    std::deque<PendingFrame> pending;
    std::vector<double> inference_ms;
    std::vector<double> frame_age_ms;
    std::vector<double> pose_delta_deg;
    tracking::ConfidenceDistribution confidence;
    std::vector<ResourceSample> resource_samples;
    std::vector<PosePoint> pose_points;
    std::vector<ClipCandidate> clip_candidates;
    std::size_t clip_used = 0;
    bool clip_preallocated = false;
    std::uint64_t clip_stride = 1;
    std::uint64_t last_resolved_timestamp = 0;
    bool has_last_resolved = false;
    bool has_last_valid_pose = false;
    std::array<float, 4> last_valid_rotation{0.0f, 0.0f, 0.0f, 1.0f};
    double yaw_min = std::numeric_limits<double>::max();
    double yaw_max = std::numeric_limits<double>::lowest();
    double pitch_min = std::numeric_limits<double>::max();
    double pitch_max = std::numeric_limits<double>::lowest();
    double roll_min = std::numeric_limits<double>::max();
    double roll_max = std::numeric_limits<double>::lowest();
    std::uint64_t pose_samples = 0;
    std::uint64_t last_sample_cpu_ns = 0;
    std::uint64_t measure_start_cpu_ns = 0;
    Clock::time_point last_sample_wall{};
    Clock::time_point measure_start{};
    bool measuring = false;
    bool started_sampling = false;
    std::uint64_t frame_index = 0;
    std::uint64_t resolved_frame_index = 0;

    const auto record_resource_sample = [&]() {
        const Clock::time_point now = Clock::now();
        const std::uint64_t cpu = process_cpu_ns();
        const double elapsed_ms = ms_between(last_sample_wall, now);
        const double cpu_ms = static_cast<double>(cpu - last_sample_cpu_ns) / 1'000'000.0;
        ResourceSample sample;
        sample.t_s = ms_between(measure_start, now) / 1000.0;
        sample.rss_bytes = process_rss_bytes();
        sample.handle_count = process_handle_count();
        sample.cpu_percent_one_core = elapsed_ms > 0.0 ? (cpu_ms / elapsed_ms) * 100.0 : 0.0;
        resource_samples.push_back(sample);
        last_sample_cpu_ns = cpu;
        last_sample_wall = now;
    };

    const auto resolve = [&](const tracking::TrackingResult& result) {
        const std::uint64_t timestamp = result.source_timestamp_ns;
        auto it = pending.begin();
        while (it != pending.end() && it->timestamp_ns != timestamp) {
            ++it;
        }
        if (it == pending.end()) {
            ++counters.unmatched_results;
            return;
        }
        const Clock::time_point now = Clock::now();
        for (auto previous = pending.begin(); previous != it; ++previous) {
            ++counters.superseded;
        }
        const Clock::time_point submit_wall = it->submit_wall;
        const std::uint64_t submitted_timestamp = it->timestamp_ns;
        pending.erase(pending.begin(), it + 1);
        const double latency = ms_between(submit_wall, now);
        const double age = static_cast<double>(steady_now_ns() - submitted_timestamp) / 1'000'000.0;
        inference_ms.push_back(std::max(0.0, latency));
        frame_age_ms.push_back(std::max(0.0, age));
        ++counters.resolved;
        if (result.valid) {
            ++counters.valid;
        } else {
            ++counters.no_face;
        }
        if (result.features_valid) {
            ++counters.features_valid;
        }
        confidence.add(result.confidence);
        if (has_last_resolved) {
            last_resolved_timestamp = std::max(last_resolved_timestamp, timestamp);
        } else {
            last_resolved_timestamp = timestamp;
            has_last_resolved = true;
        }

        const std::array<double, 3> euler = euler_deg(result.pose.rotation);
        PosePoint point;
        point.t_s = ms_between(measure_start, now) / 1000.0;
        point.yaw_deg = euler[0];
        point.pitch_deg = euler[1];
        point.roll_deg = euler[2];
        point.valid = result.valid;
        if (point.valid) {
            yaw_min = std::min(yaw_min, point.yaw_deg);
            yaw_max = std::max(yaw_max, point.yaw_deg);
            pitch_min = std::min(pitch_min, point.pitch_deg);
            pitch_max = std::max(pitch_max, point.pitch_deg);
            roll_min = std::min(roll_min, point.roll_deg);
            roll_max = std::max(roll_max, point.roll_deg);
            ++pose_samples;
            if (has_last_valid_pose) {
                pose_delta_deg.push_back(
                    quaternion_delta_deg(last_valid_rotation, result.pose.rotation));
            }
            last_valid_rotation = result.pose.rotation;
            has_last_valid_pose = true;
        }
        if (pose_points.size() < 30000) {
            pose_points.push_back(point);
        }

        if (trace.is_open()) {
            trace << resolved_frame_index << "," << std::fixed << std::setprecision(3) << point.t_s
                  << "," << result.source_timestamp_ns << "," << (result.valid ? 1 : 0) << ","
                  << (result.features_valid ? 1 : 0) << "," << std::setprecision(6)
                  << result.confidence;
            for (const float value : result.pose.rotation) {
                trace << "," << value;
            }
            for (const float value : result.pose.translation) {
                trace << "," << value;
            }
            trace << "," << point.yaw_deg << "," << point.pitch_deg << "," << point.roll_deg
                  << "\n";
        }

        for (std::size_t index = 0; index < clip_used; ++index) {
            ClipCandidate& candidate = clip_candidates[index];
            if (candidate.timestamp_ns == timestamp) {
                candidate.has_result = true;
                candidate.valid = result.valid;
                candidate.features_valid = result.features_valid;
                candidate.confidence = result.confidence;
                candidate.landmarks = result.landmarks;
                break;
            }
        }
        ++resolved_frame_index;
    };

    const auto drain = [&]() {
        while (true) {
            std::optional<tracking::TrackingResult> result = tracker->poll();
            if (!result.has_value()) {
                break;
            }
            resolve(*result);
        }
    };

    if (options.fps > 0) {
#if defined(_WIN32)
        (void)timeBeginPeriod(1);
#endif
    }

    const Clock::time_point run_start = Clock::now();
    Clock::time_point next_frame = run_start;
    const std::uint64_t expected_measured_frames =
        std::max<std::uint64_t>(1, static_cast<std::uint64_t>(options.duration_s * options.fps));
    // Reserve every measurement buffer before the baseline RSS sample. A
    // reallocation during the measured window would look like a leak.
    inference_ms.reserve(
        static_cast<std::size_t>(std::min<std::uint64_t>(expected_measured_frames, 1'000'000)));
    frame_age_ms.reserve(
        static_cast<std::size_t>(std::min<std::uint64_t>(expected_measured_frames, 1'000'000)));
    pose_delta_deg.reserve(
        static_cast<std::size_t>(std::min<std::uint64_t>(expected_measured_frames, 1'000'000)));
    pose_points.reserve(
        static_cast<std::size_t>(std::min<std::uint64_t>(expected_measured_frames, 30'000)));
    resource_samples.reserve(64);
    if (options.clip_frames > 0) {
        clip_stride = std::max<std::uint64_t>(
            1, expected_measured_frames / static_cast<std::uint64_t>(options.clip_frames));
    }

    while (true) {
        const Clock::time_point loop_now = Clock::now();
        if (!measuring) {
            if (static_cast<std::uint64_t>(ms_between(run_start, loop_now) * 1'000'000.0) >=
                warmup_ns) {
                // Let any in-flight warmup result land, then start clean.
                drain();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                drain();
                pending.clear();
                counters = {};
                inference_ms.clear();
                frame_age_ms.clear();
                pose_delta_deg.clear();
                confidence = {};
                resource_samples.clear();
                pose_points.clear();
                clip_used = 0;
                resolved_frame_index = 0;
                has_last_resolved = false;
                has_last_valid_pose = false;
                yaw_min = std::numeric_limits<double>::max();
                yaw_max = std::numeric_limits<double>::lowest();
                pitch_min = std::numeric_limits<double>::max();
                pitch_max = std::numeric_limits<double>::lowest();
                roll_min = std::numeric_limits<double>::max();
                roll_max = std::numeric_limits<double>::lowest();
                pose_samples = 0;
                measuring = true;
                measure_start = Clock::now();
                next_frame = measure_start;
                last_sample_wall = measure_start;
                last_sample_cpu_ns = process_cpu_ns();
                measure_start_cpu_ns = last_sample_cpu_ns;
                started_sampling = true;
                record_resource_sample();
                continue;
            }
        } else if (static_cast<std::uint64_t>(ms_between(measure_start, loop_now) * 1'000'000.0) >=
                   duration_ns) {
            break;
        }

        if (loop_now < next_frame) {
            std::this_thread::sleep_until(next_frame);
        }
        next_frame += period;

        const std::uint64_t timestamp_ns = steady_now_ns();

        capture::VideoFrame frame;
        std::optional<capture::FrameLease> lease;
        if (options.source == SourceKind::replay) {
            clip.fill(frame_index, timestamp_ns, frame);
        } else {
            lease = pattern.acquire(timestamp_ns);
            if (!lease.has_value()) {
                if (measuring) {
                    ++counters.source_missed;
                }
                ++frame_index;
                continue;
            }
            frame = lease->frame();
            frame.timestamp_ns = timestamp_ns;
            frame.arrival_ns = timestamp_ns;
            frame.sequence = static_cast<std::uint32_t>(frame_index & 0xFFFFFFFFu);
        }

        if (options.clip_frames > 0 && !clip_preallocated) {
            // Allocate the review clip storage once, before the measured window
            // starts. Filling the slots later is a memcpy, not an allocation.
            clip_candidates.resize(static_cast<std::size_t>(options.clip_frames));
            for (ClipCandidate& candidate : clip_candidates) {
                candidate.plane0.resize(static_cast<std::size_t>(frame.stride(0)) * frame.height);
                if (frame.plane(1) != nullptr) {
                    candidate.plane1.resize(static_cast<std::size_t>(frame.stride(1)) *
                                            (frame.height / 2u));
                }
            }
            clip_preallocated = true;
        }
        if (measuring && options.clip_frames > 0 && clip_stride > 0 &&
            (frame_index % clip_stride) == 0 && clip_used < clip_candidates.size()) {
            ClipCandidate& candidate = clip_candidates[clip_used++];
            candidate.timestamp_ns = timestamp_ns;
            candidate.frame = frame;
            candidate.plane0_size = frame.stride(0) * frame.height;
            if (candidate.plane0.size() < candidate.plane0_size) {
                candidate.plane0.resize(candidate.plane0_size);
            }
            std::memcpy(candidate.plane0.data(), frame.plane(0), candidate.plane0_size);
            if (frame.plane(1) != nullptr) {
                candidate.plane1_size = frame.stride(1) * (frame.height / 2u);
                if (candidate.plane1.size() < candidate.plane1_size) {
                    candidate.plane1.resize(candidate.plane1_size);
                }
                std::memcpy(candidate.plane1.data(), frame.plane(1), candidate.plane1_size);
            } else {
                candidate.plane1_size = 0;
            }
            candidate.frame.planes[0] = candidate.plane0.data();
            candidate.frame.planes[1] =
                candidate.plane1.empty() ? nullptr : candidate.plane1.data();
            candidate.has_result = false;
            candidate.valid = false;
            candidate.features_valid = false;
            candidate.confidence = 0.0f;
            candidate.landmarks = {};
        }

        if (measuring) {
            ++counters.push_attempts;
        }
        const Clock::time_point submit_wall = Clock::now();
        const bool submitted = tracker->submit(frame, error);
        if (measuring) {
            if (submitted) {
                ++counters.submitted;
                pending.push_back({timestamp_ns, submit_wall});
            } else {
                ++counters.submit_errors;
            }
        }

        // Poll until the next frame is due. The sleep keeps the harness from
        // burning a core while the worker infers; the recorded latency
        // therefore includes at most one millisecond of poll granularity.
        const Clock::time_point poll_deadline = next_frame;
        while (Clock::now() < poll_deadline) {
            std::optional<tracking::TrackingResult> result = tracker->poll();
            if (result.has_value()) {
                if (measuring) {
                    resolve(*result);
                }
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (measuring && started_sampling) {
            const Clock::time_point sample_now = Clock::now();
            if (static_cast<std::uint64_t>(ms_between(last_sample_wall, sample_now) *
                                           1'000'000.0) >= rss_interval_ns) {
                record_resource_sample();
            }
        }

        ++frame_index;
    }

    // Drain in-flight results for a bounded grace period so the last frames are
    // counted instead of being misclassified as stalled.
    const Clock::time_point drain_deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < drain_deadline) {
        std::optional<tracking::TrackingResult> result = tracker->poll();
        if (result.has_value()) {
            if (measuring) {
                resolve(*result);
            }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (measuring) {
        record_resource_sample();
    }

    for (const PendingFrame& frame : pending) {
        if (has_last_resolved && last_resolved_timestamp > frame.timestamp_ns) {
            ++counters.superseded;
        } else {
            ++counters.stalled;
        }
    }

    const Clock::time_point measure_end = Clock::now();
    const std::uint64_t cpu_end_ns = process_cpu_ns();
    const double measured_wall_ms = ms_between(measure_start, measure_end);
    const double total_cpu_ms =
        static_cast<double>(cpu_end_ns - measure_start_cpu_ns) / 1'000'000.0;
    const std::vector<ThreadCpuSample> thread_cpu = collect_thread_cpu();

#if defined(_WIN32)
    if (options.fps > 0) {
        (void)timeEndPeriod(1);
    }
#endif

    // Leak check: RSS growth from the first post-warmup sample to the last.
    bool rss_passed = true;
    double rss_growth_percent = 0.0;
    std::uint64_t baseline_rss = 0;
    std::uint64_t final_rss = 0;
    if (!resource_samples.empty()) {
        baseline_rss = resource_samples.front().rss_bytes;
        final_rss = resource_samples.back().rss_bytes;
        if (baseline_rss > 0) {
            rss_growth_percent =
                (static_cast<double>(final_rss) - static_cast<double>(baseline_rss)) /
                static_cast<double>(baseline_rss) * 100.0;
        }
        rss_passed = rss_growth_percent <= 5.0;
    }
    bool handle_passed = true;
    bool handle_monotonic = false;
    std::uint32_t baseline_handles = 0;
    std::uint32_t final_handles = 0;
    if (!resource_samples.empty()) {
        baseline_handles = resource_samples.front().handle_count;
        final_handles = resource_samples.back().handle_count;
        bool strictly_increasing = resource_samples.size() >= 5;
        for (std::size_t index = 1; index < resource_samples.size(); ++index) {
            if (resource_samples[index].handle_count <= resource_samples[index - 1].handle_count) {
                strictly_increasing = false;
                break;
            }
        }
        handle_monotonic = strictly_increasing && final_handles > baseline_handles;
        handle_passed = !handle_monotonic;
    }
    const bool leak_passed = rss_passed && handle_passed;

    // Clip and pose chart are written after the measured window so converting
    // and encoding cannot delay a submitted frame.
    json artifact_clip_frames = json::array();
    std::string clip_error;
    if (options.clip_frames > 0 && clip_used > 0) {
        const std::filesystem::path clip_directory(options.clip_dir);
        if (!clip_directory.empty()) {
            std::error_code code;
            std::filesystem::create_directories(clip_directory, code);
            if (code) {
                clip_error = "cannot create clip directory: " + code.message();
            } else {
                const std::vector<OverlayLandmark> overlay =
                    overlay_landmarks(options.tracker == TrackerKind::real);
                std::uint64_t index = 0;
                for (std::size_t candidate_index = 0; candidate_index < clip_used;
                     ++candidate_index) {
                    const ClipCandidate& candidate = clip_candidates[candidate_index];
                    std::vector<std::uint8_t> rgb;
                    if (!frame_to_rgb(candidate.frame, rgb)) {
                        continue;
                    }
                    const std::uint32_t target_width =
                        std::min<std::uint32_t>(320, candidate.frame.width);
                    const std::uint32_t target_height = std::max<std::uint32_t>(
                        1, (target_width * candidate.frame.height) / candidate.frame.width);
                    std::vector<std::uint8_t> scaled;
                    downscale_rgb(rgb, candidate.frame.width, candidate.frame.height, target_width,
                                  target_height, scaled);
                    if (candidate.has_result) {
                        for (const OverlayLandmark& landmark : overlay) {
                            if (landmark.index >= candidate.landmarks.count) {
                                continue;
                            }
                            const tracking::Landmark& point =
                                candidate.landmarks.points[landmark.index];
                            draw_dot(scaled, target_width, target_height, point.x, point.y,
                                     landmark.r, landmark.g, landmark.b);
                        }
                    }
                    char name[64] = {};
                    std::snprintf(name, sizeof(name), "frame-%04llu.bmp",
                                  static_cast<unsigned long long>(index));
                    const std::filesystem::path file = clip_directory / name;
                    if (!write_bmp(file, target_width, target_height, scaled)) {
                        clip_error = "cannot write clip frame: " + path_text(file);
                        break;
                    }
                    artifact_clip_frames.push_back(
                        json{{"index", index},
                             {"file", path_text(file)},
                             {"source_timestamp_ns", candidate.timestamp_ns},
                             {"valid", candidate.valid},
                             {"features_valid", candidate.features_valid},
                             {"confidence", round_to(candidate.confidence, 6)}});
                    ++index;
                }
            }
        }
    }
    std::string pose_svg_path;
    if (!options.pose_trace.empty()) {
        std::filesystem::path svg = options.pose_trace;
        svg.replace_extension(".svg");
        if (write_pose_svg(svg, pose_points)) {
            pose_svg_path = path_text(svg);
        }
    }

    const Distribution inference = summarize(std::move(inference_ms));
    const Distribution frame_age = summarize(std::move(frame_age_ms));
    const Distribution pose_delta = summarize(std::move(pose_delta_deg));

    json confidence_bins = json::array();
    for (std::size_t index = 0; index < tracking::ConfidenceDistribution::kBucketCount; ++index) {
        confidence_bins.push_back(confidence.bucket(index));
    }

    json thread_cpu_json = json::array();
    for (const ThreadCpuSample& sample : thread_cpu) {
        thread_cpu_json.push_back(
            json{{"thread_id", sample.thread_id}, {"cpu_ms", round_to(sample.cpu_ms, 3)}});
    }

    const std::uint64_t dropped_total =
        counters.submit_errors + (counters.submitted - counters.resolved);
    const double dropped_ratio =
        counters.push_attempts > 0
            ? static_cast<double>(dropped_total) / static_cast<double>(counters.push_attempts)
            : 0.0;

    const bool synthetic = tracker->synthetic();
    json report;
    report["schema"] = kSchema;
    report["generated_utc"] = utc_now_iso8601();
    report["mode"] = mode;
    report["status"] = (leak_passed || !options.strict) ? "ok" : "fail";
    report["build"] = build_json();
    report["hardware"] = hardware_json(hardware);
    report["config"] = json{{"input_size", options.input_size},
                            {"duration_s", round_to(options.duration_s, 3)},
                            {"warmup_s", round_to(options.warmup_s, 3)},
                            {"target_fps", options.fps},
                            {"rss_interval_s", round_to(options.rss_interval_s, 3)},
                            {"strict", options.strict},
                            {"clip_frames_requested", options.clip_frames}};
    report["source"] =
        json{{"kind", source_name(options.source)},
             {"path", options.source == SourceKind::replay ? options.replay_path : std::string()},
             {"width", options.source == SourceKind::replay ? 0u : options.pattern_width},
             {"height", options.source == SourceKind::replay ? 0u : options.pattern_height},
             {"format", options.source == SourceKind::replay ? "clip" : "bgra8"},
             {"frames_available", options.source == SourceKind::replay ? clip.size() : 0},
             {"synthetic", options.source == SourceKind::pattern}};
    report["tracker"] = json{{"kind", tracker->kind()},
                             {"dll", real != nullptr ? real->dll_path() : std::string()},
                             {"input_width", options.input_size},
                             {"input_height", options.input_size},
                             {"synthetic", synthetic}};
    json reasons;
    reasons["superseded"] = counters.superseded;
    reasons["stalled"] = counters.stalled;
    reasons["submit_error"] = counters.submit_errors;
    reasons["source_missed"] = counters.source_missed;
    report["metrics"] = json{
        {"frames", json{{"push_attempts", counters.push_attempts},
                        {"submitted", counters.submitted},
                        {"resolved", counters.resolved},
                        {"valid", counters.valid},
                        {"features_valid", counters.features_valid},
                        {"no_face", counters.no_face},
                        {"source_missed", counters.source_missed},
                        {"timestamp_mismatch", counters.unmatched_results},
                        {"unmatched_results", counters.unmatched_results}}},
        {"inference_ms", distribution_json(inference)},
        {"frame_age_ms", distribution_json(frame_age)},
        {"dropped", json{{"total", dropped_total},
                         {"ratio", round_to(dropped_ratio, 6)},
                         {"reasons", reasons}}},
        {"confidence", json{{"bins", confidence_bins},
                            {"samples", confidence.samples},
                            {"mean", round_to(confidence.mean(), 6)}}},
        {"rss", json{{"baseline_bytes", baseline_rss},
                     {"final_bytes", final_rss},
                     {"growth_percent", round_to(rss_growth_percent, 4)},
                     {"peak_bytes",
                      [&]() {
                          std::uint64_t peak = 0;
                          for (const ResourceSample& sample : resource_samples) {
                              peak = std::max(peak, sample.rss_bytes);
                          }
                          return peak;
                      }()},
                     {"samples", resource_samples_json(resource_samples)}}},
        {"leak", json{{"threshold_percent", 5.0},
                      {"rss_growth_percent", round_to(rss_growth_percent, 4)},
                      {"rss_passed", rss_passed},
                      {"handle_baseline", baseline_handles},
                      {"handle_final", final_handles},
                      {"handle_growth", static_cast<std::int64_t>(final_handles) -
                                            static_cast<std::int64_t>(baseline_handles)},
                      {"handle_monotonic_growth", handle_monotonic},
                      {"handle_passed", handle_passed},
                      {"passed", leak_passed}}},
        {"cpu", json{{"process_percent_one_core",
                      round_to(measured_wall_ms > 0.0
                                   ? (static_cast<double>(cpu_end_ns) / 1'000'000.0 -
                                      static_cast<double>(measure_start_cpu_ns) / 1'000'000.0) /
                                         measured_wall_ms * 100.0
                                   : 0.0,
                               3)},
                     {"total_cpu_ms", round_to(total_cpu_ms, 3)},
                     {"thread_count", thread_cpu.size()},
                     {"thread_cpu", thread_cpu_json}}},
        {"pose_stability",
         json{{"samples", pose_samples},
              {"frame_delta_deg", distribution_json(pose_delta)},
              {"yaw_range_deg", pose_samples > 0 ? round_to(yaw_max - yaw_min, 3) : 0.0},
              {"pitch_range_deg", pose_samples > 0 ? round_to(pitch_max - pitch_min, 3) : 0.0},
              {"roll_range_deg", pose_samples > 0 ? round_to(roll_max - roll_min, 3) : 0.0}}}};
    report["artifacts"] = json{{"pose_trace", options.pose_trace},
                               {"pose_svg", pose_svg_path},
                               {"clip_dir", options.clip_dir},
                               {"clip_frames", artifact_clip_frames},
                               {"clip_error", clip_error}};
    std::vector<std::string> notes;
    if (synthetic) {
        notes.emplace_back(
            "tracker=scripted: this result is synthetic and is not tracker evidence; "
            "it exercises the harness only.");
    }
    if (options.source == SourceKind::pattern) {
        notes.emplace_back(
            "source=pattern: the test card contains no face; valid/no-face counts are "
            "not tracker quality evidence.");
    } else {
        notes.emplace_back(
            "source=replay: the clip loops; the pose discontinuity at each loop boundary is "
            "source content, not tracker jitter.");
    }
    notes.emplace_back(
        "inference_ms is the caller-visible submit-to-poll latency, not a vendor-internal "
        "graph timer; ABI v1 has no statistics query.");
    notes.emplace_back(
        "dropped frames are classified after the run: superseded means a newer result "
        "arrived, stalled means no later result arrived, submit_error means push_frame failed.");
    notes.emplace_back(
        "frame_age_ms is poll time minus the frame's source timestamp; it includes polling "
        "granularity (<= 1 ms).");
    report["notes"] = notes;

    if (options.strict && !leak_passed) {
        report["status"] = "fail";
    }

    // Record the real source dimensions now that the clip is loaded.
    if (options.source == SourceKind::replay) {
        capture::VideoFrame probe;
        clip.fill(0, 0, probe);
        report["source"]["width"] = probe.width;
        report["source"]["height"] = probe.height;
        report["source"]["format"] = capture::pixel_format_name(probe.format);
        report["source"]["synthetic"] = false;
    }

    RunResult result;
    result.report = std::move(report);
    result.leak_passed = leak_passed;

    if (!options.output.empty()) {
        const std::filesystem::path path(options.output);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("cannot write output: " + options.output);
        }
        output << result.report.dump(2) << "\n";
        if (!output) {
            throw std::runtime_error("write failed for: " + options.output);
        }
    } else {
        std::printf("%s\n", result.report.dump(2).c_str());
    }
    if (!options.quiet) {
        std::fprintf(stderr,
                     "avatar_m0: input=%u tracker=%s source=%s measured=%llu "
                     "inference_p50=%.3f p95=%.3f p99=%.3f ms frame_age_p95=%.3f ms "
                     "dropped=%llu (%.3f%%) rss_growth=%.2f%% handles=%u->%u leak=%s\n",
                     options.input_size, tracker->kind().c_str(), source_name(options.source),
                     static_cast<unsigned long long>(counters.resolved), inference.p50,
                     inference.p95, inference.p99, frame_age.p95,
                     static_cast<unsigned long long>(dropped_total), dropped_ratio * 100.0,
                     rss_growth_percent, baseline_handles, final_handles,
                     leak_passed ? "pass" : "FAIL");
    }

    tracker->shutdown();
    return result;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

[[nodiscard]] bool write_test_clip(const std::filesystem::path& path, std::string& error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot write self-test clip";
        return false;
    }
    out.write(kClipMagic, static_cast<std::streamsize>(kClipMagicSize));
    const auto write = [&out](const auto& value) {
        out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    for (std::uint32_t index = 0; index < 2; ++index) {
        const std::uint64_t timestamp = static_cast<std::uint64_t>(index) * 33'333'333ull;
        const std::uint32_t sequence = index;
        const std::uint32_t width = 4;
        const std::uint32_t height = 4;
        const std::uint32_t format = AVATAR_TRACKER_PIXEL_FORMAT_NV12;
        const std::uint32_t stride = 4;
        const std::uint32_t plane_count = 2;
        const std::uint64_t plane0_size = 16;
        const std::uint64_t plane1_size = 8;
        write(timestamp);
        write(sequence);
        write(width);
        write(height);
        write(format);
        write(stride);
        write(plane_count);
        write(plane0_size);
        write(plane1_size);
        for (std::uint32_t value = 0; value < 16; ++value) {
            out.put(static_cast<char>(value * 16u));
        }
        for (std::uint32_t value = 0; value < 8; ++value) {
            out.put(static_cast<char>(128));
        }
    }
    return static_cast<bool>(out);
}

[[nodiscard]] int run_self_test() {
    bool failed = false;
    std::error_code code;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path(code) / "avatar_m0_selftest";
    std::filesystem::remove_all(directory, code);
    std::filesystem::create_directories(directory, code);
    if (code) {
        std::fprintf(stderr, "avatar_m0 self-test: cannot create temp directory: %s\n",
                     code.message().c_str());
        return 1;
    }

    const std::filesystem::path clip_path = directory / "tiny.avclip";
    std::string error;
    if (!write_test_clip(clip_path, error)) {
        std::fprintf(stderr, "avatar_m0 self-test: %s\n", error.c_str());
        return 1;
    }
    ReplayClip clip;
    if (!clip.load(clip_path, error) || clip.size() != 2) {
        std::fprintf(stderr, "avatar_m0 self-test: replay clip round-trip failed: %s\n",
                     error.c_str());
        failed = true;
    } else {
        std::printf("avatar_m0 self-test: replay clip round-trip OK\n");
    }

    const std::array<SourceKind, 2> sources = {SourceKind::pattern, SourceKind::replay};
    for (const SourceKind source : sources) {
        Options options;
        options.source = source;
        options.replay_path = path_text(clip_path);
        options.tracker = TrackerKind::scripted;
        options.input_size = 192;
        options.duration_s = 0.3;
        options.warmup_s = 0.0;
        options.rss_interval_s = 0.1;
        options.fps = 30;
        options.pattern_width = 64;
        options.pattern_height = 48;
        options.clip_frames = 2;
        options.output = path_text(directory / (std::string(source_name(source)) + ".json"));
        options.pose_trace =
            path_text(directory / (std::string(source_name(source)) + "-pose.csv"));
        options.clip_dir = path_text(directory / (std::string(source_name(source)) + "-clip"));
        options.quiet = true;
        try {
            const RunResult result = run_harness(options, collect_hardware(), "selftest");
            std::string validation_error;
            if (!validate_result(result.report, validation_error)) {
                std::fprintf(stderr, "avatar_m0 self-test: %s result failed validation: %s\n",
                             source_name(source), validation_error.c_str());
                failed = true;
                continue;
            }
            if (!std::filesystem::exists(options.output)) {
                std::fprintf(stderr, "avatar_m0 self-test: %s did not write JSON\n",
                             source_name(source));
                failed = true;
                continue;
            }
            std::printf("avatar_m0 self-test: %s OK (resolved=%llu p95=%.3f ms)\n",
                        source_name(source),
                        static_cast<unsigned long long>(
                            result.report["metrics"]["frames"]["resolved"].get<std::uint64_t>()),
                        result.report["metrics"]["inference_ms"]["p95"].get<double>());
        } catch (const std::exception& exception) {
            std::fprintf(stderr, "avatar_m0 self-test: %s threw: %s\n", source_name(source),
                         exception.what());
            failed = true;
        }
    }

    std::filesystem::remove_all(directory, code);
    if (failed) {
        std::fprintf(stderr, "avatar_m0 self-test: FAIL\n");
        return 1;
    }
    std::printf("avatar_m0 self-test: PASS\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

void print_usage() {
    std::printf("avatar_m0 - Avatar Studio headless M0 feasibility harness\n"
                "\n"
                "Options:\n"
                "  --source pattern|replay              frame source (default pattern)\n"
                "  --replay PATH                        AVCLIP1 clip for --source replay\n"
                "  --tracker scripted|real              tracker backend (default scripted)\n"
                "  --tracker-dll PATH                   libavatartracker.dll for --tracker real\n"
                "  --input-size 192|256                 prepared tracker input (default 192)\n"
                "  --duration-seconds N                 measured duration (default 60)\n"
                "  --warmup-seconds N                   discarded warmup (default 5)\n"
                "  --fps N                              submission cadence (default 30)\n"
                "  --rss-interval-seconds N             resource sample interval (default 5)\n"
                "  --pattern-width N --pattern-height N pattern source size (default 640x480)\n"
                "  --clip-frames N                      BMP frames for human review (default 48)\n"
                "  --output PATH                        write JSON to PATH instead of stdout\n"
                "  --pose-trace PATH                    write the pose CSV (and .svg chart)\n"
                "  --clip-dir PATH                      write the review BMP clip here\n"
                "  --strict                             exit 4 when the leak check fails\n"
                "  --quiet                              suppress the summary line\n"
                "  --self-test                          tiny scripted self-test\n"
                "  --help                               this text\n"
                "\n"
                "Exit codes: 0 ok, 1 runtime/IO failure, 2 usage, 3 backend unavailable, "
                "4 strict leak failure.\n");
}

[[nodiscard]] int parse_int(const std::string& name, const std::string& value) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) {
            throw std::runtime_error("bad integer");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error("invalid value for " + name + ": '" + value + "'");
    }
}

[[nodiscard]] double parse_double(const std::string& name, const std::string& value) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed != value.size()) {
            throw std::runtime_error("bad number");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error("invalid value for " + name + ": '" + value + "'");
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    std::string mode = "short";
    bool self_test = false;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--help" || argument == "-h") {
                print_usage();
                return 0;
            }
            if (argument == "--self-test") {
                self_test = true;
                continue;
            }
            if (argument == "--quiet") {
                options.quiet = true;
                continue;
            }
            if (argument == "--strict") {
                options.strict = true;
                continue;
            }
            if (argument.rfind("--", 0) != 0) {
                std::fprintf(stderr, "avatar_m0: unexpected argument '%s'\n", argument.c_str());
                return 2;
            }
            const std::size_t equals = argument.find('=');
            const std::string key =
                equals == std::string::npos ? argument : argument.substr(0, equals);
            const auto value = [&]() -> std::string {
                if (equals != std::string::npos) {
                    return argument.substr(equals + 1);
                }
                if (index + 1 >= argc) {
                    throw std::runtime_error("missing value for " + key);
                }
                return argv[++index];
            };

            if (key == "--source") {
                const std::string source = value();
                if (source == "pattern") {
                    options.source = SourceKind::pattern;
                } else if (source == "replay") {
                    options.source = SourceKind::replay;
                } else {
                    throw std::runtime_error("unknown --source '" + source + "'");
                }
            } else if (key == "--replay") {
                options.replay_path = value();
            } else if (key == "--tracker") {
                const std::string tracker = value();
                if (tracker == "scripted") {
                    options.tracker = TrackerKind::scripted;
                } else if (tracker == "real") {
                    options.tracker = TrackerKind::real;
                } else {
                    throw std::runtime_error("unknown --tracker '" + tracker + "'");
                }
            } else if (key == "--tracker-dll") {
                options.tracker_dll = value();
            } else if (key == "--input-size") {
                options.input_size = static_cast<std::uint32_t>(parse_int(key, value()));
            } else if (key == "--duration-seconds") {
                options.duration_s = parse_double(key, value());
            } else if (key == "--warmup-seconds") {
                options.warmup_s = parse_double(key, value());
            } else if (key == "--rss-interval-seconds") {
                options.rss_interval_s = parse_double(key, value());
            } else if (key == "--fps") {
                options.fps = parse_int(key, value());
            } else if (key == "--pattern-width") {
                options.pattern_width = static_cast<std::uint32_t>(parse_int(key, value()));
            } else if (key == "--pattern-height") {
                options.pattern_height = static_cast<std::uint32_t>(parse_int(key, value()));
            } else if (key == "--clip-frames") {
                options.clip_frames = parse_int(key, value());
            } else if (key == "--output") {
                options.output = value();
            } else if (key == "--pose-trace") {
                options.pose_trace = value();
            } else if (key == "--clip-dir") {
                options.clip_dir = value();
            } else if (key == "--mode") {
                mode = value();
            } else {
                std::fprintf(stderr, "avatar_m0: unknown option '%s'\n", key.c_str());
                return 2;
            }
        }
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "avatar_m0: %s\n", exception.what());
        return 2;
    }

    if (self_test) {
        return run_self_test();
    }

    if (options.tracker == TrackerKind::real && options.tracker_dll.empty()) {
        std::fprintf(stderr, "avatar_m0: --tracker real requires --tracker-dll "
                             "(libavatartracker.dll)\n");
        return 3;
    }

    try {
        const RunResult result = run_harness(options, collect_hardware(), mode);
        if (options.strict && !result.leak_passed) {
            return 4;
        }
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "avatar_m0: %s\n", exception.what());
        return 1;
    }
    return 0;
}
