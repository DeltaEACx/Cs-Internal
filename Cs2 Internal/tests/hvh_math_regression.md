`hvh_math_regression.vcxproj` tests the standalone, header-only
`features/combat/detail/hvh_math.hpp` helper without native engine callbacks.

The DeltaEAC accuracy policy exercises 128 native-sample callbacks against an
independently computed small target sphere. A moving spread fixture produces
62.5% estimated hits and fails an 80% threshold; zero spread passes 100%.
Range, physical shot angles, unavailable geometry/accuracy, failed or malformed
spread samples (including a failure on the last sample), threshold bounds and
sample-count bounds are covered. This validates the qualification policy and
does not establish live server hit outcomes.

An explicit all-direction policy permits a rear target at a 180-degree angular
FOV and rejects it at 90 degrees. Existing viewport-constrained projection tests
continue to cover the default screen-ranking policy.

Qualified-shot ranking separately prefers lethal shots, higher confidence among
lethal choices and probability-weighted damage among nonlethal choices. Tests
cover an easier lethal target farther from the crosshair, clear-path and angular
ties, deterministic ordering and rejection of malformed quality inputs. Ranking
cannot qualify a target that failed reachability, damage or accuracy gates.

First-impact confidence uses independent head and torso sphere intersections.
A mixed spread fixture hits the pawn on every sample but hits its head on only
half: the production group-specific policy reports 50% head confidence and
rejects a 70% threshold. A separate accurately aimed torso candidate passes its
own group. Clear misses remain valid zero-confidence samples; unavailable or
unsupported impact metadata cannot qualify a damage claim.

`ProjectCandidate(view, candidate, index, false)` qualifies enemy identity,
health, dormancy/immunity, weapon range, front-facing geometry and optional
viewport bounds before penetration tracing. Sort the returned selections with
`Closer` and trace them in that order. With the default `requireHittable=true`,
the helper additionally requires a fresh positive damage result meeting
`View.minDamage`. `Select` uses this stricter mode. A supplied projection matrix
ranks squared pixel distance from screen center; otherwise ranking uses spherical
angular distance. Damage only breaks an equal-distance tie.

The native caller must populate `Candidate.valid` only after validating the full
entity handle. Candidate points must be transformed with their native hitbox
bone, rather than indexing the visual anatomy cache with a native bone index.

`FindSpreadCorrection` reproduces Spectral's bounded half-degree seed search.
Its callbacks return optional native angle/tick seeds and native spread samples.
The native sampler applies its own `seed + 1` convention and must allocate enough
output storage for all pellets. The helper verifies the final corrected angle's
seed and its resulting first-pellet direction. A zero angle/seed is valid; missing
callbacks, nonfinite samples, seed mismatches and pitch corrections requiring
clamping fail. The command writer must preserve the solved roll. One correction
aligns the sampled pellet; it does not collapse a multi-pellet weapon's spread.

`ShotAim(desired,punch,recoilCorrection)` chooses the physical shot direction
before spread solving. With recoil correction disabled, it adds native punch so
the normal recoil effect remains. `RemovePunch(corrected,punch)` always removes
native punch from the solved physical angles before command writing, preserving
the solved roll. This keeps the seed search consistent with the server's
command-plus-punch direction for either recoil setting. Tests reproduce the old
disabled-recoil seed mismatch, verify both toggle combinations with an independent
pellet-direction check, and cover yaw seams, steep ordinary shots, overflow and
pitch rejection. Callers can retain the original selected angles directly for an
ordinary shot with both corrections disabled to avoid a float round-trip.

Coverage includes nearest crosshair versus high/lethal damage, blocked-nearest
fallback, range/FOV/viewport bounds, screen aspect ratio, yaw wrapping, full
identity/health/team/dormancy/immunity filters, invalid inputs, deterministic ties,
geometry-only sorting, an independent Euler-rotation check of corrected pellet
directions and command-roll loss, native seed self-consistency, bounded searches,
missing/nonfinite samples, and valid zero-angle results.

The same project also tests `detail/hvh_antiaim.hpp`. `Angles(view,commandTick,
options,tickInterval,targetYaw)` returns bounded command angles without modifying camera
input. Backward turns the player around: 180 deg away from the threat bearing when
a living enemy is known, otherwise 180 deg away from the visible camera so the head
stays hidden from anyone in front; Spin adds a phase calculated from
native command time, with configurable speed and offset. Keep/Down/Up pitch modes
retain a bounded view pitch or choose +89/-89 degrees (Down looks at the floor and
hides the head behind the torso from the front). Repeated calls and skipped
callbacks cannot advance or reset phase; 64/128 Hz inputs representing the same
elapsed time agree. Tests cover these properties, configuration endpoints, yaw
seams, large ticks, finite bounds and invalid values. Runtime activation,
shot/use/movement gates, command writing, movement correction and camera
preservation remain the caller's responsibility.
