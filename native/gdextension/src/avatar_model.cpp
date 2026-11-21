#include "avatar_model.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/gltf_document.hpp>
#include <godot_cpp/classes/gltf_mesh.hpp>
#include <godot_cpp/classes/gltf_node.hpp>
#include <godot_cpp/classes/gltf_state.hpp>
#include <godot_cpp/classes/importer_mesh.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace godot;

namespace {

constexpr uint32_t kGlbMagic = 0x46546C67;      // "glTF"
constexpr uint32_t kJsonChunkType = 0x4E4F534A; // "JSON"
constexpr int64_t kMaxTriangles = 200'000;

uint32_t read_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

String safe_node_name(const String& name) {
    String out = name.strip_edges();
    if (out.is_empty()) {
        out = "Node";
    }
    out = out.replace("/", "_").replace(":", "_").replace("@", "_").replace("\"", "_");
    return out;
}

bool parse_axis(const String& text, Vector3& axis) {
    const String value = text.strip_edges().to_upper();
    if (value == "Y" || value == "+Y") {
        axis = Vector3(0.0F, 1.0F, 0.0F);
    } else if (value == "-Y") {
        axis = Vector3(0.0F, -1.0F, 0.0F);
    } else if (value == "Z" || value == "+Z") {
        axis = Vector3(0.0F, 0.0F, 1.0F);
    } else if (value == "-Z") {
        axis = Vector3(0.0F, 0.0F, -1.0F);
    } else if (value == "X" || value == "+X") {
        axis = Vector3(1.0F, 0.0F, 0.0F);
    } else if (value == "-X") {
        axis = Vector3(-1.0F, 0.0F, 0.0F);
    } else {
        return false;
    }
    return true;
}

int64_t triangle_count_for(const Array& arrays) {
    const Variant indices = arrays[Mesh::ARRAY_INDEX];
    if (indices.get_type() == Variant::PACKED_INT32_ARRAY) {
        const PackedInt32Array packed = indices;
        return packed.size() / 3;
    }
    const Variant vertices = arrays[Mesh::ARRAY_VERTEX];
    if (vertices.get_type() == Variant::PACKED_VECTOR3_ARRAY) {
        const PackedVector3Array packed = vertices;
        return packed.size() / 3;
    }
    return 0;
}

} // namespace

AvatarModel::AvatarModel() = default;

AvatarModel::~AvatarModel() = default;

void AvatarModel::set_units(const String& units) {
    units_ = units.strip_edges().to_lower();
}

String AvatarModel::get_units() const {
    return units_;
}

void AvatarModel::set_forward_axis(const String& forward) {
    forward_axis_ = forward;
}

String AvatarModel::get_forward_axis() const {
    return forward_axis_;
}

void AvatarModel::set_up_axis(const String& up) {
    up_axis_ = up;
}

String AvatarModel::get_up_axis() const {
    return up_axis_;
}

void AvatarModel::set_uniform_scale(double scale) {
    uniform_scale_ = scale;
}

double AvatarModel::get_uniform_scale() const {
    return uniform_scale_;
}

void AvatarModel::set_face_submesh(const String& name) {
    face_submesh_ = name.strip_edges();
}

String AvatarModel::get_face_submesh() const {
    return face_submesh_;
}

void AvatarModel::clear() {
    if (container_ != nullptr) {
        if (container_->get_parent() != nullptr) {
            container_->get_parent()->remove_child(container_);
        }
        memdelete(container_);
        container_ = nullptr;
    }
    nodes_.clear();
    node_objects_.clear();
    reset_reported_state();
}

void AvatarModel::reset_reported_state() {
    mesh_names_ = PackedStringArray();
    material_names_ = PackedStringArray();
    candidate_heads_ = PackedStringArray();
    warnings_ = PackedStringArray();
    node_tree_ = Array();
    model_aabb_ = AABB();
    has_model_aabb_ = false;
    triangle_count_ = 0;
    animation_count_ = 0;
    skin_count_ = 0;
    morph_target_count_ = 0;
    import_time_ms_ = 0.0;
    peak_memory_bytes_ = 0;
    static_memory_delta_bytes_ = 0;
    decal_vertices_ = PackedVector3Array();
    decal_normals_ = PackedVector3Array();
    decal_indices_ = PackedInt32Array();
    decal_source_ = String();
    conversion_ = Transform3D();
    authored_forward_ = Vector3(0.0F, 0.0F, -1.0F);
    authored_up_ = Vector3(0.0F, 1.0F, 0.0F);
    realised_facing_ = Vector3(0.0F, 0.0F, -1.0F);
    realised_up_ = Vector3(0.0F, 1.0F, 0.0F);
    source_path_ = String();
    last_error_code_ = String();
    last_error_message_ = String();
    loaded_ = false;
}

