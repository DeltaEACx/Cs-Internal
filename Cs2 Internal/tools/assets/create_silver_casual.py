"""Build the Silver Casual character with Blender, retaining the CS2 player rig.

Run: blender --background --factory-startup --python tools/assets/create_silver_casual.py
The generated turnaround is a projection atlas, not a replacement for geometry.
Donor files are read only. Source .blend, FBX, GLB and inspection renders are saved.
"""
import bpy
import bmesh
import json
import math
import sys
from array import array
from functools import lru_cache
from pathlib import Path
from mathutils import Vector
sys.path.insert(0,str(Path(__file__).resolve().parent))
from silver_casual_head_uv import apply_head_uv, round_face_geometry
from silver_casual_clothing import refine_clothing
from silver_casual_head_detail import apply_head_detail

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "cs2_assets/custom_characters/silver_casual/source"
BUILD = ROOT / ".asset-build/silver_casual"
MODEL = BUILD / "content/csgo/models/custom/agents/silver_casual"
PREVIEWS = ROOT / "cs2_assets/preview_models"
ATLAS = SOURCE / "turnaround.png"
CY = .07248011
TAU = math.tau
for directory in (SOURCE, MODEL, BUILD / "renders"):
    directory.mkdir(parents=True, exist_ok=True)

bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
bpy.ops.import_scene.gltf(filepath=str(PREVIEWS / "characters_models_madebyruzh_gman__Ruzh__Gman_vmdl.glb"))
rig = next(obj for obj in bpy.context.scene.objects if obj.type == 'ARMATURE')
rig.name = "SilverCasualRig"
rig.animation_data_clear()
for pose_bone in rig.pose.bones:
    pose_bone.matrix_basis.identity()
rig.data.pose_position = 'REST'
gman = next(obj for obj in bpy.context.scene.objects if obj.type == 'MESH' and obj.vertex_groups)
bpy.context.view_layer.update()
bone_positions = {bone.name: rig.matrix_world @ bone.head_local for bone in rig.data.bones}
original_rig = {bone.name: [list(row) for row in bone.matrix_local] for bone in rig.data.bones}

material = bpy.data.materials.new("silver_casual")
material.use_nodes = True
shader = material.node_tree.nodes.get('Principled BSDF')
shader.inputs['Roughness'].default_value = .78
shader.inputs['Specular IOR Level'].default_value = .22
texture = material.node_tree.nodes.new('ShaderNodeTexImage')
texture.image = bpy.data.images.load(str(ATLAS))
texture.interpolation = 'Linear'
material.node_tree.links.new(texture.outputs['Color'], shader.inputs['Base Color'])

parts = []

def vertex_weights(obj, vertex):
    values = {obj.vertex_groups[item.group].name: item.weight for item in vertex.groups if item.weight > .001}
    values = dict(sorted(values.items(), key=lambda item: -item[1])[:4])
    total = sum(values.values())
    return {name: weight / total for name, weight in values.items()} if total else {'pelvis': 1.0}

def create_part(name, vertices, faces, weights, kind):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.data.materials.append(material)
    for bone in rig.data.bones:
        obj.vertex_groups.new(name=bone.name)
    for index, entries in enumerate(weights):
        for bone, value in entries.items():
            if value > .0001:
                obj.vertex_groups[bone].add([index], value, 'REPLACE')
    modifier = obj.modifiers.new('Player skeleton', 'ARMATURE')
    modifier.object = rig
    obj.parent = rig
    obj['projection_kind'] = kind
    for polygon in mesh.polygons:
        polygon.use_smooth = True
    parts.append(obj)
    return obj

def extract(source, name, predicate, kind, deform=None):
    selected = [poly for poly in source.data.polygons if predicate(poly)]
    indices = sorted({index for poly in selected for index in poly.vertices})
    remap = {old: new for new, old in enumerate(indices)}
    vertices = [source.matrix_world @ source.data.vertices[index].co for index in indices]
    if deform:
        vertices = [deform(vertex) for vertex in vertices]
    faces = [[remap[index] for index in poly.vertices] for poly in selected]
    weights = [vertex_weights(source, source.data.vertices[index]) for index in indices]
    return create_part(name, vertices, faces, weights, kind)

