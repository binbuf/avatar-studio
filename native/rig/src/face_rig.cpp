#include "avatar/rig/face_rig.h"

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "avatar/tracking/canonical_face_frame.h"

namespace avatar::rig {
namespace {
using tracking::LandmarkName;
using Mat3 = Eigen::Matrix3d;
using Vec2 = Eigen::Vector2d;
using Vec3 = Eigen::Vector3d;
using Quat = Eigen::Quaterniond;
constexpr double kPi = std::numbers::pi;
constexpr double kDegrees = 180.0 / kPi;
constexpr double kRansacThreshold = 0.008;
constexpr core::TimestampNs kHoldNs = 150'000'000;
constexpr double kPredictionSeconds = 0.050;
constexpr std::size_t kMinimumInliers = tracking::kMinimumSamplingCorrespondences;

bool stable(LandmarkName name) {
    return name != LandmarkName::iris_center_l && name != LandmarkName::iris_center_r &&
           name != LandmarkName::mouth_upper_center && name != LandmarkName::mouth_lower_center;
}

struct Pair {
    Vec2 canonical;
    Vec2 image;
};
using Pairs = std::vector<Pair>;

Vec2 project(const Mat3& h, const Vec2& p) {
    const Vec3 v = h * Vec3(p.x(), p.y(), 1.0);
    if (std::abs(v.z()) < 1e-12) {
        return Vec2::Constant(1e20);
    }
    return v.head<2>() / v.z();
}

// Hartley normalisation: zero mean, mean distance sqrt(2). Work in double,
// including the homogeneous nullspace and denormalisation.
bool dlt(const Pairs& pairs, const std::vector<std::size_t>& indices, Mat3& h) {
    if (indices.size() < 4) {
        return false;
    }
    Vec2 c = Vec2::Zero(), p = Vec2::Zero();
    for (auto i : indices) {
        c += pairs[i].canonical;
        p += pairs[i].image;
    }
    c /= static_cast<double>(indices.size());
    p /= static_cast<double>(indices.size());
    double dc = 0.0, dp = 0.0;
    for (auto i : indices) {
        dc += (pairs[i].canonical - c).norm();
        dp += (pairs[i].image - p).norm();
    }
    if (dc < 1e-10 || dp < 1e-10) {
        return false;
    }
    const double sc = std::sqrt(2.0) * static_cast<double>(indices.size()) / dc;
    const double sp = std::sqrt(2.0) * static_cast<double>(indices.size()) / dp;
    Mat3 tc, tp;
    tc << sc, 0, -sc * c.x(), 0, sc, -sc * c.y(), 0, 0, 1;
    tp << sp, 0, -sp * p.x(), 0, sp, -sp * p.y(), 0, 0, 1;
    // Fixed capacity avoids allocating a dynamic SVD workspace each update.
    Eigen::Matrix<double, 2 * tracking::kLandmarkNameCount, 9> a;
    a.setZero();
    Eigen::Index row = 0;
    for (auto i : indices) {
        const Vec2 u = (pairs[i].canonical - c) * sc;
        const Vec2 x = (pairs[i].image - p) * sp;
        a.row(row++) << -u.x(), -u.y(), -1, 0, 0, 0, x.x() * u.x(), x.x() * u.y(), x.x();
        a.row(row++) << 0, 0, 0, -u.x(), -u.y(), -1, x.y() * u.x(), x.y() * u.y(), x.y();
    }
    Eigen::Matrix<double, 9, 1> v;
    if (indices.size() == 4) {
        // Minimal RANSAC hypothesis: fix the homogeneous gauge h33=1 and
        // solve the same normalised DLT equations with pivoted LU. Singular
        // gauges are discarded; the final consensus always uses SVD below.
        const Eigen::Matrix<double, 8, 8> minimal = a.topLeftCorner<8, 8>();
        const Eigen::FullPivLU<Eigen::Matrix<double, 8, 8>> lu(minimal);
        if (lu.rank() != 8) {
            return false;
        }
        v.head<8>() = lu.solve(-a.col(8).head<8>());
        v(8) = 1;
    } else {
        const Eigen::JacobiSVD<decltype(a)> svd(a, Eigen::ComputeFullV);
        if (svd.singularValues()(7) < 1e-8 * svd.singularValues()(0)) {
            return false;
        }
        v = svd.matrixV().col(8);
    }
    Mat3 normal;
    normal << v(0), v(1), v(2), v(3), v(4), v(5), v(6), v(7), v(8);
    h = tp.inverse() * normal * tc;
    if (!h.allFinite() || std::abs(h(2, 2)) < 1e-12) {
        return false;
    }
    h /= h(2, 2);
    return std::abs(h.determinant()) > 1e-12;
}

double error(const Pairs& pairs, const std::vector<std::size_t>& indices, const Mat3& h) {
    double sum = 0;
    for (auto i : indices) {
        sum += (project(h, pairs[i].canonical) - pairs[i].image).squaredNorm();
    }
    return sum;
}

void refine(const Pairs& pairs, const std::vector<std::size_t>& indices, Mat3& h) {
    for (int iteration = 0; iteration < 3; ++iteration) {
        Eigen::Matrix<double, 8, 8> normal = Eigen::Matrix<double, 8, 8>::Zero();
        Eigen::Matrix<double, 8, 1> rhs = Eigen::Matrix<double, 8, 1>::Zero();
        for (auto i : indices) {
            const auto& pair = pairs[i];
            const double u = pair.canonical.x(), v = pair.canonical.y();
            const double w = h(2, 0) * u + h(2, 1) * v + 1;
            if (std::abs(w) < 1e-12) {
                return;
            }
            const Vec2 p = project(h, pair.canonical);
            Eigen::Matrix<double, 2, 8> j;
            j << u, v, 1, 0, 0, 0, -p.x() * u, -p.x() * v, 0, 0, 0, u, v, 1, -p.y() * u, -p.y() * v;
            j /= w;
            normal.noalias() += j.transpose() * j;
            rhs.noalias() += j.transpose() * (pair.image - p);
        }
        const Eigen::Matrix<double, 8, 1> delta = normal.ldlt().solve(rhs);
        if (!delta.allFinite()) {
            return;
        }
        Mat3 candidate = h;
        for (int i = 0; i < 8; ++i) {
            candidate(i / 3, i % 3) += delta(i);
        }
        if (error(pairs, indices, candidate) > error(pairs, indices, h)) {
            return;
        }
        h = candidate;
        if (delta.norm() < 1e-10) {
            break;
        }
    }
}

std::vector<std::size_t> inliers(const Pairs& pairs, const Mat3& h) {
    std::vector<std::size_t> result;
    result.reserve(pairs.size());
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        if ((project(h, pairs[i].canonical) - pairs[i].image).squaredNorm() <=
            kRansacThreshold * kRansacThreshold) {
            result.push_back(i);
        }
    }
    return result;
}

bool safe_homography(const Mat3& h) {
    if (!h.allFinite() || std::abs(h.determinant()) < 1e-12) {
        return false;
    }
    // No projective pole through the canonical face envelope.
    double sign = 0;
    for (double u : {-1.5, 1.5}) {
        for (double v : {-2.0, 1.5}) {
            const double w = h(2, 0) * u + h(2, 1) * v + 1;
            if (!std::isfinite(w) || std::abs(w) < 1e-6 || (sign != 0 && w * sign < 0)) {
                return false;
            }
            sign = w;
        }
    }
    return true;
}

bool solve(const Pairs& pairs, Mat3& h, std::size_t& count) {
    count = 0;
    if (pairs.size() < kMinimumInliers) {
        return false;
    }
    std::vector<std::size_t> all;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        all.push_back(i);
    }
    Mat3 candidate;
    std::vector<std::size_t> best;
    double best_error = 1e30;
    auto consider = [&] {
        auto subset = inliers(pairs, candidate);
        const double e = error(pairs, subset, candidate);
        if (subset.size() > best.size() || (subset.size() == best.size() && e < best_error)) {
            best = std::move(subset);
            best_error = e;
        }
    };
    if (dlt(pairs, all, candidate)) {
        consider();
        if (best.size() == pairs.size()) {
            h = candidate;
            refine(pairs, best, h);
            count = inliers(pairs, h).size();
            return count >= kMinimumInliers && count * 10 >= pairs.size() * 6 && safe_homography(h);
        }
    }
    // Deterministic RANSAC, identical seeds for identical inputs, at most 48
    // four-point hypotheses. The all-inlier path needs only one DLT.
    std::uint32_t random = 0x18FACEu;
    int trials = 48;
    for (int trial = 0; trial < trials; ++trial) {
        std::vector<std::size_t> sample;
        while (sample.size() < 4) {
            random = random * 1664525u + 1013904223u;
            const std::size_t i = (random >> 8) % pairs.size();
            if (std::find(sample.begin(), sample.end(), i) == sample.end()) {
                sample.push_back(i);
            }
        }
        if (dlt(pairs, sample, candidate)) {
            consider();
            // Adapt the work to the consensus: 99.9% probability of drawing
            // at least one all-inlier four-point sample, bounded at 48 trials.
            if (best.size() >= kMinimumInliers) {
                const double ratio =
                    static_cast<double>(best.size()) / static_cast<double>(pairs.size());
                const double miss = 1.0 - std::pow(ratio, 4);
                const int needed =
                    miss <= 0 ? 1 : static_cast<int>(std::ceil(std::log(0.001) / std::log(miss)));
                trials = std::min(trials, needed);
            }
        }
    }
    if (best.size() < kMinimumInliers || best.size() * 10 < pairs.size() * 6) {
        return false;
    }
    if (!dlt(pairs, best, h)) {
        return false;
    }
    refine(pairs, best, h);
    count = inliers(pairs, h).size();
    return count >= kMinimumInliers && count * 10 >= pairs.size() * 6 && safe_homography(h);
}

