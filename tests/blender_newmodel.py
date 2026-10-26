"""Blender (headless): a brand-new model (no x3_* data) - a bevelled cube with a textured material and a second UV
set - exported to FBX, to test x3model's import of models made from scratch.

    blender -b --factory-startup -P blender_newmodel.py -- <out.fbx> <texture file>
"""
import sys

import bpy

argv = sys.argv[sys.argv.index('--') + 1:]
out, tex = argv[0], argv[1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add(size=20)
cube = bpy.context.active_object
cube.name = 'MyBox'
bpy.ops.object.modifier_add(type='BEVEL')
cube.modifiers[0].width = 2
bpy.ops.object.modifier_apply(modifier=cube.modifiers[0].name)
cube.data.uv_layers.new(name='UVMap2')
mat = bpy.data.materials.new('box_mat')
mat.use_nodes = True
img = bpy.data.images.load(tex)
node = mat.node_tree.nodes.new('ShaderNodeTexImage')
node.image = img
mat.node_tree.links.new(node.outputs['Color'], mat.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
cube.data.materials.append(mat)
bpy.ops.export_scene.fbx(filepath=out, path_mode='RELATIVE')
print('SAVED', out)