def material_name(poly):
    return gman.material_slots[poly.material_index].material.name

# Keep the donor's detailed face, ears and neck. Hands retain their articulated
# finger weights; the torso and sleeves are newly modeled below.
face = extract(gman, 'Face and neck', lambda p: material_name(p) == 'head' and
        min(gman.data.vertices[i].co.z for i in p.vertices) > 1.50, 'head')
for vertex in face.data.vertices:
    amount=max(0,min(1,(1.677-vertex.co.z)/.026))
    vertex.co.x=vertex.co.x*(1-amount)+max(-.047,min(.057,vertex.co.x))*amount
    vertex.co.y=vertex.co.y*(1-amount)+max(CY-.065,min(CY+.065,vertex.co.y))*amount
    if vertex.co.z<1.705:
        for group in face.vertex_groups: group.remove([vertex.index])
        if vertex.co.z<1.665:
            t=max(0,min(1,(vertex.co.z-1.615)/.05))
            face.vertex_groups['spine_3'].add([vertex.index],1-t,'REPLACE')
            face.vertex_groups['neck_0'].add([vertex.index],t,'REPLACE')
        else:
            t=(vertex.co.z-1.665)/.04
            face.vertex_groups['neck_0'].add([vertex.index],1-t,'REPLACE')
            face.vertex_groups['head_0'].add([vertex.index],t,'REPLACE')
face.data.update()
for side, sign in [('l', 1), ('r', -1)]:
    extract(gman, 'Hand ' + side, lambda p, s=sign: material_name(p) == 'head' and
            all((gman.data.vertices[i].co.y - CY) * s > .30 for i in p.vertices), 'hand_' + side)
extract(gman, 'Hair', lambda p: material_name(p) == 'alpha', 'hair')
extract(gman, 'Eyes', lambda p: material_name(p) == 'ctm_fbi_v2_head', 'head')
extract(gman, 'Mouth', lambda p: material_name(p) in {'teeth', 'tongue'}, 'head')
bpy.data.objects.remove(gman, do_unlink=True)

# Borrow lower-body topology from the installed civilian rig; its bone rest
# matrices are identical to the face donor. Preserve the source UVs only for
# selecting jeans/shoes, then replace them with the new character atlas.
before = set(bpy.context.scene.objects)
bpy.ops.import_scene.gltf(filepath=str(PREVIEWS / "characters_models_madebyruzh_sweet__Ruzh__Sweet_vmdl.glb"))
imported = set(bpy.context.scene.objects) - before
sweet = next(obj for obj in imported if obj.type == 'MESH' and obj.vertex_groups)
sweet_rig = next(obj for obj in imported if obj.type == 'ARMATURE')
sweet_rig.animation_data_clear()
sweet_rig.data.pose_position = 'REST'
bpy.context.view_layer.update()
for bone in sweet_rig.data.bones:
    assert bone.name in original_rig
    assert max(abs(bone.matrix_local[i][j] - original_rig[bone.name][i][j]) for i in range(4) for j in range(4)) < 1e-5

def pant_face(poly):
    return max(sweet.data.vertices[i].co.z for i in poly.vertices) >= .26 and \
        max(sweet.data.vertices[i].co.z for i in poly.vertices) < 1.25 and \
        sum(sweet.data.uv_layers.active.data[i].uv.x for i in poly.loop_indices) / len(poly.loop_indices) > .70

def fitted_jeans(point):
    point = point.copy()
    side = 'l' if point.y > CY else 'r'
    center = bone_positions['leg_upper_' + side].y
    width = .69 + .20 * max(0, min(1, (point.z - .70) / .26))
    point.y = center + (point.y - center) * width
    point.x = (point.x + .008) * .83 - .008
    if point.z > .70:
        point.z = .70 + (point.z - .70) * 1.49
    waist = max(0,min(1,(point.z-.94)/.10))
    point.x = point.x*(1-waist) + max(-.097,min(.113,point.x))*waist
    point.y = point.y*(1-waist) + max(CY-.177,min(CY+.177,point.y))*waist
    return point

