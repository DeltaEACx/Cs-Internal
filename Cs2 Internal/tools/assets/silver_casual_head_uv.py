"""Coherent head, hair and skin UVs for the single Silver Casual atlas.

Call after the other character UVs are assigned:
    parts, report = apply_head_uv(parts, rig, material, atlas_image, replace_hair=True)

This changes only facial/skin UVs and optional hair geometry. It keeps the
existing atlas, material and native skeleton. No image or source asset is written.
"""
from __future__ import annotations

import math
import random
import bisect
from collections import deque
from typing import Iterable

import bpy
import numpy as np
from mathutils import Vector, kdtree
from mathutils.bvhtree import BVHTree

_CY = 0.07248011
_ATLAS_WIDTH = 1536.0
_ATLAS_HEIGHT = 1024.0


def _atlas_pixels(image):
    width, height = image.size
    if width < 1 or height < 1:
        raise ValueError('Silver Casual atlas image is not loaded.')
    pixels = np.empty(width * height * 4, dtype=np.float32)
    image.pixels.foreach_get(pixels)
    return pixels.reshape(height, width, 4)[::-1, :, :3], width, height


class _SkinSamples:
    """Foreground pixels with a three-pixel guard against the gray backdrop."""

    def __init__(self, pixels, width, height, rectangle, facial_interior=False, hair=False):
        sx, sy = width / _ATLAS_WIDTH, height / _ATLAS_HEIGHT
        x0, y0, x1, y1 = (int(round(v * (sx if i % 2 == 0 else sy)))
                          for i, v in enumerate(rectangle))
        crop = pixels[y0:y1, x0:x1]
        red, green, blue = crop[:, :, 0], crop[:, :, 1], crop[:, :, 2]
        if hair:
            # The atlas backdrop is neutral medium gray; the hair is bright,
            # cool silver. Fill enclosed strand shadows without filling skin
            # openings or the outside silhouette.
            mask = (np.maximum.reduce([red, green, blue]) > .43) & (red - green < .035) & (red - blue < .02)
        else:
            mask = (red - green > 0.018) & (red - blue > 0.032) & (red > 0.06)
        if facial_interior or hair:
            exterior = np.zeros_like(mask)
            queue = deque()
            rows, columns = mask.shape
            for y in range(rows):
                for x in (0, columns - 1):
                    if not mask[y, x] and not exterior[y, x]:
                        exterior[y, x] = True; queue.append((y, x))
            for x in range(columns):
                for y in (0, rows - 1):
                    if not mask[y, x] and not exterior[y, x]:
                        exterior[y, x] = True; queue.append((y, x))
            while queue:
                y, x = queue.popleft()
                for yy, xx in ((y-1,x),(y+1,x),(y,x-1),(y,x+1)):
                    if 0 <= yy < rows and 0 <= xx < columns and not mask[yy,xx] and not exterior[yy,xx]:
                        exterior[yy,xx] = True; queue.append((yy,xx))
            if hair:
                warm_skin = (red - green > .025) & (red - blue > .040)
                mask |= (~exterior & ~warm_skin)
            else:
                mask |= ~exterior
        # Require neighboring texels to remain foreground after bilinear sampling.
        for _ in range(max(2, int(round(3 * min(sx, sy))))):
            guarded = np.zeros_like(mask)
            guarded[1:-1, 1:-1] = (mask[1:-1, 1:-1] & mask[:-2, 1:-1] &
                                   mask[2:, 1:-1] & mask[1:-1, :-2] & mask[1:-1, 2:])
            mask = guarded
        ys, xs = np.nonzero(mask)
        if len(xs) < 24:
            raise ValueError('The Silver Casual atlas has no safe foreground samples in ' + str(rectangle))
        self.width, self.height = width, height
        self.origin = (x0, y0)
        self.mask = mask
        self.points = [(float(x + x0) + .5, float(y + y0) + .5) for x, y in zip(xs, ys)]
        self.tree = kdtree.KDTree(len(self.points))
        for index, point in enumerate(self.points):
            self.tree.insert(Vector((point[0], point[1], 0)), index)
        self.tree.balance()
        self.clamped = 0
        self.samples = 0

    def uv(self, x, y):
        px, py = x * self.width / _ATLAS_WIDTH, y * self.height / _ATLAS_HEIGHT
        ix, iy = int(px) - self.origin[0], int(py) - self.origin[1]
        if not (0 <= iy < self.mask.shape[0] and 0 <= ix < self.mask.shape[1] and self.mask[iy, ix]):
            _, index, _ = self.tree.find(Vector((px, py, 0)))
            px, py = self.points[index]
            self.clamped += 1
        self.samples += 1
        return (px / self.width, 1.0 - py / self.height)