double alpha(double cutoff, double dt) {
    return 1.0 / (1.0 + 1.0 / (2.0 * kPi * cutoff * dt));
}

struct Euro {
    double previous = 0, filtered = 0, derivative = 0;
    bool started = false;
    double step(double value, double dt, double cutoff) {
        if (!started) {
            started = true;
            previous = filtered = value;
            return value;
        }
        const double a = alpha(1.0, dt);
        derivative += a * ((value - previous) / dt - derivative);
        previous = value;
        filtered += alpha(cutoff + 0.02 * std::abs(derivative), dt) * (value - filtered);
        return filtered;
    }
};

Quat quaternion(const tracking::HeadPose& p) {
    return Quat(p.rotation[3], p.rotation[0], p.rotation[1], p.rotation[2]);
}

Vec3 angles(const Quat& q) {
    const Mat3 r = q.toRotationMatrix();
    return Vec3(std::atan2(r(1, 0), r(0, 0)), std::asin(std::clamp(-r(2, 0), -1.0, 1.0)),
                std::atan2(r(2, 1), r(2, 2))) *
           kDegrees; // roll(z), yaw(y), pitch(x)
}

Quat rotation(const Vec3& degrees) {
    return Quat(Eigen::AngleAxisd(degrees.x() / kDegrees, Vec3::UnitZ()) *
                Eigen::AngleAxisd(degrees.y() / kDegrees, Vec3::UnitY()) *
                Eigen::AngleAxisd(degrees.z() / kDegrees, Vec3::UnitX()));
}