void AvatarModel::build_axis_conversion() {
    Vector3 forward;
    Vector3 up;
    if (!parse_axis(forward_axis_, forward)) {
        warnings_.append(String("unknown forward axis '") + forward_axis_ +
                         "'; using the glTF -Z convention");
        forward = Vector3(0.0F, 0.0F, -1.0F);
    }
    if (!parse_axis(up_axis_, up)) {
        warnings_.append(String("unknown up axis '") + up_axis_ + "'; using the glTF Y convention");
        up = Vector3(0.0F, 1.0F, 0.0F);
    }
    authored_forward_ = forward.normalized();
    authored_up_ = up.normalized();

    Vector3 right = authored_forward_.cross(authored_up_);
    if (right.length_squared() < 1e-8F) {
        warnings_.append("forward and up axes are parallel; using the glTF -Z/Y convention");
        authored_forward_ = Vector3(0.0F, 0.0F, -1.0F);
        authored_up_ = Vector3(0.0F, 1.0F, 0.0F);
        right = authored_forward_.cross(authored_up_);
    }
    right = right.normalized();

    // Columns are the authored right/up/-forward basis. Its transpose maps the
    // authored axes onto Godot's +X right, +Y up, -Z forward.
    const Basis authored_basis(right, authored_up_, -authored_forward_);
    double unit_scale = 1.0;
    if (units_ == "cm") {
        unit_scale = 0.01;
    } else if (units_ == "mm") {
        unit_scale = 0.001;
    } else if (units_ != "m") {
        warnings_.append(String("unknown units '") + units_ + "'; using metres");
    }
    double scale = unit_scale * uniform_scale_;
    if (!std::isfinite(scale) || scale <= 0.0) {
        warnings_.append("uniform scale must be finite and positive; using 1.0");
        scale = unit_scale;
    }
    conversion_ = Transform3D(authored_basis.transposed() * static_cast<real_t>(scale), Vector3());
    realised_facing_ = conversion_.basis.xform(authored_forward_).normalized();
    realised_up_ = conversion_.basis.xform(authored_up_).normalized();
}

bool AvatarModel::preflight_glb(const String& path, String& detail) {
    const PackedByteArray bytes = FileAccess::get_file_as_bytes(path);
    if (bytes.size() < 20) {
        detail = "file is shorter than a GLB header";
        return false;
    }
    const uint8_t* data = bytes.ptr();
    if (read_u32(data) != kGlbMagic) {
        detail = "missing glTF binary magic";
        return false;
    }
    if (read_u32(data + 4) != 2) {
        detail = "unsupported GLB container version";
        return false;
    }
    const uint32_t declared_length = read_u32(data + 8);
    if (declared_length > static_cast<uint32_t>(bytes.size())) {
        detail = "GLB header length exceeds the file size";
        return false;
    }
    const uint32_t json_length = read_u32(data + 12);
    if (read_u32(data + 16) != kJsonChunkType) {
        detail = "the first GLB chunk is not JSON";
        return false;
    }
    if (json_length > static_cast<uint32_t>(bytes.size()) - 20U) {
        detail = "JSON chunk exceeds the file size";
        return false;
    }

    const std::string text(reinterpret_cast<const char*>(data + 20), json_length);
    nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        detail = "GLB JSON chunk is not valid JSON";
        return false;
    }
    if (!json.contains("asset") || !json["asset"].is_object() ||
        json["asset"].value("version", std::string()) != "2.0") {
        detail = "GLB asset version is not glTF 2.0";
        return false;
    }
    if (!json.contains("meshes") || !json["meshes"].is_array() || json["meshes"].empty()) {
        detail = "GLB has no meshes";
        return false;
    }

    // A package is untrusted input. A self-contained GLB must not point at
    // external buffers or images, and it must not carry a shader extension.
    for (const char* key : {"buffers", "images"}) {
        if (!json.contains(key) || !json[key].is_array()) {
            continue;
        }
        for (const auto& entry : json[key]) {
            if (entry.is_object() && entry.contains("uri")) {
                detail = String("GLB ") + key + " entry uses an external URI";
                return false;
            }
        }
    }
    if (json.contains("extensionsUsed") && json["extensionsUsed"].is_array()) {
        for (const auto& extension : json["extensionsUsed"]) {
            if (extension.is_string() && extension.get<std::string>() == "KHR_techniques_webgl") {
                detail = "GLB uses a custom shader extension";
                return false;
            }
        }
    }

    animation_count_ = json.contains("animations") && json["animations"].is_array()
                           ? static_cast<int64_t>(json["animations"].size())
                           : 0;
    skin_count_ = json.contains("skins") && json["skins"].is_array()
                      ? static_cast<int64_t>(json["skins"].size())
                      : 0;
    morph_target_count_ = 0;
    for (const auto& mesh : json["meshes"]) {
        if (!mesh.is_object() || !mesh.contains("primitives") || !mesh["primitives"].is_array()) {
            continue;
        }
        for (const auto& primitive : mesh["primitives"]) {
            if (primitive.is_object() && primitive.contains("targets") &&
                primitive["targets"].is_array()) {
                morph_target_count_ += static_cast<int64_t>(primitive["targets"].size());
            }
        }
    }
    return true;
}

