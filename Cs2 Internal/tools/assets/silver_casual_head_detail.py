"""Project the dedicated high-detail head reference without changing its pixels.

Landmarks are calibrated to the native donor's eyes, nose, closed lips and chin.
Body clothing keeps its original atlas; head/hair and exposed skin use this map.
"""
import bpy
from silver_casual_head_uv import _SkinSamples, _atlas_pixels


def _height(z):
    # Pixel rows in the 1536x1024 generated head reference, in ascending Z.
    knots = [(1.59, 852), (1.64, 730), (1.686, 650), (1.733, 530),
             (1.775, 455), (1.813, 364), (1.830, 330), (1.895, 185),
             (1.94, 95), (1.968, 65)]
    for (a, u), (b, v) in zip(knots, knots[1:]):
        if z <= b:
            return u + (v-u) * max(0, (z-a)/(b-a))
    return knots[-1][1]


def _face_column(column,row):
    # Stay inside the portrait's skin silhouette at the temples/jaw. Hair and
    # the photograph's ears are supplied by our geometry instead.
    limits=[(185,155,340),(220,145,355),(320,122,378),(400,116,382),
            (480,124,374),(550,135,365),(620,153,345),(660,176,320)]
    low,high=limits[-1][1:]
    for (a,l0,h0),(b,l1,h1) in zip(limits,limits[1:]):
        if row<=b:
            t=max(0,min(1,(row-a)/(b-a)))
            low=l0+(l1-l0)*t;high=h0+(h1-h0)*t
            break
    return max(low,min(high,column))


def apply_head_detail(parts, rig, atlas_path):
    image = bpy.data.images.load(str(atlas_path), check_existing=True)
    image.name = 'head_detail'
    pixels, width, height = _atlas_pixels(image)
    material = bpy.data.materials.new('silver_casual_head')
    material.use_nodes = True
    shader = material.node_tree.nodes.get('Principled BSDF')
    shader.inputs['Roughness'].default_value = .70
    shader.inputs['Specular IOR Level'].default_value = .24
    node = material.node_tree.nodes.new('ShaderNodeTexImage')
    node.image = image
    node.interpolation = 'Linear'
    material.node_tree.links.new(node.outputs['Color'], shader.inputs['Base Color'])
    front = _SkinSamples(pixels,width,height,(69,172,438,710),facial_interior=True)
    ear_skin = _SkinSamples(pixels,width,height,(115,413,381,490))
    neck = _SkinSamples(pixels,width,height,(117,662,403,853),facial_interior=True)
    back = _SkinSamples(pixels,width,height,(1130,567,1440,850),facial_interior=True)
    hair = {'front': _SkinSamples(pixels,width,height,(42,73,451,378),hair=True),
            'side': _SkinSamples(pixels,width,height,(565,73,1017,575),hair=True),
            'back': _SkinSamples(pixels,width,height,(1080,70,1495,580),hair=True)}
    cy = (rig.matrix_world @ rig.data.bones['head_0'].head_local).y
    eyes_center_y = cy + .001292279125
    report = {'atlas': str(atlas_path), 'size': [width,height], 'material': material.name,
              'front_eye_pixels': [184,310,364], 'parts': []}
    for obj in parts:
        kind = str(obj.get('projection_kind',''))
        if not (kind in {'head','hair'} or kind.startswith(('hand_','forearm_'))):
            continue
        obj.data.materials.clear()
        obj.data.materials.append(material)
        uv = obj.data.uv_layers.active
        if uv is None:
            raise ValueError('Detailed head part has no UV layer: '+obj.name)
        vertices = obj.data.vertices
        for polygon in obj.data.polygons:
            polygon.material_index = 0
            center = obj.matrix_world @ polygon.center
            rear_chart = center.x < .012 and abs(center.y-cy)<.074
            for loop_index in polygon.loop_indices:
                vertex = vertices[obj.data.loops[loop_index].vertex_index]
                p = obj.matrix_world @ vertex.co
                lateral = p.y-cy
                row = _height(p.z)
                if kind == 'hair':
                    # A triangle must stay inside one photo chart: interpolating
                    # from front to rear UVs would cross the atlas background.
                    if center.x >= .010:
                        value = hair['front'].uv(247+(p.y-eyes_center_y)*1970.26,row)
                    else:
                        value = hair['back'].uv(1284-lateral*1970.26,row+8)
                elif kind.startswith(('hand_','forearm_')):
                    # Coherent world-space sampling keeps the wrist seam the
                    # same tone on both meshes and avoids horizontal throat lines.
                    value = back.uv(1350+(p.x-.03)*140,730+(p.y-cy)*60+(p.z-.5)*60)
                elif 'mouth' in obj.name.lower():
                    value = front.uv(247+lateral*1970.26,531)
                elif p.z < 1.67:
                    value = neck.uv(247+lateral*1600,row)
                elif rear_chart:
                    value = back.uv(1284-lateral*1700,max(590,row))
                else:
                    value = front.uv(_face_column(247+(p.y-eyes_center_y)*1970.26,row),row)
                    # Both UVs are in the same continuous frontal skin chart.
                    # Grade into pore detail on modeled ears instead of making
                    # a visible hard boundary or projecting a second ear.
                    side_blend=min(1,max(0,(abs(lateral)-.060)/.035))
                    side_blend*=min(1,max(0,(.095-p.x)/.05))
                    ear=ear_skin.uv((355 if lateral>0 else 140)+p.x*80,447+(p.z-1.77)*120)
                    value=tuple(a*(1-side_blend)+b*side_blend for a,b in zip(value,ear))
                    if p.z < 1.720 and p.x>.055:
                        # Under-chin vertices share a graded neck sample, preserving
                        # continuity across adjacent triangles of the same chart.
                        blend=min(1,max(0,(-vertex.normal.z-.10)/.55))
                        blend*=min(1,max(0,(1.720-p.z)/.035))
                        shadow=neck.uv(247+lateral*1000,695)
                        value=tuple(a*(1-blend)+b*blend for a,b in zip(value,shadow))
                uv.data[loop_index].uv = value
        report['parts'].append(obj.name)
    image.pack()
    return material, image, report