double seconds(core::TimestampNs later, core::TimestampNs earlier) {
    return later > earlier ? static_cast<double>(later - earlier) * 1e-9 : 0.0;
}

std::optional<Vec2> centroid(const tracking::LandmarkMap& map, const tracking::LandmarkSet& points,
                             int region) {
    const std::array<std::array<LandmarkName, 4>, 3> names{
        {{LandmarkName::eye_outer_l, LandmarkName::eye_inner_l, LandmarkName::iris_center_l,
          LandmarkName::count},
         {LandmarkName::eye_outer_r, LandmarkName::eye_inner_r, LandmarkName::iris_center_r,
          LandmarkName::count},
         {LandmarkName::mouth_corner_l, LandmarkName::mouth_corner_r,
          LandmarkName::mouth_upper_center, LandmarkName::mouth_lower_center}}};
    Vec2 sum = Vec2::Zero();
    std::size_t count = 0;
    for (const auto name : names[static_cast<std::size_t>(region)]) {
        if (name == LandmarkName::count) {
            continue;
        }
        // Irises are optional. Corners and both lip centres are required.
        if (!map.usable(points, name)) {
            if (name == LandmarkName::iris_center_l || name == LandmarkName::iris_center_r) {
                continue;
            }
            return std::nullopt;
        }
        const auto point = *map.point(points, name);
        sum += Vec2(point.x, point.y);
        ++count;
    }
    return Vec2(sum / static_cast<double>(count));
}
} // namespace

struct FaceRig::State {
    static constexpr std::size_t kPendingCapacity = 4;

