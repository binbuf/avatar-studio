#ifndef AVATAR_TRACKER_TRACKER_C_API_H
#define AVATAR_TRACKER_TRACKER_C_API_H

/*
 * Avatar Studio tracker C ABI, version 1.
 *
 * This header is the only thing the application knows about a tracker backend.
 * A real backend (MediaPipe, ONNX Runtime, ...) is compiled into its own shared
 * library behind these five functions. The application never includes a vendor
 * header, and no C++ standard-library type crosses this boundary: every struct
 * is POD, every array is fixed-size, and every function returns an error code
 * instead of throwing.
 *
 * Contract (docs/design/tracking.md):
 *   - one face, one result per accepted frame;
 *   - `push_frame` never blocks and never queues. If the backend is busy the
 *     frame is dropped inside the backend;
 *   - `poll_result` returns only the newest unconsumed result, or NO_RESULT;
 *   - `source_timestamp_ns` is always the input frame's timestamp;
 *   - `features_valid` is stricter than `valid` and gates the output.
 *
 * ABI rules:
 *   - Bump AVATAR_TRACKER_ABI_VERSION for any layout or semantic change.
 *   - A caller that passes a different version gets ABI_MISMATCH; it must not
 *     attempt to interpret results from that library.
 *   - The layout below is pointer-size dependent only in `avatar_tracker_frame`
 *     (it contains pointers). Both sides of the ABI are built for the same
 *     process and pointer width.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AVATAR_TRACKER_ABI_VERSION 1u
#define AVATAR_TRACKER_LANDMARK_CAPACITY 478u
#define AVATAR_TRACKER_BLENDSHAPE_COUNT 52u

/* Symbol visibility. The default is a static/ordinary declaration; a shared
 * library build defines AVATAR_TRACKER_SHARED and (when building the library
 * itself) AVATAR_TRACKER_BUILDING. */
#if defined(AVATAR_TRACKER_SHARED) && defined(_WIN32)
#if defined(AVATAR_TRACKER_BUILDING)
#define AVATAR_TRACKER_API __declspec(dllexport)
#else
#define AVATAR_TRACKER_API __declspec(dllimport)
#endif
#elif defined(AVATAR_TRACKER_SHARED)
#define AVATAR_TRACKER_API __attribute__((visibility("default")))
#else
#define AVATAR_TRACKER_API
#endif

/* ------------------------------------------------------------------------- */
/* Error and status codes                                                    */
/* ------------------------------------------------------------------------- */

/* Zero is success, positive values are non-error status, negative values are
 * errors. `AVATAR_TRACKER_ERROR_BUSY` is reserved: push_frame drops a frame
 * rather than returning it, so a caller never has to retry. */
typedef enum avatar_tracker_error {
    AVATAR_TRACKER_OK = 0,
    AVATAR_TRACKER_NO_RESULT = 1,
    AVATAR_TRACKER_ERROR_INVALID_ARGUMENT = -1,
    AVATAR_TRACKER_ERROR_ABI_MISMATCH = -2,
    AVATAR_TRACKER_ERROR_INVALID_CONFIG = -3,
    AVATAR_TRACKER_ERROR_NOT_INITIALIZED = -4,
    AVATAR_TRACKER_ERROR_BUSY = -5,
    AVATAR_TRACKER_ERROR_FRAME_TOO_LARGE = -6,
    AVATAR_TRACKER_ERROR_INTERNAL = -7
} avatar_tracker_error;

/* ------------------------------------------------------------------------- */
/* Input description                                                         */
/* ------------------------------------------------------------------------- */

typedef enum avatar_tracker_scenario {
    AVATAR_TRACKER_SCENARIO_VALID_TRACKING = 0,
    AVATAR_TRACKER_SCENARIO_DROPOUTS = 1,
    AVATAR_TRACKER_SCENARIO_JITTER = 2,
    AVATAR_TRACKER_SCENARIO_OUTLIERS = 3,
    AVATAR_TRACKER_SCENARIO_MISSING_MOUTH = 4,
    AVATAR_TRACKER_SCENARIO_LEFT_RIGHT_SWAP = 5
} avatar_tracker_scenario;

/* Mirrors avatar::capture::PixelFormat. A backend rejects a format it cannot
 * consume; the scripted backend accepts every value. */
typedef enum avatar_tracker_pixel_format {
    AVATAR_TRACKER_PIXEL_FORMAT_NV12 = 0,
    AVATAR_TRACKER_PIXEL_FORMAT_YUY2 = 1,
    AVATAR_TRACKER_PIXEL_FORMAT_MJPEG = 2,
    AVATAR_TRACKER_PIXEL_FORMAT_BGRA8 = 3,
    AVATAR_TRACKER_PIXEL_FORMAT_RGBA8 = 4
} avatar_tracker_pixel_format;

/* Test-only fault injection. It exists so the contract suite can prove that a
 * backend exception is contained at the ABI boundary. Production callers leave
 * it at AVATAR_TRACKER_FAULT_NONE. */
