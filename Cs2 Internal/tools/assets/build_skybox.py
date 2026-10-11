"""Compile a supplied 2:1 PNG panorama into a packaged Source 2 sky cubemap.

Requires Python with NumPy/Pillow and the installed CS2 Workshop Tools.
The original panorama is preserved unchanged. A mathematical projection bakes
six full-resolution faces into a horizontal strip; Valve's resource compiler
then encodes that strip as BC6H without its implicit 256px panorama conversion.
Builds stay inside the project. --install copies only this custom sky resource
into CS2; --install-only installs a previously built and verified resource.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "cs2_assets"
DEFAULT_GAME = Path("C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game")


def resource_hash(data: bytes) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def png_dimensions(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        header = stream.read(24)
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise ValueError(f"Expected a PNG panorama: {path}")
    width, height = struct.unpack_from(">II", header, 16)
    if not height or width != height * 2:
        raise ValueError(f"Panorama must be 2:1 equirectangular; received {width} x {height}")
    return width, height


def project_cube_strip(panorama: Path, destination: Path, face_size: int, source_yaw: float = 0.0) -> None:
    """Bake six DirectX faces in linear light, preserving the panorama's artwork.

    Source 2 uses Z up, with the panorama center looking toward -X. Sampling at
    texel centers and wrapping longitude produces continuous adjacent faces;
    this does not upscale the compiler's low-resolution intermediate cubemap.
    Rows are processed in bounded chunks to keep high-resolution builds small.
    """
    import numpy as np
    from PIL import Image

    with Image.open(panorama) as image:
        pixels = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
    pixels = np.where(pixels <= .04045, pixels / 12.92, ((pixels + .055) / 1.055) ** 2.4)
    height, width = pixels.shape[:2]
    strip = Image.new("RGB", (face_size * 6, face_size))
    horizontal = (np.arange(face_size, dtype=np.float32)[None, :] + .5) * (2.0 / face_size) - 1.0
    for face in range(6):
        for first_row in range(0, face_size, 128):
            last_row = min(first_row + 128, face_size)
            vertical = (np.arange(first_row, last_row, dtype=np.float32)[:, None] + .5) * (2.0 / face_size) - 1.0
            # DirectX cube order: +X, -X, +Y, -Y, +Z, -Z.
            directions = [(1.0, -vertical, -horizontal), (-1.0, -vertical, horizontal),
                          (horizontal, 1.0, vertical), (horizontal, -1.0, -vertical),
                          (horizontal, -vertical, 1.0), (-horizontal, -vertical, -1.0)]
            x, y, z = directions[face]
            longitude = np.arctan2(y, -x)
            latitude = np.arcsin(z / np.sqrt(x * x + y * y + z * z))
            source_x = (((longitude + math.radians(source_yaw)) / (2.0 * math.pi) + .5) * width - .5) % width
            source_y = np.clip((.5 - latitude / math.pi) * height - .5, 0, height - 1)
            x0, y0 = np.floor(source_x).astype(np.int32), np.floor(source_y).astype(np.int32)
            x1, y1 = (x0 + 1) % width, np.minimum(y0 + 1, height - 1)
            fraction_x, fraction_y = (source_x - x0)[..., None], (source_y - y0)[..., None]
            top = pixels[y0, x0] * (1.0 - fraction_x) + pixels[y0, x1] * fraction_x
            bottom = pixels[y1, x0] * (1.0 - fraction_x) + pixels[y1, x1] * fraction_x
            linear = top * (1.0 - fraction_y) + bottom * fraction_y
            srgb = np.where(linear <= .0031308, linear * 12.92, 1.055 * linear ** (1.0 / 2.4) - .055)
            tile = Image.fromarray(np.round(np.clip(srgb, 0, 1) * 255).astype(np.uint8))
            strip.paste(tile, (face * face_size, first_row))
    strip.save(destination)


def validate_texture(path: Path, face_size: int) -> bytes:
    data = path.read_bytes()
    if len(data) < 40 or struct.unpack_from("<H", data, 4)[0] != 12:
        raise ValueError(f"Invalid Source 2 resource: {path}")
    metadata_size, _, _, table_relative, count = struct.unpack_from("<IHHII", data)
    table = 8 + table_relative
    if metadata_size > len(data) or count > 32 or table + count * 12 > metadata_size:
        raise ValueError(f"Invalid Source 2 resource block table: {path}")
    for index in range(count):
        entry = table + index * 12
        tag, offset, size = struct.unpack_from("<4sII", data, entry)
        if tag != b"DATA":
            continue
        start = entry + 4 + offset
        if size < 40 or start + size > metadata_size:
            raise ValueError(f"Invalid texture DATA block: {path}")
        version, flags, *fields = struct.unpack_from("<HH4fHHHBB", data, start)
        width, height, depth, texture_format, mips = fields[4:]
        if version != 1 or not flags & 0x10 or texture_format != 19:
            raise ValueError("Expected a native BC6H cube texture")
        if (width, height, depth) != (face_size, face_size, 1) or not mips:
            raise ValueError(f"Unexpected cube dimensions/mips: {width} x {height} x {depth}, {mips} mips")
        extra_offset, extra_count = struct.unpack_from("<II", data, start + 32)
        extra_table = start + 32 + extra_offset
        if extra_count > 32 or extra_table + extra_count * 12 > start + size:
            raise ValueError("Invalid texture extra-data table")
        for extra_index in range(extra_count):
            extra_entry = extra_table + extra_index * 12
            kind, extra_relative, extra_size = struct.unpack_from("<III", data, extra_entry)
            if kind != 3:  # VTEX_EXTRA_DATA_METADATA / non-power-of-two display rectangle.
                continue
            extra_start = extra_entry + 4 + extra_relative
            if extra_size < 6 or extra_start + extra_size > start + size:
                raise ValueError("Invalid texture display metadata")
            display_width, display_height = struct.unpack_from("<HH", data, extra_start + 2)
            if display_width not in (0, width) or display_height not in (0, height):
                raise ValueError(f"Cube display rectangle would crop its faces: {display_width} x {display_height}")
        if len(data) <= metadata_size:
            raise ValueError("Compiled sky texture has no pixel data")
        if mips == 1:
            expected_pixels = 6 * ((width + 3) // 4) * ((height + 3) // 4) * 16
            if len(data) - metadata_size != expected_pixels:
                raise ValueError("Expected all six complete native BC6H face payloads")
        return data
    raise ValueError(f"Compiled texture has no DATA block: {path}")


def descriptor(panorama_name: str, face_size: int) -> str:
    return f'''<!-- dmx encoding keyvalues2_noids 1 format vtex 1 -->
"CDmeVtex"
{{
    "m_inputTextureArray" "element_array"
    [
        "CDmeInputTexture"
        {{
            "m_name" "string" "Sky"
            "m_fileName" "string" "materials/custom/skybox/{panorama_name}.png"
            "m_colorSpace" "string" "srgb"
            "m_typeString" "string" "CUBE"
        }}
    ]
    "m_outputTypeString" "string" "CUBE"
    "m_outputFormat" "string" "BC6H"
    "m_nOutputMinDimension" "int" "{face_size}"
    "m_nOutputMaxDimension" "int" "{face_size}"
    "m_nDisplayRectWidth" "int" "{face_size}"
    "m_nDisplayRectHeight" "int" "{face_size}"
    "m_textureOutputChannelArray" "element_array"
    [
        "CDmeTextureOutputChannel"
        {{
            "m_inputTextureArray" "string_array" [ "Sky" ]
            "m_srcChannels" "string" "rgb"
            "m_dstChannels" "string" "rgb"
            "m_mipAlgorithm" "CDmeImageProcessor"
            {{
                "m_algorithm" "string" "Box"
            }}
            "m_outputColorSpace" "string" "linear"
        }}
    ]
    "m_vClamp" "vector3" "1 1 1"
}}
'''


def install(source: Path, game: Path, relative: Path, data: bytes, workspace: Path) -> None:
    if not (game / "csgo/gameinfo.gi").is_file():
        raise ValueError("--game must point to the CS2 game directory")
    allowed = (game / "csgo/materials/custom/skybox").resolve()
    target = (game / "csgo" / relative).resolve()
    if target.parent != allowed:
        raise ValueError("Sky install path escaped the custom skybox directory")
    if target.exists():
        previous = target.read_bytes()
        if previous != data:
            backup = workspace / "backups" / f"{target.stem}-{hashlib.sha256(previous).hexdigest()[:16]}{target.suffix}"
            backup.parent.mkdir(parents=True, exist_ok=True)
            if not backup.exists():
                backup.write_bytes(previous)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    if target.read_bytes() != data:
        raise RuntimeError("Installed sky texture failed byte-for-byte verification")
    print(f"Installed {target}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--id", default="crimson_dawn_sky", help="Resource name, lowercase letters/digits/underscores")
    parser.add_argument("--panorama-name", help="Source basename; defaults to the saved asset build settings")
    parser.add_argument("--panorama", type=Path, help="Supplied 2:1 PNG; defaults to the asset's source folder")
    parser.add_argument("--face-size", type=int, choices=[256, 512, 1024, 2048])
    parser.add_argument("--source-yaw", type=float, help="Panorama longitude offset in degrees; saved for future builds")
    parser.add_argument("--game", type=Path, default=DEFAULT_GAME)
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--install-only", action="store_true")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,63}", args.id):
        parser.error("Resource name must use lowercase letters, digits and underscores")
    workspace = ROOT / ".asset-build" / args.id
    source = ASSETS / "custom_images" / args.id / "source/materials/custom/skybox"
    settings_path = source / f"{args.id}.build.json"
    settings = json.loads(settings_path.read_text(encoding="utf-8")) if settings_path.is_file() else {}
    args.panorama_name = args.panorama_name or settings.get("panorama_name", args.id.removesuffix("_sky") + "_panorama")
    args.face_size = args.face_size if args.face_size is not None else settings.get("face_size", 1024)
    args.source_yaw = args.source_yaw if args.source_yaw is not None else settings.get("source_yaw", 0.0)
    if not isinstance(args.panorama_name, str) or not re.fullmatch(r"[a-z][a-z0-9_]{0,63}", args.panorama_name):
        parser.error("Panorama name must use lowercase letters, digits and underscores")
    if args.face_size not in [256, 512, 1024, 2048]:
        parser.error("Saved face size must be 256, 512, 1024 or 2048")
    if not isinstance(args.source_yaw, (int, float)) or not math.isfinite(args.source_yaw):
        parser.error("Source yaw must be a finite number of degrees")
    args.source_yaw %= 360.0
    relative = Path(f"materials/custom/skybox/{args.id}.vtex_c")
    packaged = ASSETS / "game_files" / relative
    manifest_path = ASSETS / "game_files/index.json"
    if args.install_only:
        data = validate_texture(packaged, args.face_size)
        expected = {"path": relative.as_posix(), "bytes": len(data), "hash": resource_hash(data)}
        matches = [entry for entry in json.loads(manifest_path.read_text()) if entry["path"] == relative.as_posix()]
        if matches != [expected]:
            raise ValueError("Packaged sky texture does not match its manifest")
        install(packaged, args.game, relative, data, workspace)
        return

    panorama = args.panorama or source / f"{args.panorama_name}.png"
    dimensions = png_dimensions(panorama)
    compiler = args.game / "bin/win64/resourcecompiler.exe"
    if not compiler.is_file():
        raise FileNotFoundError("Install CS2 Workshop Tools or supply --game with its game directory")
    source.mkdir(parents=True, exist_ok=True)
    preserved = source / f"{args.panorama_name}.png"
    if panorama.resolve() != preserved.resolve():
        shutil.copy2(panorama, preserved)
    strip_name = f"{args.id}_cube"
    strip = source / f"{strip_name}.png"
    project_cube_strip(preserved, strip, args.face_size, args.source_yaw)
    vtex = source / f"{args.id}.vtex"
    vtex.write_text(descriptor(strip_name, args.face_size), encoding="utf-8")
    content = workspace / "content/csgo/materials/custom/skybox"
    staged_game = workspace / "game/csgo"
    content.mkdir(parents=True, exist_ok=True)
    staged_game.mkdir(parents=True, exist_ok=True)
    core = workspace / "game/core"
    core.mkdir(parents=True, exist_ok=True)
    for file in (args.game / "core").glob("shaders_pc*.vpk"):
        shutil.copy2(file, core / file.name)
    paths = [Path(os.path.relpath(args.game / directory, workspace / "game")).as_posix() for directory in ["csgo", "core"]]
    (staged_game / "gameinfo.gi").write_text(
        '"GameInfo" { "game" "Custom sky build" "FileSystem" { "SearchPaths" { '
        f'"Game" "|gameinfo_path|." "Game" "{paths[0]}" "Game" "{paths[1]}" }} }} }}', encoding="utf-8")
    shutil.copy2(strip, content / strip.name)
    shutil.copy2(vtex, content / vtex.name)
    log = workspace / "compile.log"
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run([str(compiler), "-game", str(staged_game), "-i", str(content / vtex.name), "-nop4", "-f"],
                                stdout=output, stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        raise RuntimeError(f"Sky compilation failed ({result.returncode}); see {log}")
    compiled = staged_game / relative
    data = validate_texture(compiled, args.face_size)
    packaged.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(compiled, packaged)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest = [entry for entry in manifest if entry["path"] != relative.as_posix()]
    manifest.append({"path": relative.as_posix(), "bytes": len(data), "hash": resource_hash(data)})
    temporary_manifest = manifest_path.with_suffix(".json.tmp")
    temporary_manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    temporary_manifest.replace(manifest_path)
    settings_path.write_text(json.dumps({"panorama_name": args.panorama_name, "face_size": args.face_size,
                                         "source_yaw": args.source_yaw,
                                         "source_sha256": hashlib.sha256(preserved.read_bytes()).hexdigest()}, indent=2) + "\n",
                             encoding="utf-8")
    print(f"Built {args.id}: {dimensions[0]} x {dimensions[1]} panorama, six {args.face_size}px BC6H cube faces")
    print(f"Packaged {packaged} ({len(data)} bytes, SHA256 {hashlib.sha256(data).hexdigest()})")
    if args.install:
        install(packaged, args.game, relative, data, workspace)


if __name__ == "__main__":
    main()
