Launch CS2 with `-allow_third_party_software` enabled in its properties.

The **DeltaEAC** menu uses translucent charcoal panels, muted red accents, a
DeltaEAC wordmark, and a subtle mountain background. Navigation is text only:
Combat, Movement, Players, Local, Skins, World, Effects, and View. The character preview stays
docked beside the settings. Long explanations appear on hover.

The embedded menu and native features initialize concurrently. Settings unlock
when both are ready, with short polling during loading. Module checks reuse a
verified SHA-256 digest while the loaded module and file identity are unchanged;
function-entry checks and signature ambiguity checks still run. Visual hooks
enable as one batch to avoid pausing process threads separately for each hook.

The sidebar uses flat text tabs with a red underline and no shortcut footer.
Skins uses larger thumbnail tiles, compact search/filter controls, and a finish
editor beside the gallery. In narrow windows the editor stacks above the gallery.

Throwable has separate Flashbang, HE Grenade, Smoke, Molotov, Decoy, and
Incendiary subtabs with independent trajectory styles. Burning ground outlines
are shared by Molotov and Incendiary. Weapon Camo and Images use the same
existing weapon finish system. Existing feature settings keep their native IDs.

The shared C++ build settings explicitly pass `/FS` to synchronize compiler
PDB writes. `build.ps1` uses `obj/command-line` for its intermediate files,
keeping them separate from Visual Studio builds. Regression projects that link
production objects use this same script-build directory when run with `-Test`.

The repository is grouped by purpose:

```text
src/
  core/                  Runtime paths and shared infrastructure
  features/
    cosmetics/           Equipped skins and lobby custom models
    combat/              Compact Spectral HVH adapter and targeting math
    movement/            Spectral movement policies and native command adapter
    view/                Camera and view settings
    visuals/             Drawing, image finishes, effects and materials
  game/                  Catalogs, model/image registries and SDK bindings
  platform/              Windows integration
  render/                3D preview renderer
  ui/                    Overlay and catalog screens
tools/
  assets/                Asset packaging, thumbnails and camo image generation
  updater/               Binding analysis and build update configuration
  development/           Visual Studio folder synchronization
tests/                   Regression projects and fixtures
third_party/             External libraries and licenses
cs2_assets/
  custom_images/         Editable camo PNGs and texture compiler sources
  custom_models/         Existing custom model previews and sources
  game_assets/           Item catalog and catalog images
  game_files/            Compiled custom resources and their manifest
  preview_models/        GLB models and preview textures
build/                   Shared MSBuild settings
DLL/                     Built DLL
bin/, obj/, dist/        Generated executables, intermediates and release ZIPs
```

Visual Studio's source folders mirror the physical source tree. The solution is
one directory above this repository. Build from this folder:

Build with `powershell -File build.ps1`; the DLL is saved in `DLL/DeltaEAC.dll`. Build outputs stay local and are excluded from Git.

For another PC, share the ZIP produced by `powershell -File build.ps1 -Package`. Extract the whole ZIP and run `Setup.cmd`. Keep the extracted folder; run setup again if you move it. The DLL needs the included catalog, previews, and custom resources.

To check the extracted files, run `AssetTool.exe --verify`. If Steam detection fails, run `AssetTool.exe --setup "D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\csgo"` with the actual game folder.

For camo, open **Weapons > Image finishes**, enable **Image finish on your gun**,
turn off **Use the enabled custom skybox**, and choose **Image / camo**. Presets:
Woodland, Desert, Arctic, Urban Digital, Woodland Digital, Desert Digital,
Tiger Stripe, and Midnight Digital. These images cover the selected skin on the
existing weapon model. Disabling the image finish shows the selected skin again.
The same image material is used on recognized lobby weapon previews, without
requiring a spawned player. It affects this client's rendering; other lobby
members do not receive the custom appearance.

The camo textures ship in release packages and are installed by `Setup.cmd`.
For a development checkout, rebuild and install them with
`py -3 tools/assets/generate_image_finishes.py --install`. Pass `--game` with the
CS2 `game` directory for another Steam library. Python/Pillow and CS2 Workshop
Tools are needed only to regenerate textures. PNG and `.vtex` sources live in
`cs2_assets/custom_images/camo/source/materials/custom/camo`.

