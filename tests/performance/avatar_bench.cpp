// avatar_bench - repeatable pipeline benchmark for Avatar Studio.
//
// The harness drives the deterministic capture source through the ring, the
// scripted tracker and a bench-local rig stage. It is deliberately
// framework-free and synchronous: the real render thread, capture callbacks and
// transport queues arrive in later tasks (T10/T11/T26/T46). Every scenario emits
// the same JSON shape so check.mjs and later gates do not need a new parser.
//
// Scenarios:
//   capture  test pattern render + ring publish
//   ring     capture + acquire/pin + sampled pixel read + release
//   rig      ring + scripted tracker + bench-local rig smoothing/matrix compose
//   render   rig + CPU compositing/readback proxy (real GPU render is T26)
//
// Usage: avatar_bench --help

#include "avatar/capture/frame_ring.h"
#include "avatar/capture/icamera_source.h"
#include "avatar/capture/test_pattern_source.h"
#include "avatar/capture/video_frame.h"
#include "avatar/core/clock.h"
#include "avatar/tracking/scripted_tracker.h"
#include "avatar/tracking/tracking_result.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
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
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <timeapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sys/types.h>
#include <unistd.h>

#include <set>
#endif

#ifndef AVATAR_BENCH_BUILD_TYPE
#define AVATAR_BENCH_BUILD_TYPE "unknown"
#endif
#ifndef AVATAR_BENCH_GIT_COMMIT
#define AVATAR_BENCH_GIT_COMMIT "unknown"
#endif
#ifndef AVATAR_BENCH_GIT_DIRTY
#define AVATAR_BENCH_GIT_DIRTY 0
#endif
#ifndef AVATAR_BENCH_COMPILER
#define AVATAR_BENCH_COMPILER "unknown"
#endif

namespace {

using namespace avatar;

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr const char* kSchema = "avatar_bench.result.v1";
constexpr const char* kBaselineSchema = "avatar_bench.baseline.v1";

enum class Scenario { capture, ring, rig, render };
enum class TrackerKind { scripted, real };

struct Options {
    Scenario scenario = Scenario::rig;
    TrackerKind tracker = TrackerKind::scripted;
    int frames = 300;
    int warmup = 30;
    int width = 1280;
    int height = 720;
    int fps = 60;
    std::string format = "bgra8";
    std::string output;
    std::string tracker_scenario = "valid_tracking";
    bool quiet = false;
};

[[nodiscard]] const char* scenario_name(Scenario scenario) noexcept {
    switch (scenario) {
        case Scenario::capture:
            return "capture";
        case Scenario::ring:
            return "ring";
        case Scenario::rig:
            return "rig";
        case Scenario::render:
            return "render";
    }
    return "unknown";
}

[[nodiscard]] const char* tracker_name(TrackerKind tracker) noexcept {
    return tracker == TrackerKind::real ? "real" : "scripted";
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

[[nodiscard]] double round_to(double value, int decimals) {
    const double factor = std::pow(10.0, static_cast<double>(decimals));
    return std::round(value * factor) / factor;
}

[[nodiscard]] double ms_between(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// ---------------------------------------------------------------------------
// Hardware and process fingerprint
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

    // Windows 11 still reports "Windows 10" in ProductName on some builds.
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
    auto to_ns = [](const FILETIME& time) -> std::uint64_t {
        ULARGE_INTEGER value{};
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return static_cast<std::uint64_t>(value.QuadPart) * 100ull;
    };
    return to_ns(kernel) + to_ns(user);
}

class GpuMonitor {
public:
    GpuMonitor() {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            query_ = nullptr;
            return;
        }
        if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0,
                                  &counter_) != ERROR_SUCCESS) {
            PdhCloseQuery(query_);
            query_ = nullptr;
            counter_ = nullptr;
        }
    }

    GpuMonitor(const GpuMonitor&) = delete;
    GpuMonitor& operator=(const GpuMonitor&) = delete;

    ~GpuMonitor() {
        if (query_ != nullptr) {
            PdhCloseQuery(query_);
        }
    }

    void start() {
        if (query_ == nullptr) {
            return;
        }
        started_ = PdhCollectQueryData(query_) == ERROR_SUCCESS;
    }

    [[nodiscard]] std::optional<double> stop() {
        if (query_ == nullptr || !started_) {
            return std::nullopt;
        }
        if (PdhCollectQueryData(query_) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        DWORD size = 0;
        DWORD count = 0;
        PDH_STATUS status =
            PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &size, &count, nullptr);
        if (status != PDH_MORE_DATA || size == 0) {
            return std::nullopt;
        }
        std::vector<unsigned char> buffer(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &size, &count, items);
        if (status != ERROR_SUCCESS) {
            return std::nullopt;
        }
        const std::wstring process_tag = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
        double total = 0.0;
        bool found = false;
        for (DWORD index = 0; index < count; ++index) {
            if (items[index].szName == nullptr ||
                std::wcsstr(items[index].szName, process_tag.c_str()) == nullptr) {
                continue;
            }
            const DWORD code = items[index].FmtValue.CStatus;
            if (code == PDH_CSTATUS_VALID_DATA || code == PDH_CSTATUS_NEW_DATA) {
                total += items[index].FmtValue.doubleValue;
                found = true;
            }
        }
        if (!found) {
            return 0.0;
        }
        return std::min(total, 100.0);
    }

