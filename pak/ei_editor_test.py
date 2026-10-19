# ei_editor_test.py - TEST ONLY (nothing saved): shows M_EI_Vision_AT/BT in the mod editor's viewport on an
# unbound PostProcessVolume, with a cube in front of the camera so there are edges to see.
# Run in the Output Log (Cmd): py "D:\Selfmade\mwvr POC\pak\ei_editor_test.py" [AT|BT|red|off]
import sys, unreal
ELL = unreal.EditorLevelLibrary
arg = (sys.argv[1] if len(sys.argv) > 1 else "AT").upper()
for a in ELL.get_all_level_actors():
    if a.get_actor_label().startswith("EI_TEST"):
        ELL.destroy_actor(a)
if arg == "OFF":
    if unreal.EditorAssetLibrary.does_asset_exist("/Game/EI_TEST_MI"):
        unreal.EditorAssetLibrary.delete_asset("/Game/EI_TEST_MI")
    unreal.log("[EI test] removed")
if arg != "OFF":
    which = "BT" if arg == "BT" else "AT"
    mat = unreal.load_asset("/EnhancedImaging/EI/M_EI_Vision_" + which)
    world = ELL.get_editor_world()
    # a transient material instance (in memory only - never saved; "off" deletes it)
    MEL = unreal.MaterialEditingLibrary
    tmp = "/Game/EI_TEST_MI"
    if unreal.EditorAssetLibrary.does_asset_exist(tmp):
        unreal.EditorAssetLibrary.delete_asset(tmp)
    mid = unreal.AssetToolsHelpers.get_asset_tools().create_asset("EI_TEST_MI", "/Game", unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    MEL.set_material_instance_parent(mid, mat)
    MEL.set_material_instance_scalar_parameter_value(mid, "Opacity", 1.0)
    MEL.set_material_instance_scalar_parameter_value(mid, "WaveOn", 0.0)
    if arg == "RED":
        MEL.set_material_instance_vector_parameter_value(mid, "BgCol", unreal.LinearColor(1, 0, 0, 1))
    MEL.update_material_instance(mid)
    loc, rot = ELL.get_level_viewport_camera_info()
    ppv = ELL.spawn_actor_from_class(unreal.PostProcessVolume, loc)
    ppv.set_actor_label("EI_TEST_PPV")
    ppv.set_editor_property("unbound", True)
    ppv.set_editor_property("priority", 1000.0)
    s = ppv.get_editor_property("settings")
    s.set_editor_property("weighted_blendables", unreal.WeightedBlendables(array=[unreal.WeightedBlendable(weight=1.0, object=mid)]))
    ppv.set_editor_property("settings", s)
    fwd = rot.get_forward_vector()
    cube = ELL.spawn_actor_from_object(unreal.load_asset("/Engine/BasicShapes/Cube"), loc + fwd * 600.0)
    cube.set_actor_label("EI_TEST_CUBE")
    unreal.log("[EI test] %s on an unbound volume at %s - mid %s" % (arg, loc, mid))
