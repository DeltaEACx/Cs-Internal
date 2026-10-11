# Native movement regression

Build `tests/movement_native_regression.vcxproj` in Release x64 and run `bin/x64/Release/tests/movement_native_regression.exe`.

The fixture compiles the actual movement context and runtime with memory-backed entities and callable native boundaries. It verifies current movement services and schema capture, full command identity/freshness, raw/protobuf/subtick manual action guards, current hull/filter ABI and collision mask, endpoint/normal layout, valid clear traces and failure containment, engine-owned subtick allocation/reuse, and missing prerequisites.

Runtime checks cover exact intact-command replay, changed options/base/step payloads, native recycling of a movement message into an unrelated reload event, ownership preservation after the caller's base-only anti-aim pass, menu transition resets and hold-key capture across repeated menu frames. Physics convars are refreshed at most every 250ms per local identity; missing values retry and session resets invalidate the cache.

The HVH command regression additionally verifies empty native subtick fields, chronological merging with existing late manual events, selective replacement of owned jump/duck and analog components, mixed-button preservation, repeated/shrinking/growing movement windows, retained vector allocation, write/CRC rollback, and exact shot-history preservation during combined shot/base-only orientation commits.

Fixture success does not claim live gameplay testing of every assist.

The 2026-10-08 update pins the hull, filter, protobuf allocator and append entries
to the audited client hash and checks their first 24 bytes before use. The audit
independently resolves all five signatures and compares those results with
`MovementBindings.h`. Ordinary movement remains available when only hull tracing
is unavailable.

Public runtime coverage verifies Spectral's 32 analog subticks with zero base
analog, reconstructs the final vector from movement-service impulses and emitted
deltas, then applies backward anti-aim and checks preserved world movement and
intact-command replay. A fallback impulse added for slowwalk keeps its base
analog, matching the source's ordering of finalization and `input.apply`.
Held primary fire during cooldown, reload or an empty clip permits ordinary air
strafe; an actual shot still suppresses it. Released action bits do not suppress
movement. Ground movement continues alongside fire and preserves attack input.

Continuous Space coverage now confirms the binding through native Jump before
retaining physical held intent. Generated descending releases, native reassertion
of changed-and-held Jump, unchanged command bytes at a newly observed landing,
and transient weapon-selection gaps cannot erase that confirmed hold. Physical
release, focus/menu transitions and player/session changes clear it. Wheel jumps
and unowned native press events retain their manual priority.

Automatic quantized air strafe is tested through the public runtime with its
typed server convar enabled and no direction buttons. The native window contains
digital-compatible base input, one starting impulse and 16 yaw steps, plus a
camera-preservation stamp. Eight same-command mouse updates replace that window
at its original timing and anchor the correct current base angle. The caller's
subsequent backward anti-aim pass preserves movement and replay ownership.