private:
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
    bool started_ = false;
};

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
    std::ifstream file("/proc/cpuinfo");
    std::set<std::pair<int, int>> cores;
    std::string line;
    int physical = -1;
    int core = -1;
    while (std::getline(file, line)) {
        if (line.empty()) {
            if (physical >= 0 && core >= 0) {
                cores.emplace(physical, core);
            }
            physical = -1;
            core = -1;
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, colon);
        const std::string value = line.substr(colon + 1);
        if (key.find("physical id") != std::string::npos) {
            physical = std::stoi(value);
        } else if (key.find("core id") != std::string::npos) {
            core = std::stoi(value);
        }
    }
    if (!cores.empty()) {
        return static_cast<int>(cores.size());
    }
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
    std::ifstream file("/sys/class/drm/card0/device/uevent");
    std::string line;
    while (std::getline(file, line)) {
        constexpr std::string_view prefix = "DRIVER=";
        if (line.rfind(prefix.data(), 0) == 0) {
            return line.substr(prefix.size()) + " (DRM)";
        }
    }
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

class GpuMonitor {
public:
    void start() {}
    [[nodiscard]] std::optional<double> stop() { return std::nullopt; }
};

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

class GpuMonitor {
public:
    void start() {}
    [[nodiscard]] std::optional<double> stop() { return std::nullopt; }
};

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

class GpuMonitor {
public:
    void start() {}
    [[nodiscard]] std::optional<double> stop() { return std::nullopt; }
};

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

struct HardwareInfo {
    std::string os;
    std::string arch;
    std::string cpu;
    int physical_cores = 0;
    int logical_cores = 0;
    std::uint64_t memory_bytes = 0;
    std::string gpu;
    std::string fingerprint;
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
                {"fingerprint", info.fingerprint}};
}

// ---------------------------------------------------------------------------
// Git and build metadata
// ---------------------------------------------------------------------------

struct GitInfo {
    std::string commit = AVATAR_BENCH_GIT_COMMIT;
    bool dirty = AVATAR_BENCH_GIT_DIRTY != 0;
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
            const std::string prefix = "git -C " + shell_quote(root.string());
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
    return json{{"type", AVATAR_BENCH_BUILD_TYPE},
                {"git_commit", git.commit},
                {"git_dirty", git.dirty},
                {"compiler", AVATAR_BENCH_COMPILER}};
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
    std::size_t samples = 0;
};

[[nodiscard]] Distribution summarize(std::vector<double> values) {
    Distribution result;
    result.samples = values.size();
    if (values.empty()) {
        return result;
    }
    std::sort(values.begin(), values.end());
    const auto percentile = [&values](double percent) -> double {
        const double rank = std::ceil((percent / 100.0) * static_cast<double>(values.size())) - 1.0;
        const std::size_t index =
            static_cast<std::size_t>(std::clamp(rank, 0.0, static_cast<double>(values.size() - 1)));
        return values[index];
    };
    double total = 0.0;
    for (const double value : values) {
        total += value;
    }
    result.p50 = percentile(50.0);
    result.p95 = percentile(95.0);
    result.p99 = percentile(99.0);
    result.mean = total / static_cast<double>(values.size());
    result.max = values.back();
    return result;
}

[[nodiscard]] json distribution_json(const Distribution& distribution) {
    return json{{"p50", round_to(distribution.p50, 4)}, {"p95", round_to(distribution.p95, 4)},
                {"p99", round_to(distribution.p99, 4)}, {"mean", round_to(distribution.mean, 4)},
                {"max", round_to(distribution.max, 4)}, {"samples", distribution.samples}};
}

// ---------------------------------------------------------------------------
// Bench-local stages
// ---------------------------------------------------------------------------

