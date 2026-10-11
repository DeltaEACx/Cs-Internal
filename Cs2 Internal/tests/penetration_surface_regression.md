`penetration_surface_regression.vcxproj` compiles the production portable header
`features/visuals/detail/penetration_surface.hpp`. It does not invoke a game
process, scene hook, command editor, or approximate a native penetration result.

`MakeCamera`, `Direction`, and `Project` derive the camera origin and screen rays
from the view matrix's clip X/Y/W rows (0/1/3). This supports camera rotation,
translation, off-axis projection and either depth convention. Perspective
projection rejects clip W below .01; polygon clipping remains the renderer's
responsibility. NDC X/Y are in [-1,1], positive Y at the top.

CS2's native clip X/Y/W camera basis has negative handedness. The archived
read-only matrix in `.asset-build/penetration-camera-handedness-live.json`
has determinant -1.11111117; its valid TL/TR/BR/BL world winding runs opposite
to the original positive-handed fixture. Convexity checks accept either
consistent winding while retaining screen-corner order, a camera-facing normal,
and the same rejection of folds, concavity and an outside center. The suite
reproduced a rejected five-pass native cell before this correction and now
covers canonical Source axes plus the captured oblique native matrix.

`BuildCell` consumes four adjacent grid corners in TL/TR/BR/BL order and an
independently traced center. All five must pass current-weapon penetration,
carry the same valid native physics object/shape/full owner handle/kind, have
consistent planes and facing normals, and remain fresh relative to the current
camera, eye and generation/weapon/ballistics key. Unknown or blocked samples
never fill an incomplete cell. The output contains four corner positions and
the native center position, offset .05 units toward the viewer. The caller draws
four triangles: points[i], points[(i+1)%4], points[4].

`Fresh` checks each source and current projection against the sample's actual
NDC, camera ray/origin change, shot-eye change and elapsed age. Runtime code must
increment the generation on map/local/weapon/stat/filter resets and validate
current full-serial prop identity plus model/transform before reusing a prop
sample. Physics tokens are opaque equality values and must never be dereferenced
by the policy. A material pointer is not a face or shape identity. World samples
without native physics object/shape tokens cannot be joined by this helper.

The default TTL is 150ms; the view-wide runtime may explicitly use 350ms for a
rolling scan, while retaining every geometry and pose gate. Grids of 12x8,
20x12 and 28x16 cells need 213, 513 and 941 unique corner/center probes.
`PlannedSamples` rejects empty grids or axes beyond 64, so allocation size is
bounded. Native probe budgets and the center-out scheduler belong to the caller.

Regression checks use an independent physical-camera ray constructor and ray /
plane intersections. They cover perspective origins/rays, rotated/off-axis
matrices, invalid matrices and copied poses, walls/floors/ceilings/slopes,
consistent triangle winding with both camera handednesses and surface offset, a thick center surrounded by thin
passing corners, material/prop boundaries and separate depths, missing/invalid
normals, partial refresh and TTL boundaries, local/weapon serials and ballistics,
camera motion/roll/zoom, shot-eye changes and bounded grid allocation. A shared
shape with two thin cells around one blocked cell produces exactly two patches,
not a whole-mesh verdict. The four fan triangles also exercise the existing
production homogeneous viewport projector.

The separate overlay regression exercises actual ImGui rendering from a
negative-handed camera. A five-pass cell produces geometry and a positive
rendered-patch diagnostic. Fresh but isolated passing stripes, different native
shape identities, blocked materials and replaced props still produce no filled
cell. This distinguishes native passing-hit counts from validated surface fills.

These checks establish sampled coverage. Geometry or a new occluder between
sample rays can still be missed; TTL alone cannot prove visibility while a
foreground object moves into a stationary view. Refreshing native samples and
validating dynamic prop transforms is required. The overlay does not prove a
whole wall/prop penetrable or guarantee damage to an enemy behind it.
