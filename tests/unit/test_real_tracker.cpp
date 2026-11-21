#include "test_registry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#define NOMINMAX
#include <windows.h>

#include "../../native/tracking/mediapipe/backend.h"
#include "../../native/tracking/mediapipe/clip_io.h"
#include "avatar/tracker/tracker_c_api.h"

namespace {
struct Api {
    HMODULE module = nullptr;
    decltype(&avatar_tracker_create) create = nullptr;
    decltype(&avatar_tracker_configure) configure = nullptr;
    decltype(&avatar_tracker_push_frame) push = nullptr;
    decltype(&avatar_tracker_poll_result) poll = nullptr;
    decltype(&avatar_tracker_destroy) destroy = nullptr;

    template <typename T>
    void bind(T& out, const char* name) {
        const auto address = GetProcAddress(module, name);
        if (!address) {
            throw std::runtime_error("missing tracker export");
        }
        static_assert(sizeof(out) == sizeof(address));
        std::memcpy(&out, &address, sizeof(out));
    }
    void load(const std::filesystem::path& path) {
        module =
            LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            throw std::runtime_error("cannot load tracker DLL: " + std::to_string(GetLastError()));
        }
        bind(create, "avatar_tracker_create");
        bind(configure, "avatar_tracker_configure");
        bind(push, "avatar_tracker_push_frame");
        bind(poll, "avatar_tracker_poll_result");
        bind(destroy, "avatar_tracker_destroy");
    }
    ~Api() {
        if (module) {
            FreeLibrary(module);
        }
    }
} api;

struct BlankFrame {
    std::vector<uint8_t> pixels = std::vector<uint8_t>(64 * 48 * 4, 0);
    avatar_tracker_frame frame{};
    BlankFrame() {
        frame.width = 64;
        frame.height = 48;
        frame.stride = 64 * 4;
        frame.format = AVATAR_TRACKER_PIXEL_FORMAT_RGBA8;
        frame.plane_count = 1;
        frame.plane0 = pixels.data();
        frame.plane0_size = pixels.size();
    }
};

bool await_result(avatar_tracker_handle* handle, avatar_tracker_result& result) {
    const auto start = GetTickCount64();
    do {
        const auto status = api.poll(handle, &result);
        if (status == AVATAR_TRACKER_OK) {
            return true;
        }
        if (status != AVATAR_TRACKER_NO_RESULT) {
            return false;
        }
        Sleep(1);
    } while (GetTickCount64() - start < 10000);
    return false;
}

struct Handle {
    avatar_tracker_handle* value = nullptr;
    explicit Handle(const avatar_tracker_config* config = nullptr) {
        avatar_tracker_error error = AVATAR_TRACKER_ERROR_INTERNAL;
        value = api.create(config, &error);
        CHECK(value != nullptr);
        CHECK(error == AVATAR_TRACKER_OK);
    }
    ~Handle() { api.destroy(value); }
};
} // namespace