    tracking::LandmarkMap map;
    float aspect;
    tracking::ProfileLandmarks profile;
    std::array<tracking::TrackingResult, kPendingCapacity> pending{};
    std::size_t pending_count = 0;
    std::uint64_t pending_dropped = 0;
    bool seen = false, pose_started = false, have_h = false, have_good = false;
    core::TimestampNs seen_ns = 0, pose_ns = 0, h_ns = 0, good_ns = 0;
    core::TimestampNs source_ns = 0;
    std::array<Euro, 3> rotation_filters{}, translation_filters{};
    Vec3 euler = Vec3::Zero(), translation = Vec3::Zero();
    Vec3 angular_velocity = Vec3::Zero(), velocity = Vec3::Zero();
    Mat3 h = Mat3::Identity(), h_velocity = Mat3::Zero();
    Quat rejected = Quat::Identity();
    int rejects = 0;
    std::array<Vec2, 3> rest{}, anchors{};
    float confidence = 0;
    std::size_t inlier_count = 0;
    bool subset = false, failed = false;

    State(tracking::LandmarkMap landmarks, float image_aspect)
        : map(std::move(landmarks)), aspect(image_aspect) {}
};

FaceRig::FaceRig(tracking::LandmarkMap map, float aspect)
    : state_(std::make_unique<State>(std::move(map), aspect)) {}
FaceRig::~FaceRig() = default;
FaceRig::FaceRig(FaceRig&&) noexcept = default;
FaceRig& FaceRig::operator=(FaceRig&&) noexcept = default;

void FaceRig::configure(const tracking::ProfileLandmarks& profile) {
    reset();
    state_->profile = profile;
}

void FaceRig::reset() {
    auto profile = state_->profile;
    auto map = state_->map;
    const auto aspect = state_->aspect;
    state_ = std::make_unique<State>(std::move(map), aspect);
    state_->profile = std::move(profile);
}

void FaceRig::submit(const tracking::TrackingResult& result) {
    auto& s = *state_;
    if (s.seen && result.source_timestamp_ns <= s.seen_ns) {
        return;
    }
    for (std::size_t i = 0; i < s.pending_count; ++i) {
        if (s.pending[i].source_timestamp_ns == result.source_timestamp_ns) {
            return;
        }
    }
    if (s.pending_count == State::kPendingCapacity) {
        // Keep the newest results: evict the oldest buffered entry. This is a
        // bounded buffer, not a queue.
        std::size_t oldest = 0;
        for (std::size_t i = 1; i < s.pending_count; ++i) {
            if (s.pending[i].source_timestamp_ns < s.pending[oldest].source_timestamp_ns) {
                oldest = i;
            }
        }
        for (std::size_t i = oldest + 1; i < s.pending_count; ++i) {
            s.pending[i - 1] = s.pending[i];
        }
        --s.pending_count;
        ++s.pending_dropped;
    }
    s.pending[s.pending_count++] = result;
}

std::size_t FaceRig::pending_count() const noexcept {
    return state_->pending_count;
}

std::uint64_t FaceRig::pending_dropped() const noexcept {
    return state_->pending_dropped;
}

void FaceRig::apply_pending(core::TimestampNs render_time_ns) {
    auto& s = *state_;
    // Drop entries that can never be applied: they are older than the loss
    // window and the render clock only moves forward.
    std::size_t write = 0;
    for (std::size_t i = 0; i < s.pending_count; ++i) {
        const auto timestamp = s.pending[i].source_timestamp_ns;
        if (render_time_ns > timestamp && render_time_ns - timestamp > kHoldNs) {
            ++s.pending_dropped;
            continue;
        }
        if (write != i) {
            s.pending[write] = s.pending[i];
        }
        ++write;
    }
    s.pending_count = write;

    std::size_t best = s.pending_count;
    for (std::size_t i = 0; i < s.pending_count; ++i) {
        const auto timestamp = s.pending[i].source_timestamp_ns;
        if (timestamp > render_time_ns) {
            continue;
        }
        if (best == s.pending_count || timestamp > s.pending[best].source_timestamp_ns) {
            best = i;
        }
    }
    if (best == s.pending_count) {
        return;
    }

    const tracking::TrackingResult chosen = s.pending[best];
    // Everything at or before the chosen timestamp is superseded. Future
    // results stay buffered for a later render frame.
    write = 0;
    for (std::size_t i = 0; i < s.pending_count; ++i) {
        if (s.pending[i].source_timestamp_ns <= chosen.source_timestamp_ns) {
            continue;
        }
        if (write != i) {
            s.pending[write] = s.pending[i];
        }
        ++write;
    }
    s.pending_count = write;
    (void)apply_result(chosen, render_time_ns);
}