pants = extract(sweet, 'Light denim jeans', pant_face, 'pants', fitted_jeans)
bm=bmesh.new()
bm.from_mesh(pants.data)
bmesh.ops.remove_doubles(bm,verts=list(bm.verts),dist=.00008)
bm.to_mesh(pants.data)
bm.free()
edge_counts = {}
for polygon in pants.data.polygons:
    for edge in polygon.edge_keys:
        edge_counts[edge] = edge_counts.get(edge,0)+1
for edge,count in edge_counts.items():
    if count==1:
        for index in edge:
            vertex=pants.data.vertices[index]
            if vertex.co.z<.40: vertex.co.z=.160
            elif vertex.co.z>.82: vertex.co.z=1.105
for vertex in pants.data.vertices:
    if vertex.co.z>1.0:
        vertex.co.x=max(-.083,min(.100,vertex.co.x))
        vertex.co.y=max(CY-.170,min(CY+.170,vertex.co.y))
pants.data.update()
shoes = extract(sweet, 'White sneakers', lambda p: max(sweet.data.vertices[i].co.z for i in p.vertices) < .26, 'shoes')
parts.remove(shoes)
bpy.data.objects.remove(shoes, do_unlink=True)
for obj in imported:
    bpy.data.objects.remove(obj, do_unlink=True)

def blend_weights(first, second, amount):
    amount = max(0, min(1, amount))
    if amount <= .0001: return {first: 1.0}
    if amount >= .9999: return {second: 1.0}
    return {first: 1 - amount, second: amount}

def torso_weights(z):
    levels = [(1.05, 'pelvis'), (1.19, 'spine_0'), (1.31, 'spine_1'), (1.43, 'spine_2'), (1.56, 'spine_3')]
    for (low, first), (high, second) in zip(levels, levels[1:]):
        if z <= high: return blend_weights(first, second, (z - low) / (high - low))
    return {'spine_3': 1.0}

def loft(name, rings, count, kind, weights_fn):
    vertices, faces, weights = [], [], []
    for ring_index, (center, radial_a, radial_b) in enumerate(rings):
        for index in range(count):
            theta = TAU * index / count
            point = center + math.cos(theta) * radial_a + math.sin(theta) * radial_b
            vertices.append(point)
            weights.append(weights_fn(ring_index, point))
    for ring in range(len(rings) - 1):
        for index in range(count):
            following = (index + 1) % count
            faces.append((ring * count + index, ring * count + following,
                          (ring + 1) * count + following, (ring + 1) * count + index))
    if kind=='sole': faces.append(tuple(reversed(range(count))))
    return create_part(name, vertices, faces, weights, kind)

# Smooth the civilian trouser topology while preserving its skin weights.
bpy.context.view_layer.objects.active = pants
subdivision = pants.modifiers.new('Tailored denim surface', 'SUBSURF')
subdivision.levels = 2
bpy.ops.object.modifier_apply(modifier=subdivision.name)

# Rounded sneaker uppers and a separate ribbed sole replace the angular donor
# shoes. Their ankle / toe weights follow the native player's foot articulation.
for side in ('l', 'r'):
    ankle = bone_positions['ankle_' + side]
    def foot_weights(index, point, s=side):
        return blend_weights('ankle_' + s, 'ball_' + s, (point.x - .045) / .13)
    for name, profile, kind in [
        ('White sneaker upper ', [(.042,.047,.157,.065),(.065,.047,.156,.064),
            (.088,.043,.151,.061),(.112,.032,.133,.055),(.137,.001,.093,.050),
            (.167,-.030,.061,.047),(.185,-.036,.050,.043)], 'shoes'),
        ('White rubber sole ', [(.012,.047,.157,.066),(.019,.047,.160,.068),
            (.038,.047,.160,.068),(.047,.047,.157,.065)], 'sole')]:
        rings = [(Vector((x,ankle.y,z)), Vector((length,0,0)), Vector((0,width,0)))
                 for z,x,length,width in profile]
        loft(name + side, rings, 48, kind, foot_weights)