int32_t AvatarModel::import_glb(const String& path) {
    clear();
    source_path_ = path;

    const auto started = std::chrono::steady_clock::now();
    const uint64_t memory_before =
        OS::get_singleton() != nullptr ? OS::get_singleton()->get_static_memory_usage() : 0;
    const uint64_t peak_before =
        OS::get_singleton() != nullptr ? OS::get_singleton()->get_static_memory_peak_usage() : 0;

    auto finish = [&](int32_t result) {
        const auto ended = std::chrono::steady_clock::now();
        import_time_ms_ = std::chrono::duration<double, std::milli>(ended - started).count();
        const uint64_t memory_after =
            OS::get_singleton() != nullptr ? OS::get_singleton()->get_static_memory_usage() : 0;
        const uint64_t peak_after = OS::get_singleton() != nullptr
                                        ? OS::get_singleton()->get_static_memory_peak_usage()
                                        : 0;
        peak_memory_bytes_ = static_cast<int64_t>(std::max(peak_before, peak_after));
        static_memory_delta_bytes_ =
            static_cast<int64_t>(memory_after) - static_cast<int64_t>(memory_before);
        return result;
    };

    if (!FileAccess::file_exists(path)) {
        fail("AVT-101", String("model file not found: ") + path);
        return finish(ERR_FILE_NOT_FOUND);
    }

    String detail;
    if (!preflight_glb(path, detail)) {
        fail("AVT-102", detail);
        return finish(ERR_PARSE_ERROR);
    }

    build_axis_conversion();
    if (!build_scene(path)) {
        return finish(ERR_PARSE_ERROR);
    }

    if (triangle_count_ <= 0) {
        clear();
        source_path_ = path;
        fail("AVT-102", "GLB has no triangle surfaces");
        return finish(ERR_PARSE_ERROR);
    }
    if (triangle_count_ > kMaxTriangles) {
        const int64_t triangles = triangle_count_;
        clear();
        source_path_ = path;
        fail("AVT-102", String("model has ") + String::num_int64(triangles) +
                            " triangles; limit is " + String::num_int64(kMaxTriangles));
        return finish(ERR_PARSE_ERROR);
    }

    build_decal_data();
    build_candidate_heads();
    loaded_ = true;
    return finish(OK);
}

