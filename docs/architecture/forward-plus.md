# Forward+ renderer

`src/Aether.Native/Renderer/SceneRenderer.{h,cpp}`, shaders `shaders/aether_scene.hlsli`, `scene_forward.hlsl`,
`scene_post.hlsl` (Shader Model 5.1, compiled at run time by the shader manager, hot reloadable).
Clustered forward: no G-buffer, so no G-buffer memory and no restriction on material models.

## Frame

`Renderer::SceneFrame` (plugin event 4, issued from `RenderPipelineManager.endContextRendering` of the game's screen
camera: after HDRP drew the scene, before the game's overlay UI).

1. `SceneStore::Apply`: pending upserts / destroys, this frame's transforms and lights.
2. Draw lists (CPU): frustum culling by bounding sphere, material → pass, sort.
   - opaque: by vertex layout, cull mode, alpha test, material, then front to back
   - transparent: back to front
   - shadow casters: per cascade, culled against the cascade's sphere extended towards the sun
3. Shadow cascades, light clusters, frame constants → the frame's upload arena.
4. Render graph (render-graph.md): ShadowCascades → DepthPrepass → Opaque → Sky → Transparent → Composite.

One command list, one draw call per sub mesh (no instancing yet). Per draw: 4 root constants
(instance index, material index, flags, cascade) and, when the material changes, one descriptor table.

## Binding model

| Slot | Content |
|---|---|
| b0 | frame constants (camera, sun, cascades, cluster grid, SH, fog, exposure, debug view) |
| b1 | 4 root constants per draw |
| t0–t4 (root SRV buffers) | instances, materials, lights, cluster grid, light indices |
| table space1 t0–t4 | the material's textures: base, normal, mask, emissive, extra |
| table space2 t0–t2 | shadow atlas, sky cube array, exposure texture |
| table space2 t3 | scene colour (composite only) |

Pipeline states are created on demand and cached by (vertex layout, pass kind, cull mode). A mesh attribute the
layout lacks (tangent, colour, uv1) is fed from a one-vertex default buffer (slot 7, per-instance step rate).

## Lighting

- **Directional sun**: lux × colour. Cascaded shadow map: 2 cascades (low) or 3, in a 2 × 2 `D32_FLOAT` atlas with
  1024 / 1536 / 2048 / 3072 texels per cascade (low / medium / high / ultra), 40 / 60 / 80 / 120 m. Cascades are
  bounding spheres snapped to whole texels (no shimmer), practical split scheme, casters up to 250 m towards the
  sun. 3 × 3 PCF with a comparison sampler, slope-scaled raster bias + normal offset. Beyond the last cascade
  nothing is shadowed (hard cut, no fade yet).
- **Punctual lights** (point, spot; candela): assigned on the CPU to a 16 × 9 × 24 cluster grid (logarithmic depth
  slices), at most 64 per cluster. The pixel shader walks its cluster's list. No punctual shadows yet.
- **Ambient**: diffuse from the 7 SH vectors of `RenderSettings.ambientProbe`; specular from HDRP's `_SkyTexture`
  cube (mip by roughness) with an analytic environment BRDF. No reflection probes, no SSR, no SSAO yet.
- **BRDF**: GGX / Smith / Schlick, Lambert diffuse, optional clear coat lobe.
- **Sky**: full screen triangle at the far plane sampling the same sky cube.
- **Composite**: exposure (HDRP's `_ExposureTexture` while HDRP runs, else `exposureEV`), ACES or neutral tone
  mapping, sRGB encode, written to the back buffer. `compare` mode = scissor on the right half.

## Settings (aether.json, overlay, MCP `set_render_mode` / `set_setting`)

`renderMode` hdrp | aether | compare · `debugView` none | albedo | normal | roughness | metallic | depth | lighting |
shadow | materialClass · `shadows` · `sky` · `tonemap` aces | neutral · `exposureEV` · `quality` (shadow resolution
and distance).

## Diagnostics

`get_status.sceneRenderer` / `get_scene_info.aetherRenderer`: visible instances, draw calls per pass, triangles,
materials in frame, unsupported-material draws, pipeline state count, lights and cluster occupancy, cascades.
`get_render_graph.sceneGraph`: per-pass CPU and GPU time. `run_benchmark`: `aetherSceneGpuTimeMs`, `aetherScene`.

## Limitations

See docs/development/phase2-report.md for the measured state. Structural ones:
skinned meshes and particles are not drawn; no instancing / GPU culling; no motion vectors, TAA, SSAO, SSR, bloom,
volumetrics, decals; transparent objects are sorted per object, not per pixel; one shadowed light (the sun).