#define AVATAR_TRACKER_FAULT_NONE 0u
#define AVATAR_TRACKER_FAULT_CONFIGURE 1u
#define AVATAR_TRACKER_FAULT_PUSH 2u
#define AVATAR_TRACKER_FAULT_POLL 3u

/*
 * Configuration.
 *
 * Layout (64-bit and 32-bit, natural alignment):
 *   offset  0  uint32_t abi_version
 *   offset  4  uint32_t scenario
 *   offset  8  uint32_t frame_count
 *   offset 12  uint32_t seed
 *   offset 16  float    base_confidence
 *   offset 20  (4 bytes padding)
 *   offset 24  uint64_t frame_interval_ns
 *   offset 32  uint32_t input_width       (0 = unknown/backend chooses)
 *   offset 36  uint32_t input_height      (0 = unknown/backend chooses)
 *   offset 40  uint32_t simulate_fault
 *   offset 44  uint32_t reserved          (must be zero)
 *   sizeof = 48, alignof = 8
 *
 * A zero-initialised config is NOT valid: abi_version must be set. Use
 * AVATAR_TRACKER_CONFIG_INIT for the documented defaults.
 */
typedef struct avatar_tracker_config {
    uint32_t abi_version;
    uint32_t scenario;
    uint32_t frame_count;
    uint32_t seed;
    float base_confidence;
    uint64_t frame_interval_ns;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t simulate_fault;
    uint32_t reserved;
} avatar_tracker_config;

/* Positional initialiser matching the field order above. */
#define AVATAR_TRACKER_CONFIG_INIT                                                                 \
    {AVATAR_TRACKER_ABI_VERSION,                                                                   \
     AVATAR_TRACKER_SCENARIO_VALID_TRACKING,                                                       \
     60u,                                                                                          \
     0x5EED1234u,                                                                                  \
     0.92f,                                                                                        \
     16666667ull,                                                                                  \
     0u,                                                                                           \
     0u,                                                                                           \
     AVATAR_TRACKER_FAULT_NONE,                                                                    \
     0u}

/*
 * One input frame.
 *
 * The pixel pointers are borrowed for the duration of push_frame only. A
 * backend that needs pixels after the call returns must copy them into its own
 * storage. `plane_count` is 0 for a metadata-only frame (the scripted backend
 * needs nothing else); otherwise it is 1..3 and unused pointers must be NULL.
 *
 * Layout on a 64-bit target:
 *   offset  0  uint64_t       timestamp_ns
 *   offset  8  uint64_t       arrival_ns
 *   offset 16  uint32_t       sequence
 *   offset 20  uint32_t       width
 *   offset 24  uint32_t       height
 *   offset 28  uint32_t       format
 *   offset 32  uint32_t       stride
 *   offset 36  uint32_t       plane_count
 *   offset 40  const uint8_t* plane0
 *   offset 48  const uint8_t* plane1
 *   offset 56  const uint8_t* plane2
 *   offset 64  size_t         plane0_size
 *   offset 72  size_t         plane1_size
 *   offset 80  size_t         plane2_size
 *   sizeof = 88, alignof = 8
 * On a 32-bit target the pointer/size_t fields are 4 bytes and sizeof = 64.
 */
typedef struct avatar_tracker_frame {
    uint64_t timestamp_ns;
    uint64_t arrival_ns;
    uint32_t sequence;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
    uint32_t plane_count;
    const uint8_t* plane0;
    const uint8_t* plane1;
    const uint8_t* plane2;
    size_t plane0_size;
    size_t plane1_size;
    size_t plane2_size;
} avatar_tracker_frame;

/* ------------------------------------------------------------------------- */
/* Result                                                                    */
/* ------------------------------------------------------------------------- */

/* Head pose in camera space: right-handed, -Z forward, metres.
 * `rotation` is a unit quaternion in [x, y, z, w] order.
 * sizeof = 28, alignof = 4. */
typedef struct avatar_tracker_head_pose {
    float rotation[4];
    float translation[3];
} avatar_tracker_head_pose;

/* One landmark: normalised image coordinates plus relative depth.
 * sizeof = 12, alignof = 4. */
typedef struct avatar_tracker_landmark {
    float x;
    float y;
    float z;
} avatar_tracker_landmark;

/*
 * One tracking result.
 *
 * Layout (64-bit and 32-bit, natural alignment):
 *   offset    0  uint64_t                  source_timestamp_ns
 *   offset    8  uint8_t                   valid              (0 or 1)
 *   offset    9  uint8_t                   features_valid     (0 or 1)
 *   offset   10  uint8_t                   reserved[2]        (zero)
 *   offset   12  float                     confidence         (0..1)
 *   offset   16  avatar_tracker_head_pose  pose               (28 bytes)
 *   offset   44  uint32_t                  landmark_count     (<= 478)
 *   offset   48  avatar_tracker_landmark   landmarks[478]     (5736 bytes)
 *   offset 5784  float                     blendshapes[52]    (208 bytes)
 *   sizeof = 5992, alignof = 8
 *
 * Entries at or beyond `landmark_count` are zero. All 52 blendshapes are always
 * present; a backend without expression output leaves them zero. The reserved
 * bytes are zero and must not be interpreted.
 */