Run `Update-Build.cmd` to use the existing build updater. Its configuration and
recipes now live under `tools/updater`; the former `tools/update_build.ps1`
entry point still forwards to it. Run `py -3 tools/development/organize_project.py`
after changing the main project's source list to synchronize its folder view.

Custom character models use anatomical bone names for player ESP and local head attachments. The Combat page contains the compact HVH adapter described below. Restart CS2 after rebuilding so the loaded DLL uses the current code.

The main build also synchronizes `bin/x64/Release/DeltaEAC.dll` with the
current `DLL/DeltaEAC.dll`, preventing the former output path from
continuing to serve an older build.

Run `powershell -File build.ps1 -Test` for the cosmetic, visual, bone mapping, catalog, runtime path, camera, penetration and HVH regression projects.

**Camera > Camera & screen > Use third-person camera** uses a local view override
with a 35–200 unit distance slider and native wall collision. The previous console
camera command was cleared by the game's `sv_cheats` check in normal matches.
The new path obtains fresh mouse-view angles, moves the camera behind the native
shoot eye, and enables local-body visibility after the native view calculation.
It does not alter server settings, aim angles or shooting commands. Disable,
death, input replacement and map shutdown restore the owned camera state; the
requested setting remains selected for respawn/new maps. Camera regression
tests cover native calls, ownership and reset behavior.

The built-in visual preset uses a purple Rain Night sky, cyan lighting, light
purple fog and restrained bloom. Enemy chams are lit cyan in sight and purple
through walls; the first-person weapon uses a slow cyan/purple Pearl finish.
Matching ESP colors and head markers keep targets readable without skeleton
lines over the custom model. All settings remain editable in the World and
Players submenus; teammates and particle auras are off by default.

The reference-based **silver casual** character is selected for enemies and
both local-player teams when its complete resource bundle is installed and the
selection has not already been changed. It uses the usual model-loading path
when entering a game. For a different local selection, use **Skins > Custom
agents > Refresh**, choose CT or T, and press **Equip agent**. For enemies, use
**Players > Enemy models**. Disabling or changing a selection is respected.
Editable Blender/GLB sources, native compiler inputs and renders are under
`cs2_assets/custom_characters/silver_casual`.

Open **Players > Chams > Particle chams / body aura** to enable orbiting native
particle emitters around living players' head, chest and hips. Choose enemies,
teammates or your player, then select sparks, electric glow, arcs or star glimmer.
The effect's resource defines its colors. Radius, orbit speed, burst interval,
distance and player limit are adjustable. Local effects are easiest to see in
third person. The status reports active/pending emitters or unavailable bindings.
These controls work independently of the existing material-chams settings.
Effects are cleaned up on disable, death, dormancy, model changes and map/round
changes. `build.ps1 -Test` includes native particle ownership and lifecycle tests.

Open **HUD > Crosshair & scope > Autowall** and enable **Autowall surface crosshair**.
The visible camera's center ray finds the first surface and places a square in
its plane, including walls, floors, ceilings and slopes. **Surface square size**
adjusts its side length from 2 to 64 game units; perspective changes its apparent
screen size. Green means the current firearm's native bullet simulation exits
that surface with damage remaining, red means blocked, and amber means the
result is unavailable. Clear space has no surface marker. The menu reports the
reason and damage remaining after a successful penetration; this is not a
prediction of damage to a player behind further surfaces or armor.

The probe runs after the native game frame stage. Rendering projects a recent
snapshot and clips the square at the camera and viewport boundaries. It works
independently of the ESP master switch. In third person, the camera ray places
the marker while the bullet starts at the player's eye; different first surfaces
produce amber instead of claiming a penetration. Native trace storage is
released on disable/map shutdown and must finish cleanup before DLL unloading.
`build.ps1 -Test` includes native Autowall state and surface-square geometry tests.

Open **World > Penetrable surfaces** to highlight sampled wall, floor and prop
areas that the equipped firearm can penetrate. This extends Spectral's visible
surface scanner with surface-conforming geometry rather than its screen tiles.
It is independent of player chams and the Autowall crosshair. Choose walls/static
geometry, props/doors/breakables, color/opacity, distance, minimum remaining
damage and scan detail. The default is disabled. The frame-time budget and probe
limit bound the work between native queries; one native query cannot be preempted.