def _kind(obj):
    return str(obj.get('projection_kind', ''))


def _bounds(obj):
    points = [obj.matrix_world @ vertex.co for vertex in obj.data.vertices]
    return ([min(point[i] for point in points) for i in range(3)],
            [max(point[i] for point in points) for i in range(3)])


def _hair_uv(point, cy, samplers=None, center=None):
    if samplers is None:
        angle = math.atan2(point.y - cy, point.x - .02)
        u = 1220.0 + 40.0 * (angle / math.tau + .5)
        t = min(1.0, max(0.0, (point.z - 1.75) / .195))
        return (u / _ATLAS_WIDTH, 1.0 - (106.0 - 71.0 * t) / _ATLAS_HEIGHT)
    center = point if center is None else center
    lateral, forward = abs(center.y - cy), center.x - .02
    v = 957 - point.z * 483
    if lateral > abs(forward) * 1.45:
        return samplers['side'].uv(761 - (point.x - .01) * 480, v)
    if forward >= 0:
        return samplers['front'].uv(298 + (point.y - cy) * 470, v)
    return samplers['back'].uv(1240 - (point.y - cy) * 470, v)


def _hairline(point, cy):
    angle = math.atan2(point.y - cy, point.x - .02)
    # Smooth recession above both temples, with millimeter-scale variation.
    # This remains a continuous clipping curve, rather than random vertex noise.
    temple = .0085 * math.exp(-((abs(angle) - .76) / .27) ** 2)
    irregular = .00125 * math.sin(angle * 9 + .45) + .00075 * math.sin(angle * 17 - .6)
    asymmetry = .0015 * math.sin(angle) * math.exp(-(angle / 1.15) ** 4)
    return 1.824 + .064 * math.cos(angle) + temple + irregular + asymmetry


def _create_scalp_cap(face, rig, material, cy, hair_samples):
    """Clip the donor skull exactly at the hairline and add slight crown volume."""
    matrix = face.matrix_world
    normal_matrix = matrix.to_3x3().inverted().transposed()
    points, faces, remap = [], [], {}
    for polygon in face.data.polygons:
        corners = [(matrix @ face.data.vertices[i].co,
                    (normal_matrix @ face.data.vertices[i].normal).normalized()) for i in polygon.vertices]
        clipped = []
        for current, following in zip(corners, corners[1:] + corners[:1]):
            p, n = current; q, m = following
            dp, dq = p.z - _hairline(p, cy), q.z - _hairline(q, cy)
            if dp >= 0:
                clipped.append(current)
            if (dp >= 0) != (dq >= 0):
                lo, hi = 0.0, 1.0
                for _ in range(16):
                    t = (lo + hi) * .5
                    trial = p.lerp(q, t)
                    if (trial.z - _hairline(trial, cy) >= 0) == (dp >= 0): lo = t
                    else: hi = t
                t = (lo + hi) * .5
                clipped.append((p.lerp(q, t), n.lerp(m, t).normalized()))
        if len(clipped) < 3:
            continue
        indices = []
        for point, normal in clipped:
            crown = min(1, max(0, (point.z - 1.84) / .09))
            # Keep the opaque underlayer close to the skull. Volume comes from
            # swept clumps and fine strands, rather than a thick helmet shell.
            offset = .0018 + .0052 * crown
            offset += .0007 * crown * math.sin((point.y-cy)*210 + point.x*72)
            vertex = point + normal * offset
            key = tuple(round(float(value), 6) for value in point)
            if key not in remap:
                remap[key] = len(points); points.append(vertex)
            indices.append(remap[key])
        if len(set(indices)) >= 3:
            faces.append(indices)
    if len(faces) < 24:
        raise ValueError('Face mesh lacks a reusable scalp surface.')
    mesh = bpy.data.meshes.new('Silver scalp cap')
    mesh.from_pydata(points, [], faces)
    mesh.update()
    cap = bpy.data.objects.new('Silver scalp cap', mesh)
    bpy.context.scene.collection.objects.link(cap)
    cap.data.materials.append(material)
    cap.vertex_groups.new(name='head_0').add(list(range(len(points))), 1.0, 'REPLACE')
    modifier = cap.modifiers.new('Player skeleton', 'ARMATURE'); modifier.object = rig
    cap.parent = rig
    cap.matrix_world.identity()
    smoothing = cap.modifiers.new('Hair surface smoothing', 'SUBSURF')
    smoothing.levels = 1; smoothing.render_levels = 1
    cap.modifiers.move(len(cap.modifiers) - 1, 0)
    bpy.context.view_layer.objects.active = cap
    cap.select_set(True)
    bpy.ops.object.modifier_apply(modifier=smoothing.name)
    mesh = cap.data
    cap['projection_kind'] = 'hair'; cap['silver_scalp_cap'] = True
    uv = mesh.uv_layers.new(name='Character atlas')
    # A safe cool underlayer avoids old portrait skin/tint streaks at the temple.
    # The dedicated high-resolution head pass can replace these UVs afterwards.
    underlayer = hair_samples['back'].uv(1240, 64)
    for polygon in mesh.polygons:
        polygon.use_smooth = True
        for loop_index in polygon.loop_indices:
            uv.data[loop_index].uv = underlayer
    return cap


