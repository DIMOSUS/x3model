"""Blender (headless) check of an exported FBX: import it, report what Blender sees, render a preview of LOD 0.

    blender -b --factory-startup -P blender_preview.py -- <in.fbx> <out.png> [view]

view: front (default), side, top, persp. Prints one line per mesh object (vertices, faces, UV layers, materials,
custom properties) and the count of faces whose winding disagrees with the vertex normals (they would render
inside-out).
"""
import math
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index('--') + 1:]
src, out = argv[0], argv[1]
view = argv[2] if len(argv) > 2 else 'persp'

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=src)

meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
lod0 = [o for o in meshes if '_LOD0_' in o.name] or meshes
for o in meshes:
    o.hide_render = o not in lod0
    me = o.data
    bad = 0
    me.calc_loop_triangles() if hasattr(me, 'calc_loop_triangles') else None
    for p in me.polygons:
        n = Vector((0, 0, 0))
        for li in p.loop_indices:
            n += me.corner_normals[li].vector if hasattr(me, 'corner_normals') else me.loops[li].normal
        if n.length > 0 and n.dot(p.normal) < 0:
            bad += 1
    props = [k for k in o.keys() if k.startswith('x3_')]
    print('MESH %s: %d verts, %d faces, uv %s, %d materials, %d x3 props, %d faces against their normals'
          % (o.name, len(me.vertices), len(me.polygons), [u.name for u in me.uv_layers], len(me.materials),
             len(props), bad))
print('MATERIALS', len(bpy.data.materials), 'IMAGES', [i.name for i in bpy.data.images][:8])

# frame LOD 0
pts = [o.matrix_world @ Vector(c) for o in lod0 for c in o.bound_box]
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
centre, size = (lo + hi) / 2, max((hi - lo).length, 1e-6)
print('BOUNDS', tuple(round(v, 3) for v in lo), tuple(round(v, 3) for v in hi))
dirs = {'front': Vector((0, -1, 0)), 'side': Vector((1, 0, 0)), 'top': Vector((0, 0, 1)),
        'persp': Vector((0.8, -1.0, 0.6)).normalized(), 'back': Vector((0, 1, 0))}
d = dirs[view]
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
bpy.context.scene.collection.objects.link(cam)
cam.location = centre + d * size * 1.6
cam.rotation_euler = (centre - cam.location).to_track_quat('-Z', 'Y').to_euler()
cam.data.clip_end = size * 10
cam.data.clip_start = size * 0.001
bpy.context.scene.camera = cam
sun = bpy.data.objects.new('sun', bpy.data.lights.new('sun', 'SUN'))
sun.rotation_euler = (math.radians(50), 0, math.radians(30))
bpy.context.scene.collection.objects.link(sun)
sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'
sc.display.shading.color_type = 'TEXTURE'
sc.display.shading.show_backface_culling = True
sc.render.resolution_x, sc.render.resolution_y = 1024, 640
sc.render.filepath = out
bpy.ops.render.render(write_still=True)
print('RENDERED', out)
