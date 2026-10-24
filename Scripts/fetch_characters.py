#!/usr/bin/env python3
"""Download the demo characters from the Npcify API.

The script reads NPCIFY_API_KEY and the character IDs from the .env file in the
project folder (or from environment variables). For each character it writes:

  RawCharacters/<folder>/character.vrm         VRM model (input for the importer)
  RawCharacters/<folder>/animations/<id>.vrma  animation catalog
  RawCharacters/<folder>/character.json        name and idle animation

For the offline character it also writes the baked conversation that the game
ships with: Content/NpcifyData/<folder>/conversation.npcp

Then run the importer:
  UnrealEditor-Cmd NpcifyExample.uproject -run=NpcifyImportCharacters
"""

import json
import os
import struct
import sys
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CHARACTERS = [
    # (folder, character ID setting, offline)
    ("hinata", "NPCIFY_OFFLINE_CHARACTER_ID", True),
    ("vespera", "NPCIFY_ONLINE_CHARACTER_ID", False),
]


def load_env():
    values = {}
    env_file = ROOT / ".env"
    if env_file.exists():
        for raw in env_file.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip().strip('"').strip("'")
    for key in list(values) + ["NPCIFY_API_BASE_URL", "NPCIFY_API_KEY",
                               "NPCIFY_ONLINE_CHARACTER_ID", "NPCIFY_OFFLINE_CHARACTER_ID"]:
        if os.environ.get(key):
            values[key] = os.environ[key]
    return values


def request(url, api_key=None):
    headers = {"User-Agent": "npcify-example-unreal"}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=600) as response:
        return response.read()


COMPONENT_FORMATS = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
TYPE_SIZES = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}


def densify_sparse_accessors(glb):
    """Rewrite sparse accessors and accessors without a bufferView as dense data.

    VRM4U reads VRM files with an assimp fork that cannot read accessors without
    a bufferView (valid glTF 2.0: zeros, plus optional sparse values). Morph
    targets often use them.
    """
    magic, version, _ = struct.unpack_from("<III", glb, 0)
    if magic != 0x46546C67 or version != 2:
        return glb
    json_length, _ = struct.unpack_from("<II", glb, 12)
    document = json.loads(glb[20:20 + json_length])
    binary_offset = 20 + json_length
    binary_length, _ = struct.unpack_from("<II", glb, binary_offset)
    binary = bytearray(glb[binary_offset + 8:binary_offset + 8 + binary_length])
    views = document.get("bufferViews", [])

    def read(view_index, byte_offset, fmt, count, width):
        view = views[view_index]
        start = view.get("byteOffset", 0) + byte_offset
        size = struct.calcsize("<" + fmt)
        stride = view.get("byteStride") or size * width
        return [list(struct.unpack_from("<" + fmt * width, binary, start + i * stride)) for i in range(count)]

    changed = False
    for accessor in document.get("accessors", []):
        sparse = accessor.pop("sparse", None)
        if sparse is None and "bufferView" in accessor:
            continue
        changed = True
        fmt = COMPONENT_FORMATS[accessor["componentType"]]
        width = TYPE_SIZES[accessor["type"]]
        count = accessor["count"]
        if "bufferView" in accessor:
            values = read(accessor["bufferView"], accessor.get("byteOffset", 0), fmt, count, width)
        else:
            values = [[0] * width for _ in range(count)]
        if sparse is not None:
            indices = sparse["indices"]
            index_fmt = COMPONENT_FORMATS[indices["componentType"]]
            positions = read(indices["bufferView"], indices.get("byteOffset", 0), index_fmt, sparse["count"], 1)
            replacements = read(sparse["values"]["bufferView"], sparse["values"].get("byteOffset", 0), fmt,
                                sparse["count"], width)
            for (index,), value in zip(positions, replacements):
                values[index] = value
        while len(binary) % 4:
            binary.append(0)
        offset = len(binary)
        packed = struct.pack("<" + fmt * width * count, *[component for value in values for component in value])
        binary.extend(packed)
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(packed)})
        accessor["bufferView"] = len(views) - 1
        accessor["byteOffset"] = 0
    if not changed:
        return glb

    while len(binary) % 4:
        binary.append(0)
    document["buffers"][0]["byteLength"] = len(binary)
    json_bytes = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * (-len(json_bytes) % 4)
    total = 12 + 8 + len(json_bytes) + 8 + len(binary)
    return (struct.pack("<III", 0x46546C67, 2, total)
            + struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes
            + struct.pack("<II", len(binary), 0x004E4942) + bytes(binary))


