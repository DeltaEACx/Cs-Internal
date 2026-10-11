`movement_regression.vcxproj` compiles the production header
`features/movement/detail/movement_policy.hpp` without engine/native libraries.
Its `Input` is a captured current command and player movement snapshot;
`Options` contains already resolved feature toggles/hold binds. `Build` produces
bounded base movement, optional base angles, button masks and up to 64 subticks.
Apply masks as `(oldButtons & ~clearButtons) | setButtons`.

The policy implements bunnyhop, 32-step analog air strafe, slowwalk,
jumpbug, edgejump, edgestop, edgebug, fastladder and 16-step quantized test
strafer. Physics fields come from movement services and server convars.
Landing/edge policies require a callable native player-hull trace and validated
collision bounds; they do not approximate ground with a visual ray or silently
treat an unavailable trace as empty ground. The callback receives start, end,
mins and maxs and returns the actual trace end position, fraction and normal.
Each command can issue at most 64 hull requests; edge-stop lookahead is bounded.
No prediction simulation or live entity mutation occurs inside the policy.
Ordinary air strafe automatically uses digital movement and 16 yaw subticks
when the server enables `sv_quantize_movement_input`. It anchors those deltas
to the visible input angle and requests camera preservation. It works without
WASD, and directional steering follows held keys when enabled. Releasing WASD
continues to accelerate; only explicit Shift input requests air braking. Both
air paths use the effective movement-services max speed. The quantized path
integrates the actual remaining interval after preserved native events, and
can accelerate from rest. Generated steps do not constrain a replacement
window's start during same-command mouse updates.
Ground counter-movement uses a validated current weapon-data max speed when
available; absent/invalid weapon data falls back to the validated movement limit.
Slowwalk scales the requested analog vector by its input percentage independently
of either native speed limit.

Source adaptations preserve native analog range [-1,1], landing quantization,
duck expansion, half-gravity sweep, stairs scan, edge modes/passes and digital
direction preference. A successful landing window owns duck/jump timing and
suppresses conflicting ordinary bunnyhop/strafe. Jumpbug only creates that
window on its source's sufficiently flat active surface (normal.z >= .985);
the source otherwise emits duck/jump steps despite marking itself inactive.
Alternation uses native command number instead of a render/callback counter,
so repeated processing cannot advance strafe phase. Invalid physics or missing
native dependencies skip only their dependent feature. Server auto-bunnyhop
disables manual bunnyhop edits.

The caller must preserve existing attack/use and shot-history fields during the
transaction and verify command/local identity again before committing. Spectral
clears all native subticks first; this port retains button/history timing and
must neutralize existing analog deltas when replacing an analog movement path,
then install the planned deltas without applying the old impulses a second time.
When a feature supplies its own analog subtick window, the adapter clears base
forward/left after generating it, matching Spectral's final-subtick pass. The
fallback AG2 impulse for a base-only assist is added after that source pass and
keeps base analog. Quantized strafe also retains base analog.
Quantized yaw steps start after the greatest existing subtick fraction. Its yaw
steps and ladder base angles need visible-camera preservation. Set `shooting`
for a committed shot to suppress angle-changing ladder/test-strafer and air
edits; movement never owns primary/secondary attack or use bits. Reset
`DirectionState` on local-player/session changes.

`Input.jumpPressed` identifies an explicit fresh native/manual jump edge, from
the raw/PB changed-and-held bit, jump wheel input, or a pressed native jump
subtick. A fresh press takes priority over local bunnyhop, jumpbug, edgebug and
ground-edge automation even if current live flags already say airborne after
prediction. Analog-only air/slowwalk can still run without owning jump. The
adapter has no native pre-prediction snapshot: unlike the source, it conservatively
preserves held jump while vertical velocity is positive, because a landing hop
may already have been predicted rising without another hardware press. It clears
held jump at the apex/during descent and retains the landing release/press
schedule. If prediction already completed a landing or the descending trace
never supplied a usable airborne landing fraction, a grounded held jump with
no fresh manual edge and nonpositive vertical velocity rearms bunnyhop with
an explicit release at fraction 0 followed by a press at 1/64. This preserves
repeated held-Space jumps without intercepting the first fresh takeoff or a
hop already predicted rising. Releasing jump does not synthesize
another jump, including with optional edgebug jump steps. Server auto-bunnyhop
continues to own its native jump behavior.

Regression coverage verifies real planned hull coordinates and gravity, standing
hull expansion while ducked, bunnyhop release/press timing, chained edgebug duck
cycles and geometry modes, landing arbitration, two-tick cliff/stair detection,
incomplete/malformed trace rejection, friction/creep braking, diagonal speed
limits, ladder direction and camera/interaction gates, analog-delta consistency,
native timestamp bounds, command replay, shot/button coexistence, and explicit
fresh/held/released jump ownership across grounded/rising/falling states.
Combined-option checks use bunnyhop, jumpbug and edge-geometry edgebug together
with one duck window and edgebug jump steps disabled. They verify flat/edge
landing priority, missing collision windows, held-jump cycles through grounded
recovery, same-command replay, and final user-release/server-autobunnyhop guards.
Independent air-acceleration integration checks that both emitted strafe paths gain speed,
including 64 consecutive quantized ticks with alternating held/released direction
input, directional turns, and restarts from zero speed;
these checks validate policy behavior without claiming to execute native player
prediction or guarantee a server-specific landing exploit.
