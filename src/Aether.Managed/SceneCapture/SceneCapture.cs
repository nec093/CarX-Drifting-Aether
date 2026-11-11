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
			public int layerBit; // 1 << gameObject.layer, refreshed by the scan
		}

		private sealed class TrackedMaterial {
			public Material material;
			public int crc;
			public Color? colorOverride;
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
		private Texture sky_, exposure_;
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
			nextScan_ = 0;
			Native.Log(1, "Scene capture started");
		}

		/// <summary>Stops capturing and destroys every native handle (the scene path's next event applies it).</summary>
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
			renderers_.Clear();
			order_.Clear();
			meshes_.Clear();
			materials_.Clear();
			textures_.Clear();
			pendingMeshes_.Clear();
			pendingMeshSet_.Clear();
			pendingTextures_.Clear();
			pendingTextureSet_.Clear();
			lights_.Clear();
			sky_ = exposure_ = null;
		}

		// ------------------------------------------------------------------------------------------------ scanning

		private static uint Handle(UnityEngine.Object o) {
			return unchecked((uint)o.GetInstanceID());
		}

		// A material seen through a renderer's property block is its own native material: the handle mixes both ids.
		private static uint VariantHandle(Material m, Renderer r) {
			unchecked {
				uint h = 2166136261u;
				h = (h ^ (uint)m.GetInstanceID()) * 16777619u;
				h = (h ^ (uint)r.GetInstanceID()) * 16777619u;
				return h == 0 ? 1u : h;
			}
		}

		private void Scan() {
			// ---- renderers: new ones, changed ones, destroyed ones
			MeshRenderer[] found = UnityEngine.Object.FindObjectsByType<MeshRenderer>(FindObjectsInactive.Exclude, FindObjectsSortMode.None);
			var seen = new HashSet<int>();
			foreach (MeshRenderer r in found) {
				int id = r.GetInstanceID();
				seen.Add(id);
				if (!renderers_.TryGetValue(id, out Tracked t)) {
					t = new Tracked { renderer = r, filter = r.GetComponent<MeshFilter>(), handle = unchecked((uint)id), isStatic = r.gameObject.isStatic };
					renderers_[id] = t;
					order_.Add(t);
				}
				UpdateTracked(t);
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
				}
			}
			SkinnedSkipped = UnityEngine.Object.FindObjectsByType<SkinnedMeshRenderer>(FindObjectsInactive.Exclude, FindObjectsSortMode.None).Length;

			// ---- what nothing references any more
			var usedMeshes = new HashSet<uint>();
			var usedMaterials = new HashSet<uint>();
			foreach (Tracked t in order_) {
				if (t.mesh != null) {
					usedMeshes.Add(Handle(t.mesh));
				}
				if (t.materials != null) {
					foreach (uint m in t.materials) {
						usedMaterials.Add(m);
					}
				}
			}
			Sweep(meshes_, usedMeshes, AeConst.KindMesh, (mesh) => mesh == null);
			Sweep(materials_, usedMaterials, AeConst.KindMaterial, (m) => m.material == null);
			// textures: the ones the live materials name, plus the global ones
			var usedTextures = new HashSet<uint>();
			foreach (var kv in materials_) {
				if (kv.Value.material == null) {
					continue;
				}
				ids_.Clear();
				kv.Value.material.GetTexturePropertyNameIDs(ids_);
				foreach (int pid in ids_) {
					Texture tex = kv.Value.material.GetTexture(pid);
					if (tex != null) {
						usedTextures.Add(Handle(tex));
					}
				}
			}
			if (sky_ != null) usedTextures.Add(Handle(sky_));
			if (exposure_ != null) usedTextures.Add(Handle(exposure_));
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

			// ---- the environment HDRP publishes as global textures (while HDRP runs)
			TrackGlobal(ref sky_, "_SkyTexture");
			TrackGlobal(ref exposure_, "_ExposureTexture");
		}

		private readonly List<int> ids_ = new List<int>();

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
			}
		}

		private void TrackGlobal(ref Texture slot, string name) {
			Texture t = Shader.GetGlobalTexture(name);
			if (t == slot) {
				return;
			}
			slot = t;
			if (t != null) {
				QueueTexture(t);
			}
		}

		private unsafe void UpdateTracked(Tracked t) {
			Mesh mesh = t.filter != null ? t.filter.sharedMesh : null;
			t.layerBit = 1 << t.renderer.gameObject.layer;
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
			QueueMesh(mesh);
			int count = sharedMaterials_.Count;
			t.materialIds = new int[count];
			t.materials = new uint[count];
			Color? overrideColor = null;
			if (t.renderer.HasPropertyBlock()) {
				t.renderer.GetPropertyBlock(block_);
				if (block_.HasColor(BaseColorId)) {
					overrideColor = block_.GetColor(BaseColorId);
				}
			}
			for (int i = 0; i < count; ++i) {
				Material m = sharedMaterials_[i];
				t.materialIds[i] = m != null ? m.GetInstanceID() : 0;
				if (m == null || m.shader == null) {
					t.materials[i] = 0;
					continue;
				}
				uint handle = overrideColor.HasValue ? VariantHandle(m, t.renderer) : Handle(m);
				t.materials[i] = handle;
				if (!materials_.ContainsKey(handle)) {
					RegisterMaterial(m, handle, overrideColor);
				}
			}
			AeInstanceDesc d = default;
			d.structSize = (uint)sizeof(AeInstanceDesc);
			d.instance = t.handle;
			d.mesh = Handle(mesh);
			d.flags = (t.renderer.shadowCastingMode != ShadowCastingMode.Off ? AeConst.InstanceCastShadows : 0) | (t.renderer.receiveShadows ? AeConst.InstanceReceiveShadows : 0) | (t.isStatic ? AeConst.InstanceStatic : 0);
			d.layer = (uint)t.renderer.gameObject.layer;
			d.materialCount = (uint)count;
			fixed (uint* p = t.materials) {
				t.registered = Native.Aether_SceneUpsertInstance(&d, p) != 0;
			}
		}

		private unsafe void RegisterMaterial(Material m, uint handle, Color? overrideColor) {
			MaterialTranslator.Result result = MaterialTranslator.Translate(m, handle, overrideColor);
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
			Native.Aether_SceneUpsertMaterial(&d);
			materials_[handle] = new TrackedMaterial { material = m, crc = m.ComputeCRC(), colorOverride = overrideColor };
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
				RegisterMaterial(kv.Value.material, kv.Key, kv.Value.colorOverride);
			}
		}

		private void QueueMesh(Mesh mesh) {
			uint h = Handle(mesh);
			if (!meshes_.ContainsKey(h) && pendingMeshSet_.Add(h)) {
				pendingMeshes_.Enqueue(mesh);
			}
		}

		private int devFlags_;

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

		/// <summary>True when the mesh is registered or can never be; false to try again later (not on the GPU yet).</summary>
		private unsafe bool RegisterMesh(Mesh mesh) {
			int streams = mesh.vertexBufferCount;
			if (streams < 1 || streams > 4 || mesh.subMeshCount < 1) {
				++UnsupportedFormats;
				return true;
			}
			AeMeshDesc d = default;
			d.structSize = (uint)sizeof(AeMeshDesc);
			d.mesh = Handle(mesh);
			d.vertexCount = (uint)mesh.vertexCount;
			d.indexStride = mesh.indexFormat == IndexFormat.UInt16 ? 2u : 4u;
			d.streamCount = (uint)streams;
			IntPtr* buffers = &d.vertexBuffer0;
			for (int s = 0; s < streams; ++s) {
				buffers[s] = mesh.GetNativeVertexBufferPtr(s);
				d.streamStride[s] = (uint)mesh.GetVertexBufferStride(s);
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
				} else {
					++UnsupportedFormats;
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
				t = new TrackedTexture { texture = tex, global = tex == sky_ || tex == exposure_ };
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

		private void ProcessPending() {
			int budget = PointerQueriesPerFrame;
			int retries = 0;
			while (budget > 0 && pendingMeshes_.Count > retries) {
				Mesh mesh = pendingMeshes_.Dequeue();
				if (mesh == null) {
					continue;
				}
				budget -= mesh.vertexBufferCount + 1;
				if (RegisterMesh(mesh)) {
					pendingMeshSet_.Remove(Handle(mesh));
				} else {
					pendingMeshes_.Enqueue(mesh); // not uploaded by Unity yet
					++retries;
				}
			}
			retries = 0;
			while (budget > 0 && pendingTextures_.Count > retries) {
				Texture tex = pendingTextures_.Dequeue();
				if (tex == null) {
					continue;
				}
				--budget;
				if (RegisterTexture(tex)) {
					pendingTextureSet_.Remove(Handle(tex));
				} else {
					pendingTextures_.Enqueue(tex);
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
		public void Tick() {
			if (!active_) {
				return;
			}
			float now = Time.unscaledTime;
			if (now >= nextScan_) {
				nextScan_ = now + ScanInterval;
				Scan();
			}
			if (now >= nextMaterialCheck_) {
				nextMaterialCheck_ = now + MaterialCheckInterval;
				CheckMaterials();
			}
			ProcessPending();
		}

		/// <summary>
		/// Describes this frame for `camera` and hands it to the native side. Called from the render loop, after the
		/// game's cameras rendered. Returns false when nothing was submitted.
		/// </summary>
		public unsafe bool SubmitFrame(Camera camera) {
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
			}
			if (exposure_ != null && textures_.ContainsKey(Handle(exposure_))) {
				f.exposureTexture = Handle(exposure_);
			}

			// ---- one entry per registered renderer
			if (frameInstances_.Length < order_.Count) {
				frameInstances_ = new AeFrameInstance[Mathf.NextPowerOfTwo(order_.Count)];
			}
			int count = 0;
			int cullingMask = camera != null ? camera.cullingMask : ~0;
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
					fi->flags = r.enabled && !r.forceRenderingOff && (t.layerBit & cullingMask) != 0 && r.isVisible ? AeConst.FrameInstanceVisible : 0u;
					*(Matrix4x4*)fi->localToWorld = r.localToWorldMatrix;
				}

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
						AeLight* o = lights + lightCount++;
						*o = default;
						o->type = type;
						o->flags = l.shadows != LightShadows.None ? AeConst.LightCastsShadows : 0u;
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
						float intensity = l.intensity;
						o->color[0] = c.r * intensity; o->color[1] = c.g * intensity; o->color[2] = c.b * intensity;
						o->spotOuterCos = Mathf.Cos(l.spotAngle * 0.5f * Mathf.Deg2Rad);
						o->spotInnerCos = Mathf.Cos(Mathf.Min(l.innerSpotAngle, l.spotAngle) * 0.5f * Mathf.Deg2Rad);
					}
					return Native.Aether_SceneSubmitFrame(&f, instances, (uint)count, lights, (uint)lightCount) != 0;
				}
			}
		}

		public string Status() {
			return order_.Count + " renderers, " + meshes_.Count + " meshes (" + pendingMeshes_.Count + " pending), " + materials_.Count + " materials, " + textures_.Count + " textures (" +
				pendingTextures_.Count + " pending), " + lights_.Count + " lights; skipped: " + SkinnedSkipped + " skinned renderers, " + UnsupportedFormats + " unsupported meshes / textures";
		}
	}
}
