"""Blender (headless): import an FBX written by x3model and export it again with Blender's own exporter, the way a
modder's file comes back. Custom properties (the x3_* data) are kept.

    blender -b --factory-startup -P blender_resave.py -- <in.fbx> <out.fbx> [edit]

edit: also change the geometry (subdivide LOD 0's first mesh) to exercise the importer's derived path.
"""
import sys

import bpy

argv = sys.argv[sys.argv.index('--') + 1:]
src, dst = argv[0], argv[1]
edit = len(argv) > 2 and argv[2] == 'edit'

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=src)
if edit:
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH' and '_LOD0_' in o.name]
    o = meshes[0]
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.subdivide(number_cuts=1)
    bpy.ops.object.mode_set(mode='OBJECT')
    print('EDITED', o.name, len(o.data.vertices), 'vertices')
bpy.ops.export_scene.fbx(filepath=dst, use_custom_props=True, mesh_smooth_type='OFF', path_mode='RELATIVE')
print('SAVED', dst)
