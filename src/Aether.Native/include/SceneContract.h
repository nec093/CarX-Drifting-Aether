// The scene data contract between the managed side (Unity) and Aether's native renderer.
// docs/architecture/scene-data-contract.md describes ownership, lifetime and conventions; this header is the layout.
// Mirrored in src/Aether.Managed/SceneCapture/Contract.cs: change both and bump AE_SCENE_CONTRACT_VERSION.
//
// Rules of the layout: plain C, 4-byte packing friendly (only 32-bit fields and pointers at 8-byte offsets), every
// struct starts with its own size so that a mismatch between the two sides is detected instead of misread.
#pragma once
#include <stdint.h>

#define AE_SCENE_CONTRACT_VERSION 7 // 7: the host's screen space reflection settings; particles (Aether_SceneSubmitParticles) // 6: the host's post-processing settings (bloom, motion blur, antialiasing) // 5: 5: the sky is the host's own cube map (plain mip chain) with an intensity and a rotation // 4: 4: the host's exposure settings (Aether meters itself when there is no exposure texture) // 3: the host's shadow, ambient occlusion and grading settings on the frame // 2: rendering layer masks on instances and lights (light layers)

// A stable identifier chosen by the managed side (Unity's instance id). 0 = none. Unique per kind, not across kinds.
typedef uint32_t AeHandle;

// ---- conventions (fixed; see the document)
//  - World space is Unity's: left-handed, +Y up, metres.
//  - Matrices are Unity's Matrix4x4 memory layout: 16 floats, column-major (m[col*4+row]), column vectors (M * v).
//  - AeFrame::view is Camera.worldToCameraMatrix (right-handed view space, the camera looks down -Z).
//  - AeFrame::proj is GL.GetGPUProjectionMatrix(camera.projectionMatrix, false): D3D clip space, y up, reversed Z
//    (near plane at depth 1, far plane at depth 0).
//  - Front faces are clockwise on screen; an instance whose world matrix has a negative determinant flips that.
//  - Texture coordinates are Unity's (v = 0 at the bottom of the source image); textures are Unity's own GPU
//    resources, so sampling them with the mesh's uv needs no flip.
//  - Colours are linear; light colours are premultiplied by the light's intensity in HDRP's unit (lux / candela).

enum AeSemantic {
	kAeSemanticPosition = 0,
	kAeSemanticNormal = 1,
	kAeSemanticTangent = 2,
	kAeSemanticColor = 3,
	kAeSemanticTexCoord0 = 4,
	kAeSemanticTexCoord1 = 5,
	kAeSemanticCount = 6,
};

typedef struct AeVertexAttribute {
	uint8_t semantic;   // AeSemantic
	uint8_t stream;     // vertex buffer index
	uint8_t offset;     // byte offset inside the stream's vertex
	uint8_t dxgiFormat; // DXGI_FORMAT of the element
} AeVertexAttribute;

#define AE_MAX_STREAMS 4

typedef struct AeSubMesh {
	uint32_t indexStart;
	uint32_t indexCount;
	uint32_t baseVertex;
} AeSubMesh;

// Geometry. The buffers are Unity's own ID3D12Resource objects (Mesh.GetNativeVertexBufferPtr / IndexBufferPtr):
// nothing is copied. The native side takes a reference for as long as the handle lives.
typedef struct AeMeshDesc {
	uint32_t structSize;
	AeHandle mesh;
	uint32_t vertexCount;
	uint32_t indexCount;
	uint32_t indexStride;     // 2 or 4
	uint32_t streamCount;
	uint32_t streamStride[AE_MAX_STREAMS];
	void* vertexBuffer[AE_MAX_STREAMS]; // ID3D12Resource*
	void* indexBuffer;                  // ID3D12Resource*
	uint32_t attributeCount;
	AeVertexAttribute attributes[kAeSemanticCount];
	float boundsCenter[3];    // local space
	float boundsExtents[3];
	uint32_t subMeshCount;    // followed by the AeSubMesh array passed next to the struct
	uint32_t reserved;
} AeMeshDesc;

enum AeTextureDimension { kAeTexture2D = 0, kAeTextureCube = 1, kAeTextureCubeArray = 2, kAeTexture2DArray = 3, kAeTexture3D = 4 };