// Stand-in for the real FaceRig (native/rig, T18). It does the same shape of
// work the rig will do per frame: smooth the pose quaternion, compose a
// head-to-model matrix and transform the landmark set. It is deliberately
// small; the dominant foundation cost is the capture source render.
struct RigStage {
    std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> translation{0.0f, 0.0f, 0.0f};
    std::array<float, 16> matrix{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    double sink = 0.0;

    void step(const tracking::TrackingResult& result) noexcept {
        if (!result.valid) {
            return;
        }
        constexpr float kSmoothing = 0.35f;
        for (std::size_t index = 0; index < rotation.size(); ++index) {
            rotation[index] += kSmoothing * (result.pose.rotation[index] - rotation[index]);
        }
        for (std::size_t index = 0; index < translation.size(); ++index) {
            translation[index] +=
                kSmoothing * (result.pose.translation[index] - translation[index]);
        }
        const float length = std::sqrt(rotation[0] * rotation[0] + rotation[1] * rotation[1] +
                                       rotation[2] * rotation[2] + rotation[3] * rotation[3]);
        if (length > 1e-6f) {
            for (float& value : rotation) {
                value /= length;
            }
        }
        const float x = rotation[0];
        const float y = rotation[1];
        const float z = rotation[2];
        const float w = rotation[3];
        matrix = {1.0f - 2.0f * (y * y + z * z),
                  2.0f * (x * y - z * w),
                  2.0f * (x * z + y * w),
                  translation[0],
                  2.0f * (x * y + z * w),
                  1.0f - 2.0f * (x * x + z * z),
                  2.0f * (y * z - x * w),
                  translation[1],
                  2.0f * (x * z - y * w),
                  2.0f * (y * z + x * w),
                  1.0f - 2.0f * (x * x + y * y),
                  translation[2],
                  0.0f,
                  0.0f,
                  0.0f,
                  1.0f};

        const auto* points = result.landmarks.points.data();
        const std::size_t count = std::min<std::size_t>(result.landmarks.count, 64);
        for (std::size_t index = 0; index < count; ++index) {
            const float px = points[index].x;
            const float py = points[index].y;
            const float pz = points[index].z;
            sink +=
                static_cast<double>(matrix[0] * px + matrix[1] * py + matrix[2] * pz + matrix[3]);
            sink +=
                static_cast<double>(matrix[4] * px + matrix[5] * py + matrix[6] * pz + matrix[7]);
        }
    }
};

// Stand-in for the output manager's CPU readback/composite path (T46). It
// converts BGRA8 to RGBA8 over a dark background and writes a full frame.
void render_proxy(const capture::VideoFrame& frame, std::vector<std::uint8_t>& output) {
    const std::size_t width = frame.width;
    const std::size_t height = frame.height;
    output.resize(width * height * 4);
    const std::uint8_t* source = frame.plane(0);
    const std::uint32_t stride = frame.stride(0);
    if (source == nullptr || stride == 0) {
        return;
    }
    for (std::size_t y = 0; y < height; ++y) {
        const std::uint8_t* row = source + y * stride;
        std::uint8_t* destination = output.data() + y * width * 4;
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint32_t blue = row[x * 4 + 0];
            const std::uint32_t green = row[x * 4 + 1];
            const std::uint32_t red = row[x * 4 + 2];
            const std::uint32_t alpha = row[x * 4 + 3];
            const std::uint32_t inverse = 255u - alpha;
            destination[x * 4 + 0] =
                static_cast<std::uint8_t>((red * alpha + 16u * inverse) / 255u);
            destination[x * 4 + 1] =
                static_cast<std::uint8_t>((green * alpha + 18u * inverse) / 255u);
            destination[x * 4 + 2] =
                static_cast<std::uint8_t>((blue * alpha + 24u * inverse) / 255u);
            destination[x * 4 + 3] = 255;
        }
    }
}

[[nodiscard]] std::uint64_t sample_frame(const capture::VideoFrame& frame) {
    const std::uint8_t* plane = frame.plane(0);
    if (plane == nullptr) {
        return 0;
    }
    const std::size_t bytes =
        static_cast<std::size_t>(frame.stride(0)) * static_cast<std::size_t>(frame.height);
    std::uint64_t checksum = 0;
    for (std::size_t offset = 0; offset < bytes; offset += 4096) {
        checksum = checksum * 131u + plane[offset];
    }
    return checksum;
}

// ---------------------------------------------------------------------------
// Benchmark
// ---------------------------------------------------------------------------

