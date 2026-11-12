using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using UnityEngine;

namespace Aether {
	/// <summary>
	/// The frame's particles for the native particle pass: camera facing quads in batches (Aether_SceneSubmitParticles).
	///
	/// Two sources:
	/// - the game's Unity particle systems (exhaust flames, sparks, burnout, wheel dirt, map effects): their live
	///   particles are read on the CPU (ParticleSystem.GetParticles) and turned into quads here. Billboards and
	///   stretched billboards are covered; mesh particles and trails are not (drawn as billboards / not at all).
	/// - the tyre smoke (SmokeSimulation): the game simulates and draws it on the GPU with compute shaders that have no
	///   Direct3D 12 kernels, so Aether simulates it itself from the game's own emission data.
	/// </summary>
	internal sealed class ParticleCapture {
		private const int MaxParticles = 24000;

		private AeParticle[] particles_ = new AeParticle[4096];
		private int count_;
		private readonly List<AeParticleBatch> batches_ = new List<AeParticleBatch>();
		private AeParticleBatch[] batchArray_ = new AeParticleBatch[32];
		private int batchFirst_;
		private uint batchTexture_, batchFlags_;

		private struct Source {
			public ParticleSystemRenderer renderer;
			public ParticleSystem system;
		}

		private readonly List<Source> sources_ = new List<Source>();
		private ParticleSystem.Particle[] scratch_ = new ParticleSystem.Particle[1024];
		private float nextFind_;
		private bool smokeFailed_;
		private static readonly int MainTexId = Shader.PropertyToID("_MainTex"), BaseColorMapId = Shader.PropertyToID("_BaseColorMap"), BaseMapId = Shader.PropertyToID("_BaseMap");
		private static readonly int TintColorId = Shader.PropertyToID("_TintColor"), BaseColorId = Shader.PropertyToID("_BaseColor"), ColorId = Shader.PropertyToID("_Color");

		public int Systems => sources_.Count;
		public int LastCount { get; private set; }
		public int LastSmoke { get; private set; }

		public void Reset() {
			sources_.Clear();
			nextFind_ = 0;
		}

		// ---- what a source writes into

		public void BeginBatch(uint texture, uint flags) {
			batchFirst_ = count_;
			batchTexture_ = texture;
			batchFlags_ = flags;
		}

		public bool Add(Vector3 position, float size, Color color, float rotation, Vector3 stretch, Vector4 uv) {
			if (count_ >= MaxParticles) {
				return false;
			}
			if (count_ == particles_.Length) {
				Array.Resize(ref particles_, particles_.Length * 2);
			}
			particles_[count_++] = new AeParticle { position = position, size = size, color = color, rotation = rotation, stretch = stretch, uv = uv };
			return true;
		}

		public void EndBatch() {
			if (count_ > batchFirst_) {
				batches_.Add(new AeParticleBatch { texture = batchTexture_, flags = batchFlags_, first = (uint)batchFirst_, count = (uint)(count_ - batchFirst_) });
			}
		}

		// ---- the frame

		/// <summary>Collects this frame's particles and hands them to the native side. `texture`: the native handle of a Unity texture (0 while it is not registered yet). Main thread, once per frame.</summary>
		public unsafe void Submit(Camera camera, Func<Texture, uint> texture) {
			count_ = 0;
			batches_.Clear();
			if (camera != null) {
				try {
					// the smoke first: flames and sparks are drawn over it
					if (!smokeFailed_) {
						try {
							LastSmoke = StepSmoke();
						} catch (Exception e) {
							smokeFailed_ = true; // (the game's smoke types are not there, or not as expected: no tyre smoke)
							Native.Log(2, "Tyre smoke: off (" + e.GetType().Name + ": " + e.Message + ")");
						}
					}
					CollectSystems(camera, texture);
				} catch (Exception e) {
					Native.Log(2, "Particles: " + e.GetType().Name + ": " + e.Message);
				}
			}
			LastCount = count_;
			if (batchArray_.Length < batches_.Count) {
				batchArray_ = new AeParticleBatch[Mathf.NextPowerOfTwo(batches_.Count)];
			}
			batches_.CopyTo(batchArray_);
			fixed (AeParticle* p = particles_) {
				fixed (AeParticleBatch* b = batchArray_) {
					Native.Aether_SceneSubmitParticles(b, (uint)batches_.Count, p, (uint)count_, (uint)sizeof(AeParticle));
				}
			}
		}

		// (its own method: the game's smoke types are only looked up when this is first called, and a failure stays in there)
		[MethodImpl(MethodImplOptions.NoInlining)]
		private int StepSmoke() {
			return SmokeSimulation.Instance.Step(this);
		}