// A texture. `resource` is Unity's ID3D12Resource (Texture.GetNativeTexturePtr), not copied.
typedef struct AeTextureDesc {
	uint32_t structSize;
	AeHandle texture;
	void* resource;
	uint32_t width;
	uint32_t height;
	uint32_t mipCount;
	uint32_t dimension; // AeTextureDimension
} AeTextureDesc;

// How the managed side classified a material from the shader it uses and the properties that shader really has.
enum AeMaterialClass {
	kAeMaterialUnsupported = 0, // drawn with the diagnostic material
	kAeMaterialStandard = 1,    // HDRP/Lit and equivalents
	kAeMaterialVehiclePaint = 2,
	kAeMaterialVehicleGeneric = 3,
	kAeMaterialVehicleGlass = 4,
	kAeMaterialVehicleRim = 5,
	kAeMaterialVehicleStitches = 6,
	kAeMaterialColorMask = 7,
	kAeMaterialVegetation = 8,
	kAeMaterialTransparent = 9,
	kAeMaterialUnlit = 10,
	kAeMaterialVehicleLights = 11,
	kAeMaterialVehicleTire = 12,
	kAeMaterialSkip = 13,       // known helper geometry that must not be drawn (motion vector proxies)
	kAeMaterialBlobShadow = 14, // the soft contact shadow quad under a car: darkens what is behind it by its texture
	kAeMaterialClassCount = 15,
};

enum AeMaterialFlags {
	kAeMaterialAlphaTest = 1 << 0,
	kAeMaterialBlend = 1 << 1,        // alpha blended (drawn in the transparent pass)
	kAeMaterialDoubleSided = 1 << 2,
	kAeMaterialAdditive = 1 << 3,
	kAeMaterialClearCoat = 1 << 4,
	kAeMaterialNoShadow = 1 << 5,
};

// What the channels of the mask texture mean.
enum AeMaskLayout {
	kAeMaskNone = 0,
	kAeMaskHdrp = 1, // R metallic, G occlusion, B detail mask, A smoothness (HDRP's MaskMap)
	kAeMaskRmo = 2,  // R roughness, G metallic, B occlusion (the game's "pack" maps; see material-translation.md)
};

enum AeTextureSlot { kAeSlotBase = 0, kAeSlotNormal = 1, kAeSlotMask = 2, kAeSlotEmissive = 3, kAeSlotExtra = 4, kAeSlotCount = 5 };

// Aether's material: what the translation layer produced from a Unity material. Not HDRP's material model.
typedef struct AeMaterialDesc {
	uint32_t structSize;
	AeHandle material;
	uint32_t materialClass; // AeMaterialClass
	uint32_t flags;         // AeMaterialFlags
	uint32_t maskLayout;    // AeMaskLayout
	uint32_t renderQueue;
	uint32_t shaderId;      // Unity instance id of the shader (diagnostics)
	uint32_t unsupportedProperties; // number of properties the translation found but could not map
	float baseColor[4];
	float secondaryColor[4]; // vehicle paint: the second paint colour (flip-flop); alpha = weight
	float emissive[3];
	float metallic;
	float roughness;
	float normalScale;
	float alphaCutoff;
	float occlusion;        // strength of the occlusion channel, 0 = ignore
	float clearCoat;        // 0..1 strength of the clear coat layer
	float clearCoatRoughness;
	float uvScale[2];
	float uvOffset[2];
	AeHandle textures[kAeSlotCount]; // 0 = none (white / flat normal / no mask)
	uint32_t reserved[3];
} AeMaterialDesc;

enum AeInstanceFlags {
	kAeInstanceCastShadows = 1 << 0,
	kAeInstanceReceiveShadows = 1 << 1,
	kAeInstanceStatic = 1 << 2,
};

// One renderer of the scene: a mesh, a material per sub mesh, and flags. Its transform comes every frame.
typedef struct AeInstanceDesc {
	uint32_t structSize;
	AeHandle instance;
	AeHandle mesh;
	uint32_t flags;         // AeInstanceFlags
	uint32_t layer;
	uint32_t materialCount; // followed by the AeHandle array (one per sub mesh) passed next to the struct
	uint32_t renderingLayerMask; // Renderer.renderingLayerMask: a light reaches the instance when (light.layerMask & this) != 0
	uint32_t reserved;
} AeInstanceDesc;