bool AvatarModel::build_scene(const String& path) {
    Ref<GLTFDocument> document;
    document.instantiate();
    Ref<GLTFState> state;
    state.instantiate();

    document->set_root_node_mode(GLTFDocument::ROOT_NODE_MODE_KEEP_ROOT);
    state->set_create_animations(false);
    state->set_import_as_skeleton_bones(false);

    const Error error = document->append_from_file(path, state, 0, path.get_base_dir());
    if (error != OK) {
        fail("AVT-102", String("GLTFDocument could not parse the model (error ") +
                            String::num_int64(static_cast<int64_t>(error)) + ")");
        return false;
    }

    const TypedArray<Ref<GLTFNode>> gltf_nodes = state->get_nodes();
    const TypedArray<Ref<GLTFMesh>> gltf_meshes = state->get_meshes();
    const TypedArray<Ref<Material>> gltf_materials = state->get_materials();

    for (int64_t i = 0; i < gltf_meshes.size(); ++i) {
        const Ref<GLTFMesh> mesh = gltf_meshes[i];
        String name = mesh.is_valid() ? mesh->get_original_name() : String();
        if (name.is_empty()) {
            name = String("Mesh") + String::num_int64(i);
        }
        mesh_names_.append(name);
    }
    for (int64_t i = 0; i < gltf_materials.size(); ++i) {
        const Ref<Material> material = gltf_materials[i];
        String name = material.is_valid() ? material->get_name() : String();
        if (name.is_empty()) {
            name = String("Material") + String::num_int64(i);
        }
        material_names_.append(name);
    }

    const int64_t node_count = gltf_nodes.size();
    nodes_.resize(static_cast<size_t>(node_count));
    std::vector<Transform3D> global_xforms(static_cast<size_t>(node_count));
    std::vector<int32_t> effective_parents(static_cast<size_t>(node_count), -1);

    for (int64_t i = 0; i < node_count; ++i) {
        const Ref<GLTFNode> gltf_node = gltf_nodes[i];
        NodeRecord& record = nodes_[static_cast<size_t>(i)];
        if (gltf_node.is_valid()) {
            record.name = gltf_node->get_original_name();
            record.parent = gltf_node->get_parent();
            record.mesh = gltf_node->get_mesh();
            record.local_xform = gltf_node->get_xform();
        } else {
            record.name = String("Node") + String::num_int64(i);
        }
        if (record.name.is_empty()) {
            record.name = String("Node") + String::num_int64(i);
        }
        if (record.parent < 0 || record.parent >= node_count || record.parent == i) {
            record.parent = -1;
        }
    }

    // Resolve authored global transforms without relying on the scene tree.
    std::vector<uint8_t> visit_state(static_cast<size_t>(node_count), 0);
    std::function<Transform3D(int64_t)> resolve = [&](int64_t index) -> Transform3D {
        if (index < 0 || index >= node_count) {
            return Transform3D();
        }
        uint8_t& state_flag = visit_state[static_cast<size_t>(index)];
        if (state_flag == 2) {
            return global_xforms[static_cast<size_t>(index)];
        }
        if (state_flag == 1) {
            // A cycle is invalid glTF. Treat the node as a root rather than
            // recursing forever; the model still renders and the model check
            // remains usable.
            effective_parents[static_cast<size_t>(index)] = -1;
            return Transform3D();
        }
        state_flag = 1;
        const int32_t parent = nodes_[static_cast<size_t>(index)].parent;
        Transform3D parent_xform;
        if (parent >= 0) {
            parent_xform = resolve(parent);
            effective_parents[static_cast<size_t>(index)] = parent;
        }
        global_xforms[static_cast<size_t>(index)] =
            parent_xform * nodes_[static_cast<size_t>(index)].local_xform;
        state_flag = 2;
        return global_xforms[static_cast<size_t>(index)];
    };
    for (int64_t i = 0; i < node_count; ++i) {
        (void)resolve(i);
        nodes_[static_cast<size_t>(i)].model_xform =
            conversion_ * global_xforms[static_cast<size_t>(i)];
    }

    container_ = memnew(Node3D);
    container_->set_name("ImportedModel");
    container_->set_transform(conversion_);
    add_child(container_);

    node_objects_.assign(static_cast<size_t>(node_count), nullptr);
    for (int64_t i = 0; i < node_count; ++i) {
        Node3D* object = nullptr;
        if (nodes_[static_cast<size_t>(i)].mesh >= 0) {
            object = memnew(MeshInstance3D);
        } else {
            object = memnew(Node3D);
        }
        object->set_name(safe_node_name(nodes_[static_cast<size_t>(i)].name));
        object->set_transform(nodes_[static_cast<size_t>(i)].local_xform);
        node_objects_[static_cast<size_t>(i)] = object;
    }
    for (int64_t i = 0; i < node_count; ++i) {
        Node3D* object = node_objects_[static_cast<size_t>(i)];
        const int32_t parent = effective_parents[static_cast<size_t>(i)];
        if (parent >= 0 && parent != i && node_objects_[static_cast<size_t>(parent)] != nullptr) {
            node_objects_[static_cast<size_t>(parent)]->add_child(object);
        } else {
            container_->add_child(object);
        }
    }

    for (int64_t i = 0; i < node_count; ++i) {
        NodeRecord& record = nodes_[static_cast<size_t>(i)];
        if (record.mesh < 0 || record.mesh >= gltf_meshes.size()) {
            continue;
        }
        const Ref<GLTFMesh> gltf_mesh = gltf_meshes[record.mesh];
        if (!gltf_mesh.is_valid()) {
            continue;
        }
        const Ref<ImporterMesh> importer_mesh = gltf_mesh->get_mesh();
        if (!importer_mesh.is_valid()) {
            continue;
        }
        const int64_t surface_count = importer_mesh->get_surface_count();
        if (surface_count <= 0) {
            continue;
        }

        const String mesh_name = mesh_names_[record.mesh];
        const bool include_in_decal =
            face_submesh_.is_empty() || mesh_name == face_submesh_ || record.name == face_submesh_;

        Ref<ArrayMesh> array_mesh;
        array_mesh.instantiate();
        const TypedArray<Ref<Material>> instance_materials = gltf_mesh->get_instance_materials();

        for (int64_t surface = 0; surface < surface_count; ++surface) {
            const int32_t surface_index = static_cast<int32_t>(surface);
            Array arrays = importer_mesh->get_surface_arrays(surface_index);
            triangle_count_ += triangle_count_for(arrays);

            Ref<Material> material = importer_mesh->get_surface_material(surface_index);
            if (!material.is_valid() && surface < instance_materials.size()) {
                material = instance_materials[surface_index];
            }

            // A rigid avatar never skins. Removing bones and weights is what
            // keeps the bind pose stable and stops the renderer from looking
            // for a Skeleton3D that this importer never creates.
            arrays[Mesh::ARRAY_BONES] = Variant();
            arrays[Mesh::ARRAY_WEIGHTS] = Variant();
            array_mesh->add_surface_from_arrays(
                importer_mesh->get_surface_primitive_type(surface_index), arrays);
            array_mesh->surface_set_material(surface_index, material);

            if (include_in_decal) {
                append_surface(arrays, record.model_xform, true);
            }
        }

        MeshInstance3D* instance = Object::cast_to<MeshInstance3D>(node_objects_[i]);
        if (instance != nullptr) {
            instance->set_mesh(array_mesh);
            instance->set_skeleton_path(NodePath());
        }

        const AABB local_aabb = array_mesh->get_aabb();
        const AABB model_aabb = record.model_xform.xform(local_aabb);
        if (!has_model_aabb_) {
            model_aabb_ = model_aabb;
            has_model_aabb_ = true;
        } else {
            model_aabb_.merge_with(model_aabb);
        }
        record.aabb = model_aabb;
        record.has_aabb = true;

        if (include_in_decal) {
            if (decal_source_.is_empty()) {
                decal_source_ = mesh_name;
            } else if (decal_source_ != mesh_name) {
                decal_source_ = "all";
            }
        }
    }

    if (container_ == nullptr || container_->get_child_count() == 0) {
        clear();
        source_path_ = path;
        fail("AVT-102", "GLB produced no scene nodes");
        return false;
    }

    node_tree_ = Array();
    for (int64_t i = 0; i < node_count; ++i) {
        const NodeRecord& record = nodes_[static_cast<size_t>(i)];
        Array children;
        for (int64_t j = 0; j < node_count; ++j) {
            if (effective_parents[static_cast<size_t>(j)] == i) {
                children.append(nodes_[static_cast<size_t>(j)].name);
            }
        }
        Dictionary entry;
        entry["name"] = record.name;
        entry["parent"] =
            record.parent >= 0 ? nodes_[static_cast<size_t>(record.parent)].name : String();
        entry["mesh"] = record.mesh >= 0 && record.mesh < mesh_names_.size()
                            ? mesh_names_[record.mesh]
                            : String();
        entry["children"] = children;
        entry["position"] = record.model_xform.origin;
        entry["has_mesh"] = record.mesh >= 0;
        entry["aabb_position"] = record.has_aabb ? record.aabb.position : Vector3();
        entry["aabb_size"] = record.has_aabb ? record.aabb.size : Vector3();
        node_tree_.append(entry);
    }

    if (face_submesh_ != "" && decal_vertices_.is_empty()) {
        warnings_.append(String("face_submesh '") + face_submesh_ +
                         "' matched no mesh; the decal pass has no data");
    }
    if (has_model_aabb_) {
        const Vector3 size = model_aabb_.size;
        if (!size.is_finite() || size.x <= 0.0F || size.y <= 0.0F || size.z <= 0.0F) {
            warnings_.append("model AABB is degenerate; calibrate the face surface by hand");
        }
    }
    return true;
}

