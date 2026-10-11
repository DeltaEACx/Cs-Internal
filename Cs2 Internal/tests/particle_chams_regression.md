The fixture compiles the actual particle-chams planner and particle backend.
It supplies memory-backed native pawn/controller/entity-list/model data, schema
lookup offsets and mapped skeleton snapshots. Native create, CP0, destroy and
resource-load callbacks check their exact existing ABI and track owned handles.
The clock is replaced only in the test translation unit so scheduling needs no
wall-clock delays. The separate visual-bones regression checks real model-aware
skeleton mapping; this fixture checks how those mapped positions drive effects.

Coverage:

- Enabled body aura with the world-effects master off and material chams absent.
- Enemy, teammate and local selection, nearest-player limit and at most 18 emitters.
- Per-frame native CP0 movement and orbital placement without effect recreation.
- Missing/nonfinite skeleton anchors and bounded/nonfinite settings.
- Missing installed resources, failed CP0 initialization and later recovery.
- Disable, death, dormancy, changed model resource, changed full entity serial,
  round transition, native level shutdown and overlay shutdown.
- Reserved kill-effect slots remain unused by the aura.
- Failed native destruction retains ownership and retries. Cancellation persists
  across a failed destroy so unprocessed siblings cannot revive after a model swap.

The fixture validates planning, ABI and cleanup; it does not replace a visual
check of each authored VPCF in the running game. Colors come from those resources,
and the UI describes the feature as a body aura rather than a mesh replacement.

Run `build.ps1 -Test`, or build `tests/particle_chams_regression.vcxproj` and run
`bin/x64/Release/tests/particle_chams_regression.exe`.
