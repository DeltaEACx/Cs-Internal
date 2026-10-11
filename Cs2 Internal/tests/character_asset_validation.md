Silver Casual validation uses the actual preview loader, donor animation transfer,
D3D11 renderer on WARP, WIC PNG encoder and decoder, and character catalog.
It does not mutate input GLBs or install game resources.

Run these from the `Cs2 Internal` project directory after the final mesh and native
compiler output are ready:

```powershell
py -3 tools/assets/package_silver_casual.py --compiled-root .asset-build/silver_casual/game/csgo
# Explicitly publish the validated resources, preserving the embedded Blender source.
py -3 tools/assets/package_silver_casual.py --compiled-root .asset-build/silver_casual/game/csgo --apply

bin/x64/Release/tests/character_asset_validation.exe `
  cs2_assets/preview_models/models_custom_agents_silver_casual_silver_casual_vmdl.glb `
  cs2_assets/preview_models/agents_models_ctm_sas_ctm_sas_vmdl.glb `
  tests/artifacts/silver_casual-final `
  cs2_assets/preview_models/thumbnails/models_custom_agents_silver_casual_silver_casual_vmdl.png

py -3 tests/character_asset_packaging_regression.py -v
```

The native check loads rifle, pistol and knife donor poses, checks both hands,
finite skinned geometry, material texture paths and triangle indices, and renders
front/side/back views plus the actual rifle/pistol/knife attachment. All twelve PNGs must contain visible geometry and decode to
the same RGBA pixels. The optional thumbnail path requires every mesh part to
have its material texture. These renders expose visual deformation for review;
the assertions do not claim that a procedural likeness matches a reference photo.

The packaging regression uses synthetic compiled resource bytes to test publishing
semantics. It checks unchanged source and mesh buffers, normalized weights,
idempotence, retained unrelated entries, failure before writes for incomplete
resources, and required package entries/hash verification. Native resource validity
is assessed by ResourceCompiler and the separate asset geometry QA.

After AssetTool creates the portable package and the approved install completes:

```powershell
py -3 tools/assets/package_silver_casual.py --verify-package <portable-package-folder>
bin/x64/Release/tests/character_asset_validation.exe --discover <installed-game/csgo>
```

Discovery calls the same production catalog used by both local and enemy selectors.
Select Silver Casual separately for local CT/T when both teams should use it;
enemy replacement also requires its Enable control.
