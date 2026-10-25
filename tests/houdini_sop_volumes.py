"""SOP volumes in Solaris with Eldr (Houdini build, docs/houdini.md): a fog
VDB (OpenVDBAsset) on the left and a Houdini volume from Iso Offset
(HoudiniFieldAsset) on the right, both with op: paths from a SOP Import
LOP, rendered through UsdImagingGL on the LOP's stage. Fails unless both
volumes show. Not part of ctest.
Run: hython tests\\houdini_sop_volumes.py <out.png>
with HOUDINI_PACKAGE_DIR=<build-h22>\\houdini."""
import sys

import hou
from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, UsdImagingGL

out = sys.argv[1]
geo = hou.node('/obj').createNode('geo', 'vols')
sphere = geo.createNode('sphere')
vdb = geo.createNode('vdbfrompolygons')
vdb.setInput(0, sphere)
vdb.parm('builddistance').set(0)
vdb.parm('buildfog').set(1)
vdb.parm('voxelsize').set(0.05)
left = geo.createNode('xform')
left.setInput(0, vdb)
left.parm('tx').set(-1.6)
iso = geo.createNode('isooffset')  # a Houdini (not VDB) fog volume
iso.setInput(0, sphere)
iso.parm('output').set('fogvolume')
named = geo.createNode('name')  # Eldr shades the field named density
named.setInput(0, iso)
named.parm('name1').set('density')
right = geo.createNode('xform')
right.setInput(0, named)
right.parm('tx').set(1.6)
ball = geo.createNode('xform')
ball.setInput(0, sphere)
ball.parm('scale').set(0.3)
ball.parm('ty').set(-0.8)
merge = geo.createNode('merge')
for i, node in enumerate((ball, right, left)):  # a polygon prim first: numbering skips nothing
    merge.setInput(i, node)
merge.setDisplayFlag(True)

lop = hou.node('/stage').createNode('sopimport')
lop.parm('soppath').set(merge.path())
light = hou.node('/stage').createNode('distantlight::2.0')
light.setInput(0, lop)
light.parm('rx').set(-40)
light.parm('ry').set(30)
stage = light.stage()
types = sorted(p.GetTypeName() for p in stage.Traverse() if p.GetTypeName().endswith('Asset'))
assert types == ['HoudiniFieldAsset', 'OpenVDBAsset'], types

W, H = 384, 256
app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(W, H))
fbo.bind()
frustum = Gf.Frustum()
frustum.SetPerspective(40, W / H, 0.1, 100)
frustum.SetPosition(Gf.Vec3d(0, 0.3, 6))
engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "Eldr not available"
engine.SetRenderViewport(Gf.Vec4d(0, 0, W, H))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
for _ in range(100):
    engine.Render(stage.GetPseudoRoot(), params)
    if engine.IsConverged():
        break
image = fbo.toImage()
image.save(out)
del engine  # before the GL context goes

background = image.pixelColor(W // 2, 10).value()
for name, x in (('VDB (OpenVDBAsset)', W // 4), ('Houdini volume (HoudiniFieldAsset)', 3 * W // 4)):
    v = image.pixelColor(x, H // 2).value()
    print(f"{name}: {v} (background {background})")
    assert v > background + 8, f"{name} missing"
print("ok, saved", out)