def read_glb(glb):
    json_length, _ = struct.unpack_from("<II", glb, 12)
    document = json.loads(glb[20:20 + json_length])
    binary_offset = 20 + json_length
    binary_length, _ = struct.unpack_from("<II", glb, binary_offset)
    return document, bytearray(glb[binary_offset + 8:binary_offset + 8 + binary_length])


def write_glb(document, binary):
    while len(binary) % 4:
        binary.append(0)
    document["buffers"][0]["byteLength"] = len(binary)
    json_bytes = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * (-len(json_bytes) % 4)
    total = 12 + 8 + len(json_bytes) + 8 + len(binary)
    return (struct.pack("<III", 0x46546C67, 2, total)
            + struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes
            + struct.pack("<II", len(binary), 0x004E4942) + bytes(binary))


def q_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz]


def q_conj(q):
    return [-q[0], -q[1], -q[2], q[3]]


def q_rotate(q, v):
    x, y, z, _ = q_mul(q_mul(q, [v[0], v[1], v[2], 0.0]), q_conj(q))
    return [x, y, z]


def q_slerp(a, b, t):
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0.0:
        b, dot = [-x for x in b], -dot
    if dot > 0.9995:
        result = [x + (y - x) * t for x, y in zip(a, b)]
    else:
        import math
        theta = math.acos(dot)
        sin_theta = math.sin(theta)
        wa, wb = math.sin((1 - t) * theta) / sin_theta, math.sin(t * theta) / sin_theta
        result = [wa * x + wb * y for x, y in zip(a, b)]
    norm = sum(x * x for x in result) ** 0.5
    return [x / norm for x in result]


def normalize_vrma(glb):
    """Rewrite a VRMA so that every rest rotation is identity.

    VRMA files from the VRM Add-on for Blender keep Blender bone rotations in the
    rest pose. That is valid, but VRM4U expects identity rest rotations (as UniVRM
    writes them). The animation keys become world-space deltas from the rest pose,
    expressed in the parent's normalized space (the same retargeting as three-vrm).
    """
    document, binary = read_glb(glb)
    nodes = document["nodes"]
    views = document["bufferViews"]
    accessors = document["accessors"]
    parents = {}
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            parents[child] = index

    def read_floats(accessor_index):
        accessor = accessors[accessor_index]
        view = views[accessor["bufferView"]]
        width = TYPE_SIZES[accessor["type"]]
        start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        flat = struct.unpack_from("<" + "f" * width * accessor["count"], binary, start)
        return [list(flat[i * width:(i + 1) * width]) for i in range(accessor["count"])]

    rest = [node.get("rotation", [0.0, 0.0, 0.0, 1.0]) for node in nodes]
    tracks = {}
    animation = document["animations"][0]
    for channel in animation["channels"]:
        if channel["target"]["path"] == "rotation":
            sampler = animation["samplers"][channel["sampler"]]
            tracks[channel["target"]["node"]] = (read_floats(sampler["input"]), read_floats(sampler["output"]),
                                                 sampler.get("interpolation", "LINEAR"))

    def local_rotation(index, time):
        if index not in tracks:
            return rest[index]
        times, values, interpolation = tracks[index]
        if time <= times[0][0]:
            return values[0]
        for key in range(1, len(times)):
            if time <= times[key][0]:
                if interpolation == "STEP":
                    return values[key - 1]
                span = times[key][0] - times[key - 1][0]
                return q_slerp(values[key - 1], values[key], (time - times[key - 1][0]) / span if span > 0 else 0.0)
        return values[-1]

    def world(index, rotation_of):
        result = [0.0, 0.0, 0.0, 1.0]
        chain = []
        while index is not None:
            chain.append(index)
            index = parents.get(index)
        for node_index in reversed(chain):
            result = q_mul(result, rotation_of(node_index))
        return result

    rest_world = [world(i, lambda n: rest[n]) for i in range(len(nodes))]
    rest_position = []
    for index in range(len(nodes)):
        position, rotation, node_index = [0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 1.0], index
        chain = []
        while node_index is not None:
            chain.append(node_index)
            node_index = parents.get(node_index)
        for node_index in reversed(chain):
            offset = q_rotate(rotation, nodes[node_index].get("translation", [0.0, 0.0, 0.0]))
            position = [p + o for p, o in zip(position, offset)]
            rotation = q_mul(rotation, rest[node_index])
        rest_position.append(position)

    def delta(index, time):
        if index is None:
            return [0.0, 0.0, 0.0, 1.0]
        return q_mul(world(index, lambda n: local_rotation(n, time)), q_conj(rest_world[index]))

    for channel in animation["channels"]:
        if channel["target"]["path"] != "rotation":
            continue
        index = channel["target"]["node"]
        sampler = animation["samplers"][channel["sampler"]]
        times = read_floats(sampler["input"])
        parent = parents.get(index)
        values = [q_mul(q_conj(delta(parent, t[0])), delta(index, t[0])) for t in times]
        while len(binary) % 4:
            binary.append(0)
        offset = len(binary)
        binary.extend(struct.pack("<" + "f" * 4 * len(values), *[c for v in values for c in v]))
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": 16 * len(values)})
        accessors.append({"bufferView": len(views) - 1, "componentType": 5126, "count": len(values), "type": "VEC4"})
        sampler["output"] = len(accessors) - 1

    for index, node in enumerate(nodes):
        parent = parents.get(index)
        node["rotation"] = [0.0, 0.0, 0.0, 1.0]
        if parent is not None:
            node["translation"] = [p - q for p, q in zip(rest_position[index], rest_position[parent])]
    return write_glb(document, binary)


