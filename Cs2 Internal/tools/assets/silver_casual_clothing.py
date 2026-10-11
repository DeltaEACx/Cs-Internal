"""Modeled garment folds, trims and sneaker laces for Silver Casual."""
import math
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree


def refine_clothing(parts, rig, material, arm_data, create_part, cy):
    modeled_vertices = modeled_triangles = surface_paths = skipped_paths = 0
    # A little actual surface relief remains visible under game lighting rather
    # than depending entirely on the lighting photographed into the atlas.
    garments=[obj for obj in parts if obj.get('projection_kind')=='body' or
              str(obj.get('projection_kind','')).startswith('sleeve_')]
    for obj in garments:
        bpy.context.view_layer.objects.active=obj
        sub=obj.modifiers.new('Soft knit topology','SUBSURF')
        sub.levels=1
        obj.modifiers.move(len(obj.modifiers)-1,0)
        bpy.ops.object.modifier_apply(modifier=sub.name)
        kind=obj['projection_kind']
        for vertex in obj.data.vertices:
            point=vertex.co
            if kind=='body':
                theta=math.atan2((point.y-cy)/.20,(point.x-.012)/.12)
                hem=math.exp(-((point.z-1.16)/.09)**2)
                armpit=math.exp(-((point.z-1.43)/.075)**2)*abs(math.sin(theta))**5
                relief=.0032*hem*math.sin(point.z*82+math.sin(theta*3)*1.7)
                relief+=.0025*armpit*math.sin(point.z*105+theta*2)
                direction=Vector((math.cos(theta),math.sin(theta)*.65,0)).normalized()
            else:
                shoulder,elbow,wrist=arm_data[kind[-1]]
                delta=elbow-shoulder
                t=max(0,min(1.30,(point-shoulder).dot(delta)/delta.length_squared))
                center=shoulder.lerp(elbow,t)
                direction=(point-center).normalized()
                theta=math.atan2(direction.z,direction.x)
                relief=.0021*math.exp(-((t-.94)/.33)**2)*math.sin(t*39+theta*1.5)
            vertex.co+=direction*relief
        obj.data.update()

    # Cuffs/hem/collar have fabric thickness. These short bands overlap the
    # underlying surface and maintain the existing smooth deformation weights.
    for obj in parts:
        if obj.get('projection_kind')=='collar':
            bpy.context.view_layer.objects.active=obj
            solid=obj.modifiers.new('Collar fabric thickness','SOLIDIFY')
            solid.thickness=.002
            solid.offset=-.5
            obj.modifiers.move(len(obj.modifiers)-1,0)
            bpy.ops.object.modifier_apply(modifier=solid.name)

    def tube(name, points, radius, kind, weight_fn, sides=6, closed=False):
        nonlocal modeled_vertices, modeled_triangles
        if closed and (points[0]-points[-1]).length < .000001:
            points=points[:-1]
        if len(points)<2:return None
        vertices=[];faces=[];weights=[]
        for i,point in enumerate(points):
            before=points[(i-1)%len(points)] if closed else points[max(0,i-1)]
            after=points[(i+1)%len(points)] if closed else points[min(i+1,len(points)-1)]
            direction=(after-before).normalized()
            cross=direction.cross(Vector((0,0,1)))
            if cross.length<.1:cross=direction.cross(Vector((1,0,0)))
            cross.normalize();other=direction.cross(cross).normalized()
            ring_weights=weight_fn(point)
            for j in range(sides):
                angle=math.tau*j/sides
                vertices.append(point+radius*(math.cos(angle)*cross+math.sin(angle)*other))
                weights.append(ring_weights)
        for i in range(len(points) if closed else len(points)-1):
            for j in range(sides):
                k=(j+1)%sides
                following=(i+1)%len(points)
                faces.append((i*sides+j,i*sides+k,following*sides+k,following*sides+j))
        if not closed:
            faces.append(tuple(reversed(range(sides))))
            faces.append(tuple((len(points)-1)*sides+j for j in range(sides)))
        modeled_vertices+=len(vertices)
        modeled_triangles+=sum(len(face)-2 for face in faces)
        return create_part(name,vertices,faces,weights,kind)

    # New stitching is fitted to the actual garment after subdivision/folds,
    # rather than floating on an idealized cylinder. Interpolating the same
    # triangle's skin weights also keeps seams attached through bent poses.
    surfaces={}
    def surface(obj):
        key=obj.as_pointer()
        if key not in surfaces:
            obj.data.calc_loop_triangles()
            vertices=[v.co.copy() for v in obj.data.vertices]
            triangles=[tuple(t.vertices) for t in obj.data.loop_triangles]
            vertex_weights=[]
            for vertex in obj.data.vertices:
                entries={obj.vertex_groups[g.group].name:g.weight for g in vertex.groups if g.weight>.00001}
                vertex_weights.append(entries)
            surfaces[key]=(vertices,triangles,vertex_weights,
                BVHTree.FromPolygons(vertices,triangles,all_triangles=True))
        return surfaces[key]

    def weights_at(point, vertices, triangles, vertex_weights, tree):
        nearest,normal,index,distance=tree.find_nearest(point)
        if nearest is None:return {'pelvis':1.0}
        ids=triangles[index];a,b,c=(vertices[i] for i in ids)
        ab=b-a;ac=c-a;ap=nearest-a
        aa=ab.dot(ab);bb=ac.dot(ac);cross=ab.dot(ac)
        divisor=aa*bb-cross*cross
        if abs(divisor)<1e-15:
            factors=(1.,0.,0.)
        else:
            second=(bb*ap.dot(ab)-cross*ap.dot(ac))/divisor
            third=(aa*ap.dot(ac)-cross*ap.dot(ab))/divisor
            factors=(max(0.,1-second-third),max(0.,second),max(0.,third))
        combined={}
        for index,factor in zip(ids,factors):
            for bone,value in vertex_weights[index].items():
                combined[bone]=combined.get(bone,0.)+value*factor
        best=sorted(combined.items(),key=lambda item:-item[1])[:4]
        total=sum(value for bone,value in best)
        return {bone:value/total for bone,value in best if value>0} if total else {'pelvis':1.0}

    def surface_seam(name,obj,guides,radius=.0005,closed=False):
        nonlocal surface_paths,skipped_paths
        vertices,triangles,vertex_weights,tree=surface(obj)
        points=[]
        for guide in guides:
            nearest,normal,index,distance=tree.find_nearest(guide)
            # Do not bridge a missing garment patch or project a remote guide
            # across the body. Most of the thread sits inside the cloth surface.
            if nearest is None or distance>.04:
                skipped_paths+=1;return None
            point=nearest+normal*.00018
            if not points or (point-points[-1]).length>.000001:points.append(point)
        if len(points)<3:
            skipped_paths+=1;return None
        surface_paths+=1
        return tube(name,points,radius,obj['projection_kind'],
                    lambda point:weights_at(point,vertices,triangles,vertex_weights,tree),4,closed)

    def polyline(corners,steps=5):
        return [corners[i].lerp(corners[i+1],step/steps)
                for i in range(len(corners)-1) for step in range(steps)]+[corners[-1]]

    body=next((obj for obj in garments if obj['projection_kind']=='body'),None)
    if body:
        for z in (1.073,1.091):
            guides=[Vector((.012+.122*math.cos(math.tau*i/36),
                            cy+.197*math.sin(math.tau*i/36),z)) for i in range(36)]
            surface_seam('Quarter zip hem stitch '+str(z),body,guides,.00045,True)
    for side in ('l','r'):
        sleeve=next((obj for obj in garments if obj['projection_kind']=='sleeve_'+side),None)
        if not sleeve:continue
        shoulder,elbow,wrist=arm_data[side]
        along=(wrist-elbow).normalized()
        across=along.cross(Vector((1,0,0))).normalized()
        for t in (.217,.263):
            center=elbow.lerp(wrist,t)
            guides=[center+Vector((.052*.95*math.cos(math.tau*i/28),0,0))+
                    across*(.052*math.sin(math.tau*i/28)) for i in range(28)]
            surface_seam('Folded cuff stitching '+side+' '+str(t),sleeve,guides,.00048,True)
        along=(elbow-shoulder).normalized()
        across=along.cross(Vector((1,0,0))).normalized()
        if across.z<0:across=-across
        center=shoulder.lerp(elbow,.18)
        guides=[center+Vector((.077*.95*math.cos(math.pi*i/16),0,0))+
                across*(.077*math.sin(math.pi*i/16)) for i in range(17)]
        surface_seam('Shoulder inset stitching '+side,sleeve,guides,.00045)

    pants=next((obj for obj in parts if obj.get('projection_kind')=='pants'),None)
    if pants:
        for side,sign in (('l',1),('r',-1)):
            y=cy+sign*.097
            corners=[Vector((-.086,y-.033,1.035)),Vector((-.086,y+.033,1.035)),
                     Vector((-.084,y+.029,.973)),Vector((-.082,y,.955)),
                     Vector((-.084,y-.029,.973)),Vector((-.086,y-.033,1.035))]
            surface_seam('Denim back pocket stitching '+side,pants,polyline(corners),.0005,True)
            guides=[Vector((.105,cy+sign*(.082+.068*t),1.040-.076*math.sin(t*math.pi/2)))
                    for t in [i/12 for i in range(13)]]
            surface_seam('Denim front pocket opening '+side,pants,guides,.00055)
            hip=rig.data.bones['leg_upper_'+side].head_local
            knee=rig.data.bones['leg_lower_'+side].head_local
            ankle=rig.data.bones['ankle_'+side].head_local
            guides=[]
            for i in range(33):
                z=.187+i/32*.837
                start,end=(ankle,knee) if z<knee.z else (knee,hip)
                t=max(0,min(1,(z-start.z)/max(.0001,end.z-start.z)))
                center=start.lerp(end,t)
                width=.036+.035*max(0,min(1,(z-.22)/.80))
                guides.append(Vector((center.x,center.y+sign*width,z)))
            surface_seam('Denim outside leg seam '+side,pants,guides,.0005)
        for front in (True,False):
            guides=[Vector((.104 if front else -.085,cy+( .006 if front else 0),1.041-i/12*.100))
                    for i in range(13)]
            surface_seam('Denim '+('fly' if front else 'seat')+' seam',pants,guides,.0005)

    for side in ('l','r'):
        shoe=next((obj for obj in parts if obj.name=='White sneaker upper '+side),None)
        if not shoe:continue
        y=rig.data.bones['ankle_'+side].head_local.y
        for sign in (-1,1):
            corners=[Vector((-.092,y+sign*.046,.071)),Vector((-.066,y+sign*.044,.118)),
                     Vector((-.026,y+sign*.041,.150)),Vector((.043,y+sign*.051,.116)),
                     Vector((.126,y+sign*.042,.072))]
            surface_seam('Sneaker side panel stitching '+side+' '+str(sign),shoe,polyline(corners,3),.0005)
        guides=[Vector((.127+.034*math.cos(-math.pi/2+math.pi*i/16),
                        y+.047*math.sin(-math.pi/2+math.pi*i/16),
                        .104-.021*math.cos(-math.pi/2+math.pi*i/16))) for i in range(17)]
        surface_seam('Sneaker toe panel stitching '+side,shoe,guides,.00048)
        guides=[Vector((-.095,y,.072+i/8*.081)) for i in range(9)]
        surface_seam('Sneaker heel panel stitching '+side,shoe,guides,.0005)

    # Preserve the articulated donor hand topology and weights. A fraction of a
    # millimeter of relief at finger bases makes knuckles read under native light.
    hand_vertices=0
    for obj in parts:
        kind=str(obj.get('projection_kind',''))
        if not kind.startswith('hand_'):continue
        side=kind[-1]
        joints=[rig.data.bones.get('finger_'+finger+'_0_'+side)
                for finger in ('index','middle','ring','pinky')]
        joints=[joint.head_local for joint in joints if joint]
        for vertex in obj.data.vertices:
            if vertex.normal.length<.5:continue
            relief=sum(.00032*math.exp(-(vertex.co-joint).length_squared/.000080) for joint in joints)
            if relief>.00001:
                vertex.co+=vertex.normal*min(.00042,relief);hand_vertices+=1
        obj.data.update()

    for side in ('l','r'):
        ankle=rig.data.bones['ankle_'+side].head_local
        def foot_weights(point,s=side):
            amount=max(0,min(1,(point.x-.045)/.13))
            return {'ankle_'+s:1-amount,'ball_'+s:amount}
        tongue_vertices=[];tongue_faces=[];tongue_weights=[]
        for row in range(13):
            t=row/12
            for col in range(9):
                across=(col-4)/4
                point=Vector((-.025+t*.132,ankle.y+across*(.031+t*.014),
                              .190-t*.060-.007*across*across))
                tongue_vertices.append(point);tongue_weights.append(foot_weights(point))
        for row in range(12):
            for col in range(8):
                i=row*9+col;tongue_faces.append((i,i+1,i+10,i+9))
        create_part('Padded sneaker tongue '+side,tongue_vertices,tongue_faces,tongue_weights,'sole')
        # Cross lacing has real depth, narrow eyelet rails and a small knot.
        # The shape follows the sneaker's sloping instep, beneath the denim cuff.
        for row in range(6):
            x=-.010+row*.016
            z=.185-row*.0073
            width=.030+row*.0014
            for diagonal in (-1,1):
                points=[]
                for step in range(9):
                    t=step/8
                    points.append(Vector((x+(t-.5)*.009*diagonal,
                        ankle.y+(t-.5)*width*2,z+.0025*math.sin(t*math.pi)+.0004*diagonal)))
                tube('Woven lace '+side+' '+str(row)+' '+str(diagonal),points,.00115,'lace',foot_weights)
        for sign in (-1,1):
            points=[Vector((-.018+t*.104,ankle.y+sign*(.032+t*.009),.184-t*.044))
                    for t in [i/20 for i in range(21)]]
            tube('Lace eyelet rail '+side+' '+str(sign),points,.0016,'sole',foot_weights)
        # A faint raised stitch around the upper edge of the rubber sole.
        points=[Vector((.047+.156*math.cos(math.tau*i/96),
                        ankle.y+.0655*math.sin(math.tau*i/96),.047)) for i in range(97)]
        tube('Sole welt '+side,points,.0008,'sole',foot_weights,5,True)

    # Short quarter-zip teeth; their geometry catches light at close range.
    def chest(point):return {'spine_2':.25,'spine_3':.75}
    for row in range(24):
        z=1.469+row*.0047
        depth=.139-(z-1.47)*.129 if z<=1.54 else .130-(z-1.54)*.34
        for sign in (-1,1):
            tube('Zipper tooth '+str(row)+' '+str(sign),
                 [Vector((depth,cy+sign*.0006,z)),Vector((depth,cy+sign*.0025,z))],
                 .00052,'zipper',chest,4)
    return {'garments_refined':len(garments),'modeled_lace_rows':12,'zipper_teeth':48,
            'surface_seam_paths':surface_paths,'skipped_surface_paths':skipped_paths,
            'modeled_detail_vertices':modeled_vertices,'modeled_detail_triangles':modeled_triangles,
            'hand_vertices_refined':hand_vertices}