FaceRigOutput FaceRig::update(const tracking::TrackingResult& result,
                              core::TimestampNs render_time_ns) {
    submit(result);
    return sample(render_time_ns);
}

FaceRigOutput FaceRig::sample(core::TimestampNs render_time_ns) {
    apply_pending(render_time_ns);
    return sample_current(render_time_ns);
}

FaceRigOutput FaceRig::apply_result(const tracking::TrackingResult& result,
                                    core::TimestampNs render_time_ns) {
    auto& s = *state_;
    const auto now = result.source_timestamp_ns;
    if (s.seen && now <= s.seen_ns) {
        return sample_current(render_time_ns);
    }
    s.seen = true;
    s.seen_ns = now;
    const Quat input = quaternion(result.pose);
    const Vec3 input_translation(result.pose.translation[0], result.pose.translation[1],
                                 result.pose.translation[2]);
    if (!result.valid || !result.features_valid || !std::isfinite(result.confidence) ||
        result.confidence <= 0 || !input.coeffs().allFinite() || input.norm() < 1e-8 ||
        !input_translation.allFinite()) {
        s.rejects = 0;
        return sample_current(render_time_ns);
    }
    const Quat q = input.normalized();
    if (s.pose_started) {
        const double horizon = std::min(kPredictionSeconds, seconds(now, s.pose_ns));
        const Quat expected = rotation(s.euler + s.angular_velocity * horizon);
        if (expected.angularDistance(q) * kDegrees > 35.0 ||
            (input_translation - s.translation - s.velocity * horizon).norm() > 0.25) {
            if (s.rejects == 0 || s.rejected.angularDistance(q) * kDegrees > 5.0) {
                s.rejects = 1;
            } else {
                ++s.rejects;
            }
            s.rejected = q;
            if (s.rejects < 3) {
                return sample_current(render_time_ns);
            }
        }
    }
    s.rejects = 0;
    if (s.profile.landmarks.empty()) {
        const auto frame =
            tracking::CanonicalFaceFrame::from_landmarks(result.landmarks, s.map, s.aspect);
        if (!frame) {
            return sample_current(render_time_ns);
        }
        tracking::ProfileLandmarks profile;
        for (std::size_t i = 0; i < tracking::kLandmarkNameCount; ++i) {
            const auto name = static_cast<LandmarkName>(i);
            if (stable(name) && s.map.usable(result.landmarks, name)) {
                const auto uv = frame->project(*s.map.point(result.landmarks, name));
                profile.landmarks.push_back({tracking::landmark_name(name), uv[0], uv[1]});
            }
        }
        if (profile.landmarks.size() < kMinimumInliers) {
            return sample_current(render_time_ns);
        }
        s.profile = std::move(profile);
    }
    const auto correspondences =
        tracking::build_profile_correspondences(s.profile, s.map, result.landmarks);
    s.subset = !correspondences.profile_complete;
    Pairs pairs;
    for (const auto& pair : correspondences.correspondences) {
        if (stable(pair.name) && std::isfinite(pair.u) && std::isfinite(pair.v)) {
            pairs.push_back({Vec2(pair.u, pair.v), Vec2(pair.x, pair.y)});
        }
    }
    Mat3 h;
    std::size_t count = 0;
    s.failed = !solve(pairs, h, count);
    if (s.failed) {
        s.confidence *= 0.5f;
        return sample_current(render_time_ns);
    }
    std::array<Vec2, 3> anchors;
    const Mat3 inverse = h.inverse();
    for (int i = 0; i < 3; ++i) {
        const auto point = centroid(s.map, result.landmarks, i);
        if (!point) {
            return sample_current(render_time_ns);
        }
        anchors[static_cast<std::size_t>(i)] = project(inverse, *point);
        if (!anchors[static_cast<std::size_t>(i)].allFinite()) {
            return sample_current(render_time_ns);
        }
    }
    // Long gaps use the same bounded filter step as normal updates. Reacquisition
    // blends from the last state; no reset/neutral pose is injected on loss.
    const double elapsed = s.pose_started ? std::max(1e-6, seconds(now, s.pose_ns)) : 1.0 / 60;
    const double dt = std::min(elapsed, 0.05);
    Vec3 measured = angles(q);
    const Vec3 old_angles = s.euler, old_translation = s.translation;
    for (int i = 0; i < 3; ++i) {
        if (s.pose_started) {
            measured[i] = s.euler[i] + std::remainder(measured[i] - s.euler[i], 360.0);
        }
        double value = s.rotation_filters[static_cast<std::size_t>(i)].step(measured[i], dt, 1.0);
        if (s.pose_started && std::abs(value - s.euler[i]) < 0.15 &&
            std::abs(s.rotation_filters[static_cast<std::size_t>(i)].derivative) < 3.0) {
            value = s.euler[i];
        }
        s.euler[i] = value;
        s.translation[i] =
            s.translation_filters[static_cast<std::size_t>(i)].step(input_translation[i], dt, 2.0);
    }
    if (s.pose_started) {
        s.angular_velocity = (s.euler - old_angles) / elapsed;
        s.velocity = (s.translation - old_translation) / elapsed;
    }
    s.pose_started = true;
    s.pose_ns = now;
    if (s.have_h) {
        const double h_dt = std::max(1e-6, seconds(now, s.h_ns));
        s.h_velocity = (h - s.h) / h_dt;
    } else {
        s.rest = anchors;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (!s.have_h) {
            s.anchors[i] = anchors[i];
        } else {
            const double a = 1.0 - std::exp(-seconds(now, s.h_ns) / 0.150);
            s.anchors[i] += a * (anchors[i] - s.anchors[i]);
        }
        s.anchors[i] = s.anchors[i]
                           .cwiseMax(s.rest[i] - Vec2::Constant(0.12))
                           .cwiseMin(s.rest[i] + Vec2::Constant(0.12));
    }
    s.h = h;
    s.h_ns = now;
    s.good_ns = now;
    s.source_ns = now;
    s.have_good = s.have_h = true;
    s.inlier_count = count;
    s.confidence = std::clamp(result.confidence, 0.0f, 1.0f) * static_cast<float>(count) /
                   static_cast<float>(pairs.size());
    return sample_current(render_time_ns);
}

