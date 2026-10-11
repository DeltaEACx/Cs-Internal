"""Compile the rigged Silver Casual character in an isolated Source 2 workspace.

The model keeps the donor's native skeleton, hitboxes, attachments, physics, and
current CS2 animation graphs. Only the render mesh and its material change.
No game installation or package-manifest changes are performed here.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game")
DEFAULT_VIEWER = Path(os.environ.get("LOCALAPPDATA", "")) / "Temp/cs2-vrf-preview/Source2Viewer-CLI.exe"
MATERIAL = "materials/custom/agents/silver_casual/silver_casual.vmat"
HEAD_MATERIAL = "materials/custom/agents/silver_casual/silver_casual_head.vmat"
MODEL_FOLDER = "models/custom/agents/silver_casual"
SOURCE = ROOT / "cs2_assets/custom_characters/silver_casual/source"


def class_block(text: str, name: str) -> tuple[int, int]:
    match = re.search(r'_class\s*=\s*"' + re.escape(name) + '"', text)
    if match is None:
        raise ValueError(f"Missing donor ModelDoc node: {name}")
    begin = text.rfind("{", 0, match.start())
    return balanced_block(text, begin)


def balanced_block(text: str, begin: int) -> tuple[int, int]:
    quoted = escaped = False
    depth = 0
    for pos in range(begin, len(text)):
        ch = text[pos]
        if quoted:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                quoted = False
            continue
        if ch == '"':
            quoted = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return begin, pos + 1
    raise ValueError("Unclosed donor ModelDoc node")


def restore_ambient_occlusion_list(text: str) -> str:
    # VRF 20 expands the original ao_proxy_capsule_list array into duplicate
    # GenericGameData keys. Reassemble the original native array before compile.
    matches = list(re.finditer(r'game_class\s*=\s*"ao_proxy_capsule"', text))
    blocks = []
    capsules = []
    for match in matches:
        begin, end = balanced_block(text, text.rfind("{", 0, match.start()))
        key_begin, key_end = balanced_block(text, text.find("{", match.end()))
        capsules.append(text[key_begin:key_end])
        comma = re.match(r"\s*,", text[end:])
        blocks.append((begin, end + (comma.end() if comma else 0)))
    if not blocks:
        return text
    replacement = '{ _class = "GenericGameData" name = "" game_class = "ao_proxy_capsule_list" game_keys = [\n' + ",\n".join(capsules) + '\n] },'
    for number in range(len(blocks) - 1, -1, -1):
        begin, end = blocks[number]
        text = text[:begin] + (replacement if number == 0 else "") + text[end:]
    return text


def replace_class(text: str, name: str, replacement: str) -> str:
    begin, end = class_block(text, name)
    if not replacement:
        trailing = re.match(r"\s*,", text[end:])
        if trailing:
            end += trailing.end()
    return text[:begin] + replacement + text[end:]


def prepare_model(args: argparse.Namespace) -> tuple[Path, Path]:
    content = args.workspace / "content/csgo"
    game = args.workspace / "game/csgo"
    game.mkdir(parents=True, exist_ok=True)
    core = game.parent / "core"
    core.mkdir(parents=True, exist_ok=True)
    for stock in (args.game / "core").glob("shaders_pc*.vpk"):
        target = core / stock.name
        if not target.exists() or target.stat().st_size != stock.stat().st_size:
            shutil.copy2(stock, target)
    paths = ["|gameinfo_path|.", os.path.relpath(args.game / "csgo", game.parent).replace("\\", "/"),
             os.path.relpath(args.game / "core", game.parent).replace("\\", "/")]
    search = "\n".join(f'\t\t\t"Game" "{path}"' for path in paths)
    (game / "gameinfo.gi").write_text('"GameInfo"\n{\n "game" "Silver Casual Build"\n "FileSystem"\n {\n  "SearchPaths"\n  {\n' + search + '\n  }\n }\n}\n', encoding="utf-8")

    folder = content / MODEL_FOLDER
    folder.mkdir(parents=True, exist_ok=True)
    material_folder = content / "materials/custom/agents/silver_casual"
    material_folder.mkdir(parents=True, exist_ok=True)
    # CS2's compiler requires power-of-two texture dimensions. Normalized UVs
    # remain identical when the atlas is resampled for the engine texture.
    for source, filename in [(args.atlas, "turnaround.png"), (args.head_atlas, "head_detail_color.png")]:
        with Image.open(source) as atlas:
            dimensions = tuple(1 << (dimension - 1).bit_length() for dimension in atlas.size)
            atlas.convert("RGB").resize(dimensions, Image.Resampling.LANCZOS).save(material_folder / filename)
    Image.new("RGB", (8, 8), (128, 128, 255)).save(material_folder / "normal.png")
    Image.new("RGB", (8, 8), (205, 205, 205)).save(material_folder / "roughness.png")
    Image.new("RGB", (8, 8), (255, 255, 255)).save(material_folder / "ao.png")
    # This is the verified shader used by the donor's skinned body materials.
    # Body and head use independent albedos with the same compatible shader.
    for material_name, color in [("silver_casual", "turnaround.png"),
                                 ("silver_casual_head", "head_detail_color.png")]:
        (material_folder / (material_name + ".vmat")).write_text('''"Layer0"
{
 "shader" "csgo_lightmappedgeneric.vfx"
 "TextureLayer1Color" "materials/custom/agents/silver_casual/''' + color + '''"
 "TextureLayer1Normal" "materials/custom/agents/silver_casual/normal.png"
 "TextureLayer1Roughness" "materials/custom/agents/silver_casual/roughness.png"
 "TextureLayer1AmbientOcclusion" "materials/custom/agents/silver_casual/ao.png"
 "F_NORMAL_MAP" "0"
 "g_flMetalness" "0"
 "g_flModelTintAmount" "1"
 "g_vColorTint" "[1 1 1 1]"
}
''', encoding="utf-8")

    text = args.donor.read_text(encoding="utf-8")
    text = restore_ambient_occlusion_list(text)
    anim_name = "gman_discord.gg_ruzh_ModelDoc_anim_fix.dmx"
    shutil.copy2(args.donor.parent / anim_name, folder / anim_name)
    text = text.replace("characters/models/madebyruzh/gman/" + anim_name,
                        MODEL_FOLDER + "/" + anim_name)
    # Current CS2 ships the AnimGraph2/NmSkeleton assets. These legacy graph and
    # include-model references no longer exist in the installed game's VPKs.
    text = replace_class(text, "AnimIncludeModel", "")
    text = re.sub(r'anim_graph_name\s*=\s*"[^"]*"', 'anim_graph_name = ""', text)
    # LookAtChain was part of the retired animation graph and is no longer a
    # registered ModelDoc node in the current native compiler.
    for retired in ["LookAtChain", "MovementSettings", "Feet"]:
        while '_class = "' + retired + '"' in text:
            text = replace_class(text, retired, "")
    text = replace_class(text, "RenderMeshFile", '''{
 _class = "RenderMeshFile"
 name = "body"
 filename = "''' + MODEL_FOLDER + "/" + args.fbx.name + '''"
 import_scale = ''' + str(args.import_scale) + '''
}''')
    start, _ = class_block(text, "RenderMeshList")
    material_group = '''{
 _class = "MaterialGroupList"
 children = [
  {
   _class = "DefaultMaterialGroup"
   remaps = [
    { from = "silver_casual" to = "''' + MATERIAL + '''" },
    { from = "silver_casual.vmat" to = "''' + MATERIAL + '''" },
    { from = "silver_casual_head" to = "''' + HEAD_MATERIAL + '''" },
    { from = "silver_casual_head.vmat" to = "''' + HEAD_MATERIAL + '''" },
    { from = "''' + MATERIAL + '''" to = "''' + MATERIAL + '''" },
    { from = "''' + HEAD_MATERIAL + '''" to = "''' + HEAD_MATERIAL + '''" },
   ]
   use_global_default = false
   global_default_material = ""
  },
 ]
},
'''
    # The FBX importer must first resolve each material path before these
    # remaps run. Export full native VMAT paths; a blanket default would hide
    # unresolved materials by replacing the separate head atlas with the body.
    text = text[:start] + material_group + text[start:]
    model = folder / (args.model_name + ".vmdl")
    model.write_text(text, encoding="utf-8")
    if args.fbx.is_file() and args.fbx.resolve() != (folder / args.fbx.name).resolve():
        shutil.copy2(args.fbx, folder / args.fbx.name)
    return model, game


def run_logged(command: list[str], log: Path) -> None:
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        tail = "\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-45:])
        raise RuntimeError(f"Native tool failed ({result.returncode}); log: {log}\n{tail}")


def validate(args: argparse.Namespace, model: Path, game: Path) -> None:
    relative = model.relative_to(args.workspace / "content/csgo")
    compiled = game / (relative.as_posix() + "_c")
    data = compiled.read_bytes()
    if len(data) < 16 or struct.unpack_from("<H", data, 4)[0] != 12:
        raise RuntimeError("Compiler did not produce a valid Source 2 resource")
    inspection = args.workspace / (args.model_name + "_native.txt")
    run_logged([str(args.viewer), "-i", str(compiled), "-b", "DATA"], inspection)
    source = inspection.read_text(encoding="utf-8", errors="replace")
    for dependency in ["animation/graphs/worldmodel/worldmodel.vnmgraph",
                       "animation/graphs/viewmodel/viewmodel.vnmgraph",
                       "animation/graphs/ui/uimodel.vnmgraph",
                       "animation/skeletons/characters/worldmodel.vnmskel",
                       "animation/skeletons/characters/viewmodel.vnmskel", MATERIAL, HEAD_MATERIAL]:
        if dependency not in source:
            raise RuntimeError(f"Compiled character lost animation dependency: {dependency}")
    if not all(bone in source for bone in ['"pelvis"', '"head_0"', '"hand_l"', '"hand_r"']):
        raise RuntimeError("Compiled character lost its required humanoid bones")
    if "ao_proxy_capsule_list" not in source:
        raise RuntimeError("Compiled character lost its native ambient-occlusion capsule array")
    mesh_inspection = args.workspace / (args.model_name + "_mesh_native.txt")
    run_logged([str(args.viewer), "-i", str(compiled), "-b", "MDAT"], mesh_inspection)
    hitboxes = mesh_inspection.read_text(encoding="utf-8", errors="replace")
    indices = [int(index) for index in re.findall(r"m_nHitBoxIndex\s*=\s*(\d+)", hitboxes)]
    if indices != list(range(19)):
        raise RuntimeError(f"Compiled character lost its native 19 hitboxes: {indices}")
    preview = args.workspace / (args.model_name + "_compiled.glb")
    run_logged([str(args.viewer), "-i", str(compiled), "-d", "--game", str(game / "gameinfo.gi"),
                "--gltf_export_format", "glb", "--gltf_export_animations",
                "--gltf_animation_list", "ModelDoc_anim_fix", "-o", str(preview)],
               args.workspace / (args.model_name + "_preview.log"))
    raw = preview.read_bytes()
    length, kind = struct.unpack_from("<II", raw, 12)
    gltf = json.loads(raw[20:20 + length])
    positions = [gltf["accessors"][primitive["attributes"]["POSITION"]]
                 for mesh in gltf["meshes"] for primitive in mesh["primitives"]]
    bounds = {"min": [min(accessor["min"][i] for accessor in positions) for i in range(3)],
              "max": [max(accessor["max"][i] for accessor in positions) for i in range(3)]}
    height = bounds["max"][1] - bounds["min"][1]
    if not .9 < height < 2.5:
        raise RuntimeError(f"Compiled mesh has incorrect scale/orientation: {bounds}")
    joints = max((len(skin["joints"]) for skin in gltf.get("skins", [])), default=0)
    if joints != 115:
        raise RuntimeError(f"Compiled rig unexpectedly lost donor bones: {joints}")
    print(json.dumps({"compiled": str(compiled), "bytes": len(data), "joints": joints,
                      "hitboxes": len(indices),
                      "bounds_meters": bounds, "native_inspection": str(inspection)}, indent=2))


def preserve_native_source(args: argparse.Namespace, model: Path) -> None:
    if args.model_name != "silver_casual":
        return
    content = args.workspace / "content/csgo"
    permanent = SOURCE / "native/content/csgo"
    relative = model.relative_to(content)
    folder = permanent / relative.parent
    folder.mkdir(parents=True, exist_ok=True)
    for filename in [model.name, args.fbx.name, "gman_discord.gg_ruzh_ModelDoc_anim_fix.dmx"]:
        shutil.copy2(model.parent / filename, folder / filename)
    material_folder = Path("materials/custom/agents/silver_casual")
    shutil.copytree(content / material_folder, permanent / material_folder, dirs_exist_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=DEFAULT_GAME)
    parser.add_argument("--workspace", type=Path, default=ROOT / ".asset-build/silver_casual")
    parser.add_argument("--donor", type=Path, default=SOURCE / "native_template/gman.vmdl")
    parser.add_argument("--fbx", type=Path)
    parser.add_argument("--atlas", type=Path, default=ROOT / "cs2_assets/custom_characters/silver_casual/source/turnaround.png")
    parser.add_argument("--head-atlas", type=Path, default=SOURCE / "head_detail.png")
    parser.add_argument("--viewer", type=Path, default=DEFAULT_VIEWER)
    parser.add_argument("--model-name", default="silver_casual")
    parser.add_argument("--import-scale", type=float, default=0.3937007874015748,
                        help="FBX centimeter-to-native-inch conversion")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    if args.fbx is None:
        generated = args.workspace / "content/csgo" / MODEL_FOLDER / "silver_casual.fbx"
        bundled = SOURCE / "native/content/csgo" / MODEL_FOLDER / "silver_casual.fbx"
        args.fbx = generated if generated.is_file() else bundled
    model, game = prepare_model(args)
    print(f"Prepared {model}")
    if args.prepare_only:
        return
    if not args.fbx.is_file():
        raise FileNotFoundError(f"Missing rigged FBX: {args.fbx}")
    compiler = args.game / "bin/win64/resourcecompiler.exe"
    for relative in [MATERIAL, HEAD_MATERIAL]:
        material = args.workspace / "content/csgo" / relative
        run_logged([str(compiler), "-game", str(game), "-i", str(material), "-nop4", "-f"],
                   args.workspace / (material.stem + "_material_compile.log"))
    run_logged([str(compiler), "-game", str(game), "-i", str(model), "-nop4", "-f"],
               args.workspace / (args.model_name + "_compile.log"))
    validate(args, model, game)
    preserve_native_source(args, model)


if __name__ == "__main__":
    main()
