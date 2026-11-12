# Scene Data Contract (version 3)

Version 3 (2026-11-12) adds the host's settings to the frame: directional shadows (`shadowDistance`, `shadowCascades`, `shadowSplits`), ambient occlusion (`aoIntensity`, `aoRadius`, `aoDirectStrength`) and shadows / midtones / highlights grading (`smhShadows`, `smhMidtones`, `smhHighlights`, `smhLimits`).

Version 2 (2026-11-12) adds: `renderingLayerMask` on instances and `layerMask` on lights (HDRP light layers: a light reaches an instance when the masks share a bit); on the frame, what the host's renderer does after lighting (`tonemapMode` and the custom curve's six parameters, `postExposure`) and height fog (`fogBaseHeight`, `fogMaxHeight`, `fogMaxDistance`, `fogTint`, `fogFlags`). Skinned renderers are sent as a mesh of their own whose stream 0 is Unity's GPU-skinned vertex buffer.

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

## 版 5・6（2026-11-12）

- **版 5**：空はホストのキューブマップそのもの（通常のミップチェーン）。フレームに `skyIntensity`（空のテクスチャへの乗数）と `skyRotation`（上軸まわりのラジアン）を追加。`exposureTexture` は使わない（常に 0。露出は `exposureMode` と EV の上下限から Aether が測る）。フレームごとのインスタンスのフラグに `kAeFrameInstanceShadowOnly`（視界には描かず影だけ落とす）を追加。`kAeFrameInstanceVisible` は「このフレームに存在する」の意味になり、視界と影カスケードに対するカリングはネイティブ側が行う。マテリアルクラスに `kAeMaterialBlobShadow`（車の下の箱型デカール）を追加。
- **版 6**：ホストのポストプロセス設定。`bloomIntensity`、`bloomScatter`、`motionBlurIntensity`、`antialiasing`（`AeAntialiasing`）。0 はオフ。

塗装クラス（`kAeMaterialVehiclePaint`）の `baseColor` のアルファは「リバリーのテクスチャがベーススロットにある」の意味（1 = ある）。継ぎ目クラス（`kAeMaterialVehicleStitches`）はマスクスロットにパックテクスチャを持つ（B が不透明度）。

## 版 7（2026-11-12）

- フレームに、ホストの SSR 設定を追加：`ssrEnabled`、`ssrMinSmoothness`、`ssrSmoothnessFadeStart`、`ssrScreenFadeDistance`（以前の `reserved[4]` の位置）。
- パーティクル：`Aether_SceneSubmitParticles(batches, batchCount, particles, particleCount, particleSize)`。そのフレームの `Aether_SceneSubmitFrame` の前に毎フレーム呼ぶ（無いときは 0 個で呼ぶ）。`AeParticle` はカメラを向く四角形 1 枚（中心、大きさ、色と不透明度、回転、引き伸ばしのベクトル、テクスチャの範囲）。`AeParticleBatch` は同じテクスチャと合成方法を持つ並び（`kAeParticleAdditive`：加算、`kAeParticleLit`：太陽と空で照らす、`kAeParticleDisc`：テクスチャなしの丸い煙）。加算でないバッチはネイティブ側が奥から手前へ並べ替える。
