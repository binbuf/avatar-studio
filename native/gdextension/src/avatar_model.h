#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <vector>

/// Runtime GLB avatar scene.
///
/// T17 imports a self-contained `.glb` with the engine's `GLTFDocument` /
/// `GLTFState` path, then builds a deliberately rigid scene from the parsed
/// state: mesh surfaces are copied into `ArrayMesh` resources without bones,
/// skin weights or blend shapes. That bind pose is the head deformation rest state.
/// Animations, skeletons and skins are never instantiated. The importer also
/// records the realised units/axes/scale, the model AABB, the node tree and the
/// rest-position/normal/index arrays the decal pass needs.
///
/// This engine adapter imports once, then T19 streams head-deformed vertex buffers
/// on pose changes. Model and decal instances share the same ArrayMesh; CUSTOM0
/// retains immutable rest model positions for fragment projection.
class AvatarModel : public godot::Node3D {
    GDCLASS(AvatarModel, godot::Node3D)

public:
    AvatarModel();
    ~AvatarModel() override;

    // Import options. Defaults are the glTF conventions: metres, Y up, -Z
    // forward, scale 1.0. They are applied as a uniform scale and a rigid axis
    // conversion on the model root, never baked into the source asset.
    void set_units(const godot::String& units);
    godot::String get_units() const;
    void set_forward_axis(const godot::String& forward);
    godot::String get_forward_axis() const;
    void set_up_axis(const godot::String& up);
    godot::String get_up_axis() const;
    void set_uniform_scale(double scale);
    double get_uniform_scale() const;
    void set_face_submesh(const godot::String& name);
    godot::String get_face_submesh() const;

    /// Imports `path` into this node. Returns Godot's `OK` on success; on
    /// failure the node is left empty and `get_last_error_code()` is set.
    int32_t import_glb(const godot::String& path);

    /// Removes the imported scene and resets every reported value.
    void clear();

    bool is_loaded() const;
    godot::String get_source_path() const;
    godot::String get_last_error_code() const;
    godot::String get_last_error_message() const;
    godot::PackedStringArray get_warnings() const;

    // Model check surface.
    int64_t get_mesh_count() const;
    int64_t get_triangle_count() const;
    int64_t get_material_count() const;
    int64_t get_node_count() const;
    int64_t get_animation_count() const;
    int64_t get_skin_count() const;
    int64_t get_morph_target_count() const;
    bool are_animations_playing() const;
    bool has_skeleton() const;
    godot::PackedStringArray get_mesh_names() const;
    godot::PackedStringArray get_material_names() const;
    godot::Array get_node_tree() const;
    godot::PackedStringArray get_candidate_head_nodes() const;
    godot::AABB get_model_aabb() const;
    godot::Vector3 get_realised_size() const;
    godot::Vector3 get_realised_facing() const;
    godot::Vector3 get_realised_up() const;
    double get_import_time_ms() const;
    int64_t get_peak_memory_bytes() const;
    int64_t get_static_memory_delta_bytes() const;
    godot::Dictionary get_import_summary() const;