# Subtle ring offsets model a relaxed knit fit, a ribbed hem, shoulders and
# raised quarter-zip collar without importing the donor jacket or tie.
torso_profile = [
    (1.055, .192, .118), (1.075, .196, .120), (1.092, .198, .124),
    (1.115, .199, .123), (1.16, .201, .129), (1.23, .212, .133),
    (1.31, .222, .135), (1.39, .235, .132), (1.47, .248, .124),
    (1.54, .240, .115), (1.59, .208, .098), (1.635, .135, .075),
    (1.658, .085, .068)]
torso_rings = [(Vector((.012, CY, z)), Vector((depth, 0, 0)), Vector((0, width, 0)))
               for z, width, depth in torso_profile]
loft('Navy quarter zip', torso_rings, 48, 'body', lambda index, point: torso_weights(point.z))
collar = loft('Raised knit collar', [
    (Vector((.005, CY, 1.637)), Vector((.078, 0, 0)), Vector((0, .087, 0))),
    (Vector((.002, CY, 1.675)), Vector((.075, 0, 0)), Vector((0, .083, 0))),
    (Vector((-.002, CY, 1.704)), Vector((.073, 0, 0)), Vector((0, .081, 0)))],
    48, 'collar', lambda index, point: blend_weights('spine_3','neck_0',.10+.40*index))
for vertex in collar.data.vertices:
    front = max(0, (vertex.co.x - .025) / .05)
    center = max(0, 1 - abs(vertex.co.y - CY) / .045)
    vertex.co.z -= .047 * front * center
collar.data.update()

arm_data = {}
for side in ('l', 'r'):
    shoulder = bone_positions['arm_upper_' + side]
    elbow = bone_positions['arm_lower_' + side]
    wrist = bone_positions['hand_' + side]
    arm_data[side] = (shoulder, elbow, wrist)
    direction=(elbow-shoulder).normalized()
    cross=direction.cross(Vector((1,0,0))).normalized()
    start=shoulder.copy()
    start.y += -.055 if side=='l' else .055
    start.z -= .035
    entries = [(start,Vector((.068,0,0)),cross*.068)]
    for t, radius in [(0,.076),(.15,.081),(.35,.076),(.6,.071),(.82,.064),(1,.060),(1.12,.057),(1.25,.053),(1.28,.052)]:
        center = shoulder.lerp(elbow, t) if t <= 1 else elbow.lerp(wrist, t - 1)
        center.z -= .038 * max(0,1-t/.35)
        direction = (elbow - shoulder if t <= 1 else wrist - elbow).normalized()
        cross = direction.cross(Vector((1,0,0))).normalized()
        entries.append((center, Vector((radius * .95,0,0)), cross * radius))
    loft('Rolled sleeve ' + side, entries, 32, 'sleeve_' + side,
         lambda index, point, s=side: blend_weights('spine_3','arm_upper_' + s,.4) if index==0 else
         blend_weights('arm_upper_' + s, 'arm_lower_' + s, (index - 5) / 3))
    entries = []
    for t, radius in [(.20,.049),(.29,.048),(.42,.046),(.58,.040),(.75,.035),(.9,.031),(1.02,.030)]:
        center = elbow.lerp(wrist,t)
        cross = (wrist - elbow).normalized().cross(Vector((1,0,0))).normalized()
        entries.append((center, Vector((radius*.89,0,0)), cross*radius))
    loft('Forearm ' + side, entries, 32, 'forearm_' + side,
         lambda index, point, s=side: blend_weights('arm_lower_' + s, 'hand_' + s, max(0,(index-4)/3)))

# The reference's small striped patch sits on the outside of the left sleeve.
shoulder,elbow,wrist=arm_data['l']
along=(elbow-shoulder).normalized()
outward=along.cross(Vector((1,0,0))).normalized()
patch_center=shoulder.lerp(elbow,.42)+outward*.078
patch_vertices=[patch_center+Vector((sx*.028,0,0))+along*(sy*.018)
                for sx,sy in [(-1,-1),(1,-1),(1,1),(-1,1)]]