enum AeAntialiasing { kAeAntialiasingNone = 0, kAeAntialiasingFast = 1, kAeAntialiasingTemporal = 2, kAeAntialiasingMorphological = 3 };

enum AeFrameInstanceFlags {
	kAeFrameInstanceVisible = 1 << 0,    // part of this frame (the renderer culls it against the view and the shadow cascades itself)
	kAeFrameInstanceShadowOnly = 1 << 1, // never drawn in the view: it only casts shadows (a layer the camera does not show, a shadows-only renderer)
};

// Particles: camera facing quads, sent every frame (Aether_SceneSubmitParticles, before the frame they belong to).
// A batch is a run of particles that share a texture and a way of blending.
typedef struct AeParticle {
	float position[3]; // world space centre
	float size;        // metres across
	float color[4];    // linear rgb (scene units for unlit batches, albedo for lit ones), a = opacity
	float rotation;    // radians about the view direction
	float stretch[3];  // world vector from the centre to the quad's far end (all 0: a square facing the camera)
	float uv[4];       // the texture's part this particle shows: offset (xy), size (zw)
} AeParticle;

enum AeParticleFlags {
	kAeParticleAdditive = 1 << 0, // added to the picture (flames, sparks) instead of covering it
	kAeParticleLit = 1 << 1,      // lit by the sun (with its shadow) and the sky (smoke, dust)
	kAeParticleDisc = 1 << 2,     // no texture: a soft round puff
};

typedef struct AeParticleBatch {
	AeHandle texture; // 0 (or not registered yet) = drawn as kAeParticleDisc
	uint32_t flags;   // AeParticleFlags
	uint32_t first;   // index of the batch's first particle
	uint32_t count;
} AeParticleBatch;

// The dynamic part of an instance, sent every frame for every live instance.
typedef struct AeFrameInstance {
	AeHandle instance;
	uint32_t flags;          // AeFrameInstanceFlags
	float localToWorld[16];
} AeFrameInstance;

enum AeLightType { kAeLightDirectional = 0, kAeLightPoint = 1, kAeLightSpot = 2 };
enum AeLightFlags { kAeLightCastsShadows = 1 << 0, kAeLightHeadlamp = 1 << 1 /* a car's head lamp: drawn with a low beam pattern */ };

typedef struct AeLight {
	uint32_t type;
	uint32_t flags;
	float position[3];
	float range;
	float direction[3];   // the way the light travels (world space, normalized)
	float spotOuterCos;   // cos(half outer angle)
	float color[3];       // linear, premultiplied by the intensity (directional: lux, punctual: candela)
	float spotInnerCos;
	uint32_t layerMask;   // which rendering layers the light reaches (0xFFFFFFFF when the host does not use light layers)
	uint32_t reserved;
} AeLight;

enum AeTonemap { kAeTonemapUnknown = 0, kAeTonemapNone = 1, kAeTonemapNeutral = 2, kAeTonemapAces = 3, kAeTonemapCustom = 4, kAeTonemapExternal = 5 };
enum AeExposureMode { kAeExposureUnknown = 0, kAeExposureFixed = 1, kAeExposureAutomatic = 2 };
enum AeFogFlags { kAeFogColorFromSky = 1 << 0 };

enum AeProbeFlags {
	kAeProbeBoxProjection = 1 << 0, // reflections are re-projected onto the influence box (parallax correction)
	kAeProbeSphere = 1 << 1,        // the influence volume is a sphere of radius extents[0]
};

// A reflection probe: a cube map (the host's own texture, borrowed like any other) and where it applies.
// Sent as a whole list with Aether_SceneSetProbes whenever the host's set of probes changes.
typedef struct AeProbe {
	AeHandle texture;        // cube map
	uint32_t flags;          // AeProbeFlags
	float worldToLocal[12];  // rows of the 3 x 4 matrix world -> the influence volume's space (rotation and translation only)
	float extents[3];        // half size of the influence box (sphere: radius in [0])
	float blendDistance;     // metres over which the probe fades in from the volume's faces
	float capturePosition[3];
	float intensity;
} AeProbe;

