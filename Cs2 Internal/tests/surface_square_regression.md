`surface_square_regression.vcxproj` builds a standalone native test of the two
header-only helpers in `features/visuals/detail/`.

`surface_square::Build` accepts finite hit position, surface normal, incoming aim
direction and side length in the inclusive range 0.25–128 world units. It returns
four consistently wound corners, displaced 0.05 units from the surface toward the
incoming ray. Walls use world vertical; nearly horizontal planes use the aim
heading, with a deterministic world-Y fallback for nearly vertical rays. Invalid
inputs and coordinates whose float precision erases the square or offset fail
without changing the output.

`surface_square::Project` transforms using matrix rows 0, 1 and 3, clips against
`w >= 0.01`, `-w <= x <= w` and `-w <= y <= w`, then maps to screen coordinates.
It returns `Polygon.points[0..count)` and leaves the output unchanged on failure.
It makes no assumption about the engine's clip-space Z convention.

Coverage includes positive and negative axis walls, floors, ceilings, slopes,
normal/ray scale invariance, winding, side lengths, diagonals, surface offset,
near-horizontal normal jitter, vertical-ray fallback, invalid and unrepresentable
coordinates, perspective and orthographic projection, partial near-plane and
viewport clipping, an eight-vertex clipped quad, exact near-plane boundaries,
fully hidden/degenerate polygons, and invalid matrix/screen dimensions.