void AvatarModel::append_surface(const Array& arrays, const Transform3D& model_xform,
                                 bool include_in_decal) {
    if (!include_in_decal) {
        return;
    }
    const Variant vertex_variant = arrays[Mesh::ARRAY_VERTEX];
    if (vertex_variant.get_type() != Variant::PACKED_VECTOR3_ARRAY) {
        return;
    }
    const PackedVector3Array positions = vertex_variant;
    if (positions.is_empty()) {
        return;
    }

    const int64_t base = decal_vertices_.size();
    for (int64_t i = 0; i < positions.size(); ++i) {
        decal_vertices_.append(model_xform.xform(positions[i]));
    }

    const Variant normal_variant = arrays[Mesh::ARRAY_NORMAL];
    if (normal_variant.get_type() == Variant::PACKED_VECTOR3_ARRAY) {
        const PackedVector3Array normals = normal_variant;
        if (normals.size() == positions.size()) {
            for (int64_t i = 0; i < normals.size(); ++i) {
                decal_normals_.append(model_xform.basis.xform(normals[i]).normalized());
            }
        }
    }
    if (decal_normals_.size() != decal_vertices_.size()) {
        decal_normals_.resize(decal_vertices_.size());
        if (!warnings_.has(
                "one or more surfaces have no normals; the decal pass must derive them")) {
            warnings_.append(
                "one or more surfaces have no normals; the decal pass must derive them");
        }
    }

    const Variant index_variant = arrays[Mesh::ARRAY_INDEX];
    if (index_variant.get_type() == Variant::PACKED_INT32_ARRAY) {
        const PackedInt32Array indices = index_variant;
        for (int64_t i = 0; i < indices.size(); ++i) {
            decal_indices_.append(static_cast<int32_t>(base + indices[i]));
        }
    } else {
        for (int64_t i = 0; i < positions.size(); ++i) {
            decal_indices_.append(static_cast<int32_t>(base + i));
        }
    }
}

