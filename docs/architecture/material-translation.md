# Material translation

`src/Aether.Managed/SceneCapture/MaterialTranslator.cs` turns a Unity material into `AeMaterialDesc`, Aether's own
material. HDRP's material model, keywords and passes are not carried over: the translation reads the properties the
shader really has and maps them onto a small set of classes Aether's forward shader implements.

Designed from the survey of the real game (`get_material_info detail=survey`, garage scene): 13 shaders, property
layouts and texture formats. Nothing here comes from another project.

## Classification

By property signature, first match wins (a shader name is only used for the blob shadow):

| Class | Recognised by | Game shaders | Translation |
|---|---|---|---|
| `skip` | `_MotionVectorAngular` | HDRP/Custom/PhysicsMotionVectors | helper geometry, not drawn |
| `vehiclePaint` | `_Roughness` + `_Metalness` + `_Clearcoat` + `_Spectrum` | Shader Graphs/CarPaint | `_BaseColor`, `_SecondaryColor` (two-tone at grazing angles), `_Metalness`, `_Roughness`, clear coat |
| `vehicleStitches` | paint family + `_StitchesPack` | CarPaintStitches | as paint, no forced coat |
| `vehicleRim` | paint family, neither of the above | CarPaintRim | as paint |
| `vehicleGlass` | `_AlphaClippingThreshold` + `_Spectrum` | CarGlass, CarGlassNoVinyls | blended, double sided, Fresnel reflection of the sky, tint from `_BaseColor` |
| `vehicleTire` | `_TireBaseMap` | CarTireCustomization | base / pack / normal maps |
| `vehicleGeneric` | `_BaseMap` + `_PackMap` | CarGeneric, CarGenericAlpha | base, pack, normal, colour mask (`_ColormaskMap`, extra slot), emission map |
| `vehicleLights` | generic + `_EmissionMasks` | CarLights | as generic (emission masks not evaluated yet: lights do not glow) |
| `standard` | `_BaseColorMap` + `_Metallic` + `_Smoothness` | HDRP/Lit | base, normal, mask map, emissive; `transparent` when the surface is transparent |
| `standard` | `_BASE_COLOR_MAP` + `_METALNESS` family | HDRP 3ds Max physical material | base colour / map, metalness, roughness |
| `colorMask` | `_BaseColorMap` + `_MaskMap` + `_BaseColor`, no `_Metallic` | Shader Graphs/Colormask | base tinted by `_BaseColor`, normal, mask |
| `vegetation` | `_BaseColorMap` + `_DiffusionProfileHash` | CarXFoliageSimple | alpha tested, double sided |
| `skip` | name contains `Blobshadow`, or transparent with only `_BaseColorMap` | CarBlobshadow | not drawn (Aether casts real shadows) |
| `unlit` | `_MainTex` | Anzu video panels, Mobile/Particles/Additive | texture as is; additive when transparent |
| `unsupported` | anything else | Hidden/InternalErrorShader | drawn with the diagnostic checker material, counted |

Flags common to HDRP style materials: `_SurfaceType` / render queue ≥ 3000 → blend, `_AlphaCutoffEnable` → alpha
test, `_DoubleSidedEnable` → double sided.

## Texture slots and channel layouts

Slots: base (sRGB as the texture says), normal, mask, emissive, extra. Missing → white / flat normal / none.

- `maskLayout = hdrp` (HDRP MaskMap): R metallic, G occlusion, A smoothness. `metallic` and `roughness` of the desc
  hold the remap maxima.
- `maskLayout = rmo` (the game's "pack" maps): R roughness, G metallic, B occlusion. **Assumed** from the name and
  the look of the textures; the graph itself was not available. To be verified against HDRP in compare mode.

Normal maps are decoded from RG (works for DXT5nm / BC5 and plain RGB normals alike), Z reconstructed.

## Updates

- A material edited in place is found by `ComputeCRC()` (every 2 s) and translated again.
- A `MaterialPropertyBlock` with `_BaseColor` makes a per-renderer variant.
- Textures are shared by handle; a replaced GPU resource (streaming) only rewrites the descriptor.

## Diagnostics

`get_material_info detail=aether`: every native material (class, flags, scalars, bound textures with format and
size, number of texture properties the translation did not map) and totals by class.
Debug views `albedo`, `normal`, `roughness`, `metallic`, `materialClass` show the result on screen.
Unsupported materials are magenta / black checkered, never silently missing.

## Known gaps

- Clear coat strength: `_Clearcoat`'s exact meaning in the paint graph is unknown; a full coat is assumed when the
  graph has the clear coat feature on.
- Paint effects driven by `_Spectrum` (iridescence / flakes), vinyl layers (liveries) rendered by the game into
  textures other than `_BaseMap`, car light emission masks, tire customisation layers, HDRP detail maps, emissive
  intensity units, HDRP remap minima (`_MetallicRemapMin`, `_SmoothnessRemapMin`, `_AORemapMin`).
- Vegetation has no transmission / wind.
