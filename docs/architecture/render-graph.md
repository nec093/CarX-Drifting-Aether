# Render Graph

`src/Aether.Native/Renderer/RenderGraph.{h,cpp}`. A frame is a list of passes that declare the resources they read
and write, and in which D3D12 state. The graph validates, allocates, transitions and runs them.

## Model

- **Resources**
  - *Imported*: a texture Aether owns across frames (shadow atlas). State tracked by the GPU resource manager.
  - *External*: owned by the host (the back buffer). Never transitioned by the graph: every pass must use it in the
    state it was imported with.
  - *Transient*: lives inside one frame (`SceneDepth`, `SceneColor`). Backed by a pooled texture.
- **Passes**: `AddPass(name, enabled, setup, execute)`. `setup` declares `Read(id, state)` / `Write(id, state)`;
  `execute` records commands. A disabled pass stays in the description (reported as not executed).
- **Order**: passes run in the order they were added. The graph checks the order, it does not search for one.
- One command list carries the whole frame.

## Compile

1. Validation: an undeclared resource, a resource declared twice in one pass, a read declared with a write state
   (or the reverse), a host resource wanted in another state than the host hands it over in, an imported resource
   without a texture, a transient read before any pass wrote it. Any error → the frame is not executed; the reasons
   are logged (`E_RENDER_GRAPH`, rate limited) and returned by MCP `get_render_graph`.
2. Lifetime: first / last pass of every resource.
3. Allocation: a transient takes a pool texture with an identical description that is free for its lifetime
   (two transients whose lifetimes do not overlap share one → `aliased`), else a new one is created.

## Execute

Before each pass the graph records the transitions its declarations need (`GpuResources::Transition`, batched per
pass) and brackets the pass with GPU timestamps (`GpuTimer`). Each pass reports draw calls, triangles and CPU time.

## End

Transients go back to the pool. A pool texture unused for 120 frames, or any after a resolution change, is retired
against the frame's completion serial.

## Diagnostics

`get_render_graph` → `sceneGraph`: passes (executed, reads / writes with states, draw calls, triangles, CPU ms, GPU
ms), resources (kind, size, format, bytes, lifetime, aliased), pool size, errors.

## Current frame

```
ShadowCascades  writes ShadowAtlas (DEPTH_WRITE)                       [off when shadows are off or there is no sun]
DepthPrepass    writes SceneDepth  (DEPTH_WRITE)
Opaque          reads  ShadowAtlas (PIXEL_SHADER_RESOURCE); writes SceneColor (RENDER_TARGET), SceneDepth
Sky             writes SceneColor, SceneDepth (depth test only)
Transparent     reads  ShadowAtlas; writes SceneColor, SceneDepth (depth test only)   [off when nothing is blended]
Composite       reads  SceneColor (PIXEL_SHADER_RESOURCE); writes BackBuffer (external, RENDER_TARGET)
```

## Not there yet

No pass culling, no automatic ordering, no async compute, no multi-list recording, no history resources
(TAA / temporal passes will need a "persistent across frames, versioned" kind).