patch=create_part('Striped sleeve patch',patch_vertices,[(0,1,2,3)],
                  [{'arm_upper_l':1.0}]*4,'patch')

clothing_report=refine_clothing(parts,rig,material,arm_data,create_part,CY)

def lerp2(a,b,t): return (a[0]+(b[0]-a[0])*t, a[1]+(b[1]-a[1])*t)
def projected_segment(point, start, end):
    delta=end-start
    return max(0,min(1,(point-start).dot(delta)/delta.length_squared))

def atlas_coordinate(point, normal, kind):
    # The atlas contains front / left profile / back, generated from the user's
    # reference. Limb-specific transforms compensate for the native bind pose.
    x,y,z=point
    front=normal.x >= 0
    side_view=abs(normal.y) > abs(normal.x)*1.75 and kind in {'head','hair'}
    u=298+(y-CY)*483
    v=957-z*483
    if kind=='pants':
        side='l' if y>CY else 'r'
        center=bone_positions['leg_upper_'+side].y
        line=382-(z-.10)*40 if side=='l' else 212+(z-.10)*36
        leg_u=line+(y-center)*440
        transition=max(0,min(1,(z-.90)/.19))
        u=leg_u*(1-transition)+u*transition
        v=961-z*468
    elif kind=='shoes':
        side='l' if y>CY else 'r'
        center=bone_positions['ankle_'+side].y
        u=(387 if side=='l' else 208)+(y-center)*400
        v=959-z*290
    elif kind in {'sole','lace'}:
        return (203/1536, 1-946/1024)
    elif kind=='zipper':
        return (299/1536,1-228/1024)
    elif kind.startswith(('sleeve_','forearm_','hand_')):
        side=kind[-1]
        shoulder,elbow,wrist=arm_data[side]
        positive=side=='l'
        if kind.startswith('sleeve'):
            t=projected_segment(point,shoulder,elbow)
            center=shoulder.lerp(elbow,t)
            source=lerp2((400,214),(439,348),t)
            if z<elbow.z:
                t=projected_segment(point,elbow,wrist)
                center=elbow.lerp(wrist,t)
                source=lerp2((439,348),(488,481),t)
        elif kind.startswith('forearm'):
            t=projected_segment(point,elbow,wrist)
            center=elbow.lerp(wrist,t)
            source=lerp2((439,348),(488,481),t)
        else:
            center=wrist
            source=(492,480)
        if not positive: source=(596-source[0],source[1])
        u=source[0]+(y-center.y)*(330 if kind.startswith('hand') else 460)
        v=source[1]-(z-center.z)*(385 if kind.startswith('hand') else 460)
    elif kind=='collar':
        # A collar is all knit, including its inner wall. Projecting across
        # the photographed neck would paint a skin-colored stripe on the band.
        return ((355+(y-CY)*65)/1536,1-(190-(z-1.637)*70)/1024)
    if side_view:
        u=761-(x-.01)*480
        if kind=='shoes': u=760-x*420
        if kind=='hair': v=max(23,min(138,v))
        return (max(662,min(838,u))/1536,1-max(17,min(960,v))/1024)
    if not front:
        u=1240-(u-298)
    if kind=='hair': v=max(23,min(139,v))
    if kind in {'body', 'collar'} or kind.startswith('sleeve_'):
        if kind == 'body': v=min(v,433 if front else 423)
        elif kind.startswith('sleeve_'): v=min(v,350)
        u=clamp_to_fabric(u,v,'navy',kind,front)
    elif kind=='pants':
        v=max(448,min(891,v))
        u=clamp_to_fabric(u,v,'denim',kind,front)
    elif kind=='shoes':
        v=max(906,min(950,v))
        u=clamp_to_fabric(u,v,'white',kind,front)
    elif kind.startswith('forearm_'):
        u=clamp_to_fabric(u,v,'skin',kind,front)
    return (max(35,min(1500,u))/1536,1-max(19,min(959,v))/1024)