enum AeFrameFlags {
	kAeFrameCameraValid = 1 << 0, // false: nothing to render this frame (no camera, a loading screen)
};

// Everything that changes per frame except the instance transforms and the lights (passed next to it).
typedef struct AeFrame {
	uint32_t structSize;
	uint32_t contractVersion;
	uint32_t flags;         // AeFrameFlags
	uint32_t sceneId;       // changes when the set of loaded scenes changes
	uint32_t cameraId;
	uint32_t width;         // of the camera's target (the back buffer)
	uint32_t height;
	uint32_t frameIndex;
	float view[16];
	float proj[16];
	float cameraPosition[3];
	float nearPlane;
	float cameraForward[3];
	float farPlane;
	float deltaTime;
	float time;
	float exposure;         // multiplier used when exposureTexture is 0
	float ambientIntensity;
	// Ambient light as Unity's 7 SH vectors (unity_SHAr, SHAg, SHAb, SHBr, SHBg, SHBb, SHC)
	float ambientSH[7][4];
	float fogColor[3];
	float fogDensity;       // 0 = no fog
	AeHandle skyTexture;      // cube map with a plain mip chain (the host's HDRI sky asset), 0 = none (the ambient probe is the sky)
	AeHandle exposureTexture; // 1x1 texture whose red channel is the exposure multiplier, 0 = use `exposure`
	// What the host's own renderer does after lighting, so that Aether's picture can be graded the same way.
	uint32_t tonemapMode;     // AeTonemap
	float tonemapCurve[6];    // kAeTonemapCustom: toe strength, toe length, shoulder strength, shoulder length, shoulder angle, gamma
	float postExposure;       // stops added after the exposure (colour adjustments)
	// Height fog: fogDensity (above) is 1 / mean free path at and below fogBaseHeight, falling off up to fogMaxHeight.
	float fogBaseHeight;
	float fogMaxHeight;
	float fogMaxDistance;     // 0 = no limit
	float fogTint[3];
	uint32_t fogFlags;        // AeFogFlags
	// The host's directional shadow settings (0 distance = unknown: Aether's own defaults).
	float shadowDistance;     // metres from the camera
	uint32_t shadowCascades;  // 1..4
	float shadowSplits[3];    // where cascades 0..2 end, as fractions of shadowDistance
	// Screen space ambient occlusion as the host has it (0 intensity = none).
	float aoIntensity;
	float aoRadius;           // metres
	float aoDirectStrength;   // how much of the occlusion also dims direct light (0..1)
	// Shadows / midtones / highlights grading: rgb multipliers already prepared (1 = neutral), and the luminance ranges
	// (shadows start, shadows end, highlights start, highlights end). All limits 0 = no grading.
	float smhShadows[4];
	float smhMidtones[4];
	float smhHighlights[4];
	float smhLimits[4];
	// Exposure as the host sets it up; used when exposureTexture is 0 (the host's post-processing is not running).
	uint32_t exposureMode;      // AeExposureMode
	float exposureFixedEV;      // kAeExposureFixed: EV100
	float exposureMinEV;        // kAeExposureAutomatic: limits of the metered EV100
	float exposureMaxEV;
	float exposureCompensation; // stops, subtracted from the EV
	float skyIntensity;         // multiplier on the sky texture (0 = 1)
	float skyRotation;          // radians about the world's up axis
	// Post-processing as the host has it set up (0 = off).
	float bloomIntensity;       // the host's 0..1 control value
	float bloomScatter;         // 0..1
	float motionBlurIntensity;
	uint32_t antialiasing;      // AeAntialiasing
	// Screen space reflections as the host has them set up.
	uint32_t ssrEnabled;          // 0 = off
	float ssrMinSmoothness;       // surfaces less smooth than this (perceptual smoothness, 0..1) get none
	float ssrSmoothnessFadeStart; // ... and the full amount from this smoothness on
	float ssrScreenFadeDistance;  // the part of the screen over which a reflection fades out towards the screen's edge (0..1)
} AeFrame;