void AvatarModel::build_decal_data() {
    if (face_submesh_ != "" && decal_source_ == "") {
        decal_source_ = "none";
    }
    if (decal_source_ == "") {
        decal_source_ = "all";
    }
}

void AvatarModel::build_candidate_heads() {
    struct Candidate {
        String name;
        int priority;
        float top;
    };
    std::vector<Candidate> candidates;

    float model_top = 0.0F;
    float model_height = 0.0F;
    if (has_model_aabb_) {
        model_top = model_aabb_.position.y + model_aabb_.size.y;
        model_height = model_aabb_.size.y;
    }

    for (const NodeRecord& record : nodes_) {
        const String lower = record.name.to_lower();
        int priority = -1;
        if (lower.contains("head")) {
            priority = 0;
        } else if (lower.contains("face")) {
            priority = 1;
        } else if (lower.contains("neck")) {
            priority = 2;
        }

        if (priority < 0) {
            if (record.mesh < 0 || !record.has_aabb || !has_model_aabb_) {
                continue;
            }
            const float top = record.aabb.position.y + record.aabb.size.y;
            if (top < model_top - model_height * 0.2F) {
                continue;
            }
            priority = 3;
        }

        const float top = record.has_aabb ? record.aabb.position.y + record.aabb.size.y : -1e9F;
        candidates.push_back(Candidate{record.name, priority, top});
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        if (a.top != b.top) {
            return a.top > b.top;
        }
        return a.name < b.name;
    });

    for (const Candidate& candidate : candidates) {
        if (!candidate_heads_.has(candidate.name)) {
            candidate_heads_.append(candidate.name);
        }
    }
}

void AvatarModel::fail(const String& code, const String& message) {
    last_error_code_ = code;
    last_error_message_ = message;
    loaded_ = false;
}

bool AvatarModel::is_loaded() const {
    return loaded_;
}

String AvatarModel::get_source_path() const {
    return source_path_;
}

String AvatarModel::get_last_error_code() const {
    return last_error_code_;
}

String AvatarModel::get_last_error_message() const {
    return last_error_message_;
}

PackedStringArray AvatarModel::get_warnings() const {
    return warnings_;
}

int64_t AvatarModel::get_mesh_count() const {
    return mesh_names_.size();
}

int64_t AvatarModel::get_triangle_count() const {
    return triangle_count_;
}

int64_t AvatarModel::get_material_count() const {
    return material_names_.size();
}

