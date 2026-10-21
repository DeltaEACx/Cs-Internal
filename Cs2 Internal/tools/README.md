Use `../build.ps1` to build the DLL or package a release, and `../Update-Build.cmd`
for game-build updates.

Updater staging includes the DLL and embedded GUI build inputs, including the
checked-in Rust vendor patch. Generated frontend and Rust output folders are
rebuilt in staging and are excluded from its source hash.

- `assets/asset_tool.*`: package verification, installation, and portable assets.
- `assets/agent_thumbnails.*`: render agent thumbnail images.
- `assets/generate_image_finishes.py`: generate the eight camo textures; `--install`
  also installs them into CS2's custom texture directory.
- `updater/`: native binding decoder, updater scripts, configuration, build recipes,
  and the `pe_audit.cs` helper used to validate installed DLLs.
- `development/organize_project.py`: synchronize Visual Studio's folder view.

Native tools build into `../bin/x64/Release/tools`. Texture compiler scratch files
stay in the ignored `../.asset-build` folder. The root `Update-Build.cmd` launches
the updater; `tools/update_build.ps1` remains a compatibility entry point.

Recipe seeding requires an explicit fresh report; it no longer loads an archived
research report by default. Build `updater/binding_analyzer.vcxproj` first, then
run this from the repository root with the report path:

```powershell
powershell -NoProfile -File tools/updater/seed_build_recipes.ps1 -ReportPath "path\to\fresh-report.json"
```

The report's module hashes must match the installed DLLs before any recipes are
written.

The current audit is `updater/bindings-audit.json` (2026-10-21, client version
2000934). It includes auxiliary functions, vtable slots and global references;
recipe seeding reads these from the report instead of reusing historical RVAs.
After building the analyzer, reproduce its recipes from the project directory:

```powershell
powershell -NoProfile -File tools/updater/seed_build_recipes.ps1 -ReportPath tools/updater/bindings-audit.json
```

The updater accepts integer schema fields and ExitScam `{ offset, type }` fields.
It validates runtime-initialized build-number storage against `steam.inf`, derives
the game-rules global from its validated signature, and preserves the curated
headers. An interface omitted by a dump is recovered only while its audited
module hash and singleton factory still match. `build.ps1 -Test` also runs
`tests/updater_regression.ps1`.
