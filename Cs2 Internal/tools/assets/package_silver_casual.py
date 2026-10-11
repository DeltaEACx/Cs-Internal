"""Publish the final Silver Casual assets without installing files into the game.

The Blender source GLB stays embedded and unchanged. The preview copy uses an
external PNG because the native preview renderer and portable packager read URIs.
By default this only validates and prints a plan. --apply is deliberately explicit.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import tempfile
import os
import copy
import re
import zlib

MODEL = "models/custom/agents/silver_casual/silver_casual.vmdl"
PREVIEW = "models_custom_agents_silver_casual_silver_casual_vmdl.glb"
TEXTURE = "silver_casual_turnaround.png"
HEAD_TEXTURE = "silver_casual_head_detail.png"
HEAD_MATERIAL = "materials/custom/agents/silver_casual/silver_casual_head.vmat_c"
THUMBNAIL = "models_custom_agents_silver_casual_silver_casual_vmdl.png"


def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)


def read_glb(path: Path) -> tuple[dict, bytes]:
    raw = path.read_bytes()
    require(len(raw) >= 28, "Truncated GLB")
    magic, version, length, json_length, json_tag = struct.unpack_from("<5I", raw)
    require((magic, version, length, json_tag) == (0x46546C67, 2, len(raw), 0x4E4F534A), "Invalid GLB header")
    document = json.loads(raw[20:20 + json_length])
    binary_length, binary_tag = struct.unpack_from("<2I", raw, 20 + json_length)
    require(binary_tag == 0x004E4942 and 28 + json_length + binary_length == len(raw), "Invalid GLB BIN chunk")
    return document, raw[28 + json_length:]


def accessor(document: dict, binary: bytes, index: int) -> list[tuple]:
    item = document["accessors"][index]
    view = document["bufferViews"][item["bufferView"]]
    require("sparse" not in item and view.get("buffer", 0) == 0, "Unsupported accessor")
    count = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[item["type"]]
    fmt = {5121: "B", 5123: "H", 5125: "I", 5126: "f"}[item["componentType"]]
    element = struct.Struct("<" + fmt * count)
    start = view.get("byteOffset", 0) + item.get("byteOffset", 0)
    stride = view.get("byteStride", element.size)
    end = view.get("byteOffset", 0) + view["byteLength"]
    require(stride >= element.size and end <= len(binary) and start <= end and
            (not item["count"] or start + (item["count"] - 1) * stride + element.size <= end), "Accessor exceeds BIN")
    values = [element.unpack_from(binary, start + i * stride) for i in range(item["count"])]
    if item.get("normalized") and fmt != "f":
        divisor = {"B": 255, "H": 65535, "I": 4294967295}[fmt]
        values = [tuple(v / divisor for v in row) for row in values]
    require(all(math.isfinite(v) for row in values for v in row), "Nonfinite accessor value")
    return values


def validate_rig(document: dict, binary: bytes) -> dict:
    nodes = document["nodes"]
    names = {node.get("name", "").lower() for node in nodes}
    require({"hand_l", "hand_r"} <= names, "Rig has no compatible hand bones")
    vertices = triangles = 0
    for node in nodes:
        if "mesh" not in node:
            continue
        require("skin" in node, "Character mesh is not skinned")
        skin = document["skins"][node["skin"]]
        matrices = accessor(document, binary, skin["inverseBindMatrices"])
        require(len(matrices) == len(skin["joints"]) >= 10, "Incomplete skin inverse-bind matrices")
        for primitive in document["meshes"][node["mesh"]]["primitives"]:
            attributes = primitive["attributes"]
            require({"POSITION", "NORMAL", "TEXCOORD_0", "JOINTS_0", "WEIGHTS_0"} <= attributes.keys(),
                    "Preview mesh lacks position, normal, UV, joints or weights")
            positions = accessor(document, binary, attributes["POSITION"])
            weights = accessor(document, binary, attributes["WEIGHTS_0"])
            joints = accessor(document, binary, attributes["JOINTS_0"])
            normals = accessor(document, binary, attributes["NORMAL"])
            uv = accessor(document, binary, attributes["TEXCOORD_0"])
            require(len(positions) == len(weights) == len(joints) == len(normals) == len(uv), "Vertex stream counts differ")
            for weight, joint in zip(weights, joints):
                require(all(w >= 0 for w in weight) and abs(sum(weight) - 1) <= .002, "Vertex weights are not normalized")
                require(all(int(j) == j and 0 <= j < len(matrices) for w, j in zip(weight, joint) if w > 0),
                        "Weighted joint is outside skin")
            indices = accessor(document, binary, primitive["indices"])
            require(len(indices) % 3 == 0 and all(int(i[0]) == i[0] and 0 <= i[0] < len(positions) for i in indices),
                    "Triangle index outside vertices")
            vertices += len(positions)
            triangles += len(indices) // 3
    require(vertices > 100 and triangles > 100, "Degenerate character mesh")
    return {"vertices": vertices, "triangles": triangles,
            "bones": max(len(skin["joints"]) for skin in document["skins"])}


def image_destinations(document: dict) -> dict[int, str]:
    images = document["images"]
    require(1 <= len(images) <= 2, "Silver Casual expects body and optional head textures")
    destinations = {}
    for index, image in enumerate(images):
        name = str(image.get("name", "")).replace("\\", "/").rsplit("/", 1)[-1].lower()
        name = re.sub(r"\.\d{3}$", "", name)
        name = Path(name).stem
        if name in {"turnaround", "silver_casual_turnaround"}:
            destinations[index] = TEXTURE
        elif name in {"head_detail", "head_detail_color", "silver_casual_head_detail"}:
            destinations[index] = HEAD_TEXTURE
    # Blender also preserves material names. Use their base-color references
    # when an embedded image has a generated name, without assuming image order.
    textures = document.get("textures", [])
    for material in document.get("materials", []):
        name = re.sub(r"\.\d{3}$", "", material.get("name", "").lower())
        destination = {"silver_casual": TEXTURE, "silver_casual_head": HEAD_TEXTURE}.get(name)
        color = material.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if not color:
            continue
        index = color["index"]
        require(type(index) is int and 0 <= index < len(textures), "Invalid material base-color texture")
        source = textures[index].get("source")
        require(type(source) is int and 0 <= source < len(images), "Invalid material image source")
        if destination:
            require(source not in destinations or destinations[source] == destination, "Image/material texture role conflict")
            destinations[source] = destination
    if len(images) == 1 and not destinations:
        destinations[0] = TEXTURE  # Compatible with the original unnamed atlas.
    require(len(destinations) == len(images), "Cannot identify body/head image by name or material")
    require(len(set(destinations.values())) == len(images) and TEXTURE in destinations.values(),
            "Duplicate or missing body/head texture roles")
    return destinations


def validate_png(png: bytes) -> None:
    require(png.startswith(b"\x89PNG\r\n\x1a\n") and len(png) >= 45, "Invalid embedded PNG")
    position = 8
    first = True
    has_pixels = False
    while position < len(png):
        require(position + 12 <= len(png), "Truncated PNG chunk")
        size = struct.unpack_from(">I", png, position)[0]
        end = position + 12 + size
        require(end <= len(png), "PNG chunk exceeds image")
        tag = png[position + 4:position + 8]
        content = png[position + 8:position + 8 + size]
        require(zlib.crc32(tag + content) == struct.unpack_from(">I", png, position + 8 + size)[0], "Invalid PNG checksum")
        if first:
            require(tag == b"IHDR" and size == 13, "Missing PNG header")
            width, height = struct.unpack_from(">2I", content)
            require(0 < width <= 16384 and 0 < height <= 16384, "Invalid PNG dimensions")
            first = False
        else:
            require(tag != b"IHDR", "Duplicate PNG header")
        if tag == b"IDAT":
            has_pixels |= size > 0
        if tag == b"IEND":
            require(has_pixels and size == 0 and end == len(png), "Invalid PNG end")
            return
        position = end
    raise ValueError("Missing PNG end")


def external_preview(document: dict, binary: bytes) -> tuple[bytes, dict[str, bytes]]:
    destinations = image_destinations(document)
    external = copy.deepcopy(document)
    pngs = {}
    for index, filename in destinations.items():
        image = external["images"][index]
        require(image.get("mimeType") == "image/png" and "bufferView" in image and "uri" not in image,
                "Source texture must remain embedded PNG")
        view_index = image["bufferView"]
        require(type(view_index) is int and 0 <= view_index < len(document["bufferViews"]), "Invalid image buffer view")
        view = document["bufferViews"][view_index]
        start, size = view.get("byteOffset", 0), view["byteLength"]
        require(view.get("buffer", 0) == 0 and type(start) is int and type(size) is int
                and start >= 0 and size > 0 and start + size <= len(binary), "Image exceeds BIN")
        png = binary[start:start + size]
        validate_png(png)
        pngs[filename] = png
        image.pop("bufferView")
        image["uri"] = filename
    encoded = json.dumps(external, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    encoded += b" " * (-len(encoded) % 4)
    padded_bin = binary + b"\0" * (-len(binary) % 4)
    glb = struct.pack("<5I", 0x46546C67, 2, 28 + len(encoded) + len(padded_bin), len(encoded), 0x4E4F534A)
    glb += encoded + struct.pack("<2I", len(padded_bin), 0x004E4942) + padded_bin
    return glb, pngs


def fnv1a64(path: Path) -> int:
    value = 14695981039346656037
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            for byte in block:
                value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def write_if_changed(path: Path, data: bytes) -> None:
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def json_bytes(value) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def native_sources(root: Path) -> list[tuple[Path, Path]]:
    selected = []
    for namespace in ("models/custom", "materials/custom"):
        directory = root / namespace
        if directory.is_dir():
            for path in sorted(directory.rglob("*")):
                relative = path.relative_to(root)
                name = relative.as_posix()
                final_resource = name == MODEL + "_c" or name.startswith("materials/custom/agents/silver_casual/")
                if path.is_file() and path.suffix.endswith("_c") and final_resource:
                    require(path.resolve().is_relative_to(root.resolve()), "Compiled resource escapes its root")
                    selected.append((path, relative))
    require(any(relative.as_posix() == MODEL + "_c" for _, relative in selected), "Missing compiled silver_casual.vmdl_c")
    require(any(relative.parts[0] == "materials" for _, relative in selected), "Missing compiled Silver Casual materials")
    return selected


def publish(assets: Path, compiled: Path, apply: bool) -> None:
    source = assets / "custom_characters/silver_casual/source/silver_casual.glb"
    document, binary = read_glb(source)
    report = validate_rig(document, binary)
    preview, pngs = external_preview(document, binary)
    resources = native_sources(compiled)
    if HEAD_TEXTURE in pngs:
        require(any(relative.as_posix() == HEAD_MATERIAL for _, relative in resources), "Missing compiled head material")
    index_path = assets / "preview_models/index.json"
    index = json.loads(index_path.read_text(encoding="utf-8-sig"))
    require(isinstance(index, dict), "Preview index is not an object")
    index[MODEL] = PREVIEW
    print(f"Validated {report}; {len(pngs)} preview PNGs, {sum(map(len, pngs.values()))} bytes; {len(resources)} compiled resources")
    print("Source GLB remains embedded; game installation is a separate step.")
    if not apply:
        print("Read-only plan complete. Use --apply after final mesh/compiler QA.")
        return
    write_if_changed(assets / "preview_models" / PREVIEW, preview)
    for filename, png in sorted(pngs.items()):
        write_if_changed(assets / "preview_models" / filename, png)
    for source_file, relative in resources:
        write_if_changed(assets / "game_files" / relative, source_file.read_bytes())
    game_root = assets / "game_files"
    manifest = []
    for namespace in ("models/custom", "materials/custom"):
        for path in sorted((game_root / namespace).rglob("*")):
            if path.is_file() and path.suffix.endswith("_c"):
                require(path.resolve().is_relative_to(game_root.resolve()), "Game resource escapes asset root")
                manifest.append({"path": path.relative_to(game_root).as_posix(), "bytes": path.stat().st_size, "hash": fnv1a64(path)})
    write_if_changed(game_root / "index.json", json_bytes(manifest))
    write_if_changed(index_path, json_bytes(index))
    print(f"Published preview and compiled resources; manifest retains {len(manifest)} total custom resources")


def verify_package(package: Path) -> None:
    assets = package / "cs2_assets"
    index = json.loads((assets / "preview_models/index.json").read_text(encoding="utf-8-sig"))
    require(index.get(MODEL) == PREVIEW, "Portable package omits custom character preview entry")
    document, binary = read_glb(assets / "preview_models" / PREVIEW)
    validate_rig(document, binary)
    destinations = image_destinations(document)
    required = {f"cs2_assets/preview_models/{PREVIEW}",
                f"cs2_assets/preview_models/thumbnails/{THUMBNAIL}", f"cs2_assets/game_files/{MODEL}_c"}
    for index, filename in destinations.items():
        image = document["images"][index]
        require(image.get("uri") == filename and "bufferView" not in image, "Preview does not use packaged external texture")
        validate_png((assets / "preview_models" / filename).read_bytes())
        required.add(f"cs2_assets/preview_models/{filename}")
    resources = json.loads((assets / "game_files/index.json").read_text(encoding="utf-8-sig"))
    silver = [item for item in resources if "silver_casual" in item["path"].lower()]
    require(any(item["path"].startswith("materials/custom/") for item in silver), "Package omits native materials")
    if HEAD_TEXTURE in destinations.values():
        require(any(item["path"] == HEAD_MATERIAL for item in silver), "Package omits native head material")
    for item in silver:
        file = assets / "game_files" / item["path"]
        require(file.stat().st_size == item["bytes"] and fnv1a64(file) == item["hash"], "Compiled resource hash mismatch")
        required.add("cs2_assets/game_files/" + item["path"])
    package_manifest = json.loads((package / "package-manifest.json").read_text(encoding="utf-8-sig"))
    require(package_manifest["hash_algorithm"] == "fnv1a64", "Unsupported package hash algorithm")
    files = {item["path"].replace("\\", "/"): item for item in package_manifest["files"]}
    for name in required:
        require(name in files, f"Package manifest omits {name}")
        file = package / name
        require(file.stat().st_size == files[name]["bytes"] and fnv1a64(file) == files[name]["hash"], f"Package hash mismatch: {name}")
    print(f"PASS: portable preview, {len(destinations)} textures, thumbnail and {len(silver)} native Silver Casual resources")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets-root", type=Path, default=Path(__file__).resolve().parents[2] / "cs2_assets")
    parser.add_argument("--compiled-root", type=Path)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--verify-package", type=Path)
    arguments = parser.parse_args()
    try:
        if arguments.verify_package:
            require(not arguments.apply and arguments.compiled_root is None, "Package verification is read-only")
            verify_package(arguments.verify_package)
        else:
            require(arguments.compiled_root is not None, "Supply --compiled-root with models/ and materials/ trees")
            publish(arguments.assets_root, arguments.compiled_root, arguments.apply)
        return 0
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f"FAIL: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