# Sample the immutable reference atlas to keep every projection inside its
# material silhouette. This changes UVs, not the reference image.
atlas_pixels = array('f', [0]) * (1536*1024*4)
texture.image.pixels.foreach_get(atlas_pixels)
@lru_cache(maxsize=24000)
def fabric_interval(row, family, low, high):
    valid=[]
    for col in range(max(0,low),min(1535,high)+1):
        index=((1023-row)*1536+col)*4
        r,g,b=atlas_pixels[index:index+3]
        ok = ((b>r*1.16 and b>g*1.035 and b<.56) if family=='navy' else
              (b>r*1.14 and b>g*1.02) if family=='denim' else
              (min(r,g,b)>.64) if family=='white' else
              (r>g*1.07 and g>b*1.04))
        if ok: valid.append(col)
    return (min(valid)+4,max(valid)-4) if len(valid)>10 else (low+8,high-8)

def clamp_to_fabric(u,v,family,kind,front):
    row=max(0,min(1023,round(v)))
    center=298 if front else 1240
    if kind in {'body','collar'}:
        width=92 if row>280 else 120
        low,high=center-width,center+width
    elif kind in {'pants','shoes'}:
        low,high=(center-125,center-3) if u<center else (center+3,center+125)
    else:
        low,high=round(u)-32,round(u)+32
        if row>285:
            if u<center:high=min(high,center-95)
            else:low=max(low,center+95)
    low,high=fabric_interval(row,family,low,high)
    return max(low,min(high,u))

for obj in parts:
    obj.data.update()
    uv=obj.data.uv_layers.new(name='Character atlas')
    for poly in obj.data.polygons:
        normal=poly.normal
        for loop_index in poly.loop_indices:
            vertex=obj.data.vertices[obj.data.loops[loop_index].vertex_index]
            uv.data[loop_index].uv=atlas_coordinate(vertex.co,normal,obj['projection_kind'])
    if obj['projection_kind']=='patch':
        for index,(u,v) in enumerate([(777,232),(810,232),(810,251),(777,251)]):
            uv.data[index].uv=(u/1536,1-v/1024)

face_report=round_face_geometry(parts,rig,.7)
parts,head_uv_report=apply_head_uv(parts,rig,material,texture.image)
head_material,head_image,detail_report=apply_head_detail(parts,rig,SOURCE/'head_detail.png')

# Subdivision can interpolate six influences at a hip seam. Keep the final FBX
# and GLB on the same four-influence contract used by the native player shader.
normalized_vertices=0
for obj in parts:
    for vertex in obj.data.vertices:
        if len(vertex.groups)<=4 and abs(sum(g.weight for g in vertex.groups)-1)<.00001:
            continue
        weights=vertex_weights(obj,vertex)
        for existing in list(vertex.groups):
            obj.vertex_groups[existing.group].remove([vertex.index])
        for bone,weight in weights.items():
            obj.vertex_groups[bone].add([vertex.index],weight,'REPLACE')
        normalized_vertices+=1

# Remove imported animation containers and empty source objects, leaving one
# unmodified native skeleton. The game supplies its animation graphs.
for obj in list(bpy.context.scene.objects):
    if obj not in parts and obj != rig:
        bpy.data.objects.remove(obj,do_unlink=True)
rig.parent=None
rig.data.pose_position='POSE'
bpy.context.view_layer.update()
bpy.ops.object.select_all(action='DESELECT')
rig.select_set(True)
for obj in parts:obj.select_set(True)
bpy.context.view_layer.objects.active=rig
bpy.context.scene.unit_settings.system='METRIC'
bpy.context.scene.unit_settings.scale_length=1
bpy.context.preferences.filepaths.save_version=0
texture.image.pack()
bpy.ops.wm.save_as_mainfile(filepath=str(SOURCE/'silver_casual.blend'))
export_materials=(material,head_material)
material_names=[(m,m.name) for m in export_materials]
image_links=[(m,[(link.from_socket,link.to_socket) for link in m.node_tree.links]) for m in export_materials]
for m in export_materials:
    for link in list(m.node_tree.links): m.node_tree.links.remove(link)
for m,name in material_names:
    m.name='materials/custom/agents/silver_casual/'+name+'.vmat'
