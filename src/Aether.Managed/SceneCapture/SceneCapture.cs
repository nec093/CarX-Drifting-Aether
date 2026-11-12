using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace Aether {
	/// <summary>
	/// Captures Unity's scene for Aether's renderer through the scene data contract (Contract.cs).
	///
	/// What goes over (docs/architecture/scene-capture.md):
	///  - once per object, when it appears or changes: meshes (Unity's own vertex / index buffers, by pointer),
	///    textures (Unity's own GPU textures, by pointer), translated materials, renderer instances;
	///  - every frame: the camera, one transform per renderer, the lights, the environment. No allocation.
	/// Nothing in the scene is modified. A destroyed Unity object is noticed at the next scan (or the frame its
	/// renderer turns null) and its native handle is destroyed; the native side then drops its reference safely.
	/// Getting a native pointer from Unity waits for the render thread, so new objects are registered a few per frame.
	/// </summary>
	internal sealed class SceneCapture {
		private sealed class Tracked {
			public Renderer renderer;
			public MeshFilter filter;
			public uint handle;
			public Mesh mesh;
			public uint[] materials;
			public int[] materialIds; // Unity instance ids of the shared materials (change detection)
			public bool registered;
			public bool isStatic;
			// What the last look at the renderer found (UpdateTracked, about twice a second). Used every frame for a
			// renderer that stands still, instead of asking Unity again: on a track that is 7 000 of 7 500 renderers,
			// and asking each of them four things every frame cost 8 ms of the main thread.
			// live: asked every frame. Set for whatever can move by itself (skinned, under a Rigidbody or an Animator)
			// and for anything that was seen to have moved between two looks.
			public bool live;
			public bool matrixKnown;
			public bool enabled;
			public ShadowCastingMode casting;
			public Matrix4x4 matrix;
			public int layerBit; // 1 << gameObject.layer, refreshed by the scan
			public SkinnedMeshRenderer skinned; // set for a skinned renderer: stream 0 of its mesh is Unity's skinned vertex buffer
			public uint meshHandle;             // the native mesh this renderer draws (per renderer when skinned)
			public IntPtr skinnedPointer;       // the skinned vertex buffer the native side was given
			public int lodGroup = -1;            // index in lodGroups_ when a LOD group owns the renderer
			public uint lodMask;                 // the group's levels the renderer belongs to
			public int skinnedFailures;         // scans in a row in which its skinned buffer did not fit the mesh's layout
			public bool skinnedUnsupported;     // ... often enough to be counted as not drawable (it is still tried again)
			// Unity skins into more than one buffer per renderer and alternates between them from frame to frame: each
			// buffer is a mesh of its own on the native side, and the instance is pointed at this frame's one
			public Dictionary<IntPtr, uint> skinnedVariants;
			public uint rayKey;                 // skinned: the handle its ray tracing tree is kept under (every vertex buffer variant is traced as this)
			public bool rayIndicesSent, rayReading;
		}

		private sealed class TrackedMaterial {
			public Material material;
			public int crc;
			public Renderer owner; // the renderer whose own property values went into it (null: the shared material)
			public int index;      // that renderer's material slot
			public uint signature; // of the description that was sent
			public uint[] textures; // the textures that description names (slot order, 0 = none)
		}

		private sealed class TrackedTexture {
			public Texture texture;
			public IntPtr pointer;
			public uint updateCount;
			public int width, height, loadedMip;
			public bool global; // _SkyTexture / _ExposureTexture: re-queried every scan (render textures are recreated)
		}

		private const int PointerQueriesPerFrame = 24; // each one waits for the render thread
		private const float ScanInterval = 0.5f;
		private const float MaterialCheckInterval = 2.0f;

		private readonly Dictionary<int, Tracked> renderers_ = new Dictionary<int, Tracked>();
		private readonly List<Tracked> order_ = new List<Tracked>();
		private readonly Dictionary<uint, Mesh> meshes_ = new Dictionary<uint, Mesh>();
		private readonly Dictionary<uint, TrackedMaterial> materials_ = new Dictionary<uint, TrackedMaterial>();
		private readonly Dictionary<uint, TrackedTexture> textures_ = new Dictionary<uint, TrackedTexture>();
		private readonly Queue<Mesh> pendingMeshes_ = new Queue<Mesh>();
		private readonly HashSet<uint> pendingMeshSet_ = new HashSet<uint>();
		private readonly Queue<Texture> pendingTextures_ = new Queue<Texture>();
		private readonly HashSet<uint> pendingTextureSet_ = new HashSet<uint>();
		private readonly List<Light> lights_ = new List<Light>();
		private readonly List<Material> sharedMaterials_ = new List<Material>();
		private readonly List<VertexAttributeDescriptor> attributes_ = new List<VertexAttributeDescriptor>();
		private readonly MaterialPropertyBlock block_ = new MaterialPropertyBlock();
		private AeFrameInstance[] frameInstances_ = new AeFrameInstance[1024];
		private AeLight[] frameLights_ = new AeLight[64];
		private AeSubMesh[] subMeshes_ = new AeSubMesh[64];
		private uint[] materialScratch_ = new uint[64];
		private float nextScan_, nextMaterialCheck_;
		private bool active_;
		private uint sceneId_ = 1;
		private uint frameIndex_;
		private Texture sky_;
		private static readonly int BaseColorId = Shader.PropertyToID("_BaseColor");

		// statistics for the diagnostics
		public int SkinnedSkipped { get; private set; }
		public int OtherSkipped { get; private set; }
		public int UnsupportedFormats { get; private set; }
		public bool Active => active_;
		public int RendererCount => order_.Count;

		public void Start() {
			if (active_) {
				return;
			}
			active_ = true;
			devFlags_ = Native.Aether_DevCaptureFlags();
			particleTexture_ = ParticleTexture;
			lightLayers_ = HostLightLayers();
			nextScan_ = 0;
			Native.Log(1, "Scene capture started");
		}

		/// <summary>Stops capturing and destroys every native handle (the scene path's next event applies it).</summary>
		// ---- particles
		private readonly ParticleCapture particles_ = new ParticleCapture();
		private readonly HashSet<uint> particleTextures_ = new HashSet<uint>();
		private Func<Texture, uint> particleTexture_;

		/// <summary>The native handle of a particle system's texture: 0 while it is not registered (it is queued then).</summary>
		private uint ParticleTexture(Texture t) {
			if (t == null) {
				return 0;
			}
			uint handle = Handle(t);
			particleTextures_.Add(handle);
			if (textures_.ContainsKey(handle)) {
				return handle;
			}
			QueueTexture(t);
			return 0;
		}

		public void Stop() {
			if (!active_) {
				return;
			}
			active_ = false;
			Reset();
			Native.Aether_SceneClear();
			Native.Log(1, "Scene capture stopped");
		}

		/// <summary>The set of loaded scenes changed: everything is looked at again from scratch.</summary>
		public void SceneChanged() {
			++sceneId_;
			nextScan_ = 0;
		}

		private void Reset() {
			scanPhase_ = 0;
			seen_.Clear();
			pendingFailures_.Clear();
			particles_.Reset();
			particleTextures_.Clear();
			renderers_.Clear();
			order_.Clear();
			skinnedTracked_.Clear();
			meshes_.Clear();
			ResetGeometry();
			dynamicMeshes_.Clear();
			materials_.Clear();
			textures_.Clear();
			pendingMeshes_.Clear();
			pendingMeshSet_.Clear();
			pendingTextures_.Clear();
			pendingTextureSet_.Clear();
			lights_.Clear();
			sky_ = null;
			probeSignature_ = 0;
			ProbeCount = 0;
		}

		// ------------------------------------------------------------------------------------------------ scanning

		private static uint Handle(UnityEngine.Object o) {
			return unchecked((uint)o.GetInstanceID());
		}

		// A material seen through a renderer's property block is its own native material: the handle mixes both ids.
		private static uint VariantHandle(UnityEngine.Object m, Renderer r, int index = 0) {
			unchecked {
				uint h = 2166136261u;
				h = (h ^ (uint)m.GetInstanceID()) * 16777619u;
				h = (h ^ (uint)r.GetInstanceID()) * 16777619u;
				h = (h ^ (uint)index) * 16777619u;
				return h == 0 ? 1u : h;
			}
		}

		/// <summary>
		/// The renderer's own property values for its material `index`, or null. A block set for that slot wins over the
		/// one set for the whole renderer, as in Unity. The game sets the whole renderer's block when it builds a car part
		/// and the slot's block on every change after that (a new paint, CarBodyPart.SetMaterialPropertyBlock): reading
		/// the renderer's first showed the paint the car was built with forever.
		/// (Renderer.HasPropertyBlock is not asked: it does not tell about blocks set per slot.)
		/// </summary>
		private MaterialPropertyBlock BlockOf(Renderer r, int index) {
			r.GetPropertyBlock(block_, index);
			if (block_.isEmpty) {
				r.GetPropertyBlock(block_);
			}
			return block_.isEmpty ? null : block_;
		}

		// The scan is done in four steps, one per frame: on a track all of it in one frame took 10 ms.
		private int scanPhase_; // 0: none in progress
		private readonly HashSet<int> seen_ = new HashSet<int>();
		private bool scanSetChanged_;

		private void ScanStep() {
			switch (scanPhase_) {
			case 1: ScanMeshRenderers(); break;
			case 2: ScanSkinnedAndRemovals(); break;
			case 3: ScanSweepMeshesAndMaterials(); break;
			default: ScanTexturesAndLights(); break;
			}
			scanPhase_ = scanPhase_ >= 4 ? 0 : scanPhase_ + 1;
		}

		private void ScanMeshRenderers() {
			// ---- renderers: new ones (the known ones are looked at again by UpdateSlice)
			MeshRenderer[] found = UnityEngine.Object.FindObjectsByType<MeshRenderer>(FindObjectsInactive.Exclude, FindObjectsSortMode.None);
			HashSet<int> seen = seen_;
			seen.Clear();
			scanSetChanged_ = false;
			foreach (MeshRenderer r in found) {
				int id = r.GetInstanceID();
				seen.Add(id);
				if (!renderers_.TryGetValue(id, out Tracked t)) {
					t = new Tracked { renderer = r, filter = r.GetComponent<MeshFilter>(), handle = unchecked((uint)id), isStatic = r.gameObject.isStatic };
					t.live = !t.isStatic && (r.GetComponentInParent<Rigidbody>() != null || r.GetComponentInParent<Animator>() != null || r.GetComponentInParent<Animation>() != null);
					renderers_[id] = t;
					order_.Add(t);
					UpdateTracked(t);
					scanSetChanged_ = true;
				}
				// (a renderer already known is looked at again by UpdateSlice, a few per frame: looking at all of a track's
				// 7 500 renderers in one frame, twice a second, was a hitch twice a second)
			}
		}

		private void ScanSkinnedAndRemovals() {
			HashSet<int> seen = seen_;
			// skinned renderers: Unity skins on the GPU into a vertex buffer of its own per renderer; Aether draws from it
			int skinnedWaiting = 0;
			foreach (SkinnedMeshRenderer r in UnityEngine.Object.FindObjectsByType<SkinnedMeshRenderer>(FindObjectsInactive.Exclude, FindObjectsSortMode.None)) {
				int id = r.GetInstanceID();
				seen.Add(id);
				if (!renderers_.TryGetValue(id, out Tracked t)) {
					t = new Tracked { renderer = r, skinned = r, handle = unchecked((uint)id), isStatic = false, live = true };
					renderers_[id] = t;
					order_.Add(t);
					skinnedTracked_.Add(t);
				}
				UpdateTracked(t);
				skinnedWaiting += t.registered || !r.enabled || !r.isVisible ? 0 : 1; // hidden ones (the driver in the garage) are not skinned by Unity and not missing
			}
			for (int i = order_.Count - 1; i >= 0; --i) {
				Tracked t = order_[i];
				if (t.renderer == null || !seen.Contains(unchecked((int)t.handle))) {
					// destroyed, or deactivated: its handle goes; it is registered again if it comes back
					if (t.registered) {
						Native.Aether_SceneDestroy(AeConst.KindInstance, t.handle);
					}
					renderers_.Remove(unchecked((int)t.handle));
					order_.RemoveAt(i);
					scanSetChanged_ = true;
					if (t.skinned != null) {
						skinnedTracked_.Remove(t);
						if (t.rayKey != 0) {
							Native.Aether_SceneMeshForget(t.rayKey); // (its ray tracing tree goes with it)
							t.rayKey = 0;
						}
					}
				}
			}
			// the LOD groups are read again when the set of renderers changed. (Not on a timer any more: reading 2 350
			// groups allocates their level arrays anew, and that garbage was a pause every ten seconds on a track.)
			if (scanSetChanged_ || lodGroups_.Count == 0 && Time.unscaledTime >= nextLodScan_) {
				nextLodScan_ = Time.unscaledTime + 10.0f;
				ScanLodGroups();
			}
			SkinnedSkipped = skinnedWaiting; // not drawn (yet): Unity has no GPU-skinned buffer for them, or the mesh cannot be described
		}

		private void ScanSweepMeshesAndMaterials() {
			// ---- what nothing references any more
			var usedMeshes = new HashSet<uint>();
			var usedMaterials = new HashSet<uint>();
			foreach (Tracked t in order_) {
				if (t.mesh != null) {
					usedMeshes.Add(t.meshHandle);
					if (t.skinnedVariants != null) {
						foreach (uint variant in t.skinnedVariants.Values) {
							usedMeshes.Add(variant);
						}
					}
				}
				if (t.materials != null) {
					foreach (uint m in t.materials) {
						usedMaterials.Add(m);
					}
				}
			}
			Sweep(meshes_, usedMeshes, AeConst.KindMesh, (mesh) => mesh == null);
			Sweep(materials_, usedMaterials, AeConst.KindMaterial, (m) => m.material == null);
		}

		private void ScanTexturesAndLights() {
			// textures: the ones the materials as they were sent name (not the Unity materials' own properties: a livery
			// comes from the renderer's property block, and a texture only found there was dropped and uploaded again
			// on alternate scans, which showed as a livery that blinks), plus the global ones
			var usedTextures = new HashSet<uint>();
			foreach (var kv in materials_) {
				if (kv.Value.material == null || kv.Value.textures == null) {
					continue;
				}
				foreach (uint texture in kv.Value.textures) {
					if (texture != 0) {
						usedTextures.Add(texture);
					}
				}
			}
			if (sky_ != null) usedTextures.Add(Handle(sky_));
			foreach (uint texture in particleTextures_) {
				usedTextures.Add(texture); // (asked for since the last scan: the particle systems that have particles)
			}
			particleTextures_.Clear();
			ScanProbes(usedTextures);
			Sweep(textures_, usedTextures, AeConst.KindTexture, (t) => t.texture == null);

			// ---- textures whose GPU resource changed (texture streaming loads or drops mips; render textures resize)
			foreach (var kv in textures_) {
				TrackedTexture t = kv.Value;
				if (t.texture == null) {
					continue;
				}
				int loadedMip = t.texture is Texture2D t2 ? t2.loadedMipmapLevel : 0;
				if (t.global || t.texture.updateCount != t.updateCount || t.texture.width != t.width || t.texture.height != t.height || loadedMip != t.loadedMip) {
					QueueTexture(t.texture);
				}
			}

			// ---- lights
			lights_.Clear();
			lights_.AddRange(UnityEngine.Object.FindObjectsByType<Light>(FindObjectsInactive.Exclude, FindObjectsSortMode.None));

		}

		// ---- LOD groups. Unity tells per renderer whether the camera sees it (Renderer.isVisible, with the LOD level
		// already chosen), but says nothing about what is outside the view. Shadow casters are needed from there too, so
		// for those the level is chosen here, with Unity's own rule (the group's height on screen against each level's
		// threshold, scaled by the LOD bias).
		private sealed class LodGroupInfo {
			public LODGroup group;
			public float[] thresholds;
			public int current; // the level for this frame, -1 = culled
			public bool still;  // not under a Rigidbody: its place and size are read when the groups are scanned
			public Vector3 reference;
			public float size;
		}

		private static void MeasureLodGroup(LodGroupInfo info) {
			LODGroup g = info.group;
			Transform tr = g.transform;
			Vector3 scale = tr.lossyScale;
			info.size = g.size * Mathf.Max(Mathf.Abs(scale.x), Mathf.Max(Mathf.Abs(scale.y), Mathf.Abs(scale.z)));
			info.reference = tr.TransformPoint(g.localReferencePoint);
		}

		private readonly List<LodGroupInfo> lodGroups_ = new List<LodGroupInfo>();

		private void ScanLodGroups() {
			lodGroups_.Clear();
			foreach (Tracked t in order_) {
				t.lodGroup = -1;
				t.lodMask = 0;
			}
			foreach (LODGroup g in UnityEngine.Object.FindObjectsByType<LODGroup>(FindObjectsInactive.Exclude, FindObjectsSortMode.None)) {
				if (!g.enabled) {
					continue;
				}
				LOD[] lods = g.GetLODs();
				var info = new LodGroupInfo { group = g, thresholds = new float[lods.Length], still = g.GetComponentInParent<Rigidbody>() == null };
				MeasureLodGroup(info);
				int index = lodGroups_.Count;
				for (int level = 0; level < lods.Length && level < 32; ++level) {
					info.thresholds[level] = lods[level].screenRelativeTransitionHeight;
					if (lods[level].renderers == null) {
						continue;
					}
					foreach (Renderer r in lods[level].renderers) {
						if (r != null && renderers_.TryGetValue(r.GetInstanceID(), out Tracked t)) {
							t.lodGroup = index;
							t.lodMask |= 1u << level;
						}
					}
				}
				lodGroups_.Add(info);
			}
		}

		private void UpdateLodLevels(Camera camera) {
			Vector3 eye = camera.transform.position;
			float halfTan = Mathf.Tan(camera.fieldOfView * 0.5f * Mathf.Deg2Rad);
			float bias = QualitySettings.lodBias;
			foreach (LodGroupInfo info in lodGroups_) {
				LODGroup g = info.group;
				info.current = -1;
				if (g == null) {
					continue;
				}
				if (!info.still) {
					MeasureLodGroup(info);
				}
				float distance = Mathf.Max(Vector3.Distance(eye, info.reference), 1e-4f);
				float height = info.size * 0.5f / (distance * halfTan) * bias;
				for (int level = 0; level < info.thresholds.Length; ++level) {
					if (height >= info.thresholds[level]) {
						info.current = level;
						break;
					}
				}
			}
		}

		private void Sweep<T>(Dictionary<uint, T> table, HashSet<uint> used, int kind, Func<T, bool> dead) {
			List<uint> remove = null;
			foreach (var kv in table) {
				if (!used.Contains(kv.Key) || dead(kv.Value)) {
					(remove ?? (remove = new List<uint>())).Add(kv.Key);
				}
			}
			if (remove == null) {
				return;
			}
			foreach (uint h in remove) {
				table.Remove(h);
				Native.Aether_SceneDestroy(kind, h);
				if (kind == AeConst.KindMesh) {
					geometryAsked_.Remove(h);
				}
			}
		}

		private float nextLodScan_;
		private int sliceCursor_;

		/// <summary>Looks at a share of the known mesh renderers again (mesh, materials, property blocks, state): all of them about twice a second at 60 frames a second.</summary>
		private void UpdateSlice() {
			int n = order_.Count;
			if (n == 0) {
				return;
			}
			int budget = Mathf.Max(32, n / 30 + 1);
			for (int i = 0; i < budget; ++i) {
				if (sliceCursor_ >= order_.Count) {
					sliceCursor_ = 0;
				}
				Tracked t = order_[sliceCursor_++];
				if (t.renderer != null && t.skinned == null) {
					UpdateTracked(t);
				}
			}
		}

		private unsafe void UpdateTracked(Tracked t) {
			Mesh mesh = t.skinned != null ? t.skinned.sharedMesh : t.filter != null ? t.filter.sharedMesh : null;
			t.layerBit = 1 << t.renderer.gameObject.layer;
			t.enabled = t.renderer.enabled && !t.renderer.forceRenderingOff && t.renderer.gameObject.activeInHierarchy;
			t.casting = t.renderer.shadowCastingMode;
			if (!t.live) {
				Matrix4x4 now = t.renderer.localToWorldMatrix;
				if (t.matrixKnown && !now.Equals(t.matrix)) {
					t.live = true; // it moves after all: followed every frame from here on
				}
				t.matrix = now;
				t.matrixKnown = true;
			}
			sharedMaterials_.Clear();
			t.renderer.GetSharedMaterials(sharedMaterials_);
			bool changed = !t.registered || mesh != t.mesh || t.materialIds == null || t.materialIds.Length != sharedMaterials_.Count;
			if (!changed) {
				for (int i = 0; i < sharedMaterials_.Count; ++i) {
					Material m = sharedMaterials_[i];
					if ((m != null ? m.GetInstanceID() : 0) != t.materialIds[i]) {
						changed = true;
						break;
					}
				}
			}
			if (!changed) {
				// a slot that got property values of its own since the last scan, or lost them: bound again
				for (int i = 0; i < sharedMaterials_.Count; ++i) {
					Material m = sharedMaterials_[i];
					if (m != null && m.shader != null && (BlockOf(t.renderer, i) != null) != (t.materials[i] != Handle(m))) {
						changed = true;
						break;
					}
				}
			}
			if (!changed) {
				if (t.skinned != null) {
					RegisterSkinnedMesh(t, mesh); // the skinned buffer can be replaced by Unity: looked at again every scan
				}
				// the game changes these values in place (a new paint, the brake lights): translated again every scan,
				// sent only when the result differs
				for (int i = 0; i < sharedMaterials_.Count; ++i) {
					Material m = sharedMaterials_[i];
					if (m != null && m.shader != null && t.materials[i] != 0 && t.materials[i] != Handle(m)) {
						RegisterMaterial(m, t.materials[i], t.renderer, i);
					}
				}
				return;
			}
			if (mesh == null) {
				if (t.registered) {
					Native.Aether_SceneDestroy(AeConst.KindInstance, t.handle);
					t.registered = false;
				}
				t.mesh = null;
				return;
			}
			t.mesh = mesh;
			if (t.skinned != null) {
				if (!RegisterSkinnedMesh(t, mesh)) {
					t.mesh = null; // not skinned on the GPU yet (never rendered so far): the next scan tries again
					return;
				}
			} else {
				t.meshHandle = Handle(mesh);
				QueueMesh(mesh);
			}
			int count = sharedMaterials_.Count;
			t.materialIds = new int[count];
			t.materials = new uint[count];
			for (int i = 0; i < count; ++i) {
				Material m = sharedMaterials_[i];
				t.materialIds[i] = m != null ? m.GetInstanceID() : 0;
				if (m == null || m.shader == null) {
					t.materials[i] = 0;
					continue;
				}
				bool own = BlockOf(t.renderer, i) != null;
				uint handle = own ? VariantHandle(m, t.renderer, i) : Handle(m);
				t.materials[i] = handle;
				if (own || !materials_.ContainsKey(handle)) {
					RegisterMaterial(m, handle, own ? t.renderer : null, i);
				}
			}
			BindInstance(t);
		}

		/// <summary>Sends the instance: its mesh, its materials, its flags.</summary>
		private unsafe void BindInstance(Tracked t) {
			AeInstanceDesc d = default;
			d.structSize = (uint)sizeof(AeInstanceDesc);
			d.instance = t.handle;
			d.mesh = t.meshHandle;
			d.flags = (t.renderer.shadowCastingMode != ShadowCastingMode.Off ? AeConst.InstanceCastShadows : 0) | (t.renderer.receiveShadows ? AeConst.InstanceReceiveShadows : 0) | (t.isStatic ? AeConst.InstanceStatic : 0);
			d.layer = (uint)t.renderer.gameObject.layer;
			d.materialCount = (uint)t.materials.Length;
			d.renderingLayerMask = t.renderer.renderingLayerMask;
			fixed (uint* p = t.materials) {
				t.registered = Native.Aether_SceneUpsertInstance(&d, p) != 0;
			}
		}

		/// <summary>
		/// Every frame, for the skinned renderers the camera sees: Unity has skinned this frame into one of the renderer's
		/// buffers (not the one of the frame before). The instance must draw from that one, or a part that moves fast
		/// (a car's suspension at speed) is drawn where it was a frame ago.
		/// </summary>
		private void RefreshSkinned() {
			foreach (Tracked t in skinnedTracked_) {
				if (!t.registered || t.mesh == null || t.renderer == null || !t.renderer.isVisible) {
					continue;
				}
				uint before = t.meshHandle;
				if (RegisterSkinnedMesh(t, t.mesh) && t.meshHandle != before) {
					BindInstance(t);
				}
			}
		}

		private readonly List<Tracked> skinnedTracked_ = new List<Tracked>();

		/// <summary>Translates a material (with `owner`'s own property values for slot `index`, if it has any) and sends it when it is new or differs from what was sent.</summary>
		private unsafe void RegisterMaterial(Material m, uint handle, Renderer owner, int index) {
			MaterialTranslator.Result result = MaterialTranslator.Translate(m, handle, owner != null ? BlockOf(owner, index) : null);
			for (int i = 0; i < 5; ++i) {
				Texture tex = result.textures[i];
				if (tex != null) {
					result.desc.textures[i] = Handle(tex);
					if (!textures_.ContainsKey(Handle(tex))) {
						QueueTexture(tex);
					}
				}
			}
			AeMaterialDesc d = result.desc;
			uint signature = 2166136261u;
			byte* bytes = (byte*)&d;
			for (int i = 0; i < sizeof(AeMaterialDesc); ++i) {
				signature = unchecked((signature ^ bytes[i]) * 16777619u);
			}
			int crc = m.ComputeCRC();
			if (materials_.TryGetValue(handle, out TrackedMaterial known) && known.signature == signature && known.material == m) {
				known.crc = crc;
				return; // nothing changed
			}
			Native.Aether_SceneUpsertMaterial(&d);
			var textures = new uint[5];
			for (int i = 0; i < 5; ++i) {
				textures[i] = d.textures[i];
			}
			materials_[handle] = new TrackedMaterial { material = m, crc = crc, owner = owner, index = index, signature = signature, textures = textures };
		}

		private void CheckMaterials() {
			// a material edited in place (the game recolours paint, switches lights on) keeps its id: its hash tells
			List<KeyValuePair<uint, TrackedMaterial>> changed = null;
			foreach (var kv in materials_) {
				Material m = kv.Value.material;
				if (m != null && m.ComputeCRC() != kv.Value.crc) {
					(changed ?? (changed = new List<KeyValuePair<uint, TrackedMaterial>>())).Add(kv);
				}
			}
			if (changed == null) {
				return;
			}
			foreach (var kv in changed) {
				RegisterMaterial(kv.Value.material, kv.Key, kv.Value.owner, kv.Value.index);
			}
		}

		private void QueueMesh(Mesh mesh) {
			uint h = Handle(mesh);
			if (!meshes_.ContainsKey(h) && pendingMeshSet_.Add(h)) {
				pendingMeshes_.Enqueue(mesh);
			}
		}

		// ---- what the game's renderer does after lighting (the screen camera's HDRP volume stack)
		private float nextVolume_;
		private uint tonemapMode_;
		private readonly float[] tonemapCurve_ = new float[6];
		private float postExposure_;
		private bool fogOn_;
		private float fogDensity_, fogBase_, fogMax_, fogMaxDistance_;
		private Color fogColor_ = Color.black, fogTint_ = Color.white;
		private bool fogFromSky_;
		private string hostFeatures_ = "";
		private bool fsBloom_ = true, fsMotionBlur_ = true, fsAntialiasing_ = true, fsPost_ = true;
		private float bloomIntensity_, bloomScatter_, motionBlur_;
		private uint antialiasing_;
		private bool ssrEnabled_;
		private float ssrMinSmoothness_ = 0.9f, ssrFadeStart_ = 0.9f, ssrScreenFade_ = 0.1f;
		private uint exposureMode_;
		private float exposureFixed_, exposureMin_, exposureMax_, exposureCompensation_;
		public int OffscreenCasters { get; private set; }
		public float ShadowDistance => shadowDistance_ > 0 ? shadowDistance_ : 150.0f;
		private VolumeStack volumeStack_;
		private string volumeProblem_ = "";
		private int volumeGeneration_ = -1;
		private float skyIntensity_ = 1, skyRotation_;
		private string skyState_ = "";
		private float shadowDistance_;
		private uint shadowCascades_;
		private readonly float[] shadowSplits_ = new float[3];
		private float aoIntensity_, aoRadius_, aoDirect_;
		private readonly float[] smh_ = new float[16]; // shadows, midtones, highlights (prepared rgb + unused), limits

		// HDRP's shadows / midtones / highlights value: rgb is a colour, w an offset that weighs four times as much when positive
		private void PrepareSmh(Vector4 v, int at) {
			float weight = v.w * (v.w < 0 ? 1.0f : 4.0f);
			smh_[at] = Mathf.Max(v.x + weight, 0);
			smh_[at + 1] = Mathf.Max(v.y + weight, 0);
			smh_[at + 2] = Mathf.Max(v.z + weight, 0);
			smh_[at + 3] = 0;
		}

		private void ReadHostVolume(Camera camera) {
			try {
				// The game describes its look with volumes (data of the scene, evaluated by Unity's volume manager for the
				// camera's position) and with per camera feature switches ("frame settings"). Both are read as data: no
				// renderer of the game's has to run for them.
				if (!VolumeManager.instance.isInitialized) {
					volumeStack_ = null; // between two pipelines: the previous values stay
					return;
				}
				if (volumeStack_ == null || volumeGeneration_ != PipelineSwitch.VolumeGeneration) {
					volumeGeneration_ = PipelineSwitch.VolumeGeneration;
					volumeStack_ = VolumeManager.instance.CreateStack();
				}
				VolumeStack stack = volumeStack_;
				LayerMask volumeLayers = ~0;
				Transform anchor = camera.transform;
				UnityEngine.Rendering.HighDefinition.HDAdditionalCameraData cameraData;
				if (camera.TryGetComponent(out cameraData)) {
					volumeLayers = cameraData.volumeLayerMask;
					if (cameraData.volumeAnchorOverride != null) {
						anchor = cameraData.volumeAnchorOverride;
					}
				}
				VolumeManager.instance.Update(stack, anchor, volumeLayers);
				HostFrameSettings fs = new HostFrameSettings(cameraData);
				bool fsTonemap = fs.IsEnabled(UnityEngine.Rendering.HighDefinition.FrameSettingsField.Tonemapping);
				bool fsGrading = fs.IsEnabled(UnityEngine.Rendering.HighDefinition.FrameSettingsField.ColorGrading);
				bool fsAo = fs.IsEnabled(UnityEngine.Rendering.HighDefinition.FrameSettingsField.SSAO);
				bool fsFog = fs.IsEnabled(UnityEngine.Rendering.HighDefinition.FrameSettingsField.AtmosphericScattering);
				fsBloom_ = fs.IsEnabled(UnityEngine.Rendering.HighDefinition.FrameSettingsField.Bloom);
				fsMotionBlur_ = fs.IsEnabled(UnityEngine.Rendering.HighDefinition.FrameSettingsField.MotionBlur);
				fsAntialiasing_ = fs.IsEnabled(UnityEngine.Rendering.HighDefinition.FrameSettingsField.Antialiasing);
				fsPost_ = fs.IsEnabled(UnityEngine.Rendering.HighDefinition.FrameSettingsField.Postprocess);
				string state = "post-processing " + fsPost_ + ", tonemapping " + fsTonemap + ", colour grading " + fsGrading + ", SSAO " + fsAo + ", fog " + fsFog
					+ ", bloom " + fsBloom_ + ", motion blur " + fsMotionBlur_ + ", antialiasing " + fsAntialiasing_
					+ ", SSR " + fs.IsEnabled(UnityEngine.Rendering.HighDefinition.FrameSettingsField.SSR) + (fs.DefaultsKnown ? "" : " (the game's default frame settings could not be read: everything counts as on)");
				if (state != hostFeatures_) {
					hostFeatures_ = state;
					Native.Log(1, "Host camera frame settings (" + camera.name + "): " + state);
				}
				ReadHostSky(stack);
				// post-processing: what the scene's volumes ask for, where the camera has the feature switched on
				var bloom = stack.GetComponent<UnityEngine.Rendering.HighDefinition.Bloom>();
				bool bloomOn = bloom != null && fsPost_ && fsBloom_ && bloom.intensity.value > 0;
				bloomIntensity_ = bloomOn ? bloom.intensity.value : 0;
				bloomScatter_ = bloomOn ? bloom.scatter.value : 0;
				var motionBlur = stack.GetComponent<UnityEngine.Rendering.HighDefinition.MotionBlur>();
				motionBlur_ = motionBlur != null && fsPost_ && fsMotionBlur_ ? motionBlur.intensity.value : 0;
				// (Aether's own options: LOD bias, anisotropic filtering, VSync. Antialiasing is Aether's switch on the native
				// side; the mode the game's menu or camera asks for is passed on for information only.)
				HostQualitySettings.Apply();
				antialiasing_ = cameraData != null ? (uint)cameraData.antialiasing : 0u;
				var ssr = stack.GetComponent<UnityEngine.Rendering.HighDefinition.ScreenSpaceReflection>();
				ssrEnabled_ = ssr != null && ssr.enabled.value && fs.IsEnabled(UnityEngine.Rendering.HighDefinition.FrameSettingsField.SSR);
				if (ssr != null) {
					ssrMinSmoothness_ = ssr.minSmoothness;
					ssrFadeStart_ = ssr.smoothnessFadeStart;
					ssrScreenFade_ = ssr.screenFadeDistance.value;
				}
				var tone = stack.GetComponent<UnityEngine.Rendering.HighDefinition.Tonemapping>();
				if (!fsTonemap) {
					tonemapMode_ = 1; // none
				} else if (tone != null) {
					switch (tone.mode.value) {
					case UnityEngine.Rendering.HighDefinition.TonemappingMode.None: tonemapMode_ = 1; break;
					case UnityEngine.Rendering.HighDefinition.TonemappingMode.Neutral: tonemapMode_ = 2; break;
					case UnityEngine.Rendering.HighDefinition.TonemappingMode.ACES: tonemapMode_ = 3; break;
					case UnityEngine.Rendering.HighDefinition.TonemappingMode.Custom: tonemapMode_ = 4; break;
					default: tonemapMode_ = 5; break;
					}
					tonemapCurve_[0] = tone.toeStrength.value;
					tonemapCurve_[1] = tone.toeLength.value;
					tonemapCurve_[2] = tone.shoulderStrength.value;
					tonemapCurve_[3] = tone.shoulderLength.value;
					tonemapCurve_[4] = tone.shoulderAngle.value;
					tonemapCurve_[5] = tone.gamma.value;
				}
				var grade = stack.GetComponent<UnityEngine.Rendering.HighDefinition.ColorAdjustments>();
				postExposure_ = grade != null && fsGrading ? grade.postExposure.value : 0;
				var fog = stack.GetComponent<UnityEngine.Rendering.HighDefinition.Fog>();
				fogOn_ = fsFog && fog != null && fog.enabled.value && fog.meanFreePath.value > 0;
				if (fogOn_) {
					fogDensity_ = 1.0f / fog.meanFreePath.value;
					fogBase_ = fog.baseHeight.value;
					fogMax_ = fog.maximumHeight.value;
					fogMaxDistance_ = fog.maxFogDistance.value;
					fogFromSky_ = fog.colorMode.value == UnityEngine.Rendering.HighDefinition.FogColorMode.SkyColor;
					fogColor_ = fog.color.value.linear;
					fogTint_ = fog.tint.value.linear;
				}
				var exposure = stack.GetComponent<UnityEngine.Rendering.HighDefinition.Exposure>();
				if (exposure != null) {
					exposureMode_ = exposure.mode.value == UnityEngine.Rendering.HighDefinition.ExposureMode.Fixed ? 1u : 2u;
					exposureFixed_ = exposure.fixedExposure.value;
					exposureMin_ = exposure.limitMin.value;
					exposureMax_ = exposure.limitMax.value;
					exposureCompensation_ = exposure.compensation.value;
				}
				var shadowSettings = stack.GetComponent<UnityEngine.Rendering.HighDefinition.HDShadowSettings>();
				if (shadowSettings != null) {
					shadowDistance_ = shadowSettings.maxShadowDistance.value;
					shadowCascades_ = (uint)Mathf.Clamp(shadowSettings.cascadeShadowSplitCount.value, 1, 4);
					shadowSplits_[0] = shadowSettings.cascadeShadowSplit0.value;
					shadowSplits_[1] = shadowSettings.cascadeShadowSplit1.value;
					shadowSplits_[2] = shadowSettings.cascadeShadowSplit2.value;
				}
				var ao = stack.GetComponent<UnityEngine.Rendering.HighDefinition.ScreenSpaceAmbientOcclusion>();
				aoIntensity_ = ao != null && fsAo ? ao.intensity.value : 0;
				aoRadius_ = ao != null ? ao.radius.value : 0;
				aoDirect_ = ao != null ? ao.directLightingStrength.value : 0;
				var smh = stack.GetComponent<UnityEngine.Rendering.HighDefinition.ShadowsMidtonesHighlights>();
				for (int i = 12; i < 16; ++i) {
					smh_[i] = 0; // off unless the camera grades
				}
				if (smh != null && fsGrading) {
					PrepareSmh(smh.shadows.value, 0);
					PrepareSmh(smh.midtones.value, 4);
					PrepareSmh(smh.highlights.value, 8);
					smh_[12] = smh.shadowsStart.value;
					smh_[13] = smh.shadowsEnd.value;
					smh_[14] = smh.highlightsStart.value;
					smh_[15] = smh.highlightsEnd.value;
				}
			} catch (Exception e) {
				// volume components this game does not have: the frame keeps its defaults
				string problem = e.GetType().Name + ": " + e.Message;
				if (problem != volumeProblem_) {
					volumeProblem_ = problem;
					Native.Log(2, "The game's volume settings could not be read: " + e);
				}
			}
		}

		// The sky: the game's HDRI sky volume names a cube map asset, an intensity and a rotation.
		private void ReadHostSky(VolumeStack stack) {
			Texture cube = null;
			float intensity = 1, rotation = 0;
			string kind = "none";
			var environment = stack.GetComponent<UnityEngine.Rendering.HighDefinition.VisualEnvironment>();
			int skyType = environment != null ? environment.skyType.value : 0;
			if (skyType == (int)UnityEngine.Rendering.HighDefinition.SkyType.HDRI) {
				var hdri = stack.GetComponent<UnityEngine.Rendering.HighDefinition.HDRISky>();
				if (hdri != null && hdri.hdriSky.value != null) {
					cube = hdri.hdriSky.value;
					switch (hdri.skyIntensityMode.value) {
					case UnityEngine.Rendering.HighDefinition.SkyIntensityMode.Multiplier: intensity = hdri.multiplier.value; break;
					case UnityEngine.Rendering.HighDefinition.SkyIntensityMode.Lux: intensity = hdri.desiredLuxValue.value / Mathf.Max(hdri.upperHemisphereLuxValue.value, 1e-4f); break;
					default: intensity = Mathf.Pow(2.0f, hdri.exposure.value) / 1.2f; break; // HDRP: ConvertEV100ToExposure(-exposure) = 2^exposure / 1.2
					}
					rotation = -hdri.rotation.value * Mathf.Deg2Rad; // (HDRP turns the lookup direction by -rotation)
					kind = "HDRI " + cube.name + " " + cube.width + ", intensity " + intensity.ToString("F3") + " (" + hdri.skyIntensityMode.value + "), rotation " + hdri.rotation.value.ToString("F0");
				}
			} else if (skyType != 0) {
				kind = "sky type " + skyType + " (not an HDRI sky: Aether lights the scene with the ambient probe)";
			}
			if (kind != skyState_) {
				skyState_ = kind;
				Native.Log(1, "Host sky: " + kind);
			}
			skyIntensity_ = intensity;
			skyRotation_ = rotation;
			if (cube != sky_) {
				sky_ = cube;
				if (cube != null) {
					QueueTexture(cube);
				}
			}
		}

		// ---- reflection probes: the host's cube maps and where they apply
		private AeProbe[] probeScratch_ = new AeProbe[8];
		private int probeSignature_;
		public int ProbeCount { get; private set; }

		private unsafe void ScanProbes(HashSet<uint> usedTextures) {
			ReflectionProbe[] found = UnityEngine.Object.FindObjectsByType<ReflectionProbe>(FindObjectsInactive.Exclude, FindObjectsSortMode.None);
			if (probeScratch_.Length < found.Length) {
				probeScratch_ = new AeProbe[Mathf.NextPowerOfTwo(found.Length)];
			}
			int count = 0, signature = 17;
			var described = new System.Text.StringBuilder();
			fixed (AeProbe* probes = probeScratch_) {
				foreach (ReflectionProbe probe in found) {
					if (probe == null || !probe.isActiveAndEnabled) {
						continue;
					}
					Texture texture = null;
					Vector3 extents = probe.size * 0.5f;
					float blend = probe.blendDistance, intensity = probe.intensity;
					uint flags = probe.boxProjection ? 1u : 0u;
					// HDRP keeps the real settings on its own component
					if (probe.TryGetComponent(out UnityEngine.Rendering.HighDefinition.HDAdditionalReflectionData hd)) {
						texture = hd.texture;
						var volume = hd.influenceVolume;
						if (volume != null) {
							if (volume.shape == UnityEngine.Rendering.HighDefinition.InfluenceShape.Sphere) {
								flags |= 2u;
								extents = new Vector3(volume.sphereRadius, volume.sphereRadius, volume.sphereRadius);
								blend = volume.sphereBlendDistance;
							} else {
								extents = volume.boxSize * 0.5f;
								Vector3 b = volume.boxBlendDistancePositive;
								blend = Mathf.Max(b.x, Mathf.Max(b.y, b.z));
							}
						}
						intensity = hd.multiplier * hd.weight;
						flags = (flags & 2u) | (hd.isProjectionInfinite ? 0u : 1u);
					}
					if (texture == null) {
						texture = probe.texture;
					}
					if (texture == null || texture.dimension != TextureDimension.Cube) {
						continue; // nothing captured (yet)
					}
					uint handle = Handle(texture);
					usedTextures.Add(handle);
					if (!textures_.ContainsKey(handle)) {
						QueueTexture(texture);
					}
					Transform t = probe.transform;
					Matrix4x4 worldToLocal = Matrix4x4.TRS(t.position, t.rotation, Vector3.one).inverse;
					AeProbe* p = probes + count++;
					*p = default;
					p->texture = handle;
					p->flags = flags;
					for (int r = 0; r < 3; ++r) {
						for (int c = 0; c < 4; ++c) {
							p->worldToLocal[r * 4 + c] = worldToLocal[r, c];
						}
					}
					p->extents[0] = extents.x; p->extents[1] = extents.y; p->extents[2] = extents.z;
					p->blendDistance = blend;
					Vector3 capture = t.position;
					p->capturePosition[0] = capture.x; p->capturePosition[1] = capture.y; p->capturePosition[2] = capture.z;
					p->intensity = intensity;
					described.Append("; ").Append(probe.name).Append(" cube ").Append(texture.name).Append(' ').Append(texture.width).Append(" at ").Append(t.position.ToString("F1"))
						.Append(" extents ").Append(extents.ToString("F1")).Append(" blend ").Append(blend.ToString("F2")).Append(" intensity ").Append(intensity.ToString("F2")).Append(" flags ").Append(flags)
						.Append(" in store ").Append(textures_.ContainsKey(handle));
					unchecked {
						signature = signature * 31 + (int)handle;
						signature = signature * 31 + worldToLocal.GetHashCode() + extents.GetHashCode() + blend.GetHashCode() + intensity.GetHashCode() + (int)flags;
					}
				}
				if (signature != probeSignature_ || count != ProbeCount) {
					probeSignature_ = signature;
					ProbeCount = count;
					Native.Aether_SceneSetProbes(probes, (uint)count, (uint)sizeof(AeProbe));
					Native.Log(1, "Reflection probes: " + count + described);
				}
			}
		}

		private int devFlags_;
		private bool lightLayers_;

		// Whether the game's HDRP asset has light layers on (then a light only reaches renderers that share a layer with it).
		private static bool HostLightLayers() {
			try {
				// the game's own pipeline asset says whether its lights are restricted to rendering layers (read as data)
				var asset = (PipelineSwitch.GameAsset ?? GraphicsSettings.currentRenderPipeline) as UnityEngine.Rendering.HighDefinition.HDRenderPipelineAsset;
				return asset != null && asset.currentPlatformRenderPipelineSettings.supportLightLayers;
			} catch (Exception) {
				return false;
			}
		}

		private void QueueTexture(Texture tex) {
			if ((devFlags_ & 1) != 0) {
				return; // experiment: no textures
			}
			if ((devFlags_ & 2) != 0 && (tex is RenderTexture || tex.updateCount != 0)) {
				return; // experiment: no textures Unity keeps changing
			}
			if ((devFlags_ & 4) != 0 && textures_.ContainsKey(Handle(tex))) {
				return; // experiment: a texture's pointer is asked for once
			}
			if (pendingTextureSet_.Add(Handle(tex))) {
				pendingTextures_.Enqueue(tex);
			}
		}

		// ------------------------------------------------------------------------------------------------ registration

		private static byte DxgiFormat(VertexAttributeFormat format, int dimension) {
			switch (format) {
			case VertexAttributeFormat.Float32: return dimension == 1 ? (byte)41 : dimension == 2 ? (byte)16 : dimension == 3 ? (byte)6 : (byte)2;
			case VertexAttributeFormat.Float16: return dimension == 2 ? (byte)34 : dimension == 4 ? (byte)10 : (byte)0;
			case VertexAttributeFormat.UNorm8: return dimension == 4 ? (byte)28 : (byte)0;
			case VertexAttributeFormat.SNorm8: return dimension == 4 ? (byte)31 : (byte)0;
			case VertexAttributeFormat.UNorm16: return dimension == 2 ? (byte)35 : dimension == 4 ? (byte)11 : (byte)0;
			case VertexAttributeFormat.SNorm16: return dimension == 2 ? (byte)37 : dimension == 4 ? (byte)13 : (byte)0;
			default: return 0;
			}
		}

		private static int Semantic(VertexAttribute a) {
			switch (a) {
			case VertexAttribute.Position: return AeConst.SemanticPosition;
			case VertexAttribute.Normal: return AeConst.SemanticNormal;
			case VertexAttribute.Tangent: return AeConst.SemanticTangent;
			case VertexAttribute.Color: return AeConst.SemanticColor;
			case VertexAttribute.TexCoord0: return AeConst.SemanticTexCoord0;
			case VertexAttribute.TexCoord1: return AeConst.SemanticTexCoord1;
			default: return -1;
			}
		}

		/// <summary>
		/// The matrix that takes Unity's skinned vertices (SkinnedMeshRenderer.GetVertexBuffer) to the world: the space of
		/// the renderer's root bone (its own transform when it has none); for cloth that space without its scale.
		/// Renderer.localToWorldMatrix is the renderer's own transform, which is neither: with it people stood 0.9 m deep
		/// in the ground and flags hung 2 m low.
		/// Measured in the game (survey, "skinned"): Unity's own world bounds of each skinned renderer are its local bounds
		/// taken through exactly these matrices (root bones with scale 1 and 100; cloth with scale 0.5). That the vertices
		/// are in the same space as the bounds is an assumption, checked by eye against the game's picture.
		/// </summary>
		internal static Matrix4x4 SkinnedMatrix(SkinnedMeshRenderer r) {
			Transform root = r.rootBone != null ? r.rootBone : r.transform;
			if (r.TryGetComponent(out Cloth cloth) && cloth.enabled) {
				return Matrix4x4.TRS(root.position, root.rotation, Vector3.one);
			}
			return root.localToWorldMatrix;
		}

		/// <summary>Describes a skinned renderer's mesh with Unity's skinned vertex buffer as stream 0. False = not available yet.</summary>
		private bool RegisterSkinnedMesh(Tracked t, Mesh mesh) {
			if (mesh == null) {
				return false;
			}
			GraphicsBuffer skinnedBuffer = null;
			try {
				skinnedBuffer = t.skinned.GetVertexBuffer();
				if (skinnedBuffer == null) {
					return false;
				}
				IntPtr pointer = skinnedBuffer.GetNativeBufferPtr();
				int stride = skinnedBuffer.stride;
				if (pointer == IntPtr.Zero || stride <= 0) {
					return false;
				}
				if (pointer == t.skinnedPointer && meshes_.ContainsKey(t.meshHandle)) {
					return true;
				}
				if (t.skinnedVariants != null && t.skinnedVariants.TryGetValue(pointer, out uint known) && meshes_.ContainsKey(known)) {
					t.meshHandle = known; // the other buffer, registered before
					t.skinnedPointer = pointer;
					return true;
				}
				// the skinned buffer replaces the mesh's stream 0, which must hold exactly what skinning produces
				attributes_.Clear();
				mesh.GetVertexAttributes(attributes_);
				// (a skinned buffer with the mesh's own stride for stream 0 keeps that stream's whole layout: small meshes that
				// have everything in one stream)
				bool sameLayout = stride == mesh.GetVertexBufferStride(0);
				foreach (VertexAttributeDescriptor a in attributes_) {
					bool skinnedAttribute = a.attribute == VertexAttribute.Position || a.attribute == VertexAttribute.Normal || a.attribute == VertexAttribute.Tangent;
					if (!sameLayout && (a.stream == 0) != skinnedAttribute) {
						// (seen right after a renderer appears, before Unity skinned it once: the buffer is not the final
						// one yet. Tried again; only a renderer that never fits is reported.)
						if (++t.skinnedFailures == 40 && !t.skinnedUnsupported) {
							t.skinnedUnsupported = true;
							++UnsupportedFormats;
						}
						return false;
					}
				}
				if (t.skinnedVariants == null) {
					t.skinnedVariants = new Dictionary<IntPtr, uint>();
				} else if (t.skinnedVariants.Count >= 4) {
					t.skinnedVariants.Clear(); // Unity replaced its buffers: the old meshes go with the next sweep
				}
				uint handle = VariantHandle(mesh, t.renderer, 1000 + t.skinnedVariants.Count + (int)(frameIndex_ & 0xFFFF) * 8);
				if (!RegisterMeshCore(mesh, handle, pointer, stride) || !meshes_.ContainsKey(handle)) {
					return false;
				}
				t.skinnedVariants[pointer] = handle;
				if (t.rayKey != 0) {
					Native.Aether_SceneMeshAlias(handle, t.rayKey);
				}
				t.meshHandle = handle;
				t.skinnedPointer = pointer;
				t.skinnedFailures = 0;
				if (t.skinnedUnsupported) {
					t.skinnedUnsupported = false;
					--UnsupportedFormats;
				}
				return true;
			} finally {
				if (skinnedBuffer != null) {
					skinnedBuffer.Dispose(); // only this handle: the buffer itself stays Unity's
				}
			}
		}

		private bool RegisterMesh(Mesh mesh) {
			return RegisterMeshCore(mesh, Handle(mesh), IntPtr.Zero, 0);
		}

		private unsafe bool RegisterMeshCore(Mesh mesh, uint handle, IntPtr stream0, int stream0Stride) {
			int streams = mesh.vertexBufferCount;
			if (streams < 1 || mesh.vertexCount == 0) {
				// an empty mesh: one the game builds while it runs (tyre marks, trails) and has not filled yet. It is watched
				// from now on and sent whenever its contents change (UpdateDynamicMeshes).
				if (!dynamicMeshes_.ContainsKey(handle)) {
					dynamicMeshes_[handle] = new DynamicMesh { mesh = mesh };
				}
				return true;
			}
			if (streams > 4 || mesh.subMeshCount < 1) {
				++UnsupportedFormats;
				Native.Log(2, "Mesh not drawn: " + mesh.name + " has " + streams + " vertex streams, " + mesh.vertexCount + " vertices, " + mesh.subMeshCount + " sub meshes");
				return true;
			}
			AeMeshDesc d = default;
			d.structSize = (uint)sizeof(AeMeshDesc);
			d.mesh = handle;
			d.vertexCount = (uint)mesh.vertexCount;
			d.indexStride = mesh.indexFormat == IndexFormat.UInt16 ? 2u : 4u;
			d.streamCount = (uint)streams;
			IntPtr* buffers = &d.vertexBuffer0;
			for (int s = 0; s < streams; ++s) {
				bool replaced = s == 0 && stream0 != IntPtr.Zero;
				buffers[s] = replaced ? stream0 : mesh.GetNativeVertexBufferPtr(s);
				d.streamStride[s] = (uint)(replaced ? stream0Stride : mesh.GetVertexBufferStride(s));
				if (buffers[s] == IntPtr.Zero) {
					return false;
				}
			}
			d.indexBuffer = mesh.GetNativeIndexBufferPtr();
			if (d.indexBuffer == IntPtr.Zero) {
				return false;
			}
			attributes_.Clear();
			mesh.GetVertexAttributes(attributes_);
			AeVertexAttribute* attributes = &d.attribute0;
			uint count = 0;
			bool hasPosition = false;
			foreach (VertexAttributeDescriptor a in attributes_) {
				int semantic = Semantic(a.attribute);
				byte format = DxgiFormat(a.format, a.dimension);
				if (semantic < 0 || format == 0 || count >= 6) {
					continue; // not read by Aether's shaders, or a format with no direct DXGI equivalent: the default is used
				}
				attributes[count].semantic = (byte)semantic;
				attributes[count].stream = (byte)a.stream;
				attributes[count].offset = (byte)mesh.GetVertexAttributeOffset(a.attribute);
				attributes[count].dxgiFormat = format;
				hasPosition |= semantic == AeConst.SemanticPosition;
				++count;
			}
			if (!hasPosition) {
				++UnsupportedFormats;
				Native.Log(2, "Mesh not drawn: " + mesh.name + " has no position attribute Aether can read (" + attributes_.Count + " attributes)");
				return true;
			}
			d.attributeCount = count;
			Bounds b = mesh.bounds;
			d.boundsCenter[0] = b.center.x; d.boundsCenter[1] = b.center.y; d.boundsCenter[2] = b.center.z;
			d.boundsExtents[0] = b.extents.x; d.boundsExtents[1] = b.extents.y; d.boundsExtents[2] = b.extents.z;
			int subMeshes = mesh.subMeshCount;
			if (subMeshes_.Length < subMeshes) {
				subMeshes_ = new AeSubMesh[subMeshes];
			}
			uint indexCount = 0;
			for (int i = 0; i < subMeshes; ++i) {
				SubMeshDescriptor sm = mesh.GetSubMesh(i);
				// only triangle lists are drawn; anything else becomes an empty range
				bool triangles = sm.topology == MeshTopology.Triangles;
				subMeshes_[i].indexStart = (uint)sm.indexStart;
				subMeshes_[i].indexCount = triangles ? (uint)sm.indexCount : 0u;
				subMeshes_[i].baseVertex = (uint)sm.baseVertex;
				indexCount = Math.Max(indexCount, (uint)(sm.indexStart + sm.indexCount));
			}
			d.indexCount = indexCount;
			d.subMeshCount = (uint)subMeshes;
			fixed (AeSubMesh* p = subMeshes_) {
				if (Native.Aether_SceneUpsertMesh(&d, p) != 0) {
					meshes_[d.mesh] = mesh;
					if (stream0 == IntPtr.Zero && !dynamicMeshes_.ContainsKey(handle) && geometryAsked_.Add(handle)) {
						geometryQueue_.Enqueue(handle); // (its triangles for ray tracing: read back from the GPU, a few meshes at a time)
					}
				} else {
					++UnsupportedFormats;
					Native.Log(2, "Mesh not drawn: " + mesh.name + " was refused by the renderer (" + d.vertexCount + " vertices, " + indexCount + " indices, " + streams + " streams)");
				}
			}
			return true;
		}

		private unsafe bool RegisterTexture(Texture tex) {
			IntPtr pointer = tex.GetNativeTexturePtr();
			if (pointer == IntPtr.Zero) {
				return false;
			}
			uint handle = Handle(tex);
			textures_.TryGetValue(handle, out TrackedTexture t);
			bool known = t != null;
			if (!known) {
				t = new TrackedTexture { texture = tex, global = false };
				textures_[handle] = t;
			}
			int loadedMip = tex is Texture2D t2 ? t2.loadedMipmapLevel : 0;
			if (known && t.pointer == pointer) {
				t.updateCount = tex.updateCount;
				t.loadedMip = loadedMip;
				return true; // same GPU resource: nothing to tell the native side
			}
			AeTextureDesc d = default;
			d.structSize = (uint)sizeof(AeTextureDesc);
			d.texture = handle;
			d.resource = pointer;
			d.width = (uint)tex.width;
			d.height = (uint)tex.height;
			d.mipCount = (uint)tex.mipmapCount;
			switch (tex.dimension) {
			case TextureDimension.Tex2D: d.dimension = AeConst.Texture2D; break;
			case TextureDimension.Cube: d.dimension = AeConst.TextureCube; break;
			case TextureDimension.CubeArray: d.dimension = AeConst.TextureCubeArray; break;
			case TextureDimension.Tex2DArray: d.dimension = AeConst.Texture2DArray; break;
			case TextureDimension.Tex3D: d.dimension = AeConst.Texture3D; break;
			default:
				++UnsupportedFormats;
				Native.Log(2, "Texture not used: " + tex.name + " (" + tex.GetType().Name + ", dimension " + tex.dimension + ", " + tex.width + " x " + tex.height + ")");
				return true;
			}
			Native.Aether_SceneUpsertTexture(&d);
			t.pointer = pointer;
			t.updateCount = tex.updateCount;
			t.width = tex.width;
			t.height = tex.height;
			t.loadedMip = loadedMip;
			return true;
		}

		// What Unity has not put on the GPU (yet) is asked for again, but not every frame: asking for a native pointer
		// makes the main thread wait for the render thread, and one texture that never gets there (seen on a track: one
		// stays pending for the whole session) cost that wait in every frame.
		private readonly Dictionary<uint, int> pendingFailures_ = new Dictionary<uint, int>();
		private float pendingRetryAt_;

		private bool WaitsForRetry(uint handle, bool retryNow) {
			return !retryNow && pendingFailures_.ContainsKey(handle);
		}

		private void PendingFailed(UnityEngine.Object o, uint handle) {
			pendingFailures_.TryGetValue(handle, out int n);
			pendingFailures_[handle] = ++n;
			if (n == 20) {
				Native.Log(1, "Scene capture: still not on the GPU after 20 tries: " + o.GetType().Name + " " + o.name + " (asked for again once a second)");
			}
		}

		private void ProcessPending() {
			int budget = PointerQueriesPerFrame;
			int retries = 0;
			bool retryNow = Time.unscaledTime >= pendingRetryAt_;
			if (retryNow) {
				pendingRetryAt_ = Time.unscaledTime + 1.0f;
			}
			while (budget > 0 && pendingMeshes_.Count > retries) {
				Mesh mesh = pendingMeshes_.Dequeue();
				if (mesh == null) {
					continue;
				}
				if (WaitsForRetry(Handle(mesh), retryNow)) {
					pendingMeshes_.Enqueue(mesh);
					++retries;
					continue;
				}
				budget -= mesh.vertexBufferCount + 1;
				if (RegisterMesh(mesh)) {
					pendingMeshSet_.Remove(Handle(mesh));
					pendingFailures_.Remove(Handle(mesh));
				} else {
					pendingMeshes_.Enqueue(mesh); // not uploaded by Unity yet
					PendingFailed(mesh, Handle(mesh));
					++retries;
				}
			}
			retries = 0;
			while (budget > 0 && pendingTextures_.Count > retries) {
				Texture tex = pendingTextures_.Dequeue();
				if (tex == null) {
					continue;
				}
				if (WaitsForRetry(Handle(tex), retryNow)) {
					pendingTextures_.Enqueue(tex);
					++retries;
					continue;
				}
				--budget;
				if (RegisterTexture(tex)) {
					pendingTextureSet_.Remove(Handle(tex));
					pendingFailures_.Remove(Handle(tex));
				} else {
					pendingTextures_.Enqueue(tex);
					PendingFailed(tex, Handle(tex));
					++retries;
				}
			}
		}

		// ------------------------------------------------------------------------------------------------ the frame

		/// <summary>
		/// Keeps the native side's objects in step with the scene: scans, material checks and the native pointer queries.
		/// Called from Update, never from inside the render loop: Mesh.GetNative*BufferPtr and Texture.GetNativeTexturePtr
		/// make the main thread wait for the render thread, and doing that while the render pipeline is in the middle of
		/// recording a frame (graphics jobs) upset Unity's own texture uploads (docs/development/stability-investigation.md).
		/// </summary>
		// ---- what the capture costs on the main thread (milliseconds; the window restarts every few seconds)
		private struct Cost {
			public double sum, max, lastAvg, lastMax;
			public int count;
			public void Add(long ticks) {
				double ms = ticks * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
				sum += ms;
				max = Math.Max(max, ms);
				++count;
			}
			public void Roll() {
				lastAvg = count > 0 ? sum / count : 0;
				lastMax = max;
				sum = max = 0;
				count = 0;
			}
			public override string ToString() { return lastAvg.ToString("F2") + " avg / " + lastMax.ToString("F2") + " max"; }
		}

		private Cost scanCost_, tickCost_, frameCost_;
		private float nextCostRoll_;

		public void Tick() {
			if (!active_) {
				return;
			}
			float now = Time.unscaledTime;
			if (now >= nextCostRoll_) {
				nextCostRoll_ = now + 5.0f;
				scanCost_.Roll();
				tickCost_.Roll();
				frameCost_.Roll();
			}
			long tickStart = System.Diagnostics.Stopwatch.GetTimestamp();
			try {
				TickCore(now);
			} finally {
				tickCost_.Add(System.Diagnostics.Stopwatch.GetTimestamp() - tickStart);
			}
		}

		private void TickCore(float now) {
			if (scanPhase_ == 0 && now >= nextScan_) {
				// (a big scene is searched for new renderers less often: the search itself is what costs there)
				nextScan_ = now + (order_.Count > 2000 ? ScanInterval * 4.0f : ScanInterval);
				scanPhase_ = 1;
			}
			if (scanPhase_ != 0) {
				long scanStart = System.Diagnostics.Stopwatch.GetTimestamp();
				ScanStep();
				scanCost_.Add(System.Diagnostics.Stopwatch.GetTimestamp() - scanStart);
			}
			UpdateSlice();
			if (now >= nextMaterialCheck_) {
				nextMaterialCheck_ = now + MaterialCheckInterval;
				CheckMaterials();
			}
			RefreshOwnedMaterials();
			ProcessPending();
			UpdateDynamicMeshes();
			UpdateGeometry();
			UpdateSkinnedGeometry();
		}

		// ---- geometry for ray tracing. The game's meshes cannot be read on the CPU (not marked readable), so their
		// vertex and index buffers are read back from the GPU (Unity's asynchronous readback: no stall), once per mesh, and
		// handed to the native side, which builds the ray tracing tree from them. Skinned and run-time built meshes are
		// not sent: what they hold changes.
		private readonly Queue<uint> geometryQueue_ = new Queue<uint>();
		private readonly HashSet<uint> geometryAsked_ = new HashSet<uint>();
		private int geometryInFlight_, geometrySent_, geometryFailed_;
		private uint geometryGeneration_;
		private const int GeometryAtOnce = 8;
		public string GeometryStatus => skinnedRayReads_ + " skinned poses read, " + geometrySent_ + " sent, " + geometryQueue_.Count + " waiting, " + geometryInFlight_ + " being read, " + geometryFailed_ + " failed";

		// ---- skinned meshes for ray tracing. Their vertices change every frame (Unity skins on the GPU into the buffer the
		// renderer draws from), so the tree is built once from the first pose read back, and after that the vertices are
		// read back again and again, a couple of renderers per frame in turn, and the tree is refitted around them. The
		// renderer's vertex buffers take turns (one per frame) under different mesh handles: all are traced as one tree.
		private int skinnedRayCursor_;
		private int skinnedRayReads_;
		private int skinnedReadsPerFrame_ = 0;
		private float nextSkinnedSetting_;

		private unsafe void UpdateSkinnedGeometry() {
			// (how many per frame is Aether's own setting, none without ray traced reflections; looked up once a second)
			if (Time.unscaledTime >= nextSkinnedSetting_) {
				nextSkinnedSetting_ = Time.unscaledTime + 1.0f;
				skinnedReadsPerFrame_ = Native.Aether_SettingNumber("rayTracedReflections", 1) > 0.5 ? (int)Math.Round(Native.Aether_SettingNumber("skinnedRayUpdates", 0)) : 0;
			}
			int count = skinnedTracked_.Count;
			int first = count > 0 ? skinnedRayCursor_ % count : 0;
			for (int step = 0, started = 0; step < count && started < skinnedReadsPerFrame_; ++step) {
				Tracked t = skinnedTracked_[(first + step) % count];
				if (!t.registered || t.mesh == null || t.skinned == null || t.rayReading || !meshes_.ContainsKey(t.meshHandle) || !t.renderer.isVisible) {
					continue;
				}
				Mesh mesh = t.mesh;
				uint generation = geometryGeneration_;
				if (t.rayKey == 0) {
					t.rayKey = VariantHandle(mesh, t.renderer, 7777777);
					t.rayIndicesSent = false;
					Native.Aether_SceneMeshChanging(t.rayKey);
					if (t.skinnedVariants != null) {
						foreach (uint variant in t.skinnedVariants.Values) {
							Native.Aether_SceneMeshAlias(variant, t.rayKey);
						}
					}
				}
				uint key = t.rayKey;
				GraphicsBuffer vb = null;
				try {
					vb = t.skinned.GetVertexBuffer();
				} catch (Exception) {
				}
				if (vb == null) {
					continue;
				}
				// where the positions are in what skinning writes: at the start, three floats (when the buffer has the mesh's
				// own layout for stream 0: where the mesh has them)
				uint stride = (uint)vb.stride, positionOffset = 0, positionFormat = 6; // DXGI_FORMAT_R32G32B32_FLOAT
				if (vb.stride == mesh.GetVertexBufferStride(0)) {
					attributes_.Clear();
					mesh.GetVertexAttributes(attributes_);
					foreach (VertexAttributeDescriptor a in attributes_) {
						if (a.attribute == VertexAttribute.Position && a.stream == 0) {
							positionOffset = (uint)mesh.GetVertexAttributeOffset(a.attribute);
							byte format = DxgiFormat(a.format, a.dimension);
							positionFormat = format != 0 ? format : positionFormat;
						}
					}
				}
				if (!t.rayIndicesSent) {
					int subMeshes = mesh.subMeshCount;
					GraphicsBuffer ib = null;
					try {
						ib = mesh.GetIndexBuffer();
					} catch (Exception) {
					}
					if (ib == null || subMeshes < 1) {
						vb.Dispose();
						continue;
					}
					AeSubMesh[] subs = new AeSubMesh[subMeshes];
					for (int i = 0; i < subMeshes; ++i) {
						SubMeshDescriptor sm = mesh.GetSubMesh(i);
						subs[i].indexStart = (uint)sm.indexStart;
						subs[i].indexCount = sm.topology == MeshTopology.Triangles ? (uint)sm.indexCount : 0u;
						subs[i].baseVertex = (uint)sm.baseVertex;
					}
					uint indexStride = mesh.indexFormat == IndexFormat.UInt16 ? 2u : 4u;
					t.rayIndicesSent = true;
					AsyncGPUReadback.Request(ib, (AsyncGPUReadbackRequest r) => {
						if (!r.hasError && generation == geometryGeneration_) {
							var data = r.GetData<byte>();
							fixed (AeSubMesh* p = subs) {
								Native.Aether_SceneMeshIndices(key, Unity.Collections.LowLevel.Unsafe.NativeArrayUnsafeUtility.GetUnsafeReadOnlyPtr(data), (uint)data.Length, indexStride, p, (uint)subs.Length);
							}
						} else {
							t.rayIndicesSent = false;
						}
						ib.Dispose();
					});
				}
				t.rayReading = true;
				++started;
				AsyncGPUReadback.Request(vb, (AsyncGPUReadbackRequest r) => {
					t.rayReading = false;
					if (!r.hasError && generation == geometryGeneration_) {
						var data = r.GetData<byte>();
						Native.Aether_SceneMeshVertices(key, Unity.Collections.LowLevel.Unsafe.NativeArrayUnsafeUtility.GetUnsafeReadOnlyPtr(data), (uint)data.Length, stride, positionOffset, positionFormat, 0, 0, 0);
						++skinnedRayReads_;
					}
					vb.Dispose();
				});
				skinnedRayCursor_ = (first + step + 1) % count; // (the next frame goes on after the last one read)
			}
		}

		private void ResetGeometry() {
			++geometryGeneration_;
			foreach (Tracked t in skinnedTracked_) {
				t.rayKey = 0;
				t.rayIndicesSent = false;
			}
			geometryQueue_.Clear();
			geometryAsked_.Clear();
			geometrySent_ = geometryFailed_ = 0;
		}

		private unsafe void UpdateGeometry() {
			while (geometryInFlight_ < GeometryAtOnce && geometryQueue_.Count > 0) {
				uint handle = geometryQueue_.Dequeue();
				if (!meshes_.TryGetValue(handle, out Mesh mesh) || mesh == null) {
					continue;
				}
				// where the positions are
				attributes_.Clear();
				mesh.GetVertexAttributes(attributes_);
				int stream = -1, offset = 0, uvStream = -1, uvAt = 0;
				byte format = 0, uvKind = 0;
				foreach (VertexAttributeDescriptor a in attributes_) {
					if (a.attribute == VertexAttribute.Position) {
						stream = a.stream;
						offset = mesh.GetVertexAttributeOffset(a.attribute);
						format = DxgiFormat(a.format, a.dimension);
					} else if (a.attribute == VertexAttribute.TexCoord0) {
						// (what a ray's hit on this mesh looks its texture up with)
						uvStream = a.stream;
						uvAt = mesh.GetVertexAttributeOffset(a.attribute);
						uvKind = DxgiFormat(a.format, a.dimension);
					}
				}
				if (uvKind == 0) {
					uvStream = -1;
				}
				int subMeshes = mesh.subMeshCount;
				if (stream < 0 || format == 0 || subMeshes < 1) {
					++geometryFailed_;
					continue;
				}
				AeSubMesh[] subs = new AeSubMesh[subMeshes];
				for (int i = 0; i < subMeshes; ++i) {
					SubMeshDescriptor sm = mesh.GetSubMesh(i);
					subs[i].indexStart = (uint)sm.indexStart;
					subs[i].indexCount = sm.topology == MeshTopology.Triangles ? (uint)sm.indexCount : 0u;
					subs[i].baseVertex = (uint)sm.baseVertex;
				}
				GraphicsBuffer vb = null, ib = null, ub = null;
				try {
					vb = mesh.GetVertexBuffer(stream);
					ib = mesh.GetIndexBuffer();
					if (uvStream >= 0 && uvStream != stream) {
						ub = mesh.GetVertexBuffer(uvStream);
					}
				} catch (Exception) {
					// (a mesh whose buffers Unity does not hand out)
				}
				if (vb == null || ib == null) {
					vb?.Dispose();
					ib?.Dispose();
					ub?.Dispose();
					++geometryFailed_;
					continue;
				}
				// 0 none, 1 with the positions, 2 in a stream of their own (read back as a third part)
				uint uvMode = uvStream < 0 ? 0u : uvStream == stream ? 1u : ub != null ? 2u : 0u;
				uint uvOffset = (uint)uvAt, uvFormat = uvKind;
				uint generation = geometryGeneration_;
				if (ub != null) {
					uint uvStride = (uint)mesh.GetVertexBufferStride(uvStream);
					++geometryInFlight_;
					AsyncGPUReadback.Request(ub, (AsyncGPUReadbackRequest r) => {
						--geometryInFlight_;
						if (!r.hasError && generation == geometryGeneration_) {
							var data = r.GetData<byte>();
							Native.Aether_SceneMeshUvs(handle, Unity.Collections.LowLevel.Unsafe.NativeArrayUnsafeUtility.GetUnsafeReadOnlyPtr(data), (uint)data.Length, uvStride, uvOffset, uvFormat);
						}
						ub.Dispose();
					});
				}
				uint stride = (uint)mesh.GetVertexBufferStride(stream), indexStride = mesh.indexFormat == IndexFormat.UInt16 ? 2u : 4u;
				uint positionOffset = (uint)offset, positionFormat = format;
				geometryInFlight_ += 2;
				AsyncGPUReadback.Request(vb, (AsyncGPUReadbackRequest r) => {
					--geometryInFlight_;
					if (!r.hasError && generation == geometryGeneration_) {
						var data = r.GetData<byte>();
						if (Native.Aether_SceneMeshVertices(handle, Unity.Collections.LowLevel.Unsafe.NativeArrayUnsafeUtility.GetUnsafeReadOnlyPtr(data), (uint)data.Length, stride, positionOffset, positionFormat, uvMode, uvOffset, uvFormat) == 0) {
							++geometryFailed_;
						}
					} else if (r.hasError) {
						++geometryFailed_;
					}
					vb.Dispose();
				});
				AsyncGPUReadback.Request(ib, (AsyncGPUReadbackRequest r) => {
					--geometryInFlight_;
					if (!r.hasError && generation == geometryGeneration_) {
						var data = r.GetData<byte>();
						fixed (AeSubMesh* p = subs) {
							if (Native.Aether_SceneMeshIndices(handle, Unity.Collections.LowLevel.Unsafe.NativeArrayUnsafeUtility.GetUnsafeReadOnlyPtr(data), (uint)data.Length, indexStride, p, (uint)subs.Length) != 0) {
								++geometrySent_;
							} else {
								++geometryFailed_;
							}
						}
					} else if (r.hasError) {
						++geometryFailed_;
					}
					ib.Dispose();
				});
			}
		}

		// ---- materials seen through a renderer's property block, every frame. The game writes the player's changes
		// there (a paint colour while the slider moves, the brake lights): waiting for the next scan showed them up to
		// half a second late. Reading every property of every such material each frame would cost too much, so only the
		// values the game changes while it runs are compared; the full translation runs when one of them differs.
		private static readonly int[] quickVectors_ = { Shader.PropertyToID("_BaseColor"), Shader.PropertyToID("_SecondaryColor"), Shader.PropertyToID("_EmissionIntensity") };
		private static readonly int[] quickFloats_ = { Shader.PropertyToID("_Roughness"), Shader.PropertyToID("_Metalness"), Shader.PropertyToID("_Clearcoat") };
		private static readonly int quickTexture_ = Shader.PropertyToID("_BaseMap");
		private readonly Dictionary<uint, uint> quickSignatures_ = new Dictionary<uint, uint>();
		private readonly List<uint> quickChanged_ = new List<uint>();

		private void RefreshOwnedMaterials() {
			quickChanged_.Clear();
			foreach (var kv in materials_) {
				TrackedMaterial m = kv.Value;
				if (m.owner == null || m.material == null) {
					continue;
				}
				MaterialPropertyBlock block = BlockOf(m.owner, m.index);
				uint h = 2166136261u;
				if (block != null) {
					unchecked {
						foreach (int id in quickVectors_) {
							Vector4 v = block.GetVector(id);
							h = (h ^ (uint)v.GetHashCode()) * 16777619u;
						}
						foreach (int id in quickFloats_) {
							h = (h ^ (uint)block.GetFloat(id).GetHashCode()) * 16777619u;
						}
						Texture tex = block.GetTexture(quickTexture_);
						h = (h ^ (tex != null ? (uint)tex.GetInstanceID() : 0u)) * 16777619u;
					}
				}
				if (!quickSignatures_.TryGetValue(kv.Key, out uint known)) {
					quickSignatures_[kv.Key] = h; // first sight: what was sent is current
				} else if (known != h) {
					quickSignatures_[kv.Key] = h;
					quickChanged_.Add(kv.Key);
				}
			}
			foreach (uint handle in quickChanged_) {
				TrackedMaterial m = materials_[handle];
				RegisterMaterial(m.material, handle, m.owner, m.index);
			}
			if (quickSignatures_.Count > materials_.Count + 256) {
				quickSignatures_.Clear(); // handles that are gone; the next frame fills it again
			}
		}

		// ---- meshes the game writes while it runs
		private sealed class DynamicMesh {
			public Mesh mesh;
			public int vertexCount;
			public long indexCount;
		}

		private readonly Dictionary<uint, DynamicMesh> dynamicMeshes_ = new Dictionary<uint, DynamicMesh>();
		private readonly List<uint> deadDynamic_ = new List<uint>();
		public int DynamicMeshes => dynamicMeshes_.Count;

		private void UpdateDynamicMeshes() {
			if (dynamicMeshes_.Count == 0) {
				return;
			}
			deadDynamic_.Clear();
			foreach (var kv in dynamicMeshes_) {
				DynamicMesh d = kv.Value;
				if (d.mesh == null) {
					deadDynamic_.Add(kv.Key);
					continue;
				}
				int vertices = d.mesh.vertexCount;
				long indices = 0;
				for (int s = 0; s < d.mesh.subMeshCount; ++s) {
					indices += d.mesh.GetIndexCount(s);
				}
				if (vertices == d.vertexCount && indices == d.indexCount) {
					continue;
				}
				// grown or rewritten: Unity may have moved it to new buffers, and its index ranges are different
				if (vertices > 0 && indices > 0) {
					if (!RegisterMeshCore(d.mesh, kv.Key, IntPtr.Zero, 0)) {
						continue; // its buffers are not on the GPU yet: the next frame tries again
					}
				} else if (meshes_.Remove(kv.Key)) {
					Native.Aether_SceneDestroy(AeConst.KindMesh, kv.Key); // emptied again
				}
				d.vertexCount = vertices;
				d.indexCount = indices;
			}
			foreach (uint handle in deadDynamic_) {
				dynamicMeshes_.Remove(handle);
			}
		}

		/// <summary>
		/// Describes this frame for `camera` and hands it to the native side. Called from the render loop, after the
		/// game's cameras rendered. Returns false when nothing was submitted.
		/// </summary>
		public unsafe bool SubmitFrame(Camera camera) {
			long start = System.Diagnostics.Stopwatch.GetTimestamp();
			try {
				return SubmitFrameCore(camera);
			} finally {
				frameCost_.Add(System.Diagnostics.Stopwatch.GetTimestamp() - start);
			}
		}

		private unsafe bool SubmitFrameCore(Camera camera) {
			if (!active_) {
				return false;
			}
			AeFrame f = default;
			f.structSize = (uint)sizeof(AeFrame);
			f.contractVersion = AeConst.ContractVersion;
			f.sceneId = sceneId_;
			f.frameIndex = ++frameIndex_;
			f.deltaTime = Time.unscaledDeltaTime;
			f.time = Time.time;
			f.exposure = 1.0f;
			f.ambientIntensity = RenderSettings.ambientIntensity;
			if (camera != null) {
				f.flags = AeConst.FrameCameraValid;
				f.cameraId = unchecked((uint)camera.GetInstanceID());
				f.width = (uint)camera.pixelWidth;
				f.height = (uint)camera.pixelHeight;
				Matrix4x4 view = camera.worldToCameraMatrix;
				// D3D clip space, y up (false: not rendering into a Unity render texture), reversed Z; no TAA jitter
				Matrix4x4 proj = GL.GetGPUProjectionMatrix(camera.nonJitteredProjectionMatrix, false);
				*(Matrix4x4*)f.view = view;
				*(Matrix4x4*)f.proj = proj;
				Vector3 p = camera.transform.position, fwd = camera.transform.forward;
				f.cameraPosition[0] = p.x; f.cameraPosition[1] = p.y; f.cameraPosition[2] = p.z;
				f.cameraForward[0] = fwd.x; f.cameraForward[1] = fwd.y; f.cameraForward[2] = fwd.z;
				f.nearPlane = camera.nearClipPlane;
				f.farPlane = camera.farClipPlane;
				if (Time.unscaledTime >= nextVolume_) {
					nextVolume_ = Time.unscaledTime + 0.5f;
					ReadHostVolume(camera);
				}
			}
			f.tonemapMode = tonemapMode_;
			for (int i = 0; i < 6; ++i) {
				f.tonemapCurve[i] = tonemapCurve_[i];
			}
			f.postExposure = postExposure_;
			f.exposureMode = exposureMode_;
			f.exposureFixedEV = exposureFixed_;
			f.exposureMinEV = exposureMin_;
			f.exposureMaxEV = exposureMax_;
			f.exposureCompensation = exposureCompensation_;
			f.bloomIntensity = bloomIntensity_;
			f.bloomScatter = bloomScatter_;
			f.motionBlurIntensity = motionBlur_;
			f.antialiasing = antialiasing_;
			f.ssrEnabled = ssrEnabled_ ? 1u : 0u;
			f.ssrMinSmoothness = ssrMinSmoothness_;
			f.ssrSmoothnessFadeStart = ssrFadeStart_;
			f.ssrScreenFadeDistance = ssrScreenFade_;
			f.shadowDistance = shadowDistance_;
			f.shadowCascades = shadowCascades_;
			for (int i = 0; i < 3; ++i) {
				f.shadowSplits[i] = shadowSplits_[i];
			}
			f.aoIntensity = aoIntensity_;
			f.aoRadius = aoRadius_;
			f.aoDirectStrength = aoDirect_;
			for (int i = 0; i < 4; ++i) {
				f.smhShadows[i] = smh_[i];
				f.smhMidtones[i] = smh_[4 + i];
				f.smhHighlights[i] = smh_[8 + i];
				f.smhLimits[i] = smh_[12 + i];
			}
			if (fogOn_) {
				f.fogDensity = fogDensity_;
				f.fogBaseHeight = fogBase_;
				f.fogMaxHeight = fogMax_;
				f.fogMaxDistance = fogMaxDistance_;
				f.fogColor[0] = fogColor_.r; f.fogColor[1] = fogColor_.g; f.fogColor[2] = fogColor_.b;
				f.fogTint[0] = fogTint_.r; f.fogTint[1] = fogTint_.g; f.fogTint[2] = fogTint_.b;
				f.fogFlags = fogFromSky_ ? 1u : 0u;
			}
			// ambient light: Unity's probe as the seven vectors its shaders use (unity_SHAr .. unity_SHC)
			SphericalHarmonicsL2 sh = RenderSettings.ambientProbe;
			for (int c = 0; c < 3; ++c) {
				f.ambientSH[c * 4 + 0] = sh[c, 3];
				f.ambientSH[c * 4 + 1] = sh[c, 1];
				f.ambientSH[c * 4 + 2] = sh[c, 2];
				f.ambientSH[c * 4 + 3] = sh[c, 0] - sh[c, 6];
				f.ambientSH[(3 + c) * 4 + 0] = sh[c, 4];
				f.ambientSH[(3 + c) * 4 + 1] = sh[c, 5];
				f.ambientSH[(3 + c) * 4 + 2] = sh[c, 6] * 3.0f;
				f.ambientSH[(3 + c) * 4 + 3] = sh[c, 7];
				f.ambientSH[24 + c] = sh[c, 8];
			}
			f.ambientSH[27] = 1.0f;
			if (sky_ != null && textures_.ContainsKey(Handle(sky_))) {
				f.skyTexture = Handle(sky_);
				f.skyIntensity = skyIntensity_;
				f.skyRotation = skyRotation_;
			}

			// ---- particles (sent before the frame they belong to)
			particles_.Submit(camera, particleTexture_);

			// ---- one entry per registered renderer
			if (frameInstances_.Length < order_.Count) {
				frameInstances_ = new AeFrameInstance[Mathf.NextPowerOfTwo(order_.Count)];
			}
			int count = 0;
			int cullingMask = camera != null ? camera.cullingMask : ~0;
			int offscreen = 0;
			int sunMask = ~0; // the layers the sun lights (and so the layers whose renderers cast its shadows)
			foreach (Light l in lights_) {
				if (l != null && l.isActiveAndEnabled && l.type == LightType.Directional) {
					sunMask = l.cullingMask;
					break;
				}
			}
			if (camera != null) {
				UpdateLodLevels(camera);
			}
			RefreshSkinned();
			fixed (AeFrameInstance* instances = frameInstances_) {
				for (int i = 0; i < order_.Count; ++i) {
					Tracked t = order_[i];
					Renderer r = t.renderer;
					if (!t.registered || r == null) {
						continue; // a destroyed renderer is removed at the next scan
					}
					AeFrameInstance* fi = instances + count++;
					fi->instance = t.handle;
					// isVisible is Unity's own culling result for the frame just rendered (frustum, LOD group, occlusion;
					// also true for a renderer only a shadow needs): the LOD level HDRP picked is the one Aether draws
					// (a renderer that stands still: what the last look at it found, see Tracked; the others are asked every frame)
					bool enabled = !t.live ? t.enabled : r.enabled && !r.forceRenderingOff;
					bool inCamera = (t.layerBit & cullingMask) != 0;
					ShadowCastingMode casting = !t.live ? t.casting : r.shadowCastingMode;
					uint flags = 0;
					if (enabled && inCamera && r.isVisible) {
						flags = AeConst.FrameInstanceVisible | (casting == ShadowCastingMode.ShadowsOnly ? AeConst.FrameInstanceShadowOnly : 0u);
					} else if (enabled && ((casting != ShadowCastingMode.Off && (t.layerBit & sunMask) != 0) || (inCamera && casting != ShadowCastingMode.ShadowsOnly)) &&
						(t.lodGroup < 0 || (t.lodGroup < lodGroups_.Count && lodGroups_[t.lodGroup].current >= 0 && (t.lodMask & (1u << lodGroups_[t.lodGroup].current)) != 0))) {
						// Not seen by the camera (outside the view, hidden by Unity's occlusion data, or on a layer the camera
						// does not show), but the sun reaches its layer: it can cast a shadow into the view. Or it casts none
						// but is something the camera would show: it is part of the surroundings that reflections show (the
						// reflection cube is drawn all around, whatever the camera looks at; left out, things came and went in
						// the mirrored picture as the camera turned). The native side culls it against the view, every shadow
						// cascade and the cube's faces itself.
						flags = AeConst.FrameInstanceVisible | (inCamera && casting != ShadowCastingMode.ShadowsOnly ? 0u : AeConst.FrameInstanceShadowOnly);
						++offscreen;
					}
					fi->flags = flags;
					*(Matrix4x4*)fi->localToWorld = t.skinned != null ? SkinnedMatrix(t.skinned) : !t.live ? t.matrix : r.localToWorldMatrix;
				}

				OffscreenCasters = offscreen;
				// ---- lights
				if (frameLights_.Length < lights_.Count) {
					frameLights_ = new AeLight[Mathf.NextPowerOfTwo(lights_.Count)];
				}
				int lightCount = 0;
				fixed (AeLight* lights = frameLights_) {
					for (int i = 0; i < lights_.Count; ++i) {
						Light l = lights_[i];
						if (l == null || !l.isActiveAndEnabled || l.intensity <= 0) {
							continue;
						}
						uint type;
						switch (l.type) {
						case LightType.Directional: type = AeConst.LightDirectional; break;
						case LightType.Point: type = AeConst.LightPoint; break;
						case LightType.Spot: type = AeConst.LightSpot; break;
						default: continue; // area lights are not part of the contract yet
						}
						// The game's lights carry a second component with the settings of its own pipeline. It is the game's
						// switch as well: a light whose component is disabled is off (the garage's lamps by day), whatever the
						// Light itself says. Its dimmer and its fade with the distance from the camera apply too.
						float dimmer = 1;
						if (l.TryGetComponent(out UnityEngine.Rendering.HighDefinition.HDAdditionalLightData data)) {
							if (!data.isActiveAndEnabled) {
								continue;
							}
							dimmer = data.lightDimmer;
							if (type != AeConst.LightDirectional && camera != null && data.fadeDistance > 0) {
								float distance = Vector3.Distance(camera.transform.position, l.transform.position);
								dimmer *= Mathf.Clamp01((data.fadeDistance - distance) / (data.fadeDistance * 0.2f));
							}
							if (dimmer <= 0) {
								continue;
							}
						}
						AeLight* o = lights + lightCount++;
						*o = default;
						o->type = type;
						o->flags = l.shadows != LightShadows.None ? AeConst.LightCastsShadows : 0u;
						// HDRP light layers: the light's layers are the low byte of Light.renderingLayerMask
						o->layerMask = lightLayers_ ? ((uint)l.renderingLayerMask & 0xFFu) : 0xFFFFFFFFu;
						Transform tr = l.transform;
						Vector3 pos = tr.position, dir = tr.forward;
						o->position[0] = pos.x; o->position[1] = pos.y; o->position[2] = pos.z;
						o->direction[0] = dir.x; o->direction[1] = dir.y; o->direction[2] = dir.z;
						o->range = l.range;
						// linear colour times the intensity in HDRP's unit: lux for the sun, candela for point and spot lights
						Color c = l.color.linear;
						if (l.useColorTemperature) {
							c *= Mathf.CorrelatedColorTemperatureToRGB(l.colorTemperature);
						}
						float intensity = l.intensity * dimmer;
						o->color[0] = c.r * intensity; o->color[1] = c.g * intensity; o->color[2] = c.b * intensity;
						o->spotOuterCos = Mathf.Cos(l.spotAngle * 0.5f * Mathf.Deg2Rad);
						o->spotInnerCos = Mathf.Cos(Mathf.Min(l.innerSpotAngle, l.spotAngle) * 0.5f * Mathf.Deg2Rad);
					}
					return Native.Aether_SceneSubmitFrame(&f, instances, (uint)count, lights, (uint)lightCount) != 0;
				}
			}
		}

		private int SkinnedBuffers() {
			int n = 0;
			foreach (Tracked t in skinnedTracked_) {
				n += t.skinnedVariants != null ? t.skinnedVariants.Count : 0;
			}
			return n;
		}

		public string Status() {
			return order_.Count + " renderers, " + meshes_.Count + " meshes (" + pendingMeshes_.Count + " pending), " + materials_.Count + " materials, " + textures_.Count + " textures (" +
				pendingTextures_.Count + " pending), " + lights_.Count + " lights, " + lodGroups_.Count + " LOD groups, " + OffscreenCasters + " off-screen shadow casters, " + SkinnedBuffers() + " skinned buffers of " + skinnedTracked_.Count + " skinned renderers, " + dynamicMeshes_.Count + " dynamic meshes; ray tracing geometry: " + GeometryStatus + "; skipped: " + SkinnedSkipped + " skinned renderers, " + UnsupportedFormats + " unsupported meshes / textures; " + particles_.LastCount + " particles (" + particles_.LastSmoke + " tyre smoke, " + particles_.Systems + " particle systems); main thread ms: scan " + scanCost_ + ", tick " + tickCost_ + ", frame " + frameCost_ + ", pipeline " + AetherPipeline.Timing;
		}
	}
}