def m_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def m_from_node(node):
    if "matrix" in node:
        m = node["matrix"]
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    x, y, z, w = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    sx, sy, sz = node.get("scale", [1.0, 1.0, 1.0])
    t = node.get("translation", [0.0, 0.0, 0.0])
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[r[i][0] * sx, r[i][1] * sy, r[i][2] * sz, t[i]] for i in range(3)] + [[0.0, 0.0, 0.0, 1.0]]


def blend(joint_matrices, joint_set, weight_set):
    """Linear-blend skinning matrix of one vertex."""
    total = sum(weight_set) or 1.0
    m = [[0.0] * 4 for _ in range(3)]
    for joint, weight in zip(joint_set, weight_set):
        if weight <= 0.0:
            continue
        source = joint_matrices[int(joint)]
        for r in range(3):
            for c in range(4):
                m[r][c] += source[r][c] * weight / total
    return m + [[0.0, 0.0, 0.0, 1.0]]


def m_apply(m, v, w=1.0):
    return [m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3] * w for i in range(3)]


def normalize_vrm_rest_pose(glb):
    """Convert a VRM 1.0 model to the normalized form that UniVRM exports.

    Every node gets an identity rotation and unit scale, and the skin's bind pose
    becomes the rest pose. VRM4U imports this form correctly; it misplaces vertices
    of models with rotated bones and a scaled, rotated root node.
    """
    document, binary = read_glb(glb)
    nodes = document["nodes"]
    views = document["bufferViews"]
    accessors = document["accessors"]
    parents = {}
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            parents[child] = index

    world = {}

    def world_of(index):
        if index not in world:
            local = m_from_node(nodes[index])
            world[index] = m_mul(world_of(parents[index]), local) if index in parents else local
        return world[index]

    def read(accessor_index):
        accessor = accessors[accessor_index]
        view = views[accessor["bufferView"]]
        fmt = COMPONENT_FORMATS[accessor["componentType"]]
        width = TYPE_SIZES[accessor["type"]]
        size = struct.calcsize("<" + fmt)
        stride = view.get("byteStride") or size * width
        start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        values = [list(struct.unpack_from("<" + fmt * width, binary, start + i * stride)) for i in range(accessor["count"])]
        if accessor.get("normalized"):
            scale = float((1 << (8 * size)) - 1)
            values = [[c / scale for c in value] for value in values]
        return values

    def append(values, accessor_type, with_bounds=False):
        while len(binary) % 4:
            binary.append(0)
        offset = len(binary)
        width = TYPE_SIZES[accessor_type]
        binary.extend(struct.pack("<" + "f" * width * len(values), *[c for v in values for c in v]))
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": 4 * width * len(values)})
        accessor = {"bufferView": len(views) - 1, "componentType": 5126, "count": len(values), "type": accessor_type}
        if with_bounds and values:
            accessor["min"] = [min(v[i] for v in values) for i in range(width)]
            accessor["max"] = [max(v[i] for v in values) for i in range(width)]
        accessors.append(accessor)
        return len(accessors) - 1

    def unit(v):
        length = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5
        return [c / length for c in v] if length > 1e-12 else v

    def linear_scale(m):
        return (m[0][0] ** 2 + m[1][0] ** 2 + m[2][0] ** 2) ** 0.5

    old_world = [world_of(i) for i in range(len(nodes))]
    positions = [[m[0][3], m[1][3], m[2][3]] for m in old_world]

    # Bake the rest-pose skinning into every skinned primitive. Primitives often share
    # vertex accessors, so each accessor is converted once.
    skin_of_mesh = {node["mesh"]: node.get("skin") for node in nodes if "mesh" in node}
    skin_cache, converted = {}, {}
    for mesh_index, mesh in enumerate(document["meshes"]):
        skin_index = skin_of_mesh.get(mesh_index)
        if skin_index is None:
            continue
        skin = document["skins"][skin_index]
        inverse_binds = read(skin["inverseBindMatrices"])
        joint_matrices = []
        for joint, ibm in zip(skin["joints"], inverse_binds):
            joint_matrices.append(m_mul(old_world[joint], [[ibm[c * 4 + r] for c in range(4)] for r in range(4)]))
        for primitive in mesh["primitives"]:
            attributes = primitive["attributes"]
            skin_key = (skin_index, attributes["JOINTS_0"], attributes["WEIGHTS_0"])
            if skin_key not in skin_cache:
                skin_cache[skin_key] = [blend(joint_matrices, joint_set, weight_set) for joint_set, weight_set
                                        in zip(read(attributes["JOINTS_0"]), read(attributes["WEIGHTS_0"]))]
            skin_matrices = skin_cache[skin_key]

            def convert(source, kind):
                key = (source, kind, skin_key)
                if key not in converted:
                    values = read(source)
                    if kind == "point":
                        converted[key] = append([m_apply(m, v) for m, v in zip(skin_matrices, values)], "VEC3", True)
                    elif kind == "normal":
                        converted[key] = append([unit(m_apply(m, v, 0.0)) for m, v in zip(skin_matrices, values)], "VEC3")
                    elif kind == "tangent":
                        converted[key] = append([unit(m_apply(m, v[:3], 0.0)) + [v[3]]
                                                 for m, v in zip(skin_matrices, values)], "VEC4")
                    else:
                        converted[key] = append([m_apply(m, v[:3], 0.0) for m, v in zip(skin_matrices, values)],
                                                "VEC3", kind == "delta-position")
                return converted[key]

            attributes["POSITION"] = convert(attributes["POSITION"], "point")
            if "NORMAL" in attributes:
                attributes["NORMAL"] = convert(attributes["NORMAL"], "normal")
            if "TANGENT" in attributes:
                attributes["TANGENT"] = convert(attributes["TANGENT"], "tangent")
            for target in primitive.get("targets", []):
                for key in ("POSITION", "NORMAL", "TANGENT"):
                    if key in target:
                        target[key] = convert(target[key], "delta-position" if key == "POSITION" else "delta")

    # Spring-bone colliders, joint radii and the look-at offset are in node space.
    springs = document.get("extensions", {}).get("VRMC_springBone", {})
    for collider in springs.get("colliders", []):
        node_world = old_world[collider["node"]]
        for shape in collider.get("shape", {}).values():
            for key in ("offset", "tail"):
                if key in shape:
                    shape[key] = m_apply(node_world, shape[key], 0.0)
            if "radius" in shape:
                shape["radius"] *= linear_scale(node_world)
    for spring in springs.get("springs", []):
        for joint in spring.get("joints", []):
            if "hitRadius" in joint:
                joint["hitRadius"] *= linear_scale(old_world[joint["node"]])
    vrm = document.get("extensions", {}).get("VRMC_vrm", {})
    head = vrm.get("humanoid", {}).get("humanBones", {}).get("head", {}).get("node")
    look_at = vrm.get("lookAt", {})
    if head is not None and "offsetFromHeadBone" in look_at:
        look_at["offsetFromHeadBone"] = m_apply(old_world[head], look_at["offsetFromHeadBone"], 0.0)

    for index, node in enumerate(nodes):
        for key in ("matrix", "rotation", "scale"):
            node.pop(key, None)
        parent = parents.get(index)
        base = positions[parent] if parent is not None else [0.0, 0.0, 0.0]
        node["translation"] = [a - b for a, b in zip(positions[index], base)]
    for skin in document.get("skins", []):
        skin["inverseBindMatrices"] = append(
            [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -positions[j][0], -positions[j][1], -positions[j][2], 1]
             for j in skin["joints"]], "MAT4")
    return write_glb(document, binary)


