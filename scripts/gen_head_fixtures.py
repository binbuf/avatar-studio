"""T19 reproducible 20k-triangle neck grid and clean point-seam node fixtures."""
import math
import struct
from gen_model_fixtures import GlbBuilder, write_glb, OUT


def grid(builder, rows, columns, bottom, height, node=False, body=False):
    vertices, normals, indices = [], [], []
    for j in range(rows + 1):
        y = bottom + height * j / rows
        if node:
            width = 0.30 * math.sin(math.pi * j / (2 * rows))
        elif body:
            width = 0.65 * (1 - j / rows)
        else:
            width = 0.65 if y < 0.9 else (0.12 if y < 1.35 else 0.3)
            # Smoothly narrow shoulders and widen head, without cracks or split surfaces.
            if 0.9 <= y < 1.1: width = 0.65 - (y - 0.9) / 0.2 * 0.53
            if 1.25 <= y < 1.5: width = 0.12 + (y - 1.25) / 0.25 * 0.18
        for i in range(columns + 1):
            vertices.append(((i / columns * 2 - 1) * width, y, 0 if node else -0.15))
            normals.append((0, 0, -1))
    for j in range(rows):
        for i in range(columns):
            a = j * (columns + 1) + i
            indices.extend((a, a+columns+1, a+1, a+1, a+columns+1, a+columns+2))
    def accessor(values, shape, count, fmt, component, target):
        data = b''.join(struct.pack(fmt, *v) if isinstance(v, tuple) else struct.pack(fmt, v) for v in values)
        v = builder.add_view(data, target)
        lo = [min(p[k] for p in vertices) for k in range(3)] if shape == 'VEC3' else None
        hi = [max(p[k] for p in vertices) for k in range(3)] if shape == 'VEC3' else None
        return builder.add_accessor(v, component, count, shape, lo if values is vertices else None, hi if values is vertices else None)
    p = accessor(vertices, 'VEC3', len(vertices), '<3f', 5126, 34962)
    n = accessor(normals, 'VEC3', len(normals), '<3f', 5126, 34962)
    uv = accessor([(i / columns, j / rows) for j in range(rows + 1) for i in range(columns + 1)], 'VEC2', len(vertices), '<2f', 5126, 34962)
    tangent = accessor([(1.0, 0.0, 0.0, 1.0)] * len(vertices), 'VEC4', len(vertices), '<4f', 5126, 34962)
    idx = accessor(indices, 'SCALAR', len(indices), '<I', 5125, 34963)
    return {'primitives': [{'attributes': {'POSITION': p, 'NORMAL': n, 'TANGENT': tangent, 'TEXCOORD_0': uv}, 'indices': idx, 'material': 0}]}


def build(node):
    b = GlbBuilder()
    if node:
        meshes = [grid(b, 60, 100, 0, 1.2, body=True), grid(b, 40, 100, 0, .8, node=True)]
        nodes = [{'name': 'body', 'mesh': 0}, {'name': 'head', 'translation': [0, 1.2, -.15], 'children': [2]}, {'name': 'face', 'mesh': 1}]
        roots = [0, 1]
    else:
        meshes = [grid(b, 100, 100, 0, 2)]
        nodes = [{'name': 'character', 'mesh': 0}]
        roots = [0]
    doc = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': roots}], 'nodes': nodes,
           'meshes': meshes, 'materials': [{'name': 'skin', 'doubleSided': True, 'pbrMetallicRoughness': {'baseColorFactor': [.15, .65, .4, 1], 'metallicFactor': 0}}],
           'buffers': [{'byteLength': len(b.buffer)}], 'bufferViews': b.buffer_views, 'accessors': b.accessors}
    write_glb(OUT / ('head_node.glb' if node else 'head_cut.glb'), doc, bytes(b.buffer))


if __name__ == '__main__':
    build(False)
    build(True)
    # Independent nested/nonuniform authored transform fixture.
    import json
    source = (OUT / 'head_cut.glb').read_bytes()
    size = struct.unpack_from('<I', source, 12)[0]
    doc = json.loads(source[20:20+size])
    doc['nodes'] = [{'name': 'root', 'translation': [.03, .1, .02], 'children': [1]},
                    {'name': 'character', 'mesh': 0, 'scale': [.8, 1.1, 1.3],
                     'rotation': [.12, .2, .1, math.sqrt(1 - .12**2 - .2**2 - .1**2)]}]
    write_glb(OUT / 'head_transformed.glb', doc, source[28+size:])