Each filled patch requires a passing native penetration result at all four
corners and its center, matching physics object/shape identity, surface plane
and current view/shot origin. Coverage fills progressively across the view.
Blocked, unknown, expired or mismatched samples remain uncolored. Player and
weapon geometry is excluded. Weapon changes, full entity handles, map changes,
eye/camera motion and prop transforms invalidate old coverage. No entire mesh is
declared penetrable from a single ray. Small details between samples can still
be missed; this sampled overlay does not guarantee a hit on a target behind a
surface. Higher detail resolves smaller areas at a greater scanning cost.

The native query uses the verified shoot-position path and current projection
matrix, and retains the existing trace-storage cleanup rules. Regression suites
cover the native probe, perspective/plane/edge checks, sample freshness, scan
budgets, filters and lifecycle. Runtime performance and appearance still require
testing in a loaded game.

Open **Combat > Rage**, use **Enable ragebot**, and choose **Hold key** or
**Always** (the captured default). Disabling the master stays disabled when the
menu refreshes or another setting changes.
Both modes require the menu closed and CS2 focused; Always needs no bound key.
HVH starts enabled with head-only targeting,
180-degree aim FOV, 25 minimum damage, 70% estimated hit chance, multipoints,
automatic stop/scope, automatic fire, wall penetration,
recoil correction and supported no-spread compensation.
Head-only selects head points without falling back to body points; this does
not guarantee every bullet becomes a headshot. The **Targeting** submenu
defaults to **Head** and also offers **Head and body**, **Body**, and **All
hitboxes**, plus aim FOV, minimum damage, multipoint scale and wall penetration.
Minimum damage uses the first intersected native hitbox's hitgroup and armor
scaling; lethal damage can qualify below the configured threshold.
The **Firing** submenu controls automatic fire, recoil correction, estimated
hit chance, auto stop, auto scope and no-spread.
When automatic fire is off, hold primary attack; Hold key mode also requires
the activation key.
The adapter tries current hitbox centers before interior points and recorded
poses, sharing the scan fairly between enemies. It tests native bullet
reachability and qualifies ordinary spread with up to 128 native samples
shared across the command; impossible accuracy checks stop early. Each command
has a shared 48-trace and 16-accuracy-check budget, including one fresh-pose retry.
Qualified historical/penetrated alternatives rank by lethal damage and estimated
probability, then damage weighted by probability. A clear live shot fires as soon
as it passes the configured damage and hit chance; it does not wait for further
point comparisons. The angular FOV
works outside the visible viewport; 180 degrees includes rear targets.
Auto scope supports scoped rifles and starts after damage qualification,
before an unnecessary unscoped accuracy scan. Auto stop countersteers grounded
movement immediately when a clear live target fails the accuracy gate and can continue
preparing during cooldown from a briefly retained acquisition. That acquisition
authorizes braking only; every shot needs fresh qualification. Held primary
attack is preserved, and jump, interaction, secondary attack and committed
shots keep priority. It does not implement the full legacy prediction pipeline.
Native model bone indices support both stock and custom agents. Recoil is
removed from the shot command. The native HandleViewAngles hook restores the
visible camera after it consumes a committed silent command. The separate
Autowall surface square keeps its existing toggle and size control.

Enemy discovery scans ordinary slots directly and incrementally discovers
controllers in allocated higher entity pages, independently of ESP rendering.
It validates current controller and pawn serials, resets after disconnects and
command/list changes, and detects reused entity pages. Level shutdown clears
target discovery, committed-command markers and movement state before the next
input command. Native hitgroups identify custom models with reordered hitboxes;
named bones provide fallback aim points without guessing a stock bone index,
but shooting qualification still requires native hitbox geometry. Every candidate
requires a native trace that reaches that exact pawn. Live model, simulation
and bone data must still match before committing; a changed pose can be
recaptured once within the same work budget. Repeated failed scans reuse only
unchanged commands with the same eligible enemies and geometry. Missing native
data remains retryable. History recording reuses unchanged simulation snapshots
and advances discovery per local tick instead of copying poses per render frame.

Estimated hit chance checks target geometry for spread rays. Wall travel is
checked for the selected aim ray, so a narrow opening can have a lower actual
hit rate than the displayed setting suggests. Setting hit chance to zero
disables that gate. Missing accuracy or spread data cannot qualify a shot when
the gate is enabled. Live server randomness and prediction still affect results.