typedef struct avatar_tracker_result {
    uint64_t source_timestamp_ns;
    uint8_t valid;
    uint8_t features_valid;
    uint8_t reserved[2];
    float confidence;
    avatar_tracker_head_pose pose;
    uint32_t landmark_count;
    avatar_tracker_landmark landmarks[AVATAR_TRACKER_LANDMARK_CAPACITY];
    float blendshapes[AVATAR_TRACKER_BLENDSHAPE_COUNT];
} avatar_tracker_result;

/* ------------------------------------------------------------------------- */
/* Functions                                                                 */
/* ------------------------------------------------------------------------- */

/* Opaque backend handle. The caller owns exactly one and must destroy it. */
typedef struct avatar_tracker_handle avatar_tracker_handle;

/*
 * Creates a backend.
 *
 * `config` may be NULL, in which case the AVATAR_TRACKER_CONFIG_INIT defaults
 * are used. A non-NULL config whose abi_version differs from
 * AVATAR_TRACKER_ABI_VERSION fails with ABI_MISMATCH. `out_error` may be NULL;
 * when non-NULL it is always written (OK on success).
 *
 * Returns NULL on failure. No exception crosses this boundary.
 */
AVATAR_TRACKER_API avatar_tracker_handle* avatar_tracker_create(const avatar_tracker_config* config,
                                                                avatar_tracker_error* out_error);

/*
 * Replaces the configuration and rewinds the backend. Any unconsumed result is
 * discarded. Validates the ABI version exactly as create does.
 *
 * Returns OK, ABI_MISMATCH, INVALID_CONFIG or INTERNAL.
 */
AVATAR_TRACKER_API avatar_tracker_error
avatar_tracker_configure(avatar_tracker_handle* handle, const avatar_tracker_config* config);

/*
 * Submits one frame. Never blocks waiting for inference and never throws. A
 * dropped frame (backend busy, script exhausted) is still a successful call:
 * the drop is counted in the backend's stats, not reported as an error.
 *
 * Returns OK, INVALID_ARGUMENT or INTERNAL.
 */
AVATAR_TRACKER_API avatar_tracker_error
avatar_tracker_push_frame(avatar_tracker_handle* handle, const avatar_tracker_frame* frame);

/*
 * Copies the newest unconsumed result into `out_result` and returns OK.
 * Returns NO_RESULT (and zeroes the output) when no new result exists. Calling
 * poll twice without an intervening push returns NO_RESULT the second time.
 *
 * Returns OK, NO_RESULT, INVALID_ARGUMENT or INTERNAL.
 */
AVATAR_TRACKER_API avatar_tracker_error
avatar_tracker_poll_result(avatar_tracker_handle* handle, avatar_tracker_result* out_result);

/*
 * Destroys the backend. `handle` may be NULL. After this call the handle is
 * invalid; destroying it twice or using it afterwards is undefined behaviour.
 */
AVATAR_TRACKER_API void avatar_tracker_destroy(avatar_tracker_handle* handle);

#ifdef __cplusplus
} /* extern "C" */
#endif

#if defined(__cplusplus)
static_assert(AVATAR_TRACKER_LANDMARK_CAPACITY == 478, "tracker landmark capacity changed");
static_assert(AVATAR_TRACKER_BLENDSHAPE_COUNT == 52, "tracker blendshape count changed");
static_assert(sizeof(avatar_tracker_head_pose) == 28, "avatar_tracker_head_pose layout changed");
static_assert(sizeof(avatar_tracker_landmark) == 12, "avatar_tracker_landmark layout changed");
static_assert(sizeof(avatar_tracker_config) == 48, "avatar_tracker_config layout changed");
static_assert(offsetof(avatar_tracker_config, abi_version) == 0, "config.abi_version moved");
static_assert(offsetof(avatar_tracker_config, frame_interval_ns) == 24,
              "config.frame_interval_ns moved");
static_assert(offsetof(avatar_tracker_config, simulate_fault) == 40, "config.simulate_fault moved");
static_assert(sizeof(avatar_tracker_result) == 5992, "avatar_tracker_result layout changed");
static_assert(offsetof(avatar_tracker_result, source_timestamp_ns) == 0,
              "result.source_timestamp_ns moved");
static_assert(offsetof(avatar_tracker_result, confidence) == 12, "result.confidence moved");
static_assert(offsetof(avatar_tracker_result, pose) == 16, "result.pose moved");
static_assert(offsetof(avatar_tracker_result, landmark_count) == 44, "result.landmark_count moved");
static_assert(offsetof(avatar_tracker_result, landmarks) == 48, "result.landmarks moved");
static_assert(offsetof(avatar_tracker_result, blendshapes) == 5784, "result.blendshapes moved");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(avatar_tracker_frame) == 88, "avatar_tracker_frame layout changed");
#else
static_assert(sizeof(avatar_tracker_frame) == 64, "avatar_tracker_frame layout changed");
#endif
#endif /* __cplusplus */

#endif /* AVATAR_TRACKER_TRACKER_C_API_H */
