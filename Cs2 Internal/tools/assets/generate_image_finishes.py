"""Generate and compile camo images for the existing image-over-skin feature.

Requires Python/Pillow and CS2 Workshop Tools. Writes inside the project.
Use --install to copy the eight compiled textures into the supplied game folder.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "cs2_assets"
PRESETS = [
    ("woodland", "Woodland Camo", ["#334330", "#748254", "#9f9168", "#252e25"], "organic"),
    ("desert", "Desert Camo", ["#ccb68e", "#a58760", "#e1d4b4", "#695749"], "organic"),
    ("arctic", "Arctic Camo", ["#e5ebe9", "#aabcc2", "#7a929d", "#435b68"], "organic"),
    ("urban", "Urban Digital", ["#68737a", "#333c43", "#a6b0b4", "#1c252c"], "digital"),
    ("woodland_digital", "Woodland Digital", ["#334330", "#748254", "#9f9168", "#252e25"], "digital"),
    ("desert_digital", "Desert Digital", ["#ccb68e", "#a58760", "#e1d4b4", "#695749"], "digital"),
    ("tiger", "Tiger Stripe", ["#857943", "#403e26", "#b7a36a", "#22291c"], "stripe"),
    ("midnight", "Midnight Digital", ["#26354d", "#465873", "#7b8da5", "#182130"], "digital"),
]

def pattern(file, palette, seed, kind):
    rng = random.Random(seed)
    image = Image.new("RGB", (512, 512), palette[0])
    draw = ImageDraw.Draw(image)
    for color in palette[1:] + palette[1:]:
        for _ in range(34):
            x, y = rng.randrange(512), rng.randrange(512)
            if kind == "digital":
                x, y = x // 16 * 16, y // 16 * 16
                for _ in range(rng.randrange(3, 9)):
                    w, h = rng.randrange(1, 5) * 16, rng.randrange(1, 3) * 16
                    for dx in [-512, 0, 512]:
                        for dy in [-512, 0, 512]:
                            draw.rectangle((x + dx, y + dy, x + w + dx, y + h + dy), fill=color)
                    x += rng.choice([-32, 0, 32])
                    y += rng.choice([-16, 0, 16])
                continue
            if kind == "stripe":
                points = [(x + t * 12, y + math.sin(t * .6) * 12 + t * 2) for t in range(16)]
                points += [(x + t * 12, y + math.sin(t * .6) * 12 + t * 2 + 8 + 10 * math.sin(t * .25))
                           for t in reversed(range(16))]
            else:
                radius = rng.randrange(16, 62)
                points = [(x + math.cos(a * math.tau / 12) * radius * rng.uniform(.55, 1.2),
                           y + math.sin(a * math.tau / 12) * radius * rng.uniform(.45, 1.1))
                          for a in range(12)]
            for dx in [-512, 0, 512]:
                for dy in [-512, 0, 512]:
                    draw.polygon([(px + dx, py + dy) for px, py in points], fill=color)
    image.save(file)

def resource_hash(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=Path(
        "C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game"))
    parser.add_argument("--install", action="store_true")
    args = parser.parse_args()
    compiler = args.game / "bin/win64/resourcecompiler.exe"
    if not compiler.is_file():
        raise RuntimeError("Install CS2 Workshop Tools or pass --game with the CS2 game directory")
    workspace = ROOT / ".asset-build"
    source = ASSETS / "custom_images/camo/source/materials/custom/camo"
    content, game = workspace / "content/csgo", workspace / "game/csgo"
    source.mkdir(parents=True, exist_ok=True)
    game.mkdir(parents=True, exist_ok=True)
    (workspace / "game/core").mkdir(exist_ok=True)
    for file in (args.game / "core").glob("shaders_pc*.vpk"):
        shutil.copyfile(file, workspace / "game/core" / file.name)
    installed = Path(os.path.relpath(args.game / "csgo", workspace / "game")).as_posix()
    core = Path(os.path.relpath(args.game / "core", workspace / "game")).as_posix()
    (game / "gameinfo.gi").write_text(
        '"GameInfo" { "game" "Image finish build" "FileSystem" { "SearchPaths" { '
        f'"Game" "|gameinfo_path|." "Game" "{installed}" "Game" "{core}" }} }} }}')
    for id, label, palette, kind in PRESETS:
        seed = int.from_bytes(hashlib.sha256(id.encode()).digest()[:4], "little")
        pattern(source / f"{id}.png", palette, seed, kind)
        (source / f"{id}.vtex").write_text(f'''<!-- dmx encoding keyvalues2_noids 1 format vtex 1 -->
"CDmeVtex"
{{
    "m_inputTextureArray" "element_array"
    [ "CDmeInputTexture" {{ "m_name" "string" "Color" "m_fileName" "string" "materials/custom/camo/{id}.png"
      "m_colorSpace" "string" "srgb" "m_typeString" "string" "2D" }} ]
    "m_outputTypeString" "string" "2D"
    "m_outputFormat" "string" "BC7"
    "m_nOutputMinDimension" "int" "512"
    "m_nOutputMaxDimension" "int" "512"
    "m_textureOutputChannelArray" "element_array"
    [ "CDmeTextureOutputChannel" {{ "m_inputTextureArray" "string_array" [ "Color" ]
      "m_srcChannels" "string" "rgb" "m_dstChannels" "string" "rgb"
      "m_mipAlgorithm" "CDmeImageProcessor" {{ "m_algorithm" "string" "Box" }}
      "m_outputColorSpace" "string" "srgb" }} ]
}}
''')
    destination = content / "materials/custom/camo"
    shutil.copytree(source, destination, dirs_exist_ok=True)
    result = subprocess.run([str(compiler), "-game", str(game), "-i", str(destination / "*.vtex"),
                             "-nop4", "-f"], capture_output=True, text=True)
    log = workspace / "image_finishes.log"
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"Texture compilation failed; see {log}")
    manifest_file = ASSETS / "game_files/index.json"
    manifest = json.loads(manifest_file.read_text())
    names = {f"materials/custom/camo/{id}.vtex_c" for id, *_ in PRESETS}
    manifest = [entry for entry in manifest if entry["path"] not in names]
    plan = []
    for id, label, _, _ in PRESETS:
        compiled = game / f"materials/custom/camo/{id}.vtex_c"
        data = compiled.read_bytes()
        if len(data) < 16 or struct.unpack_from("<H", data, 4)[0] != 12:
            raise RuntimeError(f"Invalid compiled texture: {compiled}")
        relative = f"materials/custom/camo/{id}.vtex_c"
        target = ASSETS / "game_files" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(compiled, target)
        manifest.append({"path": relative, "bytes": len(data), "hash": resource_hash(data)})
        plan.append((target, args.game / "csgo" / relative))
        print(f"Built {label}", flush=True)
    manifest_file.write_text(json.dumps(manifest, indent=2) + "\n")
    if args.install:
        for source_file, target in plan:
            if not target.resolve().is_relative_to((args.game / "csgo/materials/custom/camo").resolve()):
                raise RuntimeError("Texture install path escaped custom camo directory")
            if target.exists() and target.read_bytes() != source_file.read_bytes():
                backup = workspace / "backups" / target.name
                backup.parent.mkdir(parents=True, exist_ok=True)
                if not backup.exists(): shutil.copyfile(target, backup)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_file, target)
            if source_file.read_bytes() != target.read_bytes(): raise RuntimeError("Install readback failed")
        print("Installed eight image textures; stock weapon resources are unchanged.")

if __name__ == "__main__":
    main()