    // Decal input: rest model-space positions, normals and triangle indices.
    // `face_submesh` restricts this data to the named mesh; otherwise every
    // imported mesh contributes.
    godot::PackedVector3Array get_decal_vertices() const;
    godot::PackedVector3Array get_decal_normals() const;
    godot::PackedInt32Array get_decal_indices() const;
    int64_t get_decal_triangle_count() const;
    godot::String get_decal_source() const;
    // Face and region coordinates are realised rest model space (metres).
    bool configure_head_region(const godot::Dictionary& region, const godot::Dictionary& face,
                               const godot::Variant& pivot);
    void set_head_rotation(const godot::Quaternion& rotation);
    godot::Dictionary get_head_region() const;
    // Rest model-space bounds of the active head region. Empty when no region
    // has been configured; the framing adapter then falls back to the model AABB.
    godot::AABB get_head_aabb() const;
    // The calibrated face surface (origin, normal, up, scale) last accepted by
    // configure_head_region; fit="head" uses its origin and inter-ocular scale.
    godot::Dictionary get_face_surface() const;
    // Child instances share the model's live vertex buffers; CUSTOM0 is immutable rest position.
    godot::Node3D* create_decal_pass(const godot::Ref<godot::ShaderMaterial>& material);
    void clear_decal_pass();
    godot::Array get_head_surface_data() const;

protected:
    static void _bind_methods();

private:
    struct HeadSurface {
        godot::Ref<godot::ArrayMesh> mesh;
        int32_t surface = 0;
        size_t node = 0;
        bool decal = false;
        godot::PackedVector3Array rest;
        godot::PackedVector3Array normals;
        godot::PackedFloat32Array tangents;
        std::vector<float> weights;
        godot::PackedByteArray bytes;
        uint32_t vertex_stride = 0, normal_stride = 0, normal_offset = 0;
    };
    std::vector<HeadSurface> head_surfaces_;
    std::vector<godot::Node3D*> decal_objects_;
    godot::Dictionary head_region_;
    godot::Dictionary face_surface_;
    godot::AABB head_aabb_;
    bool has_head_aabb_ = false;
    godot::Vector3 head_pivot_, cut_origin_, cut_normal_;
    float cut_falloff_ = 0;
    int32_t head_node_ = -1;
    godot::Quaternion head_rotation_;
    void cache_head_surface(const godot::Ref<godot::ArrayMesh>& mesh, int32_t surface, size_t node,
                            bool decal, const godot::Array& arrays);
    bool in_head_subtree(size_t node, int32_t ancestor) const;
    struct NodeRecord {
        godot::String name;
        int32_t parent = -1;
        int32_t mesh = -1;
        godot::Transform3D local_xform;
        godot::Transform3D model_xform;
        godot::AABB aabb;
        bool has_aabb = false;
    };

    void reset_reported_state();
    void build_axis_conversion();
    bool preflight_glb(const godot::String& path, godot::String& detail);
    bool build_scene(const godot::String& path);
    void append_surface(const godot::Array& arrays, const godot::Transform3D& model_xform,
                        bool include_in_decal);
    void build_decal_data();
    void build_candidate_heads();
    void fail(const godot::String& code, const godot::String& message);

    // Options.
    godot::String units_ = "m";
    godot::String forward_axis_ = "-Z";
    godot::String up_axis_ = "Y";
    double uniform_scale_ = 1.0;
    godot::String face_submesh_;

    // Realised conversion.
    godot::Transform3D conversion_;
    godot::Vector3 authored_forward_ = godot::Vector3(0.0F, 0.0F, -1.0F);
    godot::Vector3 authored_up_ = godot::Vector3(0.0F, 1.0F, 0.0F);
    godot::Vector3 realised_facing_ = godot::Vector3(0.0F, 0.0F, -1.0F);
    godot::Vector3 realised_up_ = godot::Vector3(0.0F, 1.0F, 0.0F);

    // Imported scene.
    godot::Node3D* container_ = nullptr;
    std::vector<NodeRecord> nodes_;
    std::vector<godot::Node3D*> node_objects_;

    // Reported model check.
    godot::PackedStringArray mesh_names_;
    godot::PackedStringArray material_names_;
    godot::PackedStringArray candidate_heads_;
    godot::PackedStringArray warnings_;
    godot::Array node_tree_;
    godot::AABB model_aabb_;
    bool has_model_aabb_ = false;
    int64_t triangle_count_ = 0;
    int64_t animation_count_ = 0;
    int64_t skin_count_ = 0;
    int64_t morph_target_count_ = 0;
    double import_time_ms_ = 0.0;
    int64_t peak_memory_bytes_ = 0;
    int64_t static_memory_delta_bytes_ = 0;

    // Decal input.
    godot::PackedVector3Array decal_vertices_;
    godot::PackedVector3Array decal_normals_;
    godot::PackedInt32Array decal_indices_;
    godot::String decal_source_;

    godot::String source_path_;
    godot::String last_error_code_;
    godot::String last_error_message_;
    bool loaded_ = false;
};
