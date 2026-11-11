# Scene Capture

`src/Aether.Managed/SceneCapture/SceneCapture.cs` turns the live Unity scene into Scene Data Contract calls.
It runs only while `renderMode` is `aether` or `compare`; in `hdrp` it holds nothing.

## What it does, and when

| When | What |
|---|---|
| every 0.5 s, and at once after a scene load / unload | **Scan**: `FindObjectsByType<MeshRenderer>` (active only). New renderers are registered, changed ones (mesh or material list) re-sent, vanished ones destroyed. Meshes, materials and textures nothing references any more are destroyed. Lights are listed. HDRP's global `_SkyTexture` and `_ExposureTexture` are tracked. |
| every 2 s | **Material check**: `Material.ComputeCRC()` of every tracked material; a changed one (the game recolours paint, switches lights) is translated again. |
| every frame, at most 24 native-pointer queries | **Pending queue**: `GetNativeVertexBufferPtr / IndexBufferPtr / TexturePtr` for new meshes and textures. A mesh Unity has not uploaded yet is retried later. |
| every frame | **Frame**: camera matrices, one `AeFrameInstance` (world matrix; visible = `enabled`, not `forceRenderingOff`, layer in the camera's culling mask, and `Renderer.isVisible`, i.e. Unity's own culling result incl. the LOD level it picked) per registered renderer, lights, ambient SH, fog, sky / exposure handles → `Aether_SceneSubmitFrame`, then plugin event 4. |

The frame is submitted from `RenderPipelineManager.endContextRendering` for the camera set that holds the game's
screen camera (`CameraType.Game`, no target texture, highest depth). Off-screen cameras (icons, previews) are ignored.

## Identity and change detection

- Handles are Unity instance ids. A material seen through a `MaterialPropertyBlock` with `_BaseColor` becomes its own
  native material (handle = hash of material id and renderer id) so per-renderer colours survive.
- A texture is re-queried when its `updateCount`, size or loaded mip level changed (texture streaming, resized render
  textures); the native side is only told when the `ID3D12Resource` really changed.
- A mesh is identified by the `Mesh` object. A mesh edited in place is **not** detected (see limitations).

## Lifecycle

- **Scene loading**: `SceneManager.sceneLoaded / sceneUnloaded` bump `sceneId` and force a scan on the next frame.
- **Dynamic objects**: found by the next scan (≤ 0.5 s late); their transforms are read every frame.
- **Destroyed objects**: Unity's fake-null is checked before every use; the next scan destroys their handles.
- **Mode switch to hdrp / shutdown**: `Stop()` clears the tables and calls `Aether_SceneClear`; one more scene event
  lets the render thread retire every Unity reference.
- **An exception** in the capture stops the capture (logged), never the game's render loop.

## Cost

Per frame the capture reads one `localToWorldMatrix` per renderer and copies into reused arrays: no per-frame
allocation after warm-up. The scan allocates (two `FindObjectsByType` arrays, hash sets) twice a second.
Measured numbers are in docs/development/phase2-report.md.

## Diagnostics

`get_status.managed.sceneCapture`, the KSL panel and the overlay show: renderers, meshes / textures (pending),
materials, lights, skipped skinned renderers, unsupported meshes / textures.
`get_material_info detail=survey` runs `RenderSurvey` (shaders, property layouts, vertex layouts, lights,
HDRP globals) — the data the translation was designed from.

## Limitations

- `SkinnedMeshRenderer`, particle systems, trails, terrain, `Graphics.Draw*` / instanced indirect draws are not
  captured.
- All active `MeshRenderer`s of all loaded scenes are registered. Which of them a frame draws relies on
  `Renderer.isVisible`, which Unity only updates while its own pipeline culls: Aether has no LOD selection or
  occlusion culling of its own yet (needed before HDRP can be switched off).
- A mesh whose buffers Unity replaces without replacing the `Mesh` object (dynamic meshes) keeps the old buffers
  until its renderer changes.
- New objects appear up to 0.5 s late; material edits up to 2 s late.
