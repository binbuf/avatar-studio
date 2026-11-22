"""Original T21 archetypes: flat slab, spherical head and sculpted nose head.

No UVs are required by the decal. The closed meshes expose rear-surface leakage
and a narrow projecting nose exposes depth occlusion at +/-30 degree yaw.
"""
import math
import struct

from gen_model_fixtures import GlbBuilder, OUT, write_glb


def build(kind):
    b = GlbBuilder()
    if kind == "flat":
        mesh = {"primitives": [b.add_box((1.0, 1.4, .3), (-.5, -.7, -.15))["primitive"]]}
    else:
        rows, columns = 40, 48

        def point(v, u):
            y = .7 * math.sin(v)
            r = math.cos(v)
            x, z = .5 * r * math.sin(u), -.4 * r * math.cos(u)
            if kind == "sculpted":
                # A tall narrow nose, not a separate decal/occluder mesh.
                z -= .48 * math.exp(-(x / .085)**2 - (y / .24)**2) * max(0, math.cos(u))**8
                # Stylised cheek/chin curvature.
                x *= 1 + .12 * math.sin(5 * v)
            return (x, y, z)

        vertices, normals, indices = [], [], []
        for j in range(rows + 1):
            v = -math.pi / 2 + math.pi * j / rows
            for i in range(columns + 1):
                u = -math.pi + 2 * math.pi * i / columns
                vertices.append(point(v, u))
                # Independent finite-difference surface normal (outwards).
                a, c = point(v + .0001, u), point(v - .0001, u)
                d, e = point(v, u + .0001), point(v, u - .0001)
                dv = [a[k] - c[k] for k in range(3)]
                du = [d[k] - e[k] for k in range(3)]
                n = [dv[1]*du[2]-dv[2]*du[1], dv[2]*du[0]-dv[0]*du[2], dv[0]*du[1]-dv[1]*du[0]]
                length = math.sqrt(sum(x*x for x in n))
                normals.append(tuple(x / length for x in n) if length > 1e-12 else (0, math.copysign(1, v), 0))
        for j in range(rows):
            for i in range(columns):
                a = j * (columns + 1) + i
                indices.extend((a, a+columns+1, a+1, a+1, a+columns+1, a+columns+2))

        def accessor(values, shape, fmt, component, target):
            data = b''.join(struct.pack(fmt, *v) if isinstance(v, tuple) else struct.pack(fmt, v) for v in values)
            view = b.add_view(data, target)
            lo = [min(p[k] for p in vertices) for k in range(3)] if values is vertices else None
            hi = [max(p[k] for p in vertices) for k in range(3)] if values is vertices else None
            return b.add_accessor(view, component, len(values), shape, lo, hi)

        mesh = {"primitives": [{"attributes": {
            "POSITION": accessor(vertices, "VEC3", "<3f", 5126, 34962),
            "NORMAL": accessor(normals, "VEC3", "<3f", 5126, 34962)},
            "indices": accessor(indices, "SCALAR", "<I", 5125, 34963), "material": 0}]}
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
           "nodes": [{"name": "face", "mesh": 0}], "meshes": [mesh],
           "materials": [{"name": "skin", "doubleSided": True, "pbrMetallicRoughness": {
               "baseColorFactor": [.22, .45, .32, 1], "metallicFactor": 0}}],
           "buffers": [{"byteLength": len(b.buffer)}], "bufferViews": b.buffer_views, "accessors": b.accessors}
    write_glb(OUT / ("surface_" + kind + ".glb"), doc, bytes(b.buffer))


if __name__ == "__main__":
    for archetype in ("flat", "sphere", "sculpted"):
        build(archetype)
