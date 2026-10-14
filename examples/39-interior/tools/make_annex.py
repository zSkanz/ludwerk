"""Writes the annex of examples/39-interior: ONE mesh that is a whole room.

    python examples/39-interior/tools/make_annex.py

A room six metres square and three high, with walls a quarter of a metre
thick, a window and a door in its south wall, and a tiled floor. It is one
glTF file with two materials -- plaster, and tiles with a picture -- which is
how a building arrives from a modelling program, and is the case the bounced
light of ADR 0203 has to work for: to the physics world this mesh is one
hull, its door and window shut, and the light is worked out against its
triangles instead.

Needs Pillow for the floor's picture. What it writes is committed; this is
kept so the room can be changed.
"""

import base64
import json
import struct
from pathlib import Path

from PIL import Image

OUT = Path(__file__).resolve().parents[1] / "content" / "models"

HALF = 3.0
THICK = 0.25
HIGH = 3.0
# The south wall's openings, along x and up y.
WINDOW = (-2.2, -0.2, 0.9, 2.3)
DOOR = (1.0, 2.0, 0.0, 2.1)


def box(low, high):
    """Six faces of a box, each four corners wound to look outwards, with its normal."""
    x0, y0, z0 = low
    x1, y1, z1 = high
    return [
        ((1, 0, 0), [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)]),
        ((-1, 0, 0), [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)]),
        ((0, 1, 0), [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]),
        ((0, -1, 0), [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)]),
        ((0, 0, 1), [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]),
        ((0, 0, -1), [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)]),
    ]


def primitive(boxes, tile):
    """Positions, normals, texture coordinates and indices for a list of boxes."""
    positions, normals, coords, indices = [], [], [], []
    for low, high in boxes:
        for normal, corners in box(low, high):
            first = len(positions)
            for x, y, z in corners:
                positions.append((x, y, z))
                normals.append(normal)
                # A tile every `tile` metres, laid out along the two sides
                # the face does not look along.
                if normal[1] != 0:
                    coords.append((x / tile, z / tile))
                elif normal[0] != 0:
                    coords.append((z / tile, y / tile))
                else:
                    coords.append((x / tile, y / tile))
            indices += [first, first + 1, first + 2, first, first + 2, first + 3]
    return positions, normals, coords, indices


def main():
    OUT.mkdir(parents=True, exist_ok=True)

    # The floor's picture: terracotta tiles with a darker joint.
    side = 64
    picture = Image.new("RGB", (side, side))
    for y in range(side):
        for x in range(side):
            joint = x % 32 < 2 or y % 32 < 2
            warm = ((x // 32) + (y // 32)) % 2 == 0
            picture.putpixel((x, y), (96, 52, 36) if joint else ((204, 98, 58) if warm else (186, 84, 50)))
    picture.save(OUT / "annex_tiles.png")

    outer = HALF + THICK
    # **Every piece runs on into its neighbours** -- a wall up into the
    # ceiling and down into the floor, and round each corner into the next
    # wall. Pieces that only touch share an edge that is one piece's corner
    # and the middle of another's side, and a renderer leaves a hairline of
    # daylight along such an edge.
    low, top = -THICK, HIGH + THICK
    plaster = [
        ((-outer, HIGH, -outer), (outer, top, outer)),  # the ceiling
        ((-outer, low, -outer), (outer, top, -HALF)),  # north
        ((-outer, low, -outer), (-HALF, top, outer)),  # west
        ((HALF, low, -outer), (outer, top, outer)),  # east
        # The south wall, round a window and a door.
        ((-outer, low, HALF), (WINDOW[0], top, outer)),
        ((WINDOW[0] - 0.05, low, HALF), (WINDOW[1] + 0.05, WINDOW[2], outer)),
        ((WINDOW[0] - 0.05, WINDOW[3], HALF), (WINDOW[1] + 0.05, top, outer)),
        ((WINDOW[1], low, HALF), (DOOR[0], top, outer)),
        ((DOOR[0] - 0.05, DOOR[3], HALF), (DOOR[1] + 0.05, top, outer)),
        ((DOOR[1], low, HALF), (outer, top, outer)),
    ]
    tiles = [((-outer, -THICK, -outer), (outer, 0, outer))]

    blob = bytearray()
    views, accessors, primitives = [], [], []

    def view(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        entry = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
        if target is not None:
            entry["target"] = target
        blob.extend(data)
        views.append(entry)
        return len(views) - 1

    def accessor(values, kind, component, width, target, bounds=False):
        flat = [number for value in values for number in (value if width > 1 else (value,))]
        data = struct.pack("<" + component[1] * len(flat), *flat)
        entry = {
            "bufferView": view(data, target),
            "componentType": component[0],
            "count": len(values),
            "type": kind,
        }
        if bounds:
            entry["min"] = [min(value[axis] for value in values) for axis in range(width)]
            entry["max"] = [max(value[axis] for value in values) for axis in range(width)]
        accessors.append(entry)
        return len(accessors) - 1

    for material, (boxes, tile) in enumerate(((plaster, 1.0), (tiles, 1.0))):
        positions, normals, coords, indices = primitive(boxes, tile)
        primitives.append(
            {
                "attributes": {
                    "POSITION": accessor(positions, "VEC3", (5126, "f"), 3, 34962, bounds=True),
                    "NORMAL": accessor(normals, "VEC3", (5126, "f"), 3, 34962),
                    "TEXCOORD_0": accessor(coords, "VEC2", (5126, "f"), 2, 34962),
                },
                "indices": accessor(indices, "SCALAR", (5125, "I"), 1, 34963),
                "material": material,
            }
        )

    document = {
        "asset": {"version": "2.0", "generator": "examples/39-interior/tools/make_annex.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Annex", "mesh": 0}],
        "meshes": [{"name": "Annex", "primitives": primitives}],
        "materials": [
            {
                "name": "Plaster",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.86, 0.85, 0.82, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.9,
                },
            },
            {
                "name": "Tiles",
                "pbrMetallicRoughness": {
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.6,
                },
            },
        ],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"wrapS": 10497, "wrapT": 10497}],
        "images": [{"uri": "annex_tiles.png"}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [
            {
                "byteLength": len(blob),
                "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode("ascii"),
            }
        ],
    }
    (OUT / "annex.gltf").write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8", newline="\n")
    print("wrote", OUT / "annex.gltf", "and annex_tiles.png")


if __name__ == "__main__":
    main()
