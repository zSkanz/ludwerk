"""Reads a glTF file, cuts it down and writes it as one `.glb`.

What `make_content.py` uses to make this example's models and its clip library
out of the packs they come from. The standard library and nothing else: a glTF
file is a JSON document and a block of bytes, and everything here is done to
those two.

    doc = Gltf.load(path)         a .gltf with its .bin, or a .glb
    doc.keep_clips({old: new})    the clips to keep, under the names to keep them by
    doc.drop_nodes(predicate)     nodes to leave out, each with everything under it
    doc.drop_rest_channels()      a clip's keys that only repeat where a joint rests
    doc.stretch(x, y, z)          the whole model made wider, taller or deeper
    doc.add_clip(name, keys)      a clip written here: {joint: [(time, quaternion), ...]}
    doc.add_joint(name, under, at) a joint of its own under one the file has
    doc.add_track(clip, joint, keys) where a joint is through a clip the file has
    doc.save(path)                what is still used, packed, as a .glb
"""

import json
import struct
from pathlib import Path

COMPONENTS = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
WIDTHS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}
FLOAT = 5126


class Gltf:
    def __init__(self, doc, buffers):
        self.doc = doc
        # One bytearray a buffer of the document; `save` packs what is used
        # into one.
        self.buffers = buffers

    # --- reading -------------------------------------------------------------

    @staticmethod
    def load(path):
        path = Path(path)
        data = path.read_bytes()
        if data[:4] == b"glTF":
            length = struct.unpack_from("<I", data, 12)[0]
            doc = json.loads(data[20 : 20 + length].decode("utf-8"))
            rest = data[20 + length :]
            binary = bytearray(rest[8 : 8 + struct.unpack_from("<I", rest, 0)[0]]) if len(rest) >= 8 else bytearray()
            return Gltf(doc, [binary])
        doc = json.loads(data.decode("utf-8"))
        buffers = []
        for buffer in doc.get("buffers", []):
            uri = buffer.get("uri", "")
            if uri.startswith("data:"):
                raise ValueError("a buffer inside the document is not read here")
            buffers.append(bytearray((path.parent / uri).read_bytes()))
        return Gltf(doc, buffers)

    def _span(self, accessor):
        """Where an accessor's values are: buffer, first byte, stride, count, format."""
        if "sparse" in accessor:
            raise ValueError("a sparse accessor is not read here")
        view = self.doc["bufferViews"][accessor["bufferView"]]
        width = WIDTHS[accessor["type"]]
        letter = COMPONENTS[accessor["componentType"]]
        size = struct.calcsize(letter) * width
        start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        return view["buffer"], start, view.get("byteStride", size), accessor["count"], "<" + letter * width

    def values(self, index):
        buffer, start, stride, count, layout = self._span(self.doc["accessors"][index])
        return [struct.unpack_from(layout, self.buffers[buffer], start + item * stride) for item in range(count)]

    def _rewrite(self, index, change, done):
        """Puts `change(value)` in place of every value of an accessor, once
        however many things share its bytes."""
        accessor = self.doc["accessors"][index]
        buffer, start, stride, count, layout = self._span(accessor)
        if (buffer, start, stride) in done:
            return
        done.add((buffer, start, stride))
        low = high = None
        for item in range(count):
            value = change(struct.unpack_from(layout, self.buffers[buffer], start + item * stride))
            struct.pack_into(layout, self.buffers[buffer], start + item * stride, *value)
            low = list(value) if low is None else [min(a, b) for a, b in zip(low, value)]
            high = list(value) if high is None else [max(a, b) for a, b in zip(high, value)]
        if "min" in accessor and low is not None:
            accessor["min"], accessor["max"] = low, high

    def node(self, name):
        for index, node in enumerate(self.doc["nodes"]):
            if node.get("name") == name:
                return index
        raise KeyError(name)

    def parents(self):
        parent = {}
        for index, node in enumerate(self.doc["nodes"]):
            for child in node.get("children", []):
                parent[child] = index
        return parent

    # --- cutting -------------------------------------------------------------

    def keep_clips(self, names):
        """`names` is {name in the file: name to keep it by}, and the order
        the clips are written in."""
        by_name = {clip.get("name"): clip for clip in self.doc.get("animations", [])}
        kept = []
        for old, new in names.items():
            if old not in by_name:
                raise KeyError(f"no clip called {old}")
            clip = by_name[old]
            clip["name"] = new
            kept.append(clip)
        self.doc["animations"] = kept

    def drop_nodes(self, unwanted):
        """Leaves out every node `unwanted(node)` answers true for, and what
        hangs from it. A joint of a skin is never left out."""
        joints = {joint for skin in self.doc.get("skins", []) for joint in skin["joints"]}
        nodes = self.doc["nodes"]
        gone = set()

        def mark(index):
            gone.add(index)
            for child in nodes[index].get("children", []):
                mark(child)

        for index, node in enumerate(nodes):
            if index not in joints and unwanted(node):
                mark(index)
        if gone & joints:
            raise ValueError("a joint hangs from a node that was to be left out")
        for node in nodes:
            if "children" in node:
                node["children"] = [child for child in node["children"] if child not in gone]
                if not node["children"]:
                    del node["children"]
        for scene in self.doc.get("scenes", []):
            scene["nodes"] = [index for index in scene["nodes"] if index not in gone]
        self._gone = getattr(self, "_gone", set()) | gone

    def drop_rest_channels(self, tolerance=1.0e-5):
        """Takes out of every clip the translation and scale channels whose
        every key is where the joint rests anyway: most of a clip that was
        exported with every joint keyed whether it moved or not. Rotations are
        left: a rotation key is what says a joint is part of the clip."""
        nodes = self.doc["nodes"]
        rest = {"translation": (0.0, 0.0, 0.0), "scale": (1.0, 1.0, 1.0)}
        cache = {}
        for clip in self.doc.get("animations", []):
            kept = []
            for channel in clip["channels"]:
                path = channel["target"]["path"]
                if path in rest:
                    at_rest = tuple(nodes[channel["target"]["node"]].get(path, rest[path]))
                    output = clip["samplers"][channel["sampler"]]["output"]
                    if output not in cache:
                        cache[output] = self.values(output)
                    if all(abs(a - b) <= tolerance for value in cache[output] for a, b in zip(value, at_rest)):
                        continue
                kept.append(channel)
            clip["channels"] = kept

    def stretch(self, x, y, z):
        """The whole model, at rest, made `x` times as wide, `y` as tall and
        `z` as deep: every vertex, every joint's place and every key that
        moves a joint. A joint's own turn is left alone, so each bone keeps
        its shape and is only longer or shorter by where its end went."""
        by = (x, y, z)
        done = set()
        parent = self.parents()
        nodes = self.doc["nodes"]

        # A translation is said in its parent's own axes; these files' joints
        # are turned every way, so how far each axis of a parent is stretched
        # is worked out from where that axis points in the model.
        def turned(index):
            q = (0.0, 0.0, 0.0, 1.0)
            chain = []
            at = index
            while at is not None:
                chain.append(at)
                at = parent.get(at)
            for at in reversed(chain):
                q = _mul(q, tuple(nodes[at].get("rotation", (0.0, 0.0, 0.0, 1.0))))
            return q

        def in_parent(index, value):
            up = parent.get(index)
            q = turned(up) if up is not None else (0.0, 0.0, 0.0, 1.0)
            world = _rotate(q, value)
            world = tuple(world[axis] * by[axis] for axis in range(3))
            return _rotate(_conj(q), world)

        for index, node in enumerate(nodes):
            if "matrix" in node:
                raise ValueError("a node written as a matrix is not stretched here")
            if "translation" in node:
                node["translation"] = list(in_parent(index, tuple(node["translation"])))
        for mesh in self.doc.get("meshes", []):
            for primitive in mesh["primitives"]:
                self._rewrite(primitive["attributes"]["POSITION"], lambda v: tuple(v[a] * by[a] for a in range(3)), done)
                if "NORMAL" in primitive["attributes"]:
                    self._rewrite(primitive["attributes"]["NORMAL"], lambda v: _unit(tuple(v[a] / by[a] for a in range(3))), done)
        for skin in self.doc.get("skins", []):
            if "inverseBindMatrices" not in skin:
                continue

            # A bind matrix is where its joint stood when the mesh was bound,
            # turned inside out. The joint stands somewhere else now and is
            # turned as it was: with the matrix `[A | t]`, the joint stood at
            # `-A^-1 t`, so the new one is `[A | A S A^-1 t]`.
            def rebind(m):
                a = [[m[column * 4 + row] for column in range(3)] for row in range(3)]
                t = (m[12], m[13], m[14])
                inverse = _inverse3(a)
                stood = tuple(sum(inverse[row][k] * t[k] for k in range(3)) * by[row] for row in range(3))
                moved = tuple(sum(a[row][k] * stood[k] for k in range(3)) for row in range(3))
                return (*m[:12], *moved, m[15])

            self._rewrite(skin["inverseBindMatrices"], rebind, done)
        for clip in self.doc.get("animations", []):
            for channel in clip["channels"]:
                if channel["target"]["path"] != "translation":
                    continue
                joint = channel["target"]["node"]
                self._rewrite(clip["samplers"][channel["sampler"]]["output"], lambda v, joint=joint: in_parent(joint, v), done)

    # --- adding --------------------------------------------------------------

    def _append(self, values, layout, kind, component=FLOAT, bounds=False):
        data = b"".join(struct.pack("<" + layout, *value) for value in values)
        buffer = self.buffers[0]
        while len(buffer) % 4:
            buffer.append(0)
        view = {"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data)}
        buffer.extend(data)
        self.doc["bufferViews"].append(view)
        accessor = {"bufferView": len(self.doc["bufferViews"]) - 1, "componentType": component, "count": len(values), "type": kind}
        if bounds:
            accessor["min"] = [min(value[0] for value in values)]
            accessor["max"] = [max(value[0] for value in values)]
        self.doc["accessors"].append(accessor)
        return len(self.doc["accessors"]) - 1

    def add_clip(self, name, keys):
        """A clip of rotations: `keys` is {joint's name: [(seconds, (x, y, z, w)), ...]}."""
        clip = {"name": name, "channels": [], "samplers": []}
        for joint, track in keys.items():
            times = self._append([(time,) for time, _ in track], "f", "SCALAR", bounds=True)
            turns = self._append([turn for _, turn in track], "ffff", "VEC4")
            clip["samplers"].append({"input": times, "output": turns, "interpolation": "LINEAR"})
            clip["channels"].append({"sampler": len(clip["samplers"]) - 1, "target": {"node": self.node(joint), "path": "rotation"}})
        self.doc.setdefault("animations", []).append(clip)

    def add_joint(self, name, under, at):
        """A joint called `name` under the joint called `under`, `at` from it
        in that joint's own space, at rest wherever that puts it: a prop joint
        -- a string, a slide -- that a clip of this rig can then move."""
        nodes = self.doc["nodes"]
        parent = self.node(under)
        nodes.append({"name": name, "translation": list(at)})
        index = len(nodes) - 1
        nodes[parent].setdefault("children", []).append(index)
        place, turn = world_rest(self)[index]
        # Where it rests, turned inside out: column by column.
        back = _conj(turn)
        columns = [_rotate(back, axis) for axis in ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))]
        moved = _rotate(back, tuple(-value for value in place))
        bind = (*columns[0], 0.0, *columns[1], 0.0, *columns[2], 0.0, *moved, 1.0)
        for skin in self.doc.get("skins", []):
            skin["joints"].append(index)
            if "inverseBindMatrices" in skin:
                binds = self.values(skin["inverseBindMatrices"]) + [bind]
                skin["inverseBindMatrices"] = self._append(binds, "f" * 16, "MAT4")
        return index

    def add_track(self, clip, joint, keys):
        """Where the joint called `joint` is from its parent through the clip
        called `clip`: `keys` is [(seconds, (x, y, z)), ...]."""
        for found in self.doc.get("animations", []):
            if found.get("name") == clip:
                times = self._append([(time,) for time, _ in keys], "f", "SCALAR", bounds=True)
                places = self._append([place for _, place in keys], "fff", "VEC3")
                found["samplers"].append({"input": times, "output": places, "interpolation": "LINEAR"})
                found["channels"].append(
                    {"sampler": len(found["samplers"]) - 1, "target": {"node": self.node(joint), "path": "translation"}}
                )
                return
        raise KeyError(f"no clip called {clip}")

    def length(self, clip):
        """How long the clip called `clip` is, in seconds."""
        for found in self.doc.get("animations", []):
            if found.get("name") == clip:
                return max(self.doc["accessors"][sampler["input"]]["max"][0] for sampler in found["samplers"])
        raise KeyError(f"no clip called {clip}")

    # --- writing -------------------------------------------------------------

    def save(self, path):
        """Writes what is still reached from the scene, the skins and the clips
        and nothing else, renumbered, in one buffer."""
        doc = self.doc
        gone = getattr(self, "_gone", set())
        nodes = [index for index in range(len(doc["nodes"])) if index not in gone]
        node_at = {old: new for new, old in enumerate(nodes)}

        used = {key: [] for key in ("meshes", "skins", "materials", "textures", "images", "samplers", "accessors", "bufferViews")}

        def use(kind, index):
            if index not in used[kind]:
                used[kind].append(index)

        for index in nodes:
            node = doc["nodes"][index]
            if "mesh" in node:
                use("meshes", node["mesh"])
            if "skin" in node:
                use("skins", node["skin"])
        for index in used["meshes"]:
            for primitive in doc["meshes"][index]["primitives"]:
                for accessor in primitive["attributes"].values():
                    use("accessors", accessor)
                if "indices" in primitive:
                    use("accessors", primitive["indices"])
                for target in primitive.get("targets", []):
                    for accessor in target.values():
                        use("accessors", accessor)
                if "material" in primitive:
                    use("materials", primitive["material"])
        for index in used["skins"]:
            if "inverseBindMatrices" in doc["skins"][index]:
                use("accessors", doc["skins"][index]["inverseBindMatrices"])
        for clip in doc.get("animations", []):
            clip["channels"] = [channel for channel in clip["channels"] if channel["target"]["node"] in node_at]
            samplers = []
            for channel in clip["channels"]:
                if channel["sampler"] not in samplers:
                    samplers.append(channel["sampler"])
            for channel in clip["channels"]:
                channel["sampler"] = samplers.index(channel["sampler"])
            clip["samplers"] = [clip["samplers"][index] for index in samplers]
            for sampler in clip["samplers"]:
                use("accessors", sampler["input"])
                use("accessors", sampler["output"])

        def textures_of(value):
            if isinstance(value, dict):
                if "index" in value and ("texCoord" in value or len(value) <= 3) and isinstance(value["index"], int):
                    yield value
                for inner in value.values():
                    yield from textures_of(inner)

        for index in used["materials"]:
            for reference in textures_of(doc["materials"][index]):
                use("textures", reference["index"])
        for index in used["textures"]:
            texture = doc["textures"][index]
            if "source" in texture:
                use("images", texture["source"])
            if "sampler" in texture:
                use("samplers", texture["sampler"])
        for index in used["accessors"]:
            use("bufferViews", doc["accessors"][index]["bufferView"])
        for index in used["images"]:
            if "bufferView" in doc["images"][index]:
                use("bufferViews", doc["images"][index]["bufferView"])

        at = {kind: {old: new for new, old in enumerate(indices)} for kind, indices in used.items()}

        packed = bytearray()
        views = []
        for index in used["bufferViews"]:
            view = dict(doc["bufferViews"][index])
            start = view.get("byteOffset", 0)
            while len(packed) % 4:
                packed.append(0)
            data = self.buffers[view["buffer"]][start : start + view["byteLength"]]
            view["buffer"] = 0
            view["byteOffset"] = len(packed)
            packed.extend(data)
            views.append(view)
        while len(packed) % 4:
            packed.append(0)

        out = {"asset": {"version": "2.0", "generator": "examples/37-character/tools/gltf_edit.py"}}
        out["scene"] = 0
        out["scenes"] = [{"nodes": [node_at[index] for index in scene["nodes"]]} for scene in doc.get("scenes", [])][:1]
        out["nodes"] = []
        for index in nodes:
            node = dict(doc["nodes"][index])
            if "children" in node:
                node["children"] = [node_at[child] for child in node["children"]]
            if "mesh" in node:
                node["mesh"] = at["meshes"][node["mesh"]]
            if "skin" in node:
                node["skin"] = at["skins"][node["skin"]]
            node.pop("camera", None)
            node.pop("extensions", None)
            out["nodes"].append(node)
        out["meshes"] = []
        for index in used["meshes"]:
            mesh = json.loads(json.dumps(doc["meshes"][index]))
            for primitive in mesh["primitives"]:
                primitive["attributes"] = {key: at["accessors"][value] for key, value in primitive["attributes"].items()}
                if "indices" in primitive:
                    primitive["indices"] = at["accessors"][primitive["indices"]]
                if "material" in primitive:
                    primitive["material"] = at["materials"][primitive["material"]]
                if "targets" in primitive:
                    primitive["targets"] = [{key: at["accessors"][value] for key, value in target.items()} for target in primitive["targets"]]
            out["meshes"].append(mesh)
        out["skins"] = []
        for index in used["skins"]:
            skin = dict(doc["skins"][index])
            skin["joints"] = [node_at[joint] for joint in skin["joints"]]
            if "skeleton" in skin:
                if skin["skeleton"] in node_at:
                    skin["skeleton"] = node_at[skin["skeleton"]]
                else:
                    del skin["skeleton"]
            if "inverseBindMatrices" in skin:
                skin["inverseBindMatrices"] = at["accessors"][skin["inverseBindMatrices"]]
            out["skins"].append(skin)
        out["materials"] = []
        for index in used["materials"]:
            material = json.loads(json.dumps(doc["materials"][index]))
            for reference in textures_of(material):
                reference["index"] = at["textures"][reference["index"]]
            out["materials"].append(material)
        out["textures"] = []
        for index in used["textures"]:
            texture = dict(doc["textures"][index])
            if "source" in texture:
                texture["source"] = at["images"][texture["source"]]
            if "sampler" in texture:
                texture["sampler"] = at["samplers"][texture["sampler"]]
            out["textures"].append(texture)
        out["images"] = []
        for index in used["images"]:
            image = dict(doc["images"][index])
            if "bufferView" not in image:
                raise ValueError("an image kept in a file of its own is not packed here")
            image["bufferView"] = at["bufferViews"][image["bufferView"]]
            out["images"].append(image)
        out["samplers"] = [doc["samplers"][index] for index in used["samplers"]]
        out["accessors"] = []
        for index in used["accessors"]:
            accessor = dict(doc["accessors"][index])
            accessor["bufferView"] = at["bufferViews"][accessor["bufferView"]]
            out["accessors"].append(accessor)
        out["bufferViews"] = views
        out["buffers"] = [{"byteLength": len(packed)}]
        out["animations"] = []
        for clip in doc.get("animations", []):
            clip = json.loads(json.dumps(clip))
            for sampler in clip["samplers"]:
                sampler["input"] = at["accessors"][sampler["input"]]
                sampler["output"] = at["accessors"][sampler["output"]]
            for channel in clip["channels"]:
                channel["target"]["node"] = node_at[channel["target"]["node"]]
            out["animations"].append(clip)
        for key in [key for key, value in out.items() if value == []]:
            del out[key]

        text = json.dumps(out, separators=(",", ":")).encode("utf-8")
        text += b" " * (-len(text) % 4)
        total = 12 + 8 + len(text) + 8 + len(packed)
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(struct.pack("<4sII", b"glTF", 2, total))
            handle.write(struct.pack("<I4s", len(text), b"JSON"))
            handle.write(text)
            handle.write(struct.pack("<I4s", len(packed), b"BIN\0"))
            handle.write(packed)
        return total