def round_face_geometry(parts, rig, amount=.7):
    """Bounded mature facial proportions on fresh donor geometry.

    Cheeks, nose and chin use measured anatomical regions. Eye meshes, mouth
    meshes, lips, brows and native skeleton remain fixed. Each displacement is
    capped at 5.5mm, and repeated calls do not compound the deformation.
    """
    amount = min(1.0, max(0.0, float(amount)))
    candidates = [o for o in parts if _kind(o) == 'head' and o.type == 'MESH']
    if not candidates:
        raise ValueError('No donor face for cheek refinement.')
    face = max(candidates, key=lambda o: len(o.data.vertices))
    if face.get('silver_face_roundness') is not None:
        return {'amount': face['silver_face_roundness'], 'already_applied': True,
                'version': face.get('silver_face_morph_version', 1)}
    cy = (rig.matrix_world @ rig.data.bones['head_0'].head_local).y
    inverse = face.matrix_world.inverted()
    changed, largest = 0, 0.0
    counts = {'cheek': 0, 'nose': 0, 'chin': 0}
    for vertex in face.data.vertices:
        point = face.matrix_world @ vertex.co
        if not (1.665 < point.z < 1.802 and point.x > .03 and abs(point.y-cy) < .087):
            continue
        lateral = point.y-cy
        central_gate = min(1.0, max(0.0, (abs(lateral)-.024)/.019))
        upper_cheek = math.exp(-((point.z-1.783)/.027)**2 - ((abs(lateral)-.051)/.027)**2)
        lower_cheek = math.exp(-((point.z-1.746)/.030)**2 - ((abs(lateral)-.052)/.026)**2)
        jaw = math.exp(-((point.z-1.692)/.025)**2 - ((abs(lateral)-.047)/.029)**2)
        # Fade below the eyes and protect the lip region at the centerline.
        eye_gate = min(1.0, max(0.0, (1.802-point.z)/.014))
        cheek_fill = upper_cheek * eye_gate
        dx = amount * (.0032*cheek_fill + .0025*lower_cheek + .0008*jaw) * central_gate
        dy = amount * (.0034*cheek_fill + .0030*lower_cheek + .0045*jaw) * central_gate
        if central_gate > 0 and max(cheek_fill, lower_cheek, jaw) > .05:
            counts['cheek'] += 1

        # The donor nose tip is Z~1.775, not the old Z1.753 morph center.
        # Smooth X gating avoids a ridge at the edge of the nasal patch.
        nose = math.exp(-((point.z-1.775)/.019)**2 - (lateral/.016)**2)
        nose_gate = min(1.0, max(0.0, (point.x-.128)/.016))
        if point.z > 1.750:
            dx -= amount * .0034 * nose * nose_gate
            if nose * nose_gate > .05: counts['nose'] += 1

        # Retract and slightly lift the square central chin. The gate ends below
        # the lips; separate internal mouth geometry is never selected here.
        chin = math.exp(-((point.z-1.695)/.018)**2 - (lateral/.026)**2)
        chin_gate = min(1.0, max(0.0, (point.x-.106)/.014))
        lip_gate = min(1.0, max(0.0, (1.717-point.z)/.013))
        chin *= chin_gate * lip_gate
        dx -= amount * .0035 * chin
        dz = amount * .0025 * chin
        dy += amount * .0012 * chin * min(1.0, abs(lateral)/.016)
        if chin > .05: counts['chin'] += 1
        delta = Vector((dx, math.copysign(dy,lateral), dz))
        if delta.length > .0055:
            delta *= .0055 / delta.length
        if delta.length <= 1e-9:
            continue
        vertex.co = inverse @ (point+delta)
        changed += 1; largest = max(largest, delta.length)
    face.data.update()
    face['silver_face_roundness'] = amount
    face['silver_face_morph_version'] = 2
    return {'amount': amount, 'version': 2, 'vertices_changed': changed,
            'max_displacement_m': largest, 'regions': counts}


