# XeGTAO

Aether's ambient occlusion pass (`PSAmbientOcclusion` in `shaders/scene_post.hlsl`) is a port of the main pass of
Intel's XeGTAO (https://github.com/GameTechDev/XeGTAO, `Source/Rendering/Shaders/XeGTAO.hlsli`), which implements
Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect Occlusion".

What was taken: the horizon search per slice and the visibility integral (the paper's Algorithm 1 as XeGTAO writes it,
with its falloff and sample distribution), and the fast arc cosine.
What is Aether's own: it runs as a pixel shader on the full resolution depth buffer (no depth mip chain), reads view
space positions through Aether's matrices, and is followed by Aether's own depth aware blur and temporal antialiasing
instead of XeGTAO's denoiser. No bent normals.

XeGTAO is under the MIT licence: see LICENSE in this folder.
