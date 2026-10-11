`hvh_command_regression.vcxproj` links the production `combat_command.cpp` and
exercises its optional `Shot` transaction through a bounded native-memory fixture.
The fixture has three full 0x68-byte history payloads, separate angle messages,
native/protobuf button states, analog subticks, interpolation objects, and a camera
sentinel. Native user-command sequence 77 is valid while legacy command number is
left zero.

Coverage verifies distinct base and shot angles, unrolled base CRC data, rolled
history angles, player tick/fraction stamps and presence bits in every entry,
untouched render ticks/interpolation/camera data, last-history attack selection,
button state agreement, and movement rotation using base yaw. Invalid, empty,
truncated or overlapping history data, stale command/base state, missing shot
prerequisites, nonfinite angles/fractions and unusable pitch/tick ranges reject
before provider writes. Every staged write is failed after a partial copy, then
checked for exact byte restoration. CRC failure also restores the entire command
and rebuilds the previous serialized base/button checksum. Shared angle objects
and a fraction immediately below one remain valid.

The production `hvh_input::SilentCommand` policy also drives a simulated native
overwrite callback after a real shot transaction. A repeated overwrite marker
preserves the committed bytes; fresh sequences, reused ring slots, changed entity
serials, missing commits and explicitly cleared ownership still run the native angle
rewrite. This verifies command preservation policy, not live hook scheduling.

The provider is deliberately stubbed: native CRC allocation and its tagged
destination postcondition belong to the separate engine-access regression.


Movement coverage includes empty native repeated fields (including retained inactive storage), stable chronological merging with late native events, selective replacement of analog and owned Jump/Duck/WASD bits, mixed manual actions, engine-owned vector growth, reused movement windows on native overwrite passes, active/inactive pointer uniqueness, shrink/grow reuse, and full rollback of prior count/order/payloads on partial writes or CRC failure. Base-only orientation preserves protobuf-only manual actions independently of raw buttons, and a committed shot retains exact history timestamps, spread roll, attack index and action flags after orientation.