def compact_glb(glb):
    """Drop buffer data that no accessor or image references any more."""
    document, binary = read_glb(glb)
    views = document["bufferViews"]
    used = sorted({a["bufferView"] for a in document.get("accessors", []) if "bufferView" in a}
                  | {im["bufferView"] for im in document.get("images", []) if "bufferView" in im})
    remap, packed = {}, bytearray()
    for old in used:
        view = dict(views[old])
        while len(packed) % 4:
            packed.append(0)
        start = view.get("byteOffset", 0)
        data = binary[start:start + view["byteLength"]]
        view["byteOffset"] = len(packed)
        packed.extend(data)
        remap[old] = len(remap)
        views[old] = view
    document["bufferViews"] = [views[old] for old in used]
    for accessor in document.get("accessors", []):
        if "bufferView" in accessor:
            accessor["bufferView"] = remap[accessor["bufferView"]]
    for image in document.get("images", []):
        if "bufferView" in image:
            image["bufferView"] = remap[image["bufferView"]]
    return write_glb(document, packed)


def merge_skinned_meshes(glb):
    """Merge all meshes that share a skin into one mesh.

    VRM4U breaks the vertex data of the second skinned mesh of a model (the face of
    generated characters). Models with one mesh import correctly. glTF requires the
    same morph target count in every primitive of a mesh, so primitives without
    targets get zero deltas.
    """
    document, binary = read_glb(glb)
    nodes = document["nodes"]
    meshes = document["meshes"]
    by_skin = {}
    for index, node in enumerate(nodes):
        if "mesh" in node and "skin" in node:
            by_skin.setdefault(node["skin"], []).append(index)
    for node_indices in by_skin.values():
        if len(node_indices) < 2:
            continue
        target_count = max(len(p.get("targets", [])) for i in node_indices for p in meshes[nodes[i]["mesh"]]["primitives"])
        names, weights = [], []
        for i in node_indices:
            mesh = meshes[nodes[i]["mesh"]]
            mesh_names = mesh.get("extras", {}).get("targetNames") or mesh["primitives"][0].get("extras", {}).get("targetNames")
            if mesh_names and len(mesh_names) == target_count:
                names, weights = mesh_names, mesh.get("weights", [0.0] * target_count)
        keep = node_indices[0]
        merged = meshes[nodes[keep]["mesh"]]
        zero_cache = {}
        primitives = []
        for i in node_indices:
            for primitive in meshes[nodes[i]["mesh"]]["primitives"]:
                targets = primitive.get("targets", [])
                if len(targets) < target_count:
                    count = document["accessors"][primitive["attributes"]["POSITION"]]["count"]
                    if count not in zero_cache:
                        while len(binary) % 4:
                            binary.append(0)
                        offset = len(binary)
                        binary.extend(bytes(12 * count))
                        document["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": 12 * count})
                        document["accessors"].append({"bufferView": len(document["bufferViews"]) - 1, "componentType": 5126,
                                                      "count": count, "type": "VEC3", "min": [0, 0, 0], "max": [0, 0, 0]})
                        zero_cache[count] = len(document["accessors"]) - 1
                    primitive["targets"] = targets + [{"POSITION": zero_cache[count]}] * (target_count - len(targets))
                primitive.pop("extras", None)
                primitives.append(primitive)
        merged["primitives"] = primitives
        if names:
            merged["weights"] = weights
            merged.setdefault("extras", {})["targetNames"] = names
        moved = set(node_indices[1:])
        for i in moved:
            nodes[i].pop("mesh", None)
            nodes[i].pop("skin", None)
        vrm = document.get("extensions", {}).get("VRMC_vrm", {})
        expressions = vrm.get("expressions", {})
        for group in (expressions.get("preset", {}), expressions.get("custom", {})):
            for expression in group.values():
                for bind in expression.get("morphTargetBinds", []):
                    if bind["node"] in moved:
                        bind["node"] = keep
        first_person = vrm.get("firstPerson", {})
        kept = [a for a in first_person.get("meshAnnotations", []) if a.get("node") not in moved]
        if kept:
            first_person["meshAnnotations"] = kept
        else:
            first_person.pop("meshAnnotations", None)
    return write_glb(document, binary)