def _strand_palette(samples, pixels, low, high):
    values = []
    for sample in samples.values():
        for x,y in sample.points:
            rgb = pixels[int(y),int(x)]
            luminance = .2126*rgb[0]+.7152*rgb[1]+.0722*rgb[2]
            if low < luminance < high:
                values.append((x/sample.width,1.0-y/sample.height))
    if not values:
        raise ValueError('Silver hair atlas has no strand color samples.')
    return values


def _create_swept_strands(cap, rig, material, hair_samples, pixels, cy, count=720):
    """Fine closed tapered tubes, rather than opaque photographic hair cards."""
    count = max(48,min(900,int(count)))
    mesh = cap.data; mesh.calc_loop_triangles()
    world = cap.matrix_world
    coordinates = [world@v.co for v in mesh.vertices]
    triangles = [tuple(t.vertices) for t in mesh.loop_triangles]
    tree = BVHTree.FromPolygons(coordinates,triangles,all_triangles=True)
    cumulative, total = [], 0.0
    for triangle in triangles:
        a,b,c = [coordinates[i] for i in triangle]
        total += (b-a).cross(c-a).length*.5; cumulative.append(total)
    if total <= 0: raise ValueError('Hair scalp has no usable surface.')
    boundary = [p for p in coordinates if abs(p.z-_hairline(p,cy)) < .008 and p.x > .045]
    boundary.sort(key=lambda p: math.atan2(p.y-cy,p.x-.02))
    gray_palette = _strand_palette(hair_samples,pixels,.43,.70)
    silver_palette = _strand_palette(hair_samples,pixels,.64,.86)
    rng = random.Random(92741)
    vertices, polygons, uv_values = [], [], []
    completed = 0
    for index in range(count):
        major = index % 3 == 0
        if index < min(160,len(boundary)):
            root = boundary[int(index*(len(boundary)-1)/max(1,min(160,len(boundary))-1))].copy()
        else:
            triangle = triangles[min(len(triangles)-1,bisect.bisect_left(cumulative,rng.random()*total))]
            a,b,c = [coordinates[i] for i in triangle]
            first = math.sqrt(rng.random()); second = rng.random()
            root = a*(1-first)+b*(first*(1-second))+c*(first*second)
        surface,normal,_,_ = tree.find_nearest(root)
        if surface is None: continue
        crown = min(1,max(0,(surface.z-1.83)/.09))
        length = (.028+.026*crown)*rng.uniform(.8,1.2)
        width = rng.uniform(.00045,.00080) if major else rng.uniform(.00014,.00030)
        flyaway = index % 19 == 0
        lift = (.0027 if major else .0034)*(.45+.55*crown)
        if flyaway:
            width = rng.uniform(.00010,.00018)
            lift += rng.uniform(.0020,.0040)*(.5+.5*crown)
        # Nearby roots share a sweep direction; the silhouette gets sparse,
        # finer flyaways instead of uniformly thick raised cords.
        group_sweep = .24 + .07*math.sin((surface.y-cy)*95 + surface.x*26)
        centers, normals = [], []
        current = surface.copy()
        steps = 8
        for step in range(steps+1):
            t = step/steps
            centers.append(current+normal*(.0002+lift*math.sin(math.pi*t)**.8))
            normals.append(normal.copy())
            if step == steps: break
            vertical = .34 if normal.x>.2 else (-.48 if normal.x<-.25 else .06)
            flow = Vector((-1,group_sweep+(.025 if major else -.015),vertical))
            flow -= normal*flow.dot(normal)
            if flow.length < .01: break
            probe = current+flow.normalized()*(length/steps)
            location,new_normal,_,distance = tree.find_nearest(probe)
            if location is None or distance>.025 or (location-current).length<.0002: break
            current,normal = location,new_normal
        if len(centers)<4: continue
        palette = gray_palette if major else silver_palette
        color = palette[rng.randrange(len(palette))]
        start = len(vertices); radial_count = 4
        for section,(center,normal) in enumerate(zip(centers,normals)):
            t = section/max(1,len(centers)-1)
            tangent = (centers[min(section+1,len(centers)-1)]-centers[max(0,section-1)]).normalized()
            across = normal.cross(tangent).normalized()
            outward = tangent.cross(across).normalized()
            radius = max(.000045,width*(1-t)**.72)*min(1,.72+t*3)
            for spoke in range(radial_count):
                angle = math.tau*spoke/radial_count
                vertices.append(center+across*(math.cos(angle)*radius)+outward*(math.sin(angle)*radius*.52))
        for section in range(len(centers)-1):
            for spoke in range(radial_count):
                nxt=(spoke+1)%radial_count
                polygons.append((start+section*radial_count+spoke,start+section*radial_count+nxt,
                                 start+(section+1)*radial_count+nxt,start+(section+1)*radial_count+spoke))
                uv_values.append(color)
        polygons.append(tuple(start+i for i in reversed(range(radial_count))));uv_values.append(color)
        polygons.append(tuple(start+(len(centers)-1)*radial_count+i for i in range(radial_count)));uv_values.append(color)
        completed += 1
    if completed < 24: raise ValueError('Too few valid swept hair strands.')
    strand_mesh=bpy.data.meshes.new('Swept silver hair strands');strand_mesh.from_pydata(vertices,[],polygons);strand_mesh.update()
    obj=bpy.data.objects.new('Swept silver hair strands',strand_mesh);bpy.context.scene.collection.objects.link(obj)
    strand_mesh.materials.append(material)
    obj.vertex_groups.new(name='head_0').add(list(range(len(vertices))),1.0,'REPLACE')
    modifier=obj.modifiers.new('Player skeleton','ARMATURE');modifier.object=rig
    obj.parent=rig;obj.matrix_world.identity();obj['projection_kind']='hair';obj['silver_hair_strands']=True
    uv=strand_mesh.uv_layers.new(name='Character atlas')
    for polygon,color in zip(strand_mesh.polygons,uv_values):
        polygon.use_smooth=True
        for loop_index in polygon.loop_indices:uv.data[loop_index].uv=color
    obj['strand_count']=completed
    return obj


