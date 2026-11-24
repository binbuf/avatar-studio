#include "backend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#define NOMINMAX
#include <windows.h>

#include <bcrypt.h>

#include <mediapipe/framework/formats/image.h>
#include <mediapipe/framework/formats/image_frame.h>
#include <mediapipe/tasks/cc/vision/face_landmarker/face_landmarker.h>

namespace avatar::tracker_backend {
namespace {
constexpr char kModelSha256[] = "64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff";

std::string checked_model(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        throw std::runtime_error("bundled face_landmarker.task missing");
    }
    std::string data(3758596, '\0');
    file.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (file.gcount() != static_cast<std::streamsize>(data.size()) || file.peek() != EOF) {
        throw std::runtime_error("tracker model size mismatch");
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("SHA256 unavailable");
    }
    std::array<uint8_t, 32> digest{};
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status >= 0) {
        status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(data.data()),
                                static_cast<ULONG>(data.size()), 0);
    }
    if (status >= 0) {
        status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    }
    if (hash) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    constexpr char hex[] = "0123456789abcdef";
    std::string actual;
    for (const auto byte : digest) {
        actual += hex[byte >> 4];
        actual += hex[byte & 15];
    }
    if (status < 0 || actual != kModelSha256) {
        throw std::runtime_error("tracker model SHA256 mismatch");
    }
    return data;
}

void rotation_to_quaternion(const mediapipe::Matrix& m, float* q) {
    const float trace = m(0, 0) + m(1, 1) + m(2, 2);
    if (trace > 0) {
        const float s = std::sqrt(trace + 1) * 2;
        q[3] = 0.25f * s;
        q[0] = (m(2, 1) - m(1, 2)) / s;
        q[1] = (m(0, 2) - m(2, 0)) / s;
        q[2] = (m(1, 0) - m(0, 1)) / s;
    } else {
        int i = m(1, 1) > m(0, 0) ? 1 : 0;
        i = m(2, 2) > m(i, i) ? 2 : i;
        const int j = (i + 1) % 3, k = (j + 1) % 3;
        const float s = std::sqrt(1 + m(i, i) - m(j, j) - m(k, k)) * 2;
        q[i] = 0.25f * s;
        q[j] = (m(j, i) + m(i, j)) / s;
        q[k] = (m(k, i) + m(i, k)) / s;
        q[3] = (m(k, j) - m(j, k)) / s;
    }
    const float norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!std::isfinite(norm) || norm < 0.01f) {
        throw std::runtime_error("invalid face transform");
    }
    for (size_t i = 0; i < 4; ++i) {
        q[i] /= norm;
    }
}
} // namespace

namespace mp = mediapipe::tasks::vision::face_landmarker;

struct MediaPipeEngine::Impl {
    std::unique_ptr<mp::FaceLandmarker> landmarker;
};

MediaPipeEngine::MediaPipeEngine(const std::string& model_path) : impl_(std::make_unique<Impl>()) {
    auto options = std::make_unique<mp::FaceLandmarkerOptions>();
    options->base_options.model_asset_buffer =
        std::make_unique<std::string>(checked_model(model_path));
    options->base_options.delegate = mediapipe::tasks::core::BaseOptions::CPU;
    options->running_mode = mediapipe::tasks::vision::core::RunningMode::VIDEO;
    options->num_faces = 1;
    options->min_face_detection_confidence = 0.5f;
    options->min_face_presence_confidence = 0.5f;
    options->min_tracking_confidence = 0.5f;
    options->output_face_blendshapes = true;
    options->output_facial_transformation_matrixes = true;
    auto created = mp::FaceLandmarker::Create(std::move(options));
    if (!created.ok()) {
        throw std::runtime_error(created.status().ToString());
    }
    impl_->landmarker = std::move(created.value());
}

MediaPipeEngine::~MediaPipeEngine() {
    if (impl_ && impl_->landmarker) {
        (void)impl_->landmarker->Close();
    }
}

avatar_tracker_result MediaPipeEngine::infer(const PreparedFrame& input, int64_t timestamp_ms) {
    const auto size = static_cast<int>(input.box.size);
    auto pixels = std::make_shared<mediapipe::ImageFrame>(mediapipe::ImageFormat::SRGB, size, size);
    for (int y = 0; y < size; ++y) {
        std::memcpy(pixels->MutablePixelData() + y * pixels->WidthStep(),
                    input.rgb.data() + y * size * 3, static_cast<size_t>(size) * 3);
    }
    auto detected = impl_->landmarker->DetectForVideo(mediapipe::Image(pixels), timestamp_ms);
    if (!detected.ok()) {
        throw std::runtime_error(detected.status().ToString());
    }
    avatar_tracker_result result{};
    const auto& faces = detected->face_landmarks;
    if (faces.empty()) {
        return result;
    }
    if (faces.size() != 1 || faces[0].landmarks.size() != 478) {
        throw std::runtime_error("unexpected face landmark topology");
    }
    size_t inside = 0;
    for (size_t i = 0; i < 478; ++i) {
        const auto& p = faces[0].landmarks[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            return {};
        }
        result.landmarks[i] = input.box.unproject(p.x, p.y, p.z);
        const auto& original = result.landmarks[i];
        inside += original.x >= 0 && original.x <= 1 && original.y >= 0 && original.y <= 1 ? 1 : 0;
    }
    result.valid = 1;
    result.landmark_count = 478;
    // Tasks C++ does not publish face-presence logits. This is an explicit geometric
    // trust proxy after MediaPipe's three 0.5 thresholds, not an invented raw score.
    result.confidence = static_cast<float>(inside) / 478;
    // Visibility is per feature, not "every one of the 478 points is visible".
    // At a profile view the far-side eye and cheek points drop below 0.5; the
    // mouth and near eye are still good, and the rig can solve from them.
    const auto visible = [&](size_t index) {
        const auto& p = faces[0].landmarks[index];
        return (!p.visibility || *p.visibility >= 0.5f) && (!p.presence || *p.presence >= 0.5f);
    };
    const bool eye_r_visible = visible(33) && visible(133) && visible(468);
    const bool eye_l_visible = visible(263) && visible(362) && visible(473);
    const bool mouth_visible = visible(61) && visible(291) && visible(13) && visible(14);
    result.features_valid =
        features_usable(result) && mouth_visible && (eye_r_visible || eye_l_visible) ? 1 : 0;
    const auto& transforms = detected->facial_transformation_matrixes;
    if (!transforms || transforms->size() != 1 || (*transforms)[0].rows() != 4 ||
        (*transforms)[0].cols() != 4) {
        return {};
    }
    const auto& matrix = (*transforms)[0];
    rotation_to_quaternion(matrix, result.pose.rotation);
    for (int i = 0; i < 3; ++i) {
        // MediaPipe's canonical face geometry uses centimetres; ABI v1 uses metres.
        result.pose.translation[i] = matrix(i, 3) * 0.01f;
        if (!std::isfinite(result.pose.translation[i])) {
            return {};
        }
    }
    if (detected->face_blendshapes && !detected->face_blendshapes->empty()) {
        for (const auto& category : (*detected->face_blendshapes)[0].categories) {
            if (category.index >= 0 && category.index < 52 && std::isfinite(category.score)) {
                result.blendshapes[category.index] = std::clamp(category.score, 0.0f, 1.0f);
            }
        }
    }
    return result;
}

} // namespace avatar::tracker_backend
