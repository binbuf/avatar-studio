#include "avatar/rig/head_region.h"
#include "avatar_model.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
using namespace godot;
namespace {
avatar::rig::HeadPoint point(const Vector3& p) {
    return {p.x, p.y, p.z};
}
Vector3 vector(const avatar::rig::HeadPoint& p) {
    return {p[0], p[1], p[2]};
}
void pack_oct(uint8_t* bytes, const Vector2& oct) {
    const uint16_t packed[2] = {
        static_cast<uint16_t>(std::clamp(oct.x * 65535.0f, 0.0f, 65535.0f)),
        static_cast<uint16_t>(std::clamp(oct.y * 65535.0f, 0.0f, 65535.0f))};
    std::memcpy(bytes, packed, 4);
}
} // namespace
void AvatarModel::cache_head_surface(const Ref<ArrayMesh>& mesh, int32_t surface, size_t node,
                                     bool decal, const Array& arrays) {
    HeadSurface s;
    s.mesh = mesh;
    s.surface = surface;
    s.node = node;
    s.decal = decal;
    s.rest = arrays[Mesh::ARRAY_VERTEX];
    if (arrays[Mesh::ARRAY_NORMAL].get_type() == Variant::PACKED_VECTOR3_ARRAY)
        s.normals = arrays[Mesh::ARRAY_NORMAL];
    if (arrays[Mesh::ARRAY_TANGENT].get_type() == Variant::PACKED_FLOAT32_ARRAY)
        s.tangents = arrays[Mesh::ARRAY_TANGENT];
    auto* rs = RenderingServer::get_singleton();
    const Dictionary data = rs->mesh_get_surface(mesh->get_rid(), surface);
    s.bytes = data["vertex_data"];
    const BitField<RenderingServer::ArrayFormat> format =
        uint64_t(mesh->surface_get_format(surface));
    const auto count = static_cast<int32_t>(s.rest.size());
    s.vertex_stride = rs->mesh_surface_get_format_vertex_stride(format, count);
    s.normal_stride = rs->mesh_surface_get_format_normal_tangent_stride(format, count);
    s.normal_offset =
        rs->mesh_surface_get_format_offset(format, count, RenderingServer::ARRAY_NORMAL);
    s.weights.resize(static_cast<size_t>(count));

    head_surfaces_.push_back(std::move(s));
}
bool AvatarModel::in_head_subtree(size_t node, int32_t ancestor) const {
    for (size_t count = 0; count < nodes_.size() && node < nodes_.size(); ++count) {
        if (static_cast<int32_t>(node) == ancestor)
            return true;
        const int32_t parent = nodes_[node].parent;
        if (parent < 0)
            break;
        node = static_cast<size_t>(parent);
    }
    return false;
}
bool AvatarModel::configure_head_region(const Dictionary& input, const Dictionary& face,
                                        const Variant& pivot) {
    auto invalid = [&]() {
        last_error_code_ = "AVT-117";
        last_error_message_ = "head region requires a plane below the face, positive falloff, or a "
                              "unique subtree containing the face";
        return false;
    };
    if (!loaded_ || !face.has("origin") || !face.has("normal") || !face.has("scale"))
        return invalid();
    if (face["origin"].get_type() != Variant::VECTOR3 ||
        face["normal"].get_type() != Variant::VECTOR3)
        return invalid();
    const auto scalar = [](const Variant& v) {
        return v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT;
    };
    if (!scalar(face["scale"]) || (face.has("up") && face["up"].get_type() != Variant::VECTOR3))
        return invalid();
    const Vector3 origin = face["origin"], normal = face["normal"];
    const float scale = face["scale"];
    const Vector3 up = face.get("up", Vector3(0, 1, 0));
    if (!origin.is_finite() || !normal.is_finite() || normal.length_squared() < 1e-8 ||
        !up.is_finite() || up.length_squared() < 1e-8 || !std::isfinite(scale) || scale <= 0)
        return invalid();
    Dictionary region = input.duplicate(true);
    bool derived = false;
    if (region.is_empty()) {
        int matches = 0;
        String name;
        for (const auto& n : nodes_)
            if (n.name.to_lower() == "head") {
                ++matches;
                name = n.name;
            }
        if (matches == 1) {
            region["mode"] = "node";
            region["node"] = name;
        } else {
            derived = true;
            region["mode"] = "cut";
            Dictionary cut;
            cut["origin"] = origin - up.normalized() * (scale * 1.5f);
            cut["normal"] = up.normalized();
            cut["falloff"] = scale * 0.5;
            region["cut"] = cut;
        }
    }
    const String mode = region.get("mode", "");
    int32_t selected = -1;
    Vector3 new_pivot, cut_origin, cut_normal;
    AABB head_bounds;
    bool has_head_bounds = false;
    double falloff = 0;
    if (mode == "node") {
        const String name = region.get("node", "");
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i].name == name) {
                if (selected != -1)
                    return invalid();
                selected = static_cast<int32_t>(i);
            }
        if (selected < 0)
            return invalid();
        AABB bounds;
        bool found = false;
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (in_head_subtree(i, selected) && nodes_[i].has_aabb) {
                bounds = found ? bounds.merge(nodes_[i].aabb) : nodes_[i].aabb;
                found = true;
            }
        if (!found || !bounds.grow(scale * 0.01f).has_point(origin))
            return invalid();
        head_bounds = bounds;
        has_head_bounds = true;
        // An explicit face_submesh must belong entirely to this subtree.
        if (!face_submesh_.is_empty())
            for (const auto& s : head_surfaces_)
                if (s.decal && !in_head_subtree(s.node, selected))
                    return invalid();
        new_pivot = nodes_[static_cast<size_t>(selected)].model_xform.origin;
    } else if (mode == "cut") {
        if (region.get("cut", Variant()).get_type() != Variant::DICTIONARY)
            return invalid();
        const Dictionary cut = region["cut"];
        if (cut.get("origin", Variant()).get_type() != Variant::VECTOR3 ||
            cut.get("normal", Variant()).get_type() != Variant::VECTOR3)
            return invalid();
        cut_origin = cut["origin"];
        cut_normal = cut["normal"];
        if (!scalar(cut.get("falloff", Variant())))
            return invalid();
        falloff = cut["falloff"];
        if (!cut_origin.is_finite() || !cut_normal.is_finite() ||
            std::abs(cut_normal.length() - 1) > 0.001 || !std::isfinite(falloff) || falloff <= 0 ||
            cut_normal.dot(up.normalized()) <= 0 || (origin - cut_origin).dot(cut_normal) <= 0)
            return invalid();
        if (pivot.get_type() != Variant::NIL && pivot.get_type() != Variant::VECTOR3)
            return invalid();
        new_pivot = pivot.get_type() == Variant::NIL
                        ? vector(avatar::rig::head_pivot(point(origin), point(normal.normalized()),
                                                         static_cast<float>(scale)))
                        : Vector3(pivot);
        if (!new_pivot.is_finite())
            return invalid();
    } else
        return invalid();
    // Commit only after validation. Reset the old subtree and vertices before changing modes.
    set_head_rotation(Quaternion());
    head_node_ = selected;
    head_pivot_ = new_pivot;
    cut_origin_ = cut_origin;
    cut_normal_ = cut_normal;
    cut_falloff_ = static_cast<float>(falloff);
    head_region_ = region;
    head_region_["pivot"] = new_pivot;
    head_region_["derived"] = derived;
    last_error_code_ = String();
    last_error_message_ = String();
    const String warning =
        "RIG-006: head region missing; derived a default cut from the face surface";
    if (warnings_.has(warning))
        warnings_.remove_at(warnings_.find(warning));
    if (derived)
        warnings_.append(warning);
    for (auto& s : head_surfaces_)
        for (int64_t i = 0; i < s.rest.size(); ++i)
            s.weights[static_cast<size_t>(i)] =
                selected >= 0
                    ? 0
                    : avatar::rig::head_weight(point(nodes_[s.node].model_xform.xform(s.rest[i])),
                                               point(cut_origin), point(cut_normal), cut_falloff_);
    if (selected < 0) {
        // Cut mode has no node AABB: derive the head bounds from the vertices
        // that actually move. The framing sweep then bounds only the head, not
        // the untouched body.
        has_head_bounds = false;
        for (const auto& s : head_surfaces_) {
            const Transform3D& transform = nodes_[s.node].model_xform;
            for (int64_t i = 0; i < s.rest.size(); ++i) {
                if (s.weights[static_cast<size_t>(i)] <= 0)
                    continue;
                const Vector3 vertex = transform.xform(s.rest[i]);
                if (!has_head_bounds) {
                    head_bounds = AABB(vertex, Vector3());
                    has_head_bounds = true;
                } else {
                    head_bounds.expand_to(vertex);
                }
            }
        }
    }
    head_aabb_ = has_head_bounds ? head_bounds : AABB();
    has_head_aabb_ = has_head_bounds;
    face_surface_ = face.duplicate(true);
    float radius = 0;
    for (const auto& s : head_surfaces_)
        for (int64_t i = 0; i < s.rest.size(); ++i)
            radius = std::max(radius,
                              nodes_[s.node].model_xform.xform(s.rest[i]).distance_to(head_pivot_));
    const Vector3 extent(radius, radius, radius);
    const AABB swept(head_pivot_ - extent, extent * 2);
    for (auto& s : head_surfaces_)
        s.mesh->set_custom_aabb(nodes_[s.node].model_xform.affine_inverse().xform(swept));
    return true;
}
void AvatarModel::set_head_rotation(const Quaternion& rotation) {
    if (!rotation.is_finite() || rotation.length_squared() < 1e-8)
        return;
    head_rotation_ = rotation.normalized();
    if (head_node_ >= 0) {
        const size_t index = static_cast<size_t>(head_node_);
        const Transform3D& rest = nodes_[index].model_xform;
        const Transform3D deformation(Basis(head_rotation_),
                                      head_pivot_ - Basis(head_rotation_).xform(head_pivot_));
        const int32_t parent = nodes_[index].parent;
        const Transform3D parent_rest =
            parent < 0 ? conversion_ : nodes_[static_cast<size_t>(parent)].model_xform;
        node_objects_[index]->set_transform(parent_rest.affine_inverse() * deformation * rest);
        return;
    }
    const avatar::rig::HeadRotation q(
        {head_rotation_.x, head_rotation_.y, head_rotation_.z, head_rotation_.w});
    const float full_sine = std::sin(q.angle), full_cosine = std::cos(q.angle);
    for (auto& s : head_surfaces_) {
        if (s.bytes.is_empty())
            continue;
        uint8_t* bytes = s.bytes.ptrw();
        const auto& transform = nodes_[s.node].model_xform;
        const Transform3D inverse = transform.affine_inverse();
        const Basis normal_to_model = transform.basis.inverse().transposed();
        const Basis normal_to_local = transform.basis.transposed();
        for (int64_t i = 0; i < s.rest.size(); ++i) {
            const float w = s.weights[static_cast<size_t>(i)];
            if (w == 0)
                continue;
            const float sine = w == 1 ? full_sine : std::sin(q.angle * w);
            const float cosine = w == 1 ? full_cosine : std::cos(q.angle * w);
            Vector3 p = s.rest[i];
            if (w > 0 && q.angle != 0)
                p = inverse.xform(
                    head_pivot_ +
                    vector(q.rotate(point(transform.xform(p) - head_pivot_), sine, cosine)));
            const float xyz[3] = {p.x, p.y, p.z};
            std::memcpy(bytes + i * s.vertex_stride, xyz, 12);
            if (s.normals.size() == s.rest.size()) {
                Vector3 n = s.normals[i];
                if (w > 0 && q.angle != 0)
                    n = normal_to_local.xform(
                        vector(q.rotate(point(normal_to_model.xform(n)), sine, cosine)));
                pack_oct(bytes + s.normal_offset + i * s.normal_stride,
                         n.normalized().octahedron_encode());
            }
            if (s.tangents.size() == s.rest.size() * 4) {
                Vector3 t(s.tangents[i * 4], s.tangents[i * 4 + 1], s.tangents[i * 4 + 2]);
                if (w > 0 && q.angle != 0)
                    t = inverse.basis.xform(
                        vector(q.rotate(point(transform.basis.xform(t)), sine, cosine)));
                pack_oct(bytes + s.normal_offset + i * s.normal_stride + 4,
                         t.normalized().octahedron_tangent_encode(s.tangents[i * 4 + 3]));
            }
        }
        s.mesh->surface_update_vertex_region(s.surface, 0, s.bytes);
    }
}
Dictionary AvatarModel::get_head_region() const {
    return head_region_.duplicate(true);
}
AABB AvatarModel::get_head_aabb() const {
    return has_head_aabb_ ? head_aabb_ : AABB();
}
Dictionary AvatarModel::get_face_surface() const {
    return face_surface_.duplicate(true);
}
Node3D* AvatarModel::create_decal_pass(const Ref<ShaderMaterial>& material) {
    if (material.is_null() || !loaded_)
        return nullptr;
    clear_decal_pass();
    Node3D* first = nullptr;
    // One child per mesh, regardless of its surface count. Non-face surfaces use an invisible
    // shader.
    Ref<Shader> invisible_shader;
    invisible_shader.instantiate();
    invisible_shader->set_code("shader_type spatial; void fragment() { discard; }");
    Ref<ShaderMaterial> invisible;
    invisible.instantiate();
    invisible->set_shader(invisible_shader);
    for (size_t i = 0; i < nodes_.size(); ++i) {
        Ref<ArrayMesh> mesh;
        bool include = false;
        for (const auto& s : head_surfaces_)
            if (s.node == i) {
                mesh = s.mesh;
                include = include || s.decal;
            }
        if (!include || mesh.is_null())
            continue;
        auto* decal = memnew(MeshInstance3D);
        decal->set_name("HeadDecal");
        decal->set_mesh(mesh);
        decal->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
        for (const auto& s : head_surfaces_)
            if (s.node == i)
                decal->set_surface_override_material(s.surface, s.decal ? material : invisible);
        node_objects_[i]->add_child(decal);
        decal_objects_.push_back(decal);
        if (first == nullptr)
            first = decal;
    }
    return first;
}
void AvatarModel::clear_decal_pass() {
    for (auto* object : decal_objects_) {
        if (object->get_parent() != nullptr)
            object->get_parent()->remove_child(object);
        memdelete(object);
    }
    decal_objects_.clear();
}
Array AvatarModel::get_head_surface_data() const {
    Array out;
    for (const auto& s : head_surfaces_) {
        Dictionary item;
        item["rest_local"] = s.rest;
        item["rest_normals"] = s.normals;
        item["rest_tangents"] = s.tangents;
        item["node"] = nodes_[s.node].name;
        item["rest_transform"] = nodes_[s.node].model_xform;
        PackedVector3Array vertices, normals;
        vertices.resize(s.rest.size());
        normals.resize(s.normals.size());
        for (int64_t i = 0; i < s.rest.size(); ++i) {
            float xyz[3];
            std::memcpy(xyz, s.bytes.ptr() + i * s.vertex_stride, 12);
            vertices.set(i, Vector3(xyz[0], xyz[1], xyz[2]));
            if (i < s.normals.size()) {
                uint16_t oct[2];
                std::memcpy(oct, s.bytes.ptr() + s.normal_offset + i * s.normal_stride, 4);
                normals.set(
                    i, Vector3::octahedron_decode(Vector2(oct[0] / 65535.0f, oct[1] / 65535.0f)));
            }
        }
        PackedFloat32Array tangents;
        tangents.resize(s.tangents.size());
        for (int64_t i = 0; i < s.tangents.size() / 4; ++i) {
            uint16_t oct[2];
            std::memcpy(oct, s.bytes.ptr() + s.normal_offset + i * s.normal_stride + 4, 4);
            float sign = 1;
            const Vector3 t = Vector3::octahedron_tangent_decode(
                Vector2(oct[0] / 65535.0f, oct[1] / 65535.0f), &sign);
            tangents.set(i * 4, t.x);
            tangents.set(i * 4 + 1, t.y);
            tangents.set(i * 4 + 2, t.z);
            tangents.set(i * 4 + 3, sign);
        }
        item["tangents"] = tangents;
        item["vertices"] = vertices;
        item["normals"] = normals;
        Transform3D current;
        if (head_node_ >= 0 && in_head_subtree(s.node, head_node_)) {
            current = Transform3D(Basis(head_rotation_),
                                  head_pivot_ - Basis(head_rotation_).xform(head_pivot_));
        }
        item["transform"] = current * nodes_[s.node].model_xform;
        item["decal"] = s.decal;
        out.append(item);
    }
    return out;
}