def apply_head_uv(parts: Iterable, rig, material, atlas_image=None, replace_hair=True, hair_strands=True, strand_count=720):
    """Return (updated_parts, diagnostics), retaining one atlas material.

    Facial charts depend on mesh position, never triangle normals. All visible
    face features share the same front chart; the rear skull/neck and articulated
    skin limbs use small foreground-only skin patches. Native bone data is untouched.
    """
    parts = list(parts)
    if atlas_image is None:
        atlas_image = next((node.image for node in material.node_tree.nodes
                            if node.type == 'TEX_IMAGE' and node.image), None)
    if atlas_image is None:
        raise ValueError('Silver Casual material has no atlas image.')
    pixels, width, height = _atlas_pixels(atlas_image)
    front = _SkinSamples(pixels, width, height, (240, 38, 356, 173), facial_interior=True)
    limbs = _SkinSamples(pixels, width, height, (475, 400, 493, 435))
    neck = _SkinSamples(pixels, width, height, (270, 143, 327, 174), facial_interior=True)
    mouth = _SkinSamples(pixels, width, height, (287, 121, 310, 130))
    hair_samples = {'front': _SkinSamples(pixels,width,height,(246,15,351,99),hair=True),
                    'side': _SkinSamples(pixels,width,height,(701,15,826,128),hair=True),
                    'back': _SkinSamples(pixels,width,height,(1180,13,1295,134),hair=True)}
    cy = (rig.matrix_world @ rig.data.bones['head_0'].head_local).y
    report = {'parts': [], 'hair_replaced': False, 'skeleton_bones': len(rig.data.bones)}
    face_objects = [obj for obj in parts if _kind(obj) == 'head' and obj.type == 'MESH']
    if not face_objects:
        raise ValueError('Silver Casual has no face geometry.')
    face = max(face_objects, key=lambda obj: len(obj.data.vertices))

    for obj in parts:
        kind = _kind(obj)
        if not (kind in {'head', 'hair'} or kind.startswith(('hand_', 'forearm_'))):
            continue
        mesh = obj.data
        if not mesh.vertices or not mesh.polygons:
            continue
        uv = mesh.uv_layers.active or mesh.uv_layers.new(name='Character atlas')
        low, high = _bounds(obj)
        # Identify the rear hemisphere by position. A whole polygon uses one
        # chart, avoiding sharp cavity-normal flips along eyelids and nostrils.
        for polygon in mesh.polygons:
            center = obj.matrix_world @ polygon.center
            rear = center.x < .015
            for loop_index in polygon.loop_indices:
                vertex = mesh.vertices[mesh.loops[loop_index].vertex_index]
                point = obj.matrix_world @ vertex.co
                if kind == 'hair':
                    value = _hair_uv(point, cy, hair_samples, center)
                elif kind.startswith(('hand_', 'forearm_')):
                    # A skin-only forearm patch keeps all fingertips/cuffs away
                    # from photographed arm edges, clothing and gray background.
                    along = (point.z - low[2]) / max(.001, high[2] - low[2])
                    depth = (point.x - low[0]) / max(.001, high[0] - low[0])
                    value = limbs.uv(480 + 8 * depth, 429 - 22 * along)
                elif 'mouth' in obj.name.lower():
                    value = mouth.uv(298, 125)
                elif point.z < 1.665 or rear:
                    # Continue the portrait's neck shading from the same
                    # vertical level as the face, avoiding a pale chin wedge.
                    u = 298 + (point.y - cy) * 200
                    v = 957 - 1.665 * 483 + (1.665 - min(point.z,1.665)) * 200
                    value = neck.uv(u, v)
                else:
                    value = front.uv(298 + (point.y - cy) * 470, 957 - point.z * 483)
                    if kind=='head' and point.z<1.725 and point.x>.045:
                        # Blend per vertex, so adjacent triangles share the same
                        # chin UV rather than drawing a jagged chart boundary.
                        blend=min(1,max(0,(-vertex.normal.z-.10)/.55))
                        blend*=min(1,max(0,(1.725-point.z)/.040))
                        shadow=neck.uv(298+(point.y-cy)*110,149+min(2,max(0,(1.69-point.z)*35)))
                        value=tuple(a*(1-blend)+b*blend for a,b in zip(value,shadow))
                uv.data[loop_index].uv = value
        report['parts'].append({'name': obj.name, 'kind': kind, 'loops': len(mesh.loops)})

    if replace_hair:
        old_hair = [obj for obj in parts if _kind(obj) == 'hair']
        cap = _create_scalp_cap(face, rig, material, cy, hair_samples)
        for obj in old_hair:
            parts.remove(obj)
            bpy.data.objects.remove(obj, do_unlink=True)
        parts.append(cap)
        report['hair_replaced'] = True
        report['hair_vertices'] = len(cap.data.vertices)
        report['hair_polygons'] = len(cap.data.polygons)
        if hair_strands:
            strands = _create_swept_strands(cap,rig,material,hair_samples,pixels,cy,strand_count)
            parts.append(strands)
            report['hair_strands'] = strands['strand_count']
            report['strand_vertices'] = len(strands.data.vertices)
            report['strand_polygons'] = len(strands.data.polygons)
    report['foreground_samples'] = sum(s.samples for s in (front, limbs, neck, mouth))
    report['hair_atlas_samples'] = sum(s.samples for s in hair_samples.values())
    report['clamped_hair_samples'] = sum(s.clamped for s in hair_samples.values())
    report['clamped_foreground_samples'] = sum(s.clamped for s in (front, limbs, neck, mouth))
    return parts, report
