"""Regenerate Visual Studio's folder view from the current source list."""
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def main():
    project = ROOT / "Cs2 Internal.vcxproj"
    ns = {"ms": "http://schemas.microsoft.com/developer/msbuild/2003"}
    sources = [(kind, item.attrib["Include"]) for kind in ["ClCompile", "ClInclude"]
               for item in ET.parse(project).findall(f".//ms:{kind}", ns)]
    folders = set()
    for _, filename in sources:
        parts = filename.split("\\")[:-1]
        folders.update("\\".join(parts[:i]) for i in range(1, len(parts) + 1))
    lines = ['<?xml version="1.0" encoding="utf-8"?>',
             '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">', '  <ItemGroup>']
    lines += [f'    <Filter Include="{folder}" />' for folder in sorted(folders)]
    lines += ['  </ItemGroup>', '  <ItemGroup>']
    for kind, filename in sources:
        folder = filename.rsplit("\\", 1)[0]
        lines.append(f'    <{kind} Include="{filename}"><Filter>{folder}</Filter></{kind}>')
    lines += ['  </ItemGroup>', '</Project>']
    project.with_suffix(".vcxproj.filters").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