FaceRigOutput FaceRig::sample_current(core::TimestampNs render_time_ns) const {
    const auto& s = *state_;
    FaceRigOutput output;
    output.source_timestamp_ns = s.source_ns;
    output.profile_subset = s.subset;
    output.solve_failed = s.failed;
    output.inlier_count = s.inlier_count;
    if (!s.have_good) {
        return output;
    }
    const double age = seconds(render_time_ns, s.good_ns);
    const bool held = render_time_ns > s.good_ns && render_time_ns - s.good_ns > kHoldNs;
    output.features_valid = !held;
    output.confidence = held ? 0 : s.confidence;
    const double horizon = std::min(kPredictionSeconds, age);
    const Quat q = rotation(s.euler + s.angular_velocity * horizon);
    const Vec3 t = s.translation + s.velocity * horizon;
    const Mat3 inverse_rotation = q.toRotationMatrix().transpose();
    const Vec3 inverse_translation = -inverse_rotation * t;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            output.head_from_camera[static_cast<std::size_t>(row * 4 + col)] =
                static_cast<float>(inverse_rotation(row, col));
        }
        output.head_from_camera[static_cast<std::size_t>(row * 4 + 3)] =
            static_cast<float>(inverse_translation(row));
    }
    // H samples the source image, not the predicted head pose. Extrapolate only
    // during failed solves, with damping, and never beyond the loss window.
    Mat3 image_h = s.h;
    if (s.failed) {
        const Mat3 candidate = s.h + s.h_velocity * (horizon * std::exp(-age / 0.050));
        double max_shift = 0;
        for (double u : {-1.5, 1.5}) {
            for (double v : {-2.0, 1.5}) {
                max_shift = std::max(
                    max_shift, (project(candidate, Vec2(u, v)) - project(s.h, Vec2(u, v))).norm());
            }
        }
        if (safe_homography(candidate) && max_shift <= kRansacThreshold) {
            image_h = candidate;
        }
    }
    for (int i = 0; i < 9; ++i) {
        output.canonical_to_image[static_cast<std::size_t>(i)] =
            static_cast<float>(image_h(i / 3, i % 3));
    }
    for (std::size_t i = 0; i < 3; ++i) {
        output.regions[i] = {static_cast<float>(s.anchors[i].x()),
                             static_cast<float>(s.anchors[i].y())};
    }
    return output;
}
} // namespace avatar::rig
