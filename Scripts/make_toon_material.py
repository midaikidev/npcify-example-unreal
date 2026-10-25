"""Makes /Game/Npcify/Rendering/PP_NpcifyToon, the anime post-process material.

Run in the editor (Python console) or headless:
  UnrealEditor-Cmd NpcifyExample.uproject -run=pythonscript -script="<repo>/Scripts/make_toon_material.py"

The material runs after tonemapping:
- Cel shading: the lighting of each pixel (scene color / base color) becomes two flat
  tones, lit and shade, with a tinted shade. Pixels with custom stencil 1 (the VRM
  characters, which have their own MToon shading) and unlit pixels (sky) keep their color.
- Ink lines: depth and normal edges darken the color below them, and fade with distance.
"""

import unreal

PACKAGE_PATH = "/Game/Npcify/Rendering"
ASSET_NAME = "PP_NpcifyToon"

# The code reads ESceneTextureId values (MaterialSceneTextureId.h): 1 SceneDepth,
# 5 BaseColor, 8 WorldNormal, 14 PostProcessInput0, 25 CustomStencil.
SCENE_TEXTURES = {
    "InColor": "PPI_POST_PROCESS_INPUT0",
    "InDepth": "PPI_SCENE_DEPTH",
    "InNormal": "PPI_WORLD_NORMAL",
    "InBase": "PPI_BASE_COLOR",
    "InStencil": "PPI_CUSTOM_STENCIL",
}

HLSL = r"""
float2 uv = GetDefaultSceneTextureUV(Parameters, 14);
float2 px = View.BufferSizeAndInvSize.zw * Thickness;
// The center pixel comes from the inputs. The engine strips scene texture inputs
// that the code does not use, and the neighbor lookups need those bindings.
float3 color = InColor.rgb;
float depth = InDepth.r;
float3 normal = InNormal.rgb / max(length(InNormal.rgb), 1e-4);
float3 base = InBase.rgb;
float stencil = InStencil.r;

float3 lin = pow(max(color, 0), 2.2);
float3 result = lin;
if (stencil < 0.5 && max(base.r, max(base.g, base.b)) > 0.01)
{
    float3 ratio = lin / max(base, 0.03);
    float light = dot(ratio, float3(0.2126, 0.7152, 0.0722));
    float lit = smoothstep(Threshold * 0.75, Threshold * 1.15, light);
    float3 flatTone = base * lerp(ShadeTint, LitLevel.xxx, lit);
    result = lerp(lin, flatTone, Strength);
}

float2 offsets[4] = { float2(px.x, 0), float2(-px.x, 0), float2(0, px.y), float2(0, -px.y) };
float depthEdge = 0;
float normalEdge = 0;
for (int i = 0; i < 4; i++)
{
    float2 neighbor = uv + offsets[i];
    // Only a farther neighbor makes a line, so the line is on the near object.
    depthEdge = max(depthEdge, (SceneTextureLookup(neighbor, 1, false).r - depth) / depth);
    float3 m = SceneTextureLookup(neighbor, 8, false).rgb;
    normalEdge = max(normalEdge, 1 - dot(normal, m / max(length(m), 1e-4)));
}
float edge = max(smoothstep(0.05, 0.08, depthEdge), smoothstep(0.35, 0.5, normalEdge));
float fade = 1 - saturate((depth - FadeStart) / max(FadeEnd - FadeStart, 1));
result = lerp(result, result * InkColor, edge * fade * InkStrength);
if (Debug > 0.5)
{
    if (uv.x < 0.33) return color;
    if (uv.x < 0.66) return base;
    return float3(stencil, depth / 10000, dot(lin / max(base, 0.03), float3(0.2126, 0.7152, 0.0722)) * 0.5);
}
return pow(result, 1 / 2.2);
"""

SCALARS = {
    "Threshold": 0.9,
    "LitLevel": 1.8,
    "Strength": 0.85,
    "Thickness": 1.3,
    "InkStrength": 0.85,
    "FadeStart": 2500.0,
    "FadeEnd": 8500.0,
    "Debug": 0.0,
}
VECTORS = {
    "ShadeTint": (0.62, 0.57, 0.78),
    "InkColor": (0.30, 0.22, 0.28),
}


def make_material():
    library = unreal.MaterialEditingLibrary
    path = f"{PACKAGE_PATH}/{ASSET_NAME}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(ASSET_NAME, PACKAGE_PATH, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_POST_PROCESS)
    material.set_editor_property("blendable_location", unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_TONEMAPPING)

    custom = library.create_material_expression(material, unreal.MaterialExpressionCustom, -400, 0)
    custom.set_editor_property("code", HLSL)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("description", "Npcify toon")

    names = list(SCENE_TEXTURES) + list(SCALARS) + list(VECTORS)
    inputs = []
    for name in names:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    custom.set_editor_property("inputs", inputs)

    for index, (name, texture_id) in enumerate(SCENE_TEXTURES.items()):
        node = library.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -1000, index * 160)
        node.set_editor_property("scene_texture_id", getattr(unreal.SceneTextureId, texture_id))
        library.connect_material_expressions(node, "Color", custom, name)

    y = 900
    for name, value in SCALARS.items():
        node = library.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -800, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", value)
        library.connect_material_expressions(node, "", custom, name)
        y += 110
    for name, value in VECTORS.items():
        node = library.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -800, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", unreal.LinearColor(value[0], value[1], value[2], 1.0))
        mask = library.create_material_expression(material, unreal.MaterialExpressionComponentMask, -600, y)
        mask.set_editor_property("r", True)
        mask.set_editor_property("g", True)
        mask.set_editor_property("b", True)
        mask.set_editor_property("a", False)
        library.connect_material_expressions(node, "", mask, "")
        library.connect_material_expressions(mask, "", custom, name)
        y += 160

    library.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    library.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log(f"Npcify toon material saved: {path}")


make_material()