# --- quaternions, (x, y, z, w) as glTF writes them -----------------------------------


def _mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def _conj(q):
    return (-q[0], -q[1], -q[2], q[3])


def _rotate(q, v):
    x, y, z, w = q
    cx, cy, cz = y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]
    dx, dy, dz = y * cz - z * cy, z * cx - x * cz, x * cy - y * cx
    return (v[0] + 2 * (w * cx + dx), v[1] + 2 * (w * cy + dy), v[2] + 2 * (w * cz + dz))


def _unit(v):
    length = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5
    return tuple(a / length for a in v) if length > 0 else v


def _inverse3(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]


def world_rest(gltf):
    """Every node at rest in the model's space: {index: (place, turn)}."""
    parent = gltf.parents()
    nodes = gltf.doc["nodes"]
    done = {}

    def of(index):
        if index not in done:
            node = nodes[index]
            place = tuple(node.get("translation", (0.0, 0.0, 0.0)))
            turn = tuple(node.get("rotation", (0.0, 0.0, 0.0, 1.0)))
            if index in parent:
                up_place, up_turn = of(parent[index])
                moved = _rotate(up_turn, place)
                done[index] = (tuple(up_place[a] + moved[a] for a in range(3)), _mul(up_turn, turn))
            else:
                done[index] = (place, turn)
        return done[index]

    return {index: of(index) for index in range(len(nodes))}