int64_t AvatarModel::get_node_count() const {
    return static_cast<int64_t>(nodes_.size());
}

int64_t AvatarModel::get_animation_count() const {
    return animation_count_;
}

int64_t AvatarModel::get_skin_count() const {
    return skin_count_;
}

int64_t AvatarModel::get_morph_target_count() const {
    return morph_target_count_;
}

bool AvatarModel::are_animations_playing() const {
    return false;
}

bool AvatarModel::has_skeleton() const {
    return false;
}

PackedStringArray AvatarModel::get_mesh_names() const {
    return mesh_names_;
}

PackedStringArray AvatarModel::get_material_names() const {
    return material_names_;
}

Array AvatarModel::get_node_tree() const {
    return node_tree_;
}

PackedStringArray AvatarModel::get_candidate_head_nodes() const {
    return candidate_heads_;
}

AABB AvatarModel::get_model_aabb() const {
    return model_aabb_;
}

Vector3 AvatarModel::get_realised_size() const {
    return model_aabb_.size;
}

Vector3 AvatarModel::get_realised_facing() const {
    return realised_facing_;
}

Vector3 AvatarModel::get_realised_up() const {
    return realised_up_;
}

double AvatarModel::get_import_time_ms() const {
    return import_time_ms_;
}

int64_t AvatarModel::get_peak_memory_bytes() const {
    return peak_memory_bytes_;
}

int64_t AvatarModel::get_static_memory_delta_bytes() const {
    return static_memory_delta_bytes_;
}

Dictionary AvatarModel::get_import_summary() const {
    Dictionary summary;
    summary["schema"] = "avatar_model.import.v1";
    summary["path"] = source_path_;
    summary["loaded"] = loaded_;
    summary["error_code"] = last_error_code_;
    summary["error_message"] = last_error_message_;
    summary["units"] = units_;
    summary["forward"] = forward_axis_;
    summary["up"] = up_axis_;
    summary["uniform_scale"] = uniform_scale_;
    summary["realised_facing"] = realised_facing_;
    summary["realised_up"] = realised_up_;
    summary["realised_size"] = get_realised_size();
    summary["aabb_position"] = model_aabb_.position;
    summary["aabb_size"] = model_aabb_.size;
    summary["mesh_count"] = get_mesh_count();
    summary["triangle_count"] = triangle_count_;
    summary["material_count"] = get_material_count();
    summary["node_count"] = get_node_count();
    summary["animation_count"] = animation_count_;
    summary["skin_count"] = skin_count_;
    summary["morph_target_count"] = morph_target_count_;
    summary["animations_playing"] = false;
    summary["has_skeleton"] = false;
    summary["import_time_ms"] = import_time_ms_;
    summary["peak_memory_bytes"] = peak_memory_bytes_;
    summary["static_memory_delta_bytes"] = static_memory_delta_bytes_;
    summary["mesh_names"] = mesh_names_;
    summary["material_names"] = material_names_;
    summary["candidate_head_nodes"] = candidate_heads_;
    summary["warnings"] = warnings_;
    summary["decal_triangle_count"] = get_decal_triangle_count();
    summary["decal_source"] = decal_source_;
    return summary;
}

PackedVector3Array AvatarModel::get_decal_vertices() const {
    return decal_vertices_;
}

PackedVector3Array AvatarModel::get_decal_normals() const {
    return decal_normals_;
}

PackedInt32Array AvatarModel::get_decal_indices() const {
    return decal_indices_;
}

int64_t AvatarModel::get_decal_triangle_count() const {
    return decal_indices_.size() / 3;
}

String AvatarModel::get_decal_source() const {
    return decal_source_;
}