TEST_CASE(real_tracker_nulls_config_validation_and_exception_containment) {
    avatar_tracker_result result{};
    BlankFrame frame;
    api.destroy(nullptr);
    CHECK(api.configure(nullptr, nullptr) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
    CHECK(api.push(nullptr, &frame.frame) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
    CHECK(api.poll(nullptr, &result) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
    avatar_tracker_config config = AVATAR_TRACKER_CONFIG_INIT;
    avatar_tracker_error error = AVATAR_TRACKER_OK;
    config.abi_version = 99;
    CHECK(api.create(&config, &error) == nullptr);
    CHECK(error == AVATAR_TRACKER_ERROR_ABI_MISMATCH);
    config = AVATAR_TRACKER_CONFIG_INIT;
    config.base_confidence = NAN;
    CHECK(api.create(&config, &error) == nullptr);
    CHECK(error == AVATAR_TRACKER_ERROR_INVALID_CONFIG);
    config = AVATAR_TRACKER_CONFIG_INIT;
    config.reserved = 1;
    CHECK(api.create(&config, &error) == nullptr);
    CHECK(error == AVATAR_TRACKER_ERROR_INVALID_CONFIG);
    for (uint32_t fault = 1; fault <= 3; ++fault) {
        config = AVATAR_TRACKER_CONFIG_INIT;
        config.simulate_fault = fault;
        Handle handle(&config);
        if (!handle.value) {
            continue;
        }
        CHECK(api.configure(handle.value, nullptr) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
        CHECK(api.push(handle.value, nullptr) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
        CHECK(api.poll(handle.value, nullptr) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
        const avatar_tracker_config clean = AVATAR_TRACKER_CONFIG_INIT;
        if (fault == AVATAR_TRACKER_FAULT_CONFIGURE) {
            CHECK(api.configure(handle.value, &clean) == AVATAR_TRACKER_ERROR_INTERNAL);
        } else if (fault == AVATAR_TRACKER_FAULT_PUSH) {
            CHECK(api.push(handle.value, &frame.frame) == AVATAR_TRACKER_ERROR_INTERNAL);
        } else {
            result.valid = 1;
            CHECK(api.poll(handle.value, &result) == AVATAR_TRACKER_ERROR_INTERNAL);
            CHECK(result.valid == 0);
        }
    }
}

TEST_CASE(real_tracker_async_copy_timestamp_preservation_no_face_and_reconfigure) {
    Handle handle;
    if (!handle.value) {
        return;
    }
    BlankFrame frame;
    avatar_tracker_result result{};
    CHECK(api.poll(handle.value, &result) == AVATAR_TRACKER_NO_RESULT);
    for (const uint64_t timestamp : {999999ull, 999999ull, 1ull, 1000000ull}) {
        frame.frame.timestamp_ns = timestamp;
        CHECK(api.push(handle.value, &frame.frame) == AVATAR_TRACKER_OK);
        // Borrowed input may be reused immediately after push returns.
        std::fill(frame.pixels.begin(), frame.pixels.end(), uint8_t(255));
        CHECK(await_result(handle.value, result));
        CHECK(result.source_timestamp_ns == timestamp);
        CHECK(result.valid == 0);
        CHECK(result.features_valid == 0);
        CHECK(result.landmark_count == 0);
        CHECK(api.poll(handle.value, &result) == AVATAR_TRACKER_NO_RESULT);
        CHECK(result.source_timestamp_ns == 0);
        std::fill(frame.pixels.begin(), frame.pixels.end(), uint8_t(0));
    }
    CHECK(api.push(handle.value, &frame.frame) == AVATAR_TRACKER_OK);
    auto config = avatar_tracker_config AVATAR_TRACKER_CONFIG_INIT;
    config.input_width = config.input_height = 192;
    CHECK(api.configure(handle.value, &config) == AVATAR_TRACKER_OK);
    CHECK(api.poll(handle.value, &result) == AVATAR_TRACKER_NO_RESULT);
    frame.frame.timestamp_ns = 0;
    CHECK(api.push(handle.value, &frame.frame) == AVATAR_TRACKER_OK);
    CHECK(await_result(handle.value, result));
    CHECK(result.source_timestamp_ns == 0);
}

TEST_CASE(real_tracker_latest_only_nonblocking_busy_drop_and_frame_validation) {
    Handle handle;
    if (!handle.value) {
        return;
    }
    BlankFrame frame;
    auto bad = frame.frame;
    bad.plane0_size--;
    CHECK(api.push(handle.value, &bad) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
    bad = frame.frame;
    bad.plane0 = nullptr;
    CHECK(api.push(handle.value, &bad) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
    bad = frame.frame;
    bad.width = 8193;
    CHECK(api.push(handle.value, &bad) == AVATAR_TRACKER_ERROR_FRAME_TOO_LARGE);
    bad = frame.frame;
    bad.plane_count = 0;
    CHECK(api.push(handle.value, &bad) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
    bad = frame.frame;
    bad.format = AVATAR_TRACKER_PIXEL_FORMAT_MJPEG;
    CHECK(api.push(handle.value, &bad) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
    const auto start = GetTickCount64();
    for (uint32_t i = 0; i < 1000; ++i) {
        frame.frame.timestamp_ns = i;
        CHECK(api.push(handle.value, &frame.frame) == AVATAR_TRACKER_OK);
    }
    CHECK(GetTickCount64() - start < 1000); // catches synchronous inference/queued execution
    Sleep(2000);
    frame.frame.timestamp_ns = 2000000000ull;
    CHECK(api.push(handle.value, &frame.frame) == AVATAR_TRACKER_OK);
    Sleep(2000);
    frame.frame.timestamp_ns = 3000000000ull;
    CHECK(api.push(handle.value, &frame.frame) == AVATAR_TRACKER_OK);
    Sleep(2000);
    avatar_tracker_result result{};
    CHECK(api.poll(handle.value, &result) == AVATAR_TRACKER_OK);
    CHECK(result.source_timestamp_ns == 3000000000ull);
    CHECK(api.poll(handle.value, &result) == AVATAR_TRACKER_NO_RESULT);
}

TEST_CASE(real_tracker_letterbox_edge_padding_inverse_mapping_and_colour) {
    using namespace avatar::tracker_backend;
    std::array<uint8_t, 8> pixels{255, 0, 0, 255, 0, 255, 0, 255};
    avatar_tracker_frame f{};
    f.width = 2;
    f.height = 1;
    f.stride = 8;
    f.format = AVATAR_TRACKER_PIXEL_FORMAT_RGBA8;
    f.plane_count = 1;
    f.plane0 = pixels.data();
    f.plane0_size = pixels.size();
    CHECK(validate_frame(f) == AVATAR_TRACKER_OK);
    const auto prepared = prepare(f, 4);
    CHECK(prepared.box.scale == 2);
    CHECK(prepared.box.pad_x == 0);
    CHECK(prepared.box.pad_y == 1);
    CHECK(std::equal(prepared.rgb.begin(), prepared.rgb.begin() + 12, prepared.rgb.begin() + 12));
    const auto a = prepared.box.unproject(0, 0.25f, 0.5f);
    const auto b = prepared.box.unproject(1, 0.75f, 0);
    CHECK(a.x == 0 && a.y == 0 && a.z == 0.5f);
    CHECK(b.x == 1 && b.y == 1);
    OwnedFrame owned;
    owned.copy(f);
    std::fill(pixels.begin(), pixels.end(), uint8_t(0));
    const auto copied = prepare(owned.frame, 4);
    CHECK(copied.rgb == prepared.rgb);
    CHECK(owned.frame.plane0 != f.plane0);
    CHECK(next_timestamp_ms(1, -1) == 0);
    CHECK(next_timestamp_ms(1, 0) == 1);
    CHECK(next_timestamp_ms(0, 1) == 2);
    CHECK(next_timestamp_ms(3000000, 2) == 3);
    std::array<uint8_t, 6> nv12{16, 235, 16, 235, 128, 128};
    f.width = f.height = 2;
    f.stride = 2;
    f.format = AVATAR_TRACKER_PIXEL_FORMAT_NV12;
    f.plane_count = 2;
    f.plane0 = nv12.data();
    f.plane1 = nv12.data() + 4;
    f.plane0_size = 4;
    f.plane1_size = 2;
    CHECK(validate_frame(f) == AVATAR_TRACKER_OK);
    const auto rgb = prepare(f, 2);
    CHECK(rgb.rgb[0] == 0 && rgb.rgb[1] == 0 && rgb.rgb[2] == 0);
    CHECK(rgb.rgb[3] == 255 && rgb.rgb[4] == 255 && rgb.rgb[5] == 255);
}

TEST_CASE(real_tracker_features_valid_requires_both_eyes_irises_and_mouth) {
    using avatar::tracker_backend::features_usable;
    avatar_tracker_result result{};
    result.valid = 1;
    result.confidence = 1;
    result.landmark_count = 478;
    for (auto& p : result.landmarks) {
        p = {0.5f, 0.5f, 0};
    }
    result.landmarks[33].x = 0.3f;
    result.landmarks[263].x = 0.7f;
    result.landmarks[61].x = 0.4f;
    CHECK(features_usable(result));
    result.landmarks[13] = {};
    CHECK(!features_usable(result));
    result.landmarks[13] = {0.5f, 0.5f, 0};
    result.landmarks[468].x = NAN;
    CHECK(!features_usable(result));
    result.valid = 0;
    CHECK(!features_usable(result));
}

int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::strcmp(argv[1], "--clip") == 0) {
            api.load(std::filesystem::absolute(argv[2]));
            std::ifstream clip(argv[3], std::ios::binary);
            auto frames = avatar::tracker_backend::read_clip(clip);
            avatar_tracker_config config = AVATAR_TRACKER_CONFIG_INIT;
            config.input_width = config.input_height = static_cast<uint32_t>(std::stoul(argv[4]));
            if (config.input_width != 192 && config.input_width != 256) {
                throw std::runtime_error("clip input must be 192 or 256");
            }
            Handle handle(&config);
            if (!handle.value) {
                return 1;
            }
            uint32_t valid = 0, features = 0, expressions = 0;
            for (const auto& frame : frames) {
                avatar_tracker_result result{};
                if (api.push(handle.value, &frame.frame) != AVATAR_TRACKER_OK ||
                    !await_result(handle.value, result) ||
                    result.source_timestamp_ns != frame.frame.timestamp_ns) {
                    throw std::runtime_error("recorded clip failed through the C ABI");
                }
                if (result.valid && result.landmark_count != 478) {
                    throw std::runtime_error("recorded face topology mismatch");
                }
                valid += result.valid;
                features += result.features_valid;
                bool expressive = false;
                for (const auto coefficient : result.blendshapes) {
                    if (!std::isfinite(coefficient) || coefficient < 0 || coefficient > 1) {
                        throw std::runtime_error("recorded blendshape invalid");
                    }
                    expressive = expressive || coefficient > 0;
                }
                expressions += expressive ? 1 : 0;
            }
            const bool passed = valid && features && expressions;
            std::printf(
                "recorded-clip: %s input=%u frames=%zu valid=%u features=%u expressions=%u\n",
                passed ? "PASS" : "FAIL", config.input_width, frames.size(), valid, features,
                expressions);
            return passed ? 0 : 1;
        }
        if (argc == 4 && std::strcmp(argv[1], "--reject-corrupt-model") == 0) {
            const auto source = std::filesystem::absolute(argv[2]);
            const auto destination = std::filesystem::absolute(argv[3]);
            std::filesystem::create_directories(destination / "tracker-models");
            for (const auto* file : {"libavatartracker.dll", "opencv_world3410.dll"}) {
                // Bazel artifacts carry the Windows read-only bit. Make a previous
                // isolated test copy replaceable before repeating this test.
                if (std::filesystem::exists(destination / file)) {
                    std::filesystem::permissions(destination / file,
                                                 std::filesystem::perms::owner_write,
                                                 std::filesystem::perm_options::add);
                }
                std::filesystem::copy_file(source.parent_path() / file, destination / file,
                                           std::filesystem::copy_options::overwrite_existing);
            }
            const auto model = destination / "tracker-models" / "face_landmarker.task";
            std::filesystem::copy_file(source.parent_path() / "tracker-models" /
                                           "face_landmarker.task",
                                       model, std::filesystem::copy_options::overwrite_existing);
            std::fstream altered(model, std::ios::binary | std::ios::in | std::ios::out);
            altered.put('X');
            altered.close();
            api.load(destination / "libavatartracker.dll");
            avatar_tracker_error error = AVATAR_TRACKER_OK;
            auto* handle = api.create(nullptr, &error);
            api.destroy(handle);
            const bool rejected = !handle && error == AVATAR_TRACKER_ERROR_INTERNAL;
            std::printf("model-rejection: %s\n", rejected ? "PASS" : "FAIL");
            return rejected ? 0 : 1;
        }
        if (argc != 2) {
            return 2;
        }
        api.load(std::filesystem::absolute(argv[1]));
        return avatar::test::run_all();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
