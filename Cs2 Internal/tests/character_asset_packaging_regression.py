"""Deterministic publishing tests; tiny synthetic resources are packaging fixtures."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
from argparse import Namespace
from PIL import Image

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools/assets/package_silver_casual.py"
SPEC = importlib.util.spec_from_file_location("silver_package", MODULE_PATH)
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)
BUILD_SPEC = importlib.util.spec_from_file_location("silver_build", MODULE_PATH.with_name("build_silver_casual.py"))
build = importlib.util.module_from_spec(BUILD_SPEC)
BUILD_SPEC.loader.exec_module(build)


def png_fixture(color=(0x20, 0x40, 0x60, 0xff)):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", 1, 1, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"\0" + bytes(color))) + chunk(b"IEND", b""))


def glb_fixture(two_textures=False, head_first=False):
    document = {"asset": {"version": "2.0"}, "bufferViews": [], "accessors": [],
                "images": [], "textures": [{"source": 0}], "materials": [{"pbrMetallicRoughness": {
                    "baseColorTexture": {"index": 0}}}], "nodes": [{"name": "hand_l"}, {"name": "hand_r"}] +
                [{"name": f"bone_{i}"} for i in range(8)] + [{"name": "body", "mesh": 0, "skin": 0}],
                "skins": [{"joints": list(range(10))}], "meshes": []}
    binary = bytearray()

    def append(rows, component, kind):
        binary.extend(b"\0" * (-len(binary) % 4))
        offset = len(binary)
        fmt = {5123: "H", 5126: "f"}[component]
        for row in rows:
            binary.extend(struct.pack("<" + fmt * len(row), *row))
        view = len(document["bufferViews"])
        document["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": len(binary) - offset})
        index = len(document["accessors"])
        document["accessors"].append({"bufferView": view, "componentType": component, "type": kind, "count": len(rows)})
        return index

    positions = [(i / 100, 1., 0.) for i in range(101)]
    attributes = {"POSITION": append(positions, 5126, "VEC3"),
                  "NORMAL": append([(0., 0., 1.)] * 101, 5126, "VEC3"),
                  "TEXCOORD_0": append([(0., 0.)] * 101, 5126, "VEC2"),
                  "JOINTS_0": append([(0, 0, 0, 0)] * 101, 5123, "VEC4"),
                  "WEIGHTS_0": append([(1., 0., 0., 0.)] * 101, 5126, "VEC4")}
    indices = append([(i,) for i in [0, 1, 2] * 102], 5123, "SCALAR")
    matrix = tuple(float(row == column) for row in range(4) for column in range(4))
    document["skins"][0]["inverseBindMatrices"] = append([matrix] * 10, 5126, "MAT4")
    document["meshes"] = [{"primitives": [{"attributes": attributes, "indices": indices, "material": 0}]}]
    binary.extend(b"\0" * (-len(binary) % 4))
    images = [("turnaround", png_fixture())]
    if two_textures:
        images.append(("head_detail", png_fixture((0xc0, 0xb0, 0xa0, 0xff))))
        if head_first:
            images.reverse()
        document["textures"] = [{"source": int(head_first)}, {"source": int(not head_first)}]
        document["materials"][0]["name"] = "silver_casual"
        document["materials"].append({"name": "silver_casual_head", "pbrMetallicRoughness": {"baseColorTexture": {"index": 1}}})
        document["meshes"][0]["primitives"].append({"attributes": attributes.copy(), "indices": indices, "material": 1})
    document["images"] = []
    for name, png in images:
        image = {"bufferView": len(document["bufferViews"]), "mimeType": "image/png"}
        if two_textures:
            image["name"] = name
        document["images"].append(image)
        document["bufferViews"].append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(png)})
        binary.extend(png)
        binary.extend(b"\0" * (-len(binary) % 4))
    document["buffers"] = [{"byteLength": len(binary)}]
    text = json.dumps(document).encode()
    text += b" " * (-len(text) % 4)
    return struct.pack("<5I", 0x46546C67, 2, 28 + len(text) + len(binary), len(text), 0x4E4F534A) + text + \
        struct.pack("<2I", len(binary), 0x004E4942) + binary


class PackagingRegression(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="silver-package-")
        self.root = Path(self.temporary.name)
        self.assets = self.root / "cs2_assets"
        self.compiled = self.root / "compiled"
        self.source = self.assets / "custom_characters/silver_casual/source/silver_casual.glb"
        self.source.parent.mkdir(parents=True)
        self.original = glb_fixture()
        self.source.write_bytes(self.original)
        (self.assets / "preview_models").mkdir()
        (self.assets / "preview_models/index.json").write_text(json.dumps({"agents/other.vmdl": "other.glb"}))
        self.native_model = self.compiled / (package.MODEL + "_c")
        self.native_material = self.compiled / "materials/custom/agents/silver_casual/body.vmat_c"
        for file, content in [(self.native_model, b"compiled-model-fixture"), (self.native_material, b"compiled-material-fixture")]:
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(content)
        (self.native_model.parent / "scale_test.vmdl_c").write_bytes(b"debug model must not be published")
        other = self.assets / "game_files/models/custom/other/other.vmdl_c"
        other.parent.mkdir(parents=True)
        other.write_bytes(b"preserved unrelated resource")

    def tearDown(self):
        self.temporary.cleanup()

    def test_external_preview_keeps_embedded_source_and_all_vertex_bytes(self):
        document, binary = package.read_glb(self.source)
        report = package.validate_rig(document, binary)
        self.assertEqual(report["vertices"], 101)
        preview, pngs = package.external_preview(document, binary)
        destination = self.root / "preview.glb"
        destination.write_bytes(preview)
        external, external_binary = package.read_glb(destination)
        self.assertEqual(external["images"][0]["uri"], package.TEXTURE)
        self.assertNotIn("bufferView", external["images"][0])
        self.assertEqual(external_binary, binary)
        self.assertEqual(pngs, {package.TEXTURE: png_fixture()})
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_two_textures_match_names_and_materials_independent_of_image_order(self):
        self.original = glb_fixture(two_textures=True, head_first=True)
        self.source.write_bytes(self.original)
        document, binary = package.read_glb(self.source)
        original_document = json.loads(json.dumps(document))
        preview, pngs = package.external_preview(document, binary)
        destination = self.root / "preview.glb"
        destination.write_bytes(preview)
        external, external_binary = package.read_glb(destination)
        self.assertEqual([image["uri"] for image in external["images"]], [package.HEAD_TEXTURE, package.TEXTURE])
        self.assertEqual(pngs[package.TEXTURE], png_fixture())
        self.assertEqual(pngs[package.HEAD_TEXTURE], png_fixture((0xc0, 0xb0, 0xa0, 0xff)))
        self.assertEqual(external_binary, binary)
        self.assertEqual(document, original_document)
        self.assertEqual(external["skins"], document["skins"])
        self.assertEqual(external["meshes"], document["meshes"])
        for image in document["images"]:
            image.pop("name")
        self.assertEqual(package.external_preview(document, binary)[1], pngs)
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_image_bounds_role_conflicts_and_invalid_png_are_rejected(self):
        self.source.write_bytes(glb_fixture(two_textures=True, head_first=True))
        original, binary = package.read_glb(self.source)
        for invalid in ["offset", "buffer", "view", "png", "roles", "unknown"]:
            document = json.loads(json.dumps(original))
            data = binary
            image = document["images"][0]
            view = document["bufferViews"][image["bufferView"]]
            if invalid == "offset": view["byteOffset"] = -1
            elif invalid == "buffer": view["buffer"] = 1
            elif invalid == "view": image["bufferView"] = -1
            elif invalid == "png":
                data = bytearray(data); data[view["byteOffset"] + 20] ^= 1; data = bytes(data)
            elif invalid == "roles": image["name"] = "turnaround"
            elif invalid == "unknown":
                for item in document["images"]: item.pop("name")
                for item in document["materials"]: item.pop("name")
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                package.external_preview(document, data)

    def test_two_texture_publish_requires_head_material_and_verifies_both_files(self):
        self.source.write_bytes(glb_fixture(two_textures=True, head_first=True))
        before = {file: file.read_bytes() for file in self.assets.rglob("*") if file.is_file()}
        with self.assertRaisesRegex(ValueError, "head material"):
            package.publish(self.assets, self.compiled, True)
        self.assertEqual(before, {file: file.read_bytes() for file in self.assets.rglob("*") if file.is_file()})
        native_head = self.compiled / package.HEAD_MATERIAL
        native_head.write_bytes(b"compiled-head-material-fixture")
        package.publish(self.assets, self.compiled, True)
        thumbnail = self.assets / "preview_models/thumbnails" / package.THUMBNAIL
        thumbnail.parent.mkdir(); thumbnail.write_bytes(png_fixture())
        self.assertEqual((self.assets / "preview_models" / package.HEAD_TEXTURE).read_bytes(), png_fixture((0xc0, 0xb0, 0xa0, 0xff)))
        manifest = {"hash_algorithm": "fnv1a64", "files": [
            {"path": file.relative_to(self.root).as_posix(), "bytes": file.stat().st_size, "hash": package.fnv1a64(file)}
            for file in self.assets.rglob("*") if file.is_file()]}
        manifest_path = self.root / "package-manifest.json"
        manifest_path.write_text(json.dumps(manifest))
        package.verify_package(self.root)
        manifest["files"] = [item for item in manifest["files"] if not item["path"].endswith(package.HEAD_TEXTURE)]
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "manifest omits.*head_detail"):
            package.verify_package(self.root)

    def test_modeldoc_head_remap_preserves_native_rig_hitboxes_and_physics(self):
        body, head = self.root / "body.png", self.root / "head.png"
        Image.new("RGB", (3, 5), (20, 40, 60)).save(body)
        Image.new("RGB", (9, 7), (200, 160, 140)).save(head)
        fbx = self.root / "silver_casual.fbx"; fbx.write_bytes(b"rigged-fbx-fixture")
        args = Namespace(workspace=self.root / "build", game=self.root / "stock", atlas=body, head_atlas=head,
                         donor=build.SOURCE / "native_template/gman.vmdl", fbx=fbx,
                         model_name="silver_casual", import_scale=0.3937007874015748)
        model, _ = build.prepare_model(args)
        original, generated = args.donor.read_text(), model.read_text()
        for node in ["Skeleton", "HitboxSetList", "PhysicsShapeList", "AttachmentList"]:
            begin, end = build.class_block(original, node)
            new_begin, new_end = build.class_block(generated, node)
            self.assertEqual(generated[new_begin:new_end], original[begin:end], node)
        self.assertIn('from = "silver_casual" to = "' + build.MATERIAL + '"', generated)
        self.assertIn('from = "silver_casual_head" to = "' + build.HEAD_MATERIAL + '"', generated)
        self.assertIn('from = "' + build.MATERIAL + '" to = "' + build.MATERIAL + '"', generated)
        self.assertIn('from = "' + build.HEAD_MATERIAL + '" to = "' + build.HEAD_MATERIAL + '"', generated)
        self.assertIn('use_global_default = false', generated)
        self.assertIn('global_default_material = ""', generated)
        materials = args.workspace / "content/csgo/materials/custom/agents/silver_casual"
        self.assertIn('/turnaround.png', (materials / "silver_casual.vmat").read_text())
        self.assertIn('/head_detail_color.png', (materials / "silver_casual_head.vmat").read_text())
        with Image.open(materials / "head_detail_color.png") as image:
            self.assertEqual(image.size, (16, 8))

    def test_apply_is_idempotent_and_preserves_other_entries(self):
        package.publish(self.assets, self.compiled, True)
        index = json.loads((self.assets / "preview_models/index.json").read_text())
        self.assertEqual(index["agents/other.vmdl"], "other.glb")
        self.assertEqual(index[package.MODEL], package.PREVIEW)
        self.assertFalse((self.assets / "game_files" / self.native_model.relative_to(self.compiled).parent / "scale_test.vmdl_c").exists())
        manifest = json.loads((self.assets / "game_files/index.json").read_text())
        self.assertEqual(len(manifest), 3)
        for item in manifest:
            file = self.assets / "game_files" / item["path"]
            self.assertEqual(file.stat().st_size, item["bytes"])
            self.assertEqual(package.fnv1a64(file), item["hash"])
        published = {file: (file.read_bytes(), file.stat().st_mtime_ns) for file in self.assets.rglob("*") if file.is_file()}
        package.publish(self.assets, self.compiled, True)
        self.assertEqual(published, {file: (file.read_bytes(), file.stat().st_mtime_ns) for file in self.assets.rglob("*") if file.is_file()})
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_dry_run_and_missing_native_resource_do_not_publish(self):
        before = {file: file.read_bytes() for file in self.assets.rglob("*") if file.is_file()}
        package.publish(self.assets, self.compiled, False)
        self.native_material.unlink()
        with self.assertRaisesRegex(ValueError, "materials"):
            package.publish(self.assets, self.compiled, True)
        self.assertEqual(before, {file: file.read_bytes() for file in self.assets.rglob("*") if file.is_file()})

    def test_package_smoke_requires_thumbnail_and_detects_bad_hash(self):
        package.publish(self.assets, self.compiled, True)
        thumbnail = self.assets / "preview_models/thumbnails" / package.THUMBNAIL
        thumbnail.parent.mkdir()
        thumbnail.write_bytes(png_fixture())
        manifest = {"version": 1, "hash_algorithm": "fnv1a64", "files": [
            {"path": file.relative_to(self.root).as_posix(), "bytes": file.stat().st_size, "hash": package.fnv1a64(file)}
            for file in self.assets.rglob("*") if file.is_file()]}
        (self.root / "package-manifest.json").write_text(json.dumps(manifest))
        package.verify_package(self.root)
        thumbnail.write_bytes(b"damaged thumbnail")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            package.verify_package(self.root)


if __name__ == "__main__":
    unittest.main()