		private void CollectSystems(Camera camera, Func<Texture, uint> texture) {
			float now = Time.unscaledTime;
			if (now >= nextFind_) {
				nextFind_ = now + 1.0f;
				sources_.Clear();
				foreach (ParticleSystemRenderer r in UnityEngine.Object.FindObjectsByType<ParticleSystemRenderer>(FindObjectsInactive.Exclude, FindObjectsSortMode.None)) {
					ParticleSystem system = r.GetComponent<ParticleSystem>();
					if (system != null) {
						sources_.Add(new Source { renderer = r, system = system });
					}
				}
			}
			int cullingMask = camera.cullingMask;
			foreach (Source s in sources_) {
				ParticleSystemRenderer r = s.renderer;
				ParticleSystem system = s.system;
				if (r == null || system == null || !r.enabled || r.renderMode == ParticleSystemRenderMode.None || (cullingMask & (1 << r.gameObject.layer)) == 0) {
					continue;
				}
				int n = system.particleCount;
				Material m = r.sharedMaterial;
				if (n == 0 || m == null) {
					continue;
				}
				if (scratch_.Length < n) {
					scratch_ = new ParticleSystem.Particle[Mathf.NextPowerOfTwo(n)];
				}
				n = system.GetParticles(scratch_, n);

				// ---- the material: its texture, its tint, how it blends
				Texture tex = m.HasProperty(MainTexId) ? m.GetTexture(MainTexId) : m.HasProperty(BaseColorMapId) ? m.GetTexture(BaseColorMapId) : m.HasProperty(BaseMapId) ? m.GetTexture(BaseMapId) : null;
				Color tint = m.HasProperty(TintColorId) ? m.GetColor(TintColorId).linear * 2.0f : m.HasProperty(BaseColorId) ? m.GetColor(BaseColorId).linear : m.HasProperty(ColorId) ? m.GetColor(ColorId).linear : Color.white;
				tint.a = Mathf.Clamp01(tint.a);
				bool additive = m.shader != null && m.shader.name.IndexOf("Additive", StringComparison.OrdinalIgnoreCase) >= 0;
				uint flags = additive ? AeConst.ParticleAdditive : AeConst.ParticleLit;

				// ---- the system: where its particles live, how they are shaped
				ParticleSystem.MainModule main = system.main;
				bool local = main.simulationSpace == ParticleSystemSimulationSpace.Local;
				Transform space = main.simulationSpace == ParticleSystemSimulationSpace.Custom ? main.customSimulationSpace : system.transform;
				bool transform = (local || main.simulationSpace == ParticleSystemSimulationSpace.Custom) && space != null;
				Matrix4x4 toWorld = transform ? space.localToWorldMatrix : Matrix4x4.identity;
				float scale = transform ? Mathf.Abs(space.lossyScale.x) : 1.0f;
				bool stretched = r.renderMode == ParticleSystemRenderMode.Stretch;
				float lengthScale = r.lengthScale, velocityScale = r.velocityScale;
				ParticleSystem.TextureSheetAnimationModule sheet = system.textureSheetAnimation;
				int tilesX = sheet.enabled ? Mathf.Max(sheet.numTilesX, 1) : 1, tilesY = sheet.enabled ? Mathf.Max(sheet.numTilesY, 1) : 1;
				int tiles = tilesX * tilesY;
				int cycles = sheet.enabled ? Mathf.Max(sheet.cycleCount, 1) : 1;

				BeginBatch(texture(tex), flags);
				for (int i = 0; i < n; ++i) {
					ref ParticleSystem.Particle p = ref scratch_[i];
					Vector3 position = transform ? toWorld.MultiplyPoint3x4(p.position) : p.position;
					float size = p.GetCurrentSize(system) * scale;
					Color color = ((Color)p.GetCurrentColor(system)).linear * tint;
					Vector3 stretch = Vector3.zero;
					if (stretched) {
						Vector3 velocity = transform ? toWorld.MultiplyVector(p.velocity) : p.velocity;
						float speed = velocity.magnitude;
						if (speed > 1e-4f) {
							// (Unity: the quad is its size times the length scale long, plus the speed times the velocity scale)
							stretch = velocity * ((size * lengthScale + speed * velocityScale) * 0.5f / speed);
						}
					}
					Vector4 uv = new Vector4(0, 0, 1, 1);
					if (tiles > 1) {
						// (the sheet played over the particle's life, row by row from the top: the common setup)
						float life = 1.0f - p.remainingLifetime / Mathf.Max(p.startLifetime, 1e-4f);
						int frame = Mathf.Min((int)(Mathf.Repeat(life * cycles, 1.0f) * tiles), tiles - 1);
						uv = new Vector4((frame % tilesX) / (float)tilesX, (frame / tilesX) / (float)tilesY, 1.0f / tilesX, 1.0f / tilesY);
					}
					if (!Add(position, size, color, -p.rotation * Mathf.Deg2Rad, stretch, uv)) {
						break;
					}
				}
				EndBatch();
			}
		}
	}
}
