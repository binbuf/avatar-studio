#!/usr/bin/env python3
"""Generate the tiny GLB fixtures used by the T17 runtime import tests.

The corpus exists to catch real import problems without re-exporting or editing
the development model. These fixtures are deliberately small and generated from
plain glTF 2.0 JSON plus a binary buffer:

  feet_origin.glb    Y-up, metres, origin at the feet; one box, 12 triangles.
  axis_variant.glb   Z-up, +Y forward; one box whose converted size is known.
  animated_rig.glb   One skinned mesh plus a translation animation. The importer
                     must ignore both and keep the bind pose.
  invalid.glb        Not a GLB at all; must fail with AVT-102.

Run from the repository root:

    python scripts/gen_model_fixtures.py

The generated .glb files are tracked; the script is the reproducible source.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tests" / "fixtures" / "models"


def pad4(data: bytes, fill: bytes = b"\x00") -> bytes:
    remainder = len(data) % 4
    return data if remainder == 0 else data + fill * (4 - remainder)


class GlbBuilder:
    def __init__(self) -> None:
        self.buffer = bytearray()
        self.buffer_views: list[dict] = []
        self.accessors: list[dict] = []

    def add_view(self, data: bytes, target: int | None = None) -> int:
        while len(self.buffer) % 4:
            self.buffer.append(0)
        offset = len(self.buffer)
        self.buffer.extend(data)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.buffer_views.append(view)
        return len(self.buffer_views) - 1

    def add_accessor(
        self,
        view: int,
        component_type: int,
        count: int,
        accessor_type: str,
        minimum: list | None = None,
        maximum: list | None = None,
    ) -> int:
        accessor = {
            "bufferView": view,
            "componentType": component_type,
            "count": count,
            "type": accessor_type,
        }
        if minimum is not None:
            accessor["min"] = minimum
        if maximum is not None:
            accessor["max"] = maximum
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def add_box(
        self,
        size: tuple[float, float, float],
        origin: tuple[float, float, float] = (0.0, 0.0, 0.0),
        with_skin: bool = False,
    ) -> dict:
        sx, sy, sz = size
        ox, oy, oz = origin
        # Six faces, four vertices each, so every face has a correct normal.
        faces = [
            ((1, 0, 0), [(sx, 0, 0), (sx, sy, 0), (sx, sy, sz), (sx, 0, sz)]),
            ((-1, 0, 0), [(0, 0, 0), (0, 0, sz), (0, sy, sz), (0, sy, 0)]),
            ((0, 1, 0), [(0, sy, 0), (0, sy, sz), (sx, sy, sz), (sx, sy, 0)]),
            ((0, -1, 0), [(0, 0, 0), (sx, 0, 0), (sx, 0, sz), (0, 0, sz)]),
            ((0, 0, 1), [(0, 0, sz), (sx, 0, sz), (sx, sy, sz), (0, sy, sz)]),
            ((0, 0, -1), [(0, 0, 0), (0, sy, 0), (sx, sy, 0), (sx, 0, 0)]),
        ]
        positions: list[tuple[float, float, float]] = []
        normals: list[tuple[float, float, float]] = []
        indices: list[int] = []
        for normal, corners in faces:
            base = len(positions)
            for corner in corners:
                positions.append((corner[0] + ox, corner[1] + oy, corner[2] + oz))
                normals.append(normal)
            indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])

        position_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
        normal_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
        index_bytes = b"".join(struct.pack("<H", i) for i in indices)

        position_view = self.add_view(position_bytes, target=34962)
        normal_view = self.add_view(normal_bytes, target=34962)
        index_view = self.add_view(index_bytes, target=34963)

        min_pos = [min(p[i] for p in positions) for i in range(3)]
        max_pos = [max(p[i] for p in positions) for i in range(3)]
        position_accessor = self.add_accessor(
            position_view, 5126, len(positions), "VEC3", min_pos, max_pos
        )
        normal_accessor = self.add_accessor(normal_view, 5126, len(normals), "VEC3")
        index_accessor = self.add_accessor(index_view, 5123, len(indices), "SCALAR")

        attributes = {
            "POSITION": position_accessor,
            "NORMAL": normal_accessor,
        }
        if with_skin:
            joint_bytes = b"".join(struct.pack("<4B", 0, 0, 0, 0) for _ in positions)
            weight_bytes = b"".join(struct.pack("<4f", 1.0, 0.0, 0.0, 0.0) for _ in positions)
            joint_view = self.add_view(joint_bytes, target=34962)
            weight_view = self.add_view(weight_bytes, target=34962)
            attributes["JOINTS_0"] = self.add_accessor(joint_view, 5121, len(positions), "VEC4")
            attributes["WEIGHTS_0"] = self.add_accessor(weight_view, 5126, len(positions), "VEC4")

        primitive = {
            "attributes": attributes,
            "indices": index_accessor,
            "material": 0,
            "mode": 4,
        }
        return {"primitive": primitive, "positions": positions}


def write_glb(path: Path, document: dict, binary: bytes) -> None:
    json_bytes = pad4(json.dumps(document, separators=(",", ":")).encode("utf-8"), b" ")
    binary_bytes = pad4(binary, b"\x00")
    total = 12 + 8 + len(json_bytes) + 8 + len(binary_bytes)
    payload = struct.pack("<III", 0x46546C67, 2, total)
    payload += struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes
    payload += struct.pack("<II", len(binary_bytes), 0x004E4942) + binary_bytes
    path.write_bytes(payload)


def build_box_fixture(path: Path, size, origin, node_name: str, mesh_name: str,
                      material_name: str) -> None:
    builder = GlbBuilder()
    box = builder.add_box(size, origin)
    document = {
        "asset": {"version": "2.0", "generator": "avatar-studio fixtures"},
        "scene": 0,
        "scenes": [{"name": "Scene", "nodes": [0]}],
        "nodes": [{"name": node_name, "mesh": 0}],
        "meshes": [{"name": mesh_name, "primitives": [box["primitive"]]}],
        "materials": [
            {
                "name": material_name,
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.8, 0.55, 0.3, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.7,
                },
            }
        ],
        "buffers": [{"byteLength": len(builder.buffer)}],
        "bufferViews": builder.buffer_views,
        "accessors": builder.accessors,
    }
    write_glb(path, document, bytes(builder.buffer))


def build_animated_rig(path: Path) -> None:
    builder = GlbBuilder()
    box = builder.add_box((0.2, 0.2, 0.2), (-0.1, -0.1, -0.1), with_skin=True)

    identity = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
    inverse_bind_view = builder.add_view(struct.pack("<16f", *identity))
    inverse_bind_accessor = builder.add_accessor(inverse_bind_view, 5126, 1, "MAT4")

    times = [0.0, 1.0]
    time_bytes = b"".join(struct.pack("<f", t) for t in times)
    time_view = builder.add_view(time_bytes)
    time_accessor = builder.add_accessor(time_view, 5126, len(times), "SCALAR", [0.0], [1.0])

    translations = [(0.0, 0.0, 0.0), (5.0, 5.0, 5.0)]
    translation_bytes = b"".join(struct.pack("<3f", *t) for t in translations)
    translation_view = builder.add_view(translation_bytes)
    translation_accessor = builder.add_accessor(
        translation_view, 5126, len(translations), "VEC3"
    )

    document = {
        "asset": {"version": "2.0", "generator": "avatar-studio fixtures"},
        "scene": 0,
        "scenes": [{"name": "Scene", "nodes": [0]}],
        "nodes": [
            {"name": "root", "children": [1, 2]},
            {"name": "head", "mesh": 0, "skin": 0},
            {"name": "bone"},
        ],
        "meshes": [{"name": "head", "primitives": [box["primitive"]]}],
        "materials": [
            {
                "name": "skin",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.9, 0.7, 0.6, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.6,
                },
            }
        ],
        "skins": [
            {
                "name": "rig",
                "joints": [2],
                "skeleton": 2,
                "inverseBindMatrices": inverse_bind_accessor,
            }
        ],
        "animations": [
            {
                "name": "wave",
                "samplers": [
                    {"input": time_accessor, "output": translation_accessor,
                     "interpolation": "LINEAR"}
                ],
                "channels": [
                    {"sampler": 0, "target": {"node": 1, "path": "translation"}}
                ],
            }
        ],
        "buffers": [{"byteLength": len(builder.buffer)}],
        "bufferViews": builder.buffer_views,
        "accessors": builder.accessors,
    }
    write_glb(path, document, bytes(builder.buffer))


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    build_box_fixture(
        OUT / "feet_origin.glb",
        (0.6, 1.8, 0.3),
        (0.0, 0.0, 0.0),
        "character",
        "body",
        "body",
    )
    build_box_fixture(
        OUT / "axis_variant.glb",
        (0.4, 0.2, 1.8),
        (0.0, 0.0, 0.0),
        "head",
        "head",
        "skin",
    )
    build_animated_rig(OUT / "animated_rig.glb")
    (OUT / "invalid.glb").write_bytes(b"not a glb\n")
    for path in sorted(OUT.glob("*.glb")):
        print(f"{path.relative_to(ROOT)}: {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