Spectral's spread correction uses the game's angle/tick seed and native weapon
spread sample, preserving correction roll and the selected shot timestamp in
input history. The live scan uses a bounded iterative solve and requires exact
agreement between the sampled and corrected native seeds. If a solve cannot be
verified, ordinary spread must satisfy the configured accuracy threshold.
Correction is used only for single-bullet weapons when the server's
`sv_usercmd_custom_random_seed` boolean is explicitly readable as disabled.
The reader validates its native type and reads one byte, so union padding and
the game's floating-point denormal settings cannot change that decision.
Default servers add a private seed, so the adapter retains ordinary spread
there and reports this limitation. Unknown server state also keeps ordinary
spread. Airborne hits are not guaranteed, and a shotgun's pellet pattern cannot
be removed by one angle correction. The R8 cocking sequence is not included.

Shot angles and penetration checks use the native shooting position for the
selected history timestamp. The native helper applies the game's interpolation
adjustment internally. The same original timestamp selects the spread seed and
is written into the attack history, including when spread correction is inactive.

HVH uses the audited CreateMove hook and transactional command/history/button
edits with movement correction and native checksum serialization. A repeated
native overwrite pass preserves only a command this instance successfully
committed, matching its address, sequence and complete local-player handle.
Fresh or unrelated commands and pending weapon switches follow the original
input function. Already committed shots can finish after key release; the
selected activation mode gates new shots. Failed validation leaves the command alone.
Disabling HVH, releasing its key in Hold mode, opening
the menu or changing focus stops new edits. Native trace storage is shared with
the surface probe and released during shutdown after input callbacks drain.

**Combat > Anti Aim** is a separate page with **Activation** and **Angles**
submenus. Enable it to use **Away from target** yaw or **Spin** with
adjustable spin speed and yaw offset; pitch can follow the view, point down or
point up. The captured defaults enable backward yaw and down pitch. It runs
while enabled, or optionally follows the HVH activation mode.
Spin follows game ticks rather than render frames, and the same native camera
restoration keeps the view steady. Movement is corrected to preserve WASD
direction. Validated HVH shots keep their complete shooting history (including
spread roll and timing), while anti aim changes only body/base angles. A held
manual trigger allows anti aim during firearm cooldown or reload; a ready manual
shot keeps native shooting angles. Anti aim pauses for
interaction, unsafe movement states and grenade throws, and does not require a
ready-to-fire weapon. Its command edits leave shot history untouched.

**Movement > Assists** contains Spectral's nine movement assists in **Jumping**,
**Air**, and **Ground & ladders** submenus: bunnyhop, jumpbug,
edgebug, air strafe, quantized air strafe, slow walk, edge jump, edge stop and
fast ladder. Enable the movement master and the desired assists. Each has an
optional hold key; clearing the key with Escape selects Always. Hold jump for
bunnyhop. The captured defaults enable master, bunnyhop and directional air
strafe; the other assists remain individually selectable. Quantized strafe
requires the server's quantized movement setting;
server autobunnyhop takes priority over the bunnyhop helper. Slow-walk speed,
directional air movement and edgebug modes/windows are configurable.
Fresh keyboard and wheel jump presses take priority over landing assists and
airborne bunnyhop suppression. This preserves the first takeoff command when
prediction has already changed the live player flags to airborne. Repeated
native jump presses during a confirmed continuous Space hold do not block the
descending release and the next landing pulse. Releasing Space ends the cycle.

The port uses the installed client's player-hull trace/filter and native
subtick allocator, rather than the incompatible legacy hull-call signature.
The shared input hook initializes independently of bullet/penetration bindings,
and movement physics reads use their own hash-verified client readiness.
An unrelated skin or shooting initialization failure therefore does not prevent
movement from running. The optional command-tick fallback reads the field from
its declaring base controller class. The movement master switch retains its
state across menu refreshes and stops ordinary assists when disabled.
Only movement buttons, analog inputs and required movement angles/subticks are
edited; native attack inputs and all shot histories are preserved. Shooting and
interactions take priority over assists, and missing physics/trace data skips
the dependent assist. State resets on menu/focus changes, death and reconnect.
Fast ladder and quantized strafe use the existing camera-preservation hook.
The regression build includes portable movement policies and native adapter,
subtick storage growth/rollback, shot coexistence and reconnect cases.