def order_materials_by_primitive(glb):
    """Renumber materials in the order of the primitives that use them.

    VRM4U (UE 5.8) gives the n-th mesh section the n-th material slot, whatever
    material the primitive names. Generated characters list their materials in a
    different order (for example the hair would get the eye material).
    """
    document, binary = read_glb(glb)
    materials = document.get("materials", [])
    order = []
    for mesh in document["meshes"]:
        for primitive in mesh["primitives"]:
            if "material" in primitive and primitive["material"] not in order:
                order.append(primitive["material"])
    order += [index for index in range(len(materials)) if index not in order]
    if order == list(range(len(materials))):
        return glb
    remap = {old: new for new, old in enumerate(order)}
    document["materials"] = [materials[old] for old in order]
    for mesh in document["meshes"]:
        for primitive in mesh["primitives"]:
            if "material" in primitive:
                primitive["material"] = remap[primitive["material"]]
    expressions = document.get("extensions", {}).get("VRMC_vrm", {}).get("expressions", {})
    for group in (expressions.get("preset", {}), expressions.get("custom", {})):
        for expression in group.values():
            for key in ("materialColorBinds", "textureTransformBinds"):
                for bind in expression.get(key, []):
                    bind["material"] = remap[bind["material"]]
    return write_glb(document, binary)


