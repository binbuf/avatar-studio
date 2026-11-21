#include "avatar/rig/head_region.h"
#include "test_registry.h"
#include <limits>
using namespace avatar::rig;
TEST_CASE(head_weight_rest_space_endpoints_and_smooth_band) {
    const HeadPoint origin{0, 1, 0}, normal{0, 1, 0};
    CHECK_EQ(head_weight({0, 0, 0}, origin, normal, .4f), 0.0f);
    CHECK_EQ(head_weight(origin, origin, normal, .4f), 0.0f);
    CHECK(std::abs(head_weight({0, 1.2f, 0}, origin, normal, .4f) - .5f) < 1e-6f);
    CHECK_EQ(head_weight({0, 2, 0}, origin, normal, .4f), 1.0f);
    CHECK_EQ(head_weight({0, 2, 0}, origin, normal, 0), 0.0f);
    CHECK(head_weight({0, 1.0001f, 0}, origin, normal, .4f) < 1e-6f);
    CHECK(std::abs(head_weight({2, 0, 0}, {1, 0, 0}, {1, 0, 0}, 2) - .5f) < 1e-6f);
}
TEST_CASE(head_pivot_uses_face_surface_and_scale) {
    const auto pivot = head_pivot({1, 2, 3}, {0, 0, -1}, .4f);
    CHECK_EQ(pivot[0], 1.0f);
    CHECK_EQ(pivot[1], 2.0f);
    CHECK(std::abs(pivot[2] - 3.3f) < 1e-6f);
}
TEST_CASE(head_rotation_shortest_arc_and_normal_length) {
    const float angle = .6f;
    const HeadRotation positive({0, std::sin(angle / 2), 0, std::cos(angle / 2)});
    const HeadRotation negative({0, -std::sin(angle / 2), 0, -std::cos(angle / 2)});
    const auto a = positive.rotate({1, 0, 0}, std::sin(angle * .5f), std::cos(angle * .5f));
    const auto b =
        negative.rotate({1, 0, 0}, std::sin(negative.angle * .5f), std::cos(negative.angle * .5f));
    CHECK(std::abs(a[0] - std::cos(.3f)) < 1e-6f);
    CHECK(std::abs(a[2] + std::sin(.3f)) < 1e-6f);
    CHECK(std::abs(a[0] - b[0]) + std::abs(a[2] - b[2]) < 1e-6f);
    CHECK(std::abs(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] - 1) < 1e-6f);
}