bpy.ops.export_scene.fbx(filepath=str(MODEL/'silver_casual.fbx'),use_selection=True,
    object_types={'ARMATURE','MESH'},axis_forward='X',axis_up='Z',global_scale=1,
    apply_unit_scale=True,apply_scale_options='FBX_SCALE_NONE',add_leaf_bones=False,
    bake_anim=False,mesh_smooth_type='FACE',use_mesh_modifiers=True,
    armature_nodetype='NULL',use_armature_deform_only=False,path_mode='AUTO')
for m,name in material_names:
    m.name=name
for m,links in image_links:
    for source_socket,target_socket in links:
        m.node_tree.links.new(source_socket,target_socket)
bpy.ops.export_scene.gltf(filepath=str(SOURCE/'silver_casual.glb'),export_format='GLB',
    use_selection=True,export_animations=False,export_yup=True,export_skins=True,
    export_materials='EXPORT',export_image_format='AUTO',export_texcoords=True)

scene=bpy.context.scene
scene.render.engine='BLENDER_EEVEE'
scene.render.resolution_x=768
scene.render.resolution_y=1024
scene.render.resolution_percentage=100
scene.render.image_settings.file_format='PNG'
scene.render.film_transparent=False
scene.world.color=(.08,.08,.08)
scene.view_settings.view_transform='Standard'
scene.view_settings.exposure=0
scene.view_settings.look='None'
camera_data=bpy.data.cameras.new('Preview camera')
camera=bpy.data.objects.new('Preview camera',camera_data)
scene.collection.objects.link(camera)
camera_data.type='ORTHO'
camera_data.ortho_scale=2.16
scene.camera=camera
for location,energy,size in [((3,-2,3),210,4),((2,3,2),150,4),((-2,1,3),190,3)]:
    data=bpy.data.lights.new('Softbox','AREA')
    data.energy=energy
    data.size=size
    obj=bpy.data.objects.new('Softbox',data)
    scene.collection.objects.link(obj)
    obj.location=location
    obj.rotation_euler=(Vector((0,CY,1))-obj.location).to_track_quat('-Z','Y').to_euler()
for label,location in [('front',(4,CY,1.0)),('three_quarter',(3,-1.3,1.2)),('back',(-4,CY,1.0)),('side',(0,-4,1.0))]:
    camera.location=location
    camera.rotation_euler=(Vector((0,CY,1))-camera.location).to_track_quat('-Z','Y').to_euler()
    scene.render.filepath=str(BUILD/'renders'/f'{label}.png')
    bpy.ops.render.render(write_still=True)
camera_data.ortho_scale=.46
scene.render.resolution_x=1024
scene.render.resolution_y=1024
for label,location in [('head_front',(4,CY,1.78)),('head_three_quarter',(3,-1.3,1.78))]:
    camera.location=location
    camera.rotation_euler=(Vector((0,CY,1.78))-camera.location).to_track_quat('-Z','Y').to_euler()
    scene.render.filepath=str(BUILD/'renders'/f'{label}.png')
    bpy.ops.render.render(write_still=True)
camera_data.ortho_scale=2.16
scene.render.resolution_x=768
scene.render.resolution_y=1024
camera.location=(3,-1.3,1.2)
camera.rotation_euler=(Vector((0,CY,1))-camera.location).to_track_quat('-Z','Y').to_euler()
bpy.ops.wm.save_as_mainfile(filepath=str(SOURCE/'silver_casual.blend'))
report={'bones':len(rig.data.bones),'parts':[{ 'name':o.name,'vertices':len(o.data.vertices),'polygons':len(o.data.polygons)} for o in parts],
        'face':face_report,'head_uv':head_uv_report,'head_detail':detail_report,'clothing':clothing_report,'normalized_vertices':normalized_vertices,'fbx':str(MODEL/'silver_casual.fbx'),'glb':str(SOURCE/'silver_casual.glb')}
(SOURCE/'geometry.json').write_text(json.dumps(report,indent=2)+'\n')
print('SILVER_CASUAL_READY',json.dumps(report))