def prepare_vrm(glb):
    """Make a VRM file importable by VRM4U without changing how it looks."""
    glb = densify_sparse_accessors(glb)
    glb = normalize_vrm_rest_pose(glb)
    glb = merge_skinned_meshes(glb)
    glb = order_materials_by_primitive(glb)
    return compact_glb(glb)


def download_signed(api_url, api_key, target, transform=None):
    # Signed storage URLs must not receive the API key.
    signed = json.loads(request(api_url, api_key))["url"]
    target.parent.mkdir(parents=True, exist_ok=True)
    data = request(signed)
    target.write_bytes(transform(data) if transform else data)
    print(f"  {target.relative_to(ROOT)} ({target.stat().st_size // 1024} KB)")


def main():
    env = load_env()
    api_key = env.get("NPCIFY_API_KEY", "")
    if not api_key or api_key.startswith("<"):
        sys.exit("Set NPCIFY_API_KEY in .env first (copy .env.example).")
    base_url = env.get("NPCIFY_API_BASE_URL", "https://api.npcify.ai").rstrip("/")

    for folder, id_key, offline in CHARACTERS:
        character_id = env.get(id_key)
        if not character_id:
            sys.exit(f"Set {id_key} in .env first.")
        avatar = f"{base_url}/avatars/{urllib.parse.quote(character_id)}"
        info = json.loads(request(avatar + "/info", api_key))
        print(f"{info['name']} ({folder})")
        raw = ROOT / "RawCharacters" / folder
        download_signed(avatar + "/download-url", api_key, raw / "character.vrm", prepare_vrm)
        for animation in info.get("animations", []):
            animation_id = animation["id"]
            download_signed(f"{avatar}/animations/{urllib.parse.quote(animation_id)}/download-url",
                            api_key, raw / "animations" / f"{animation_id}.vrma", normalize_vrma)
        manifest = {
            "characterId": character_id,
            "name": info["name"],
            "idleAnimationId": info.get("idleAnimationId") or "idle",
            "animations": [a["id"] for a in info.get("animations", [])],
        }
        (raw / "character.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

        if offline:
            if not info.get("bakedConversationEnabled"):
                sys.exit(f"{info['name']} has no baked conversation. Enable it in the Npcify dashboard.")
            target = ROOT / "Content" / "NpcifyData" / folder / "conversation.npcp"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(request(avatar + "/baked-conversation/asset", api_key))
            print(f"  {target.relative_to(ROOT)} (revision {info.get('bakedConversationRevision')})")


if __name__ == "__main__":
    main()
