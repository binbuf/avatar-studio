/*
 * Compiles the tracker C ABI from C, with the project's warning policy.
 *
 * This translation unit is deliberately not C++: the header must be consumable
 * by a C host and must not require a C++ compiler, a standard-library header or
 * a name-mangled symbol. The static assertions mirror the layout table in the
 * header, so a field move fails the build rather than silently corrupting the
 * ABI.
 */

#include "avatar/tracker/tracker_c_api.h"

#include <stddef.h>
#include <stdint.h>

#define AVATAR_C_ASSERT(condition, name) typedef char avatar_c_assert_##name[(condition) ? 1 : -1]

AVATAR_C_ASSERT(AVATAR_TRACKER_ABI_VERSION == 1u, abi_version);
AVATAR_C_ASSERT(AVATAR_TRACKER_LANDMARK_CAPACITY == 478u, landmark_capacity);
AVATAR_C_ASSERT(AVATAR_TRACKER_BLENDSHAPE_COUNT == 52u, blendshape_count);

AVATAR_C_ASSERT(sizeof(avatar_tracker_head_pose) == 28u, head_pose_size);
AVATAR_C_ASSERT(sizeof(avatar_tracker_landmark) == 12u, landmark_size);
AVATAR_C_ASSERT(sizeof(avatar_tracker_config) == 48u, config_size);
AVATAR_C_ASSERT(offsetof(avatar_tracker_config, abi_version) == 0u, config_abi_version);
AVATAR_C_ASSERT(offsetof(avatar_tracker_config, frame_interval_ns) == 24u, config_interval);
AVATAR_C_ASSERT(offsetof(avatar_tracker_config, simulate_fault) == 40u, config_fault);

AVATAR_C_ASSERT(sizeof(avatar_tracker_result) == 5992u, result_size);
AVATAR_C_ASSERT(offsetof(avatar_tracker_result, source_timestamp_ns) == 0u, result_timestamp);
AVATAR_C_ASSERT(offsetof(avatar_tracker_result, confidence) == 12u, result_confidence);
AVATAR_C_ASSERT(offsetof(avatar_tracker_result, pose) == 16u, result_pose);
AVATAR_C_ASSERT(offsetof(avatar_tracker_result, landmark_count) == 44u, result_landmark_count);
AVATAR_C_ASSERT(offsetof(avatar_tracker_result, landmarks) == 48u, result_landmarks);
AVATAR_C_ASSERT(offsetof(avatar_tracker_result, blendshapes) == 5784u, result_blendshapes);

#if UINTPTR_MAX == UINT64_MAX
AVATAR_C_ASSERT(sizeof(avatar_tracker_frame) == 88u, frame_size_64);
#else
AVATAR_C_ASSERT(sizeof(avatar_tracker_frame) == 64u, frame_size_32);
#endif

int main(void) {
    /* Exercise the public aggregate types and the documented initialiser. */
    avatar_tracker_config config = AVATAR_TRACKER_CONFIG_INIT;
    avatar_tracker_frame frame = {0};
    avatar_tracker_result result = {0};

    frame.timestamp_ns = 0u;
    frame.plane_count = 0u;
    frame.plane0 = NULL;
    frame.plane1 = NULL;
    frame.plane2 = NULL;

    result.source_timestamp_ns = 0u;
    result.valid = 0u;
    result.features_valid = 0u;
    result.confidence = 0.0f;

    return (config.abi_version == AVATAR_TRACKER_ABI_VERSION && frame.plane_count == 0u &&
            result.landmark_count == 0u)
               ? 0
               : 1;
}
