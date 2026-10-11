`hvh_hitboxes_regression.vcxproj` links the production `combat_hitboxes.cpp`.
Its native-memory fixture deliberately supplies schema offsets different from the
installed client and bounds every readable fixture region.

The fixture exercises empty mesh 0 followed by populated mesh 1, per-mesh remap
starts, shared storage, duplicate suppression, and model bone-name fallback for a
custom head at native index 14. It verifies the allocated set layout, native
hitbox field offsets, array ordinals independent of resource hashes, and the
20-entry output bound. Missing schemas, zero allocation flags, misleading legacy
pointers, malformed counts, invalid geometry, unreadable or overflowing spans,
invalid bone-name maps, and a model handle changing during the read reject.

The bone count comes from the native CModel field at `+0x178`. ModelState `+0x88`
is a second pointer, so its upper 32 bits at `+0x8C` must never become a count.
The fixture covers upper values 1006, 65536, and UINT32_MAX independently of the
model count.

`visual_bones_regression.vcxproj` separately runs the production pose reader and
`hvh_points::Collect`. The pose reader takes the smaller of the uint16 live cache
count at ModelState `+0x94` and the CModel count; it rejects missing or invalid
counts. Tests cover a shorter live cache, a shorter model, zero and oversized
counts, and adversarial high metadata bits next to the uint16 count. Point tests
cover native bone 73 without indexing the 27-slot anatomical array, quaternion
rotation, translation-only hitboxes, malformed metadata, custom-head mapping,
valid projected fallback points, deduplication, and missing poses producing no
points. The fallback supplies existing bone positions; native tracing must still
confirm that a shot reaches the enemy pawn.

The same visual fixture covers optional interior multipoints: a center and six
capsule/box samples, independent shape containment checks, custom native hitgroups,
bone quaternion rotation, translation-only shapes, point scale bounds, degenerate
deduplication and metadata-free fallbacks retaining centers only. Geometry uses
the caller's existing pre-scaled hitbox extent convention.

The same visual fixture exercises the production typed boolean-convar reader.
It covers false values with dirty or NaN union padding, true values under the
game's denormal floating-point mode, native nonzero-byte semantics, wrong value
types, missing/unreadable objects, bounded cyclic traversal and registry-address
overflow. The public reader rejects unverified clients and keeps an unavailable
value distinct from a known false value.
