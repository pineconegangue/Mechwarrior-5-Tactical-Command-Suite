# =====================================================================================================
# build_ei_materials.py - builds the Enhanced Imaging .pak content in the MW5 Mercenaries mod editor
# (Unreal 4.26) from EI_Vision.ush. Re-run it after every shader change; it rebuilds everything.
#
# Run: Window > Developer Tools > Output Log, switch the input box from "Cmd" to "Python", then
#        exec(open(r"D:\Selfmade\mwvr POC\pak\build_ei_materials.py").read())
#      (or: py "D:\Selfmade\mwvr POC\pak\build_ei_materials.py" in Cmd mode).
# Needs: Edit > Plugins > "Python Editor Script Plugin" and "Editor Scripting Utilities" enabled.
#
# Makes, under MOD_ROOT (the mod's content folder - CameraCapture loads /EnhancedImaging/EI/...):
#   RT_EI_UnitsDefault   4x4 R32f render target - the UnitsTex default (linear, like the real one)
#   M_EI_Vision_BT       post-process, Before Translucency  (explosions / MWVR's in-world HUD on top)
#   M_EI_Vision_AT       post-process, After Tonemapping    (everything under the vision)
# Both: one Custom node holding EI_Vision.ush, its parameters, a SceneTexture:SceneDepth node (needed for
# SceneTextureLookup to compile), and an EI_PakVersion parameter CameraCapture checks.
# =====================================================================================================
import os
import unreal

MOD_ROOT = "/EnhancedImaging/EI"   # change if your mod is named differently (and CameraCapture's kPakMat)
PAK_VERSION = 1                    # keep in step with CameraCapture's kEiPakVersion

HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else r"D:\Selfmade\mwvr POC\pak"
CODE = open(os.path.join(HERE, "EI_Vision.ush"), "r", encoding="utf-8").read()

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(msg):
    unreal.log("[EI pak] " + msg)


def fresh(name, cls, factory):
    path = MOD_ROOT + "/" + name
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)
    return tools.create_asset(name, MOD_ROOT, cls, factory)


# ---- the units default texture: a tiny R32f render target (linear, as the runtime ones are) ---------
rt = fresh("RT_EI_UnitsDefault", unreal.TextureRenderTarget2D, unreal.TextureRenderTargetFactoryNew())
rt.set_editor_property("render_target_format", unreal.TextureRenderTargetFormat.RTF_R32F)
rt.set_editor_property("size_x", 4)
rt.set_editor_property("size_y", 4)
rt.set_editor_property("clear_color", unreal.LinearColor(0, 0, 0, 0))
EAL.save_loaded_asset(rt)

SCALARS = [  # name, default (the current look)
    ("Opacity", 0.0), ("WaveOn", 0.0), ("Front", 0.0), ("Band", 400.0), ("Fade", 5000.0),
    ("Ripple", 0.006), ("UnitsOn", 0.0), ("EdgeGain", 1.0),
]
VECTORS = [
    ("EdgeCol", unreal.LinearColor(0.33, 1.00, 0.54, 1)),
    ("UnitCol", unreal.LinearColor(0.62, 1.00, 0.72, 1)),
    ("GlowCol", unreal.LinearColor(0.33, 1.00, 0.54, 1)),
    ("BgCol", unreal.LinearColor(0.03, 0.055, 0.055, 1)),
]


def build(name, location, use_exposure):
    mat = fresh(name, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_POST_PROCESS)
    mat.set_editor_property("blendable_location", location)
    # Highest priority: drawn after every other mod's post-process at the same stage (UltraVisualAndWeather's
    # fog, YAML's thermal / night vision), so nothing is laid over the vision. Load order does not affect this.
    mat.set_editor_property("blendable_priority", 10000)

    custom = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, -400, 0)
    custom.set_editor_property("code", CODE)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("description", "EI_Vision")
    names = ["UnitsTex"] + [n for n, _ in SCALARS] + [n for n, _ in VECTORS] + ["Exposure", "SceneKeep"]
    inputs = []
    for n in names:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        inputs.append(ci)
    custom.set_editor_property("inputs", inputs)

    y = -600
    tex = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureObjectParameter, -900, y)
    tex.set_editor_property("parameter_name", "UnitsTex")
    tex.set_editor_property("texture", rt)
    tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    MEL.connect_material_expressions(tex, "", custom, "UnitsTex")
    y += 80
    for n, v in SCALARS:
        e = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -900, y)
        e.set_editor_property("parameter_name", n)
        e.set_editor_property("default_value", v)
        MEL.connect_material_expressions(e, "", custom, n)
        y += 60
    for n, v in VECTORS:
        e = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -900, y)
        e.set_editor_property("parameter_name", n)
        e.set_editor_property("default_value", v)
        MEL.connect_material_expressions(e, "", custom, n)
        y += 120
    if use_exposure:
        ex = MEL.create_material_expression(mat, unreal.MaterialExpressionEyeAdaptation, -900, y)
    else:
        ex = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -900, y)
        ex.set_editor_property("r", 1.0)
    MEL.connect_material_expressions(ex, "", custom, "Exposure")
    y += 80
    st = MEL.create_material_expression(mat, unreal.MaterialExpressionSceneTexture, -900, y)
    st.set_editor_property("scene_texture_id", unreal.SceneTextureId.PPI_SCENE_DEPTH)
    MEL.connect_material_expressions(st, "Color", custom, "SceneKeep")
    y += 100
    # the version CameraCapture reads (GetScalarParameterValue): a parameter only exists if it reaches the
    # output, so it is added to the result multiplied by 0.
    ver = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -900, y)
    ver.set_editor_property("parameter_name", "EI_PakVersion")
    ver.set_editor_property("default_value", float(PAK_VERSION))
    zero = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -650, y)
    zero.set_editor_property("const_b", 0.0)
    MEL.connect_material_expressions(ver, "", zero, "A")
    add = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, -150, 0)
    MEL.connect_material_expressions(custom, "", add, "A")
    MEL.connect_material_expressions(zero, "", add, "B")
    MEL.connect_material_property(add, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)
    log("built " + MOD_ROOT + "/" + name)


build("M_EI_Vision_BT", unreal.BlendableLocation.BL_BEFORE_TRANSLUCENCY, True)
build("M_EI_Vision_AT", unreal.BlendableLocation.BL_AFTER_TONEMAPPING, False)
log("done - EI_PAK %d. Open each material once to check it compiled (no red errors), then package the mod." % PAK_VERSION)