void AvatarModel::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_units", "units"), &AvatarModel::set_units);
    ClassDB::bind_method(D_METHOD("get_units"), &AvatarModel::get_units);
    ClassDB::bind_method(D_METHOD("set_forward_axis", "forward"), &AvatarModel::set_forward_axis);
    ClassDB::bind_method(D_METHOD("get_forward_axis"), &AvatarModel::get_forward_axis);
    ClassDB::bind_method(D_METHOD("set_up_axis", "up"), &AvatarModel::set_up_axis);
    ClassDB::bind_method(D_METHOD("get_up_axis"), &AvatarModel::get_up_axis);
    ClassDB::bind_method(D_METHOD("set_uniform_scale", "scale"), &AvatarModel::set_uniform_scale);
    ClassDB::bind_method(D_METHOD("get_uniform_scale"), &AvatarModel::get_uniform_scale);
    ClassDB::bind_method(D_METHOD("set_face_submesh", "name"), &AvatarModel::set_face_submesh);
    ClassDB::bind_method(D_METHOD("get_face_submesh"), &AvatarModel::get_face_submesh);

    ClassDB::bind_method(D_METHOD("import_glb", "path"), &AvatarModel::import_glb);
    ClassDB::bind_method(D_METHOD("clear"), &AvatarModel::clear);
    ClassDB::bind_method(D_METHOD("is_loaded"), &AvatarModel::is_loaded);
    ClassDB::bind_method(D_METHOD("get_source_path"), &AvatarModel::get_source_path);
    ClassDB::bind_method(D_METHOD("get_last_error_code"), &AvatarModel::get_last_error_code);
    ClassDB::bind_method(D_METHOD("get_last_error_message"), &AvatarModel::get_last_error_message);
    ClassDB::bind_method(D_METHOD("get_warnings"), &AvatarModel::get_warnings);

    ClassDB::bind_method(D_METHOD("get_mesh_count"), &AvatarModel::get_mesh_count);
    ClassDB::bind_method(D_METHOD("get_triangle_count"), &AvatarModel::get_triangle_count);
    ClassDB::bind_method(D_METHOD("get_material_count"), &AvatarModel::get_material_count);
    ClassDB::bind_method(D_METHOD("get_node_count"), &AvatarModel::get_node_count);
    ClassDB::bind_method(D_METHOD("get_animation_count"), &AvatarModel::get_animation_count);
    ClassDB::bind_method(D_METHOD("get_skin_count"), &AvatarModel::get_skin_count);
    ClassDB::bind_method(D_METHOD("get_morph_target_count"), &AvatarModel::get_morph_target_count);
    ClassDB::bind_method(D_METHOD("are_animations_playing"), &AvatarModel::are_animations_playing);
    ClassDB::bind_method(D_METHOD("has_skeleton"), &AvatarModel::has_skeleton);
    ClassDB::bind_method(D_METHOD("get_mesh_names"), &AvatarModel::get_mesh_names);
    ClassDB::bind_method(D_METHOD("get_material_names"), &AvatarModel::get_material_names);
    ClassDB::bind_method(D_METHOD("get_node_tree"), &AvatarModel::get_node_tree);
    ClassDB::bind_method(D_METHOD("get_candidate_head_nodes"),
                         &AvatarModel::get_candidate_head_nodes);
    ClassDB::bind_method(D_METHOD("get_model_aabb"), &AvatarModel::get_model_aabb);
    ClassDB::bind_method(D_METHOD("get_realised_size"), &AvatarModel::get_realised_size);
    ClassDB::bind_method(D_METHOD("get_realised_facing"), &AvatarModel::get_realised_facing);
    ClassDB::bind_method(D_METHOD("get_realised_up"), &AvatarModel::get_realised_up);
    ClassDB::bind_method(D_METHOD("get_import_time_ms"), &AvatarModel::get_import_time_ms);
    ClassDB::bind_method(D_METHOD("get_peak_memory_bytes"), &AvatarModel::get_peak_memory_bytes);
    ClassDB::bind_method(D_METHOD("get_static_memory_delta_bytes"),
                         &AvatarModel::get_static_memory_delta_bytes);
    ClassDB::bind_method(D_METHOD("get_import_summary"), &AvatarModel::get_import_summary);

    ClassDB::bind_method(D_METHOD("get_decal_vertices"), &AvatarModel::get_decal_vertices);
    ClassDB::bind_method(D_METHOD("get_decal_normals"), &AvatarModel::get_decal_normals);
    ClassDB::bind_method(D_METHOD("get_decal_indices"), &AvatarModel::get_decal_indices);
    ClassDB::bind_method(D_METHOD("get_decal_triangle_count"),
                         &AvatarModel::get_decal_triangle_count);
    ClassDB::bind_method(D_METHOD("get_decal_source"), &AvatarModel::get_decal_source);

    ADD_PROPERTY(PropertyInfo(Variant::STRING, "units"), "set_units", "get_units");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "forward_axis"), "set_forward_axis",
                 "get_forward_axis");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "up_axis"), "set_up_axis", "get_up_axis");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "uniform_scale"), "set_uniform_scale",
                 "get_uniform_scale");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "face_submesh"), "set_face_submesh",
                 "get_face_submesh");
}
