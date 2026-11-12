# Independence from HDRP

In render mode `aether` (the default) the game's own render pipeline does not exist. Aether puts its own pipeline
asset (`AetherPipelineAsset`, `src/Aether.Managed/Pipeline/AetherPipeline.cs`) in place of the game's before the first
frame; Unity then never creates an `HDRenderPipeline` instance, so none of its render targets, buffers or shader
variants are allocated. Verified in the garage scene: `Player.log` never mentions `HDRenderPipeline`, and every global
texture HDRP publishes (`_SkyTexture`, `_ExposureTexture`, `_CameraDepthTexture`, ...) is null.

Render modes `hdrp` and `compare` put the game's pipeline back. They exist for reference pictures and measurements,
not as a fallback: nothing in Aether's own path needs them.

## What the game's pipeline used to provide, and where it comes from now

| Dependency | Before | Now | Status |
|---|---|---|---|
| The call once per frame, inside the render loop | `RenderPipelineManager.endContextRendering` raised by HDRP | `AetherPipeline.Render` | replaced |
| Culling, `Renderer.isVisible`, LOD level, GPU skinning | HDRP's culling of the camera | `context.Cull` per camera in Aether's pipeline (Unity still does the work) | replaced; Unity's culling is still used |
| Shadow casters outside the view | HDRP's shadow culling marked them visible | Aether flags every enabled renderer that casts shadows and lies on a layer the sun lights; LOD level chosen with Unity's rule; the native side culls per cascade | replaced |
| Volume settings (sky, exposure, tone mapping, grading, fog, shadows, AO, bloom, motion blur) | `HDCamera.volumeStack` | Unity's `VolumeManager`, started by Aether with the game's default profile and the game pipeline's component types, evaluated for the camera | replaced; the components are HDRP's data classes (read as data) |
| Per camera feature switches (frame settings) | `HDCamera.frameSettings` | `HDAdditionalCameraData` and the project's default frame settings, read as data (`HostFrameSettings`) | replaced |
| Sky | HDRP's convolved `_SkyTexture` | the HDRI sky's cube map asset with HDRP's intensity and rotation rules; a cosine convolved irradiance map made by Aether every frame | replaced for HDRI skies; other sky types fall back to the ambient probe |
| Exposure | `_ExposureTexture` | Aether's own metering with the volume's limits and compensation | replaced; uniform weighting instead of HDRP's centre weighting |
| Lights | Unity `Light` + HDRP's conversion to its units | the same `Light` values; `HDAdditionalLightData` read as data (enabled state, dimmer, fade distance) | replaced |
| Light layers | HDRP asset setting | the game's pipeline asset read as data | replaced |
| Reflection probes | HDRP's probe atlas | the probes' own cube map assets | replaced (custom / baked probes); realtime probes are not rendered |
| UI | HDRP's UI passes | `DrawRenderers` for camera canvases (UI layer), Unity itself for overlay canvases | replaced |
| Blob shadows under cars | HDRP unlit decal-like pass | Aether's decal pass | replaced |
| Antialiasing, bloom, motion blur | HDRP post-processing | Aether's own passes | replaced (motion blur: camera motion only) |

## What is not there without the game's pipeline

- Screen space reflections, volumetric fog, contact shadows.
- Particle systems (`ParticleSystemRenderer`) and the game's own GPU particles (an HDRP custom pass).
- Cameras that render into a render texture (mirrors, icons): Aether only draws the screen camera.
- Sky types other than HDRI.
- Realtime reflection probes.

## Things to know

- Unity's `VolumeManager` is started and stopped by the active pipeline, and only loads the volume component types that
  declare support for the active pipeline's asset type. Aether calls `Initialize` itself, then loads the game
  pipeline's types (`LoadBaseTypes`, internal) and rebuilds the default state (`EvaluateVolumeDefaultState`, internal).
  Volume stacks made before a restart are dead: `PipelineSwitch.VolumeGeneration` tells.
- When switching back to the game's pipeline at run time, HDRP's static camera cache must be cleared
  (`HDCamera.ClearAll`): it holds volume stacks from before the restart.
- A light whose `HDAdditionalLightData` component is disabled is off for the game, whatever `Light.enabled` says.
- The DX12 fix for HDRP's compute shaders (`HdrpDx12Fix`) is only applied when the game's pipeline is about to run.
