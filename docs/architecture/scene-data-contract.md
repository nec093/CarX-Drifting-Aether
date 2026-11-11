# Scene Data Contract (version 1)

The interface between the managed side (Unity, `src/Aether.Managed/SceneCapture`) and Aether's native renderer
(`src/Aether.Native/Scene/SceneStore`). The layout is `src/Aether.Native/include/SceneContract.h`, mirrored field for
field in `SceneCapture/Contract.cs`. Change both and bump `AE_SCENE_CONTRACT_VERSION`.

## Rules

- **Checked before use.** Every struct starts with its own size. At start-up `SceneContract.Verify()` compares the
  managed sizes with `Aether_SceneStructSize(kind)` and the contract version; a mismatch keeps the scene renderer off
  and is logged (`Scene contract mismatch`). Every call re-checks the size and the fields; a bad call is rejected and
  counted as `E_SCENE_CONTRACT` (first 20 logged), never half applied.
- **Handles, not pointers.** Objects are named by `AeHandle` = Unity's instance id (`0` = none), unique per kind.
  An *upsert* with a known handle replaces the object.
- **Zero copy.** Meshes and textures are Unity's own `ID3D12Resource` objects
  (`Mesh.GetNativeVertexBufferPtr / GetNativeIndexBufferPtr`, `Texture.GetNativeTexturePtr`). The native side
  validates the pointer (`QueryInterface` under SEH), takes a COM reference and drops it only through the retirement
  queue, after the GPU finished the last list that could use it. Raw pointers are never stored.
- **Buffers must fit.** A mesh whose description needs more bytes than its buffers hold is rejected (not drawn).

## Objects

| Kind | Struct | Carries |
|---|---|---|
| mesh (0) | `AeMeshDesc` + `AeSubMesh[]` | up to 4 vertex streams, index buffer (16 / 32 bit), up to 6 attributes (position, normal, tangent, colour, uv0, uv1) with stream / offset / DXGI format, local bounds |
| texture (1) | `AeTextureDesc` | resource, size, mips, dimension (2D, cube, cube array, 2D array, 3D) |
| material (2) | `AeMaterialDesc` | Aether's material: class, flags, colours, scalars, 5 texture slots (see material-translation.md) |
| instance (3) | `AeInstanceDesc` + `AeHandle[]` | mesh, one material per sub mesh, shadow flags, layer |
| frame | `AeFrame` + `AeFrameInstance[]` + `AeLight[]` | camera, per-instance world matrix and visibility, lights, ambient SH, fog, sky and exposure textures |

Exports (`AetherApi.h`, API version 2): `Aether_SceneUpsertMesh / Texture / Material / Instance`,
`Aether_SceneDestroy(kind, handle)`, `Aether_SceneClear`, `Aether_SceneSubmitFrame`, `Aether_SceneStructSize`,
`Aether_GetRenderMode`.

## Conventions

- World space is Unity's: left-handed, +Y up, metres.
- Matrices are `Matrix4x4` memory: 16 floats, column-major, column vectors (`mul(M, v)` in HLSL).
- `view` = `Camera.worldToCameraMatrix` (right-handed view space, camera looks down −Z).
- `proj` = `GL.GetGPUProjectionMatrix(camera.nonJitteredProjectionMatrix, false)`: D3D clip space, reversed Z
  (near = 1, far = 0). Depth test is `GREATER_EQUAL`, depth clears to 0.
- Front faces are clockwise. An instance with a negative determinant flips culling and facing.
- Colours are linear. Light colour is premultiplied by intensity in HDRP's unit (directional: lux, punctual: candela).
- Texture coordinates are Unity's; the textures are Unity's own resources, so no flip.

These were validated with the host test's validation scene (`tests/host/ValidationScene.cpp`): position, depth
order, culling, a mirrored instance, shadows and blending are checked by pixel probes.

## Threads and lifetime

- **Main thread** calls the exports. They only validate, take references and append to a pending list (mutex).
- **Render thread** (`Renderer::SceneFrame`, plugin event 4) swaps the pending list in, applies it in order, then
  renders. One frame of latency between a change and its effect; no blocking either way.
- **Destroy / replace** moves the old resource reference and its descriptors to the retirement queue with the serial
  of the list being recorded. They are released when the GPU has completed that serial (see dx12-integration.md,
  "Completion").
- **Scene unload**: the managed side calls `Aether_SceneClear` (or destroys what vanished in its next scan).
- **Render mode `hdrp`**: capture stops, the store is cleared, every Unity reference is retired.
- **Shutdown**: everything is released after the GPU is confirmed idle; if it cannot be confirmed, the references
  are leaked on purpose rather than freed under the GPU.

## Extending

New fields go into the `reserved` members first. A layout change needs a version bump on both sides; the size check
catches a stale pair.