[[nodiscard]] json run_benchmark(const Options& options) {
    if (options.tracker == TrackerKind::real) {
        throw std::runtime_error(
            "tracker 'real' is not available yet: the MediaPipe backend lands in T11. "
            "Use --tracker scripted.");
    }
    if (options.frames < 1) {
        throw std::runtime_error("--frames must be at least 1");
    }
    if (options.warmup < 0) {
        throw std::runtime_error("--warmup must be non-negative");
    }
    if (options.width < 2 || options.height < 2) {
        throw std::runtime_error("--width and --height must be at least 2");
    }
    if (options.fps < 0) {
        throw std::runtime_error("--fps must be non-negative (0 = unpaced)");
    }
    if (options.format != "bgra8" && options.format != "nv12") {
        throw std::runtime_error("--format must be bgra8 or nv12");
    }
    if (options.scenario == Scenario::render && options.format != "bgra8") {
        throw std::runtime_error("the render proxy requires --format bgra8");
    }

    const HardwareInfo hardware = collect_hardware();
    const json build = build_json();

    const std::uint64_t frame_period_ns =
        options.fps > 0 ? 1'000'000'000ull / static_cast<std::uint64_t>(options.fps)
                        : 16'666'667ull;

    core::ManualTimeSource time_source(1'000'000'000ull);
    core::FrameClock clock(time_source);

    capture::TestPatternConfig source_config;
    source_config.width = static_cast<std::uint32_t>(options.width);
    source_config.height = static_cast<std::uint32_t>(options.height);
    source_config.format =
        options.format == "nv12" ? capture::PixelFormat::nv12 : capture::PixelFormat::bgra8;
    source_config.colour_bars = true;
    source_config.moving_marker = true;
    source_config.timestamp_burnin = true;
    source_config.alpha_checkerboard = source_config.format == capture::PixelFormat::bgra8;

    capture::TestPatternSource source(clock, source_config);
    const core::Status start_status = source.start();
    if (!start_status.ok()) {
        throw std::runtime_error("test pattern source failed to start: " +
                                 start_status.to_string());
    }

    tracking::ScriptedTrackerConfig tracker_config;
    tracker_config.scenario = tracking::ScriptedScenario::valid_tracking;
    tracker_config.frame_count = static_cast<std::uint32_t>(options.warmup + options.frames + 16);
    tracker_config.frame_interval_ns = frame_period_ns;
    tracking::ScriptedTracker tracker(tracker_config);

    RigStage rig;
    std::vector<std::uint8_t> render_buffer;
    GpuMonitor gpu_monitor;

#if defined(_WIN32)
    if (options.fps > 0) {
        (void)timeBeginPeriod(1);
    }
#endif

    std::vector<double> frame_time_ms;
    std::vector<double> frame_interval_ms;
    std::vector<double> app_latency_ms;
    std::vector<double> capture_ms;
    std::vector<double> ring_ms;
    std::vector<double> tracker_ms;
    std::vector<double> rig_ms;
    std::vector<double> render_ms;
    frame_time_ms.reserve(static_cast<std::size_t>(options.frames));
    frame_interval_ms.reserve(static_cast<std::size_t>(options.frames));
    app_latency_ms.reserve(static_cast<std::size_t>(options.frames));
    capture_ms.reserve(static_cast<std::size_t>(options.frames));
    ring_ms.reserve(static_cast<std::size_t>(options.frames));
    tracker_ms.reserve(static_cast<std::size_t>(options.frames));
    rig_ms.reserve(static_cast<std::size_t>(options.frames));
    render_ms.reserve(static_cast<std::size_t>(options.frames));

    std::uint64_t checksum = 0;
    std::uint64_t produced = 0;
    std::uint64_t measured = 0;
    std::uint64_t missed = 0;
    std::uint64_t ring_dropped_before = 0;
    std::uint64_t tracker_dropped_before = 0;
    std::uint64_t cpu_start_ns = 0;
    Clock::time_point wall_start{};
    std::optional<Clock::time_point> previous_measured_start;

    const std::chrono::nanoseconds period(static_cast<std::int64_t>(frame_period_ns));
    Clock::time_point next_frame = Clock::now();
    const int total_frames = options.warmup + options.frames;

    for (int frame_index = 0; frame_index < total_frames; ++frame_index) {
        if (options.fps > 0) {
            std::this_thread::sleep_until(next_frame);
            next_frame += period;
        }

        const Clock::time_point frame_start = Clock::now();
        const bool measuring = frame_index >= options.warmup;
        if (measuring && frame_index == options.warmup) {
            ring_dropped_before = source.ring().dropped();
            tracker_dropped_before = tracker.stats().dropped;
            cpu_start_ns = process_cpu_ns();
            wall_start = frame_start;
            gpu_monitor.start();
        }

        time_source.advance(frame_period_ns);
        const bool did_produce = source.produce();
        const Clock::time_point capture_end = Clock::now();
        if (measuring && did_produce) {
            ++produced;
        }

        auto lease = source.ring().try_acquire_latest();
        std::uint64_t frame_checksum = 0;
        if (lease.has_value() && options.scenario != Scenario::capture) {
            frame_checksum = sample_frame(lease->frame());
        }
        const Clock::time_point ring_end = Clock::now();

        double tracker_stage_ms = 0.0;
        double rig_stage_ms = 0.0;
        double render_stage_ms = 0.0;
        if (lease.has_value() &&
            (options.scenario == Scenario::rig || options.scenario == Scenario::render)) {
            tracker.submit(lease->frame());
            const Clock::time_point tracker_end = Clock::now();
            tracker_stage_ms = ms_between(ring_end, tracker_end);

            const std::optional<tracking::TrackingResult> result = tracker.latest();
            if (result.has_value()) {
                rig.step(*result);
            }
            const Clock::time_point rig_end = Clock::now();
            rig_stage_ms = ms_between(tracker_end, rig_end);

            if (options.scenario == Scenario::render) {
                render_proxy(lease->frame(), render_buffer);
                const Clock::time_point render_end = Clock::now();
                render_stage_ms = ms_between(rig_end, render_end);
            }
        }
        if (lease.has_value()) {
            lease->release();
        }
        const Clock::time_point frame_end = Clock::now();

        if (!measuring) {
            continue;
        }
        if (!did_produce || !lease.has_value()) {
            ++missed;
            continue;
        }
        checksum = checksum * 31u + frame_checksum;
        ++measured;

        frame_time_ms.push_back(ms_between(frame_start, frame_end));
        app_latency_ms.push_back(ms_between(frame_start, frame_end));
        capture_ms.push_back(ms_between(frame_start, capture_end));
        ring_ms.push_back(ms_between(capture_end, ring_end));
        if (options.scenario == Scenario::rig || options.scenario == Scenario::render) {
            tracker_ms.push_back(tracker_stage_ms);
            rig_ms.push_back(rig_stage_ms);
        }
        if (options.scenario == Scenario::render) {
            render_ms.push_back(render_stage_ms);
        }
        if (previous_measured_start.has_value()) {
            frame_interval_ms.push_back(ms_between(*previous_measured_start, frame_start));
        }
        previous_measured_start = frame_start;
    }

    const Clock::time_point wall_end = Clock::now();
    const std::uint64_t cpu_end_ns = process_cpu_ns();
    const std::uint64_t rss_bytes = process_rss_bytes();
    const std::optional<double> gpu_percent = gpu_monitor.stop();

#if defined(_WIN32)
    if (options.fps > 0) {
        (void)timeEndPeriod(1);
    }
#endif

    const double wall_ms = ms_between(wall_start, wall_end);
    const double cpu_ms = static_cast<double>(cpu_end_ns - cpu_start_ns) / 1'000'000.0;
    const double cpu_percent = wall_ms > 0.0 ? (cpu_ms / wall_ms) * 100.0 : 0.0;

    const Distribution frame_time = summarize(std::move(frame_time_ms));
    const Distribution frame_interval = summarize(std::move(frame_interval_ms));
    const Distribution app_latency = summarize(std::move(app_latency_ms));
    const Distribution capture_stage = summarize(std::move(capture_ms));
    const Distribution ring_stage = summarize(std::move(ring_ms));
    const Distribution tracker_stage = summarize(std::move(tracker_ms));
    const Distribution rig_stage = summarize(std::move(rig_ms));
    const Distribution render_stage = summarize(std::move(render_ms));

    json metrics;
    metrics["frames"] = json{{"requested", options.frames},
                             {"warmup", options.warmup},
                             {"produced", produced},
                             {"measured", measured},
                             {"missed", missed}};
    metrics["dropped_frames"] = source.ring().dropped() - ring_dropped_before;
    metrics["dropped_tracking_frames"] = tracker.stats().dropped - tracker_dropped_before;
    metrics["frame_time_ms"] = distribution_json(frame_time);
    metrics["frame_interval_ms"] = distribution_json(frame_interval);
    metrics["app_latency_ms"] = distribution_json(app_latency);
    metrics["end_to_end_latency_ms"] = nullptr;
    metrics["stage_ms"] =
        json{{"capture", distribution_json(capture_stage)},
             {"ring", distribution_json(ring_stage)},
             {"tracker", options.scenario == Scenario::rig || options.scenario == Scenario::render
                             ? distribution_json(tracker_stage)
                             : json(nullptr)},
             {"rig", options.scenario == Scenario::rig || options.scenario == Scenario::render
                         ? distribution_json(rig_stage)
                         : json(nullptr)},
             {"render", options.scenario == Scenario::render ? distribution_json(render_stage)
                                                             : json(nullptr)}};
    metrics["rss_bytes"] = rss_bytes;
    metrics["cpu_percent_one_core"] = round_to(cpu_percent, 3);
    if (gpu_percent.has_value()) {
        metrics["gpu_percent"] = round_to(*gpu_percent, 3);
    } else {
        metrics["gpu_percent"] = nullptr;
    }

    std::vector<std::string> notes;
    if (options.scenario == Scenario::capture) {
        notes.emplace_back("capture isolates the deterministic source render and ring publish; "
                           "ring/rig add the consumer stages.");
    }
    if (options.scenario == Scenario::render) {
        notes.emplace_back(
            "render is a CPU BGRA->RGBA compositing/readback proxy; the real GPU render "
            "benchmark lands in T26.");
    }
    notes.emplace_back(
        "The harness is synchronous: app_latency_ms and frame_time_ms measure the same "
        "interval until the render and tracking threads land (T26).");
    notes.emplace_back(
        "end_to_end_latency_ms (photon to display) needs a real camera and transport; "
        "T26/T46 own it.");

    json result;
    result["schema"] = kSchema;
    result["generated_utc"] = utc_now_iso8601();
    result["scenario"] = scenario_name(options.scenario);
    result["tracker"] = tracker_name(options.tracker);
    result["status"] = "ok";
    result["build"] = build;
    result["config"] = json{{"frames", options.frames},
                            {"warmup_frames", options.warmup},
                            {"width", options.width},
                            {"height", options.height},
                            {"pixel_format", options.format},
                            {"fps", options.fps},
                            {"tracker_scenario", options.tracker_scenario},
                            {"scenario", scenario_name(options.scenario)}};
    result["hardware"] = hardware_json(hardware);
    result["metrics"] = metrics;
    result["checksum"] = checksum;
    result["rig_sink"] = round_to(rig.sink, 6);
    result["notes"] = notes;
    result["end_to_end_note"] =
        "photon-to-display latency is not measurable in the foundation harness; "
        "T26 instruments the render pipeline and T46 the transport.";
    if (!gpu_percent.has_value()) {
        result["gpu_note"] =
            "GPU utilization counter unavailable; the foundation harness has no GPU "
            "workload and T26 owns render benchmarks.";
    }
    return result;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

[[nodiscard]] bool validate_distribution(const json& value, const std::string& name,
                                         std::string& error) {
    if (!value.is_object()) {
        error = name + " is not an object";
        return false;
    }
    for (const char* key : {"p50", "p95", "p99", "mean", "max"}) {
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
    const double p50 = value["p50"].get<double>();
    const double p95 = value["p95"].get<double>();
    const double p99 = value["p99"].get<double>();
    if (!(p50 <= p95 && p95 <= p99)) {
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
    if (!result.contains("scenario") || !result["scenario"].is_string()) {
        error = "scenario is missing";
        return false;
    }
    const std::string scenario = result["scenario"].get<std::string>();
    if (scenario != "capture" && scenario != "ring" && scenario != "rig" && scenario != "render") {
        error = "unknown scenario '" + scenario + "'";
        return false;
    }
    if (!result.contains("hardware") || !result["hardware"].is_object()) {
        error = "hardware is missing";
        return false;
    }
    if (result["hardware"].value("fingerprint", "").empty()) {
        error = "hardware.fingerprint is empty";
        return false;
    }
    if (!result.contains("build") || !result["build"].is_object()) {
        error = "build is missing";
        return false;
    }
    if (result["build"].value("type", "").empty()) {
        error = "build.type is empty";
        return false;
    }
    if (result["build"].value("git_commit", "").empty()) {
        error = "build.git_commit is empty";
        return false;
    }
    if (!result.contains("config") || !result["config"].is_object()) {
        error = "config is missing";
        return false;
    }
    if (!result.contains("metrics") || !result["metrics"].is_object()) {
        error = "metrics is missing";
        return false;
    }
    const json& metrics = result["metrics"];
    for (const char* key : {"frames", "dropped_frames", "dropped_tracking_frames", "frame_time_ms",
                            "frame_interval_ms", "app_latency_ms", "rss_bytes",
                            "cpu_percent_one_core", "gpu_percent", "stage_ms"}) {
        if (!metrics.contains(key)) {
            error = std::string("metrics.") + key + " is missing";
            return false;
        }
    }
    if (!metrics["frames"].is_object() || !metrics["frames"].contains("measured") ||
        !metrics["frames"]["measured"].is_number_unsigned()) {
        error = "metrics.frames.measured is missing";
        return false;
    }
    if (!validate_distribution(metrics["frame_time_ms"], "metrics.frame_time_ms", error)) {
        return false;
    }
    if (!validate_distribution(metrics["frame_interval_ms"], "metrics.frame_interval_ms", error)) {
        return false;
    }
    if (!validate_distribution(metrics["app_latency_ms"], "metrics.app_latency_ms", error)) {
        return false;
    }
    if (!metrics["stage_ms"].is_object()) {
        error = "metrics.stage_ms is not an object";
        return false;
    }
    for (const char* key : {"capture", "ring", "tracker", "rig", "render"}) {
        if (!metrics["stage_ms"].contains(key)) {
            error = std::string("metrics.stage_ms.") + key + " is missing";
            return false;
        }
        if (!metrics["stage_ms"][key].is_null() &&
            !validate_distribution(metrics["stage_ms"][key], std::string("metrics.stage_ms.") + key,
                                   error)) {
            return false;
        }
    }
    if (metrics["frames"]["measured"].get<std::uint64_t>() == 0) {
        error = "metrics.frames.measured is zero";
        return false;
    }
    if (!metrics["gpu_percent"].is_null()) {
        const double gpu = metrics["gpu_percent"].get<double>();
        if (gpu < 0.0 || gpu > 100.0) {
            error = "metrics.gpu_percent is outside 0..100";
            return false;
        }
    }
    if (metrics["cpu_percent_one_core"].get<double>() < 0.0) {
        error = "metrics.cpu_percent_one_core is negative";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

[[nodiscard]] int run_self_test() {
    const std::array<Scenario, 4> scenarios = {Scenario::capture, Scenario::ring, Scenario::rig,
                                               Scenario::render};
    bool failed = false;
    for (const Scenario scenario : scenarios) {
        Options options;
        options.scenario = scenario;
        options.frames = 5;
        options.warmup = 1;
        options.width = 64;
        options.height = 48;
        options.fps = 0;
        options.quiet = true;
        try {
            const json result = run_benchmark(options);
            std::string error;
            if (!validate_result(result, error)) {
                std::fprintf(stderr, "avatar_bench self-test: %s failed validation: %s\n",
                             scenario_name(scenario), error.c_str());
                failed = true;
                continue;
            }
            std::printf("avatar_bench self-test: %s OK (measured=%llu p50=%.4f ms)\n",
                        scenario_name(scenario),
                        static_cast<unsigned long long>(
                            result["metrics"]["frames"]["measured"].get<std::uint64_t>()),
                        result["metrics"]["frame_time_ms"]["p50"].get<double>());
        } catch (const std::exception& exception) {
            std::fprintf(stderr, "avatar_bench self-test: %s threw: %s\n", scenario_name(scenario),
                         exception.what());
            failed = true;
        }
    }

    // The real tracker must be rejected clearly until T11 lands.
    Options real;
    real.tracker = TrackerKind::real;
    real.frames = 1;
    real.warmup = 0;
    real.width = 64;
    real.height = 48;
    real.fps = 0;
    try {
        (void)run_benchmark(real);
        std::fprintf(stderr, "avatar_bench self-test: --tracker real was not rejected\n");
        failed = true;
    } catch (const std::runtime_error&) {
        std::printf("avatar_bench self-test: tracker=real rejected as expected\n");
    }

    if (failed) {
        std::fprintf(stderr, "avatar_bench self-test: FAIL\n");
        return 1;
    }
    std::printf("avatar_bench self-test: PASS\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

void print_usage() {
    std::printf(
        "avatar_bench - Avatar Studio pipeline performance harness\n"
        "\n"
        "Options:\n"
        "  --scenario capture|ring|rig|render  pipeline depth (default rig)\n"
        "  --tracker scripted|real             tracker backend (real is T11)\n"
        "  --frames N                          measured frames (default 300)\n"
        "  --warmup N                          discarded warmup frames (default 30)\n"
        "  --width N --height N                source size (default 1280x720)\n"
        "  --format bgra8|nv12                 source pixel format (default bgra8)\n"
        "  --fps N                             paced frames per second, 0 = unpaced (default 60)\n"
        "  --output PATH                       write JSON to PATH instead of stdout\n"
        "  --quiet                             suppress progress on stderr\n"
        "  --self-test                         run all scenarios tiny and validate JSON\n"
        "  --help                              this text\n"
        "\n"
        "Exit codes: 0 ok, 1 self-test/IO failure, 2 usage error, 3 unavailable backend.\n");
}

[[nodiscard]] Scenario parse_scenario(const std::string& value) {
    if (value == "capture") {
        return Scenario::capture;
    }
    if (value == "ring") {
        return Scenario::ring;
    }
    if (value == "rig") {
        return Scenario::rig;
    }
    if (value == "render") {
        return Scenario::render;
    }
    throw std::runtime_error("unknown --scenario '" + value + "'");
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

} // namespace

int main(int argc, char** argv) {
    Options options;
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
            if (argument.rfind("--", 0) != 0) {
                std::fprintf(stderr, "avatar_bench: unexpected argument '%s'\n", argument.c_str());
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

            if (key == "--scenario") {
                options.scenario = parse_scenario(value());
            } else if (key == "--tracker") {
                const std::string tracker = value();
                if (tracker == "scripted") {
                    options.tracker = TrackerKind::scripted;
                } else if (tracker == "real") {
                    options.tracker = TrackerKind::real;
                } else {
                    throw std::runtime_error("unknown --tracker '" + tracker + "'");
                }
            } else if (key == "--frames") {
                options.frames = parse_int(key, value());
            } else if (key == "--warmup") {
                options.warmup = parse_int(key, value());
            } else if (key == "--width") {
                options.width = parse_int(key, value());
            } else if (key == "--height") {
                options.height = parse_int(key, value());
            } else if (key == "--fps") {
                options.fps = parse_int(key, value());
            } else if (key == "--format") {
                options.format = value();
            } else if (key == "--output") {
                options.output = value();
            } else {
                std::fprintf(stderr, "avatar_bench: unknown option '%s'\n", key.c_str());
                return 2;
            }
        }
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "avatar_bench: %s\n", exception.what());
        return 2;
    }

    if (self_test) {
        return run_self_test();
    }
    if (options.tracker == TrackerKind::real) {
        std::fprintf(stderr,
                     "avatar_bench: tracker 'real' is not available yet: the MediaPipe backend "
                     "lands in T11. Use --tracker scripted.\n");
        return 3;
    }

    try {
        if (!options.quiet) {
            std::fprintf(
                stderr, "avatar_bench: scenario=%s tracker=%s %dx%d frames=%d warmup=%d fps=%d\n",
                scenario_name(options.scenario), tracker_name(options.tracker), options.width,
                options.height, options.frames, options.warmup, options.fps);
        }
        const json result = run_benchmark(options);
        std::string error;
        if (!validate_result(result, error)) {
            std::fprintf(stderr, "avatar_bench: internal result validation failed: %s\n",
                         error.c_str());
            return 1;
        }
        if (!options.quiet) {
            std::fprintf(stderr,
                         "avatar_bench: measured=%llu app_p50=%.4f ms app_p95=%.4f ms "
                         "frame_p99=%.4f ms dropped=%llu cpu=%.1f%%\n",
                         static_cast<unsigned long long>(
                             result["metrics"]["frames"]["measured"].get<std::uint64_t>()),
                         result["metrics"]["app_latency_ms"]["p50"].get<double>(),
                         result["metrics"]["app_latency_ms"]["p95"].get<double>(),
                         result["metrics"]["frame_time_ms"]["p99"].get<double>(),
                         static_cast<unsigned long long>(
                             result["metrics"]["dropped_frames"].get<std::uint64_t>()),
                         result["metrics"]["cpu_percent_one_core"].get<double>());
        }
        if (options.output.empty()) {
            std::printf("%s\n", result.dump(2).c_str());
        } else {
            const std::filesystem::path path(options.output);
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }
            std::ofstream output(path, std::ios::binary);
            if (!output) {
                std::fprintf(stderr, "avatar_bench: cannot write '%s'\n", options.output.c_str());
                return 1;
            }
            output << result.dump(2) << "\n";
            if (!output) {
                std::fprintf(stderr, "avatar_bench: write failed for '%s'\n",
                             options.output.c_str());
                return 1;
            }
            if (!options.quiet) {
                std::fprintf(stderr, "avatar_bench: wrote %s\n", options.output.c_str());
            }
        }
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "avatar_bench: %s\n", exception.what());
        return 1;
    }
    return 0;
}
