using System;
using System.Reflection;
using Unity.Collections;
using UnityEngine;

namespace Aether {
	/// <summary>
	/// Tyre smoke without the game's renderer.
	///
	/// How the game does it (GpuExtensions.dll, read from the decompiled code): every car has a TireSmokeGpuEmitter that
	/// only works when WheelSmokeGpuSimulation raises its static event OnGpuEmissionStep; the emitter then writes, per
	/// wheel, where the tyre is and how it moves (SmokeWheel) and one EmittingSpot for every particle to spawn this frame
	/// (from tyre temperature, slip and the surface). The simulation and the drawing are compute shaders in an HDRP
	/// custom pass (ParticleRenderPass). Those shaders have no Direct3D 12 kernels and the pass is not run by Aether's
	/// pipeline, so the simulation object never exists and nobody raises the event.
	///
	/// What Aether does: raises that event itself once per frame (through the event's backing field), with a state
	/// object of its own, and so gets the game's own emission: the same spawn points, counts and colours (the player's
	/// smoke colour, the surface's dust colour, the quality setting's opacity). The motion of the particles is Aether's
	/// own model, not the game's: how the game's compute shaders move, grow and fade the smoke is not visible in its C#
	/// code. The numbers below are chosen by eye.
	/// </summary>
	internal sealed class SmokeSimulation {
		public static readonly SmokeSimulation Instance = new SmokeSimulation();

		private struct Puff {
			public Vector3 position, velocity;
			public float age, life, size, growth, turn, spin;
			public Color color;
		}

		private const int MaxPuffs = 6000;
		private const int SpawnCapacity = 65536; // (the emitters write spawn entries without a bound check: far more room than they use)
		private const int MaxCars = 64;

		private readonly Puff[] puffs_ = new Puff[MaxPuffs];
		private int count_;
		private WheelGpuEmissionState state_;
		private NativeArray<EmittingSpot> spawn_;
		private NativeArray<SmokeCar> cars_;
		private NativeArray<SmokeWheel> wheels_;
		private TireEmissionConfig config_;
		private float lifetime_ = 3.0f;
		private float nextLookup_;
		private FieldInfo stepField_, clearField_;
		private bool announced_;
		private float opacity_ = 1.0f, nextOpacityRead_; // Aether's own option (overlay)

		/// <summary>One frame: the game's emission, the motion, the quads. Returns the number of puffs alive. Main thread, once per frame.</summary>
		public int Step(ParticleCapture sink) {
			float dt = Mathf.Min(Time.deltaTime, 0.05f);
			EnsureConfig();
			if (config_ != null && dt > 0) {
				Emit();
			}
			Simulate(dt);
			if (Time.unscaledTime >= nextOpacityRead_) {
				nextOpacityRead_ = Time.unscaledTime + 0.5f;
				opacity_ = Mathf.Clamp((float)Native.Aether_SettingNumber("smokeOpacity", 1.0), 0.0f, 2.0f);
			}
			if (count_ > 0 && opacity_ > 0.0f) {
				sink.BeginBatch(0, AeConst.ParticleLit | AeConst.ParticleDisc);
				for (int i = 0; i < count_; ++i) {
					ref Puff p = ref puffs_[i];
					float t = p.age / p.life;
					Color c = p.color;
					// in quickly, out slowly; thinner as it spreads
					c.a = Mathf.Clamp01(c.a * Mathf.Clamp01(p.age / 0.12f) * Mathf.Pow(1.0f - t, 1.6f) * 0.55f * opacity_);
					float size = p.size + p.growth * (1.0f - Mathf.Exp(-p.age * 1.1f));
					sink.Add(p.position, size, c, p.turn, Vector3.zero, new Vector4(0, 0, 1, 1));
				}
				sink.EndBatch();
			}
			return count_;
		}

		private void EnsureConfig() {
			if (config_ != null || Time.unscaledTime < nextLookup_) {
				return;
			}
			nextLookup_ = Time.unscaledTime + 2.0f;
			// the game's settings objects are loaded with the scene that has the smoke pass
			TireEmissionConfig[] found = Resources.FindObjectsOfTypeAll<TireEmissionConfig>();
			if (found.Length == 0) {
				return;
			}
			config_ = found[0];
			TireSmokeVfxConfig[] vfx = Resources.FindObjectsOfTypeAll<TireSmokeVfxConfig>();
			if (vfx.Length > 0 && vfx[0].SpawnParams.Lifetime > 0.2f && vfx[0].SpawnParams.Lifetime < 30.0f) {
				lifetime_ = vfx[0].SpawnParams.Lifetime;
			}
			if (!spawn_.IsCreated) {
				spawn_ = new NativeArray<EmittingSpot>(SpawnCapacity, Allocator.Persistent);
				cars_ = new NativeArray<SmokeCar>(MaxCars, Allocator.Persistent);
				wheels_ = new NativeArray<SmokeWheel>(MaxCars * 4, Allocator.Persistent);
			}
			state_ = new WheelGpuEmissionState();
			state_.Fill(config_);
			state_.SpawnData = spawn_;
			state_.CarsData = cars_;
			state_.WheelsData = wheels_;
			const BindingFlags flags = BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Static;
			stepField_ = typeof(WheelSmokeGpuSimulation).GetField("OnGpuEmissionStep", flags);
			clearField_ = typeof(WheelSmokeGpuSimulation).GetField("CollectGpuClearRequests", flags);
			if (!announced_) {
				announced_ = true;
				Native.Log(1, "Tyre smoke: the game's emitters are driven by Aether (settings " + config_.name + ", particle lifetime " + lifetime_.ToString("F1") + " s" + (stepField_ == null ? "; the emission event was not found: no smoke" : "") + ")");
			}
		}

		private void Emit() {
			if (stepField_ == null) {
				return;
			}
			// a car that asked for its smoke to go (a reset, a teleport): all of it goes, the simple way
			if (clearField_ != null && clearField_.GetValue(null) is Action<CarSmokeClearState> clear) {
				var requests = new NativeParallelHashSet<uint>(MaxCars, Allocator.Temp);
				clear(new CarSmokeClearState(requests));
				if (!requests.IsEmpty) {
					count_ = 0;
				}
				requests.Dispose();
			}
			if (!(stepField_.GetValue(null) is Action<WheelGpuEmissionState> step)) {
				return; // no car with a smoke emitter
			}
			state_.ActiveId = 0;
			state_.CarId = 0;
			step(state_);
			int spawned = Mathf.Min(state_.ActiveId, spawn_.Length);
			for (int i = 0; i < spawned; ++i) {
				EmittingSpot spot = spawn_[i];
				SmokeWheel wheel = spot.wheelID < (uint)wheels_.Length ? wheels_[(int)spot.wheelID] : default;
				Spawn(spot, wheel);
			}
		}

		private void Spawn(EmittingSpot spot, SmokeWheel wheel) {
			if (count_ == MaxPuffs) {
				// full: the oldest goes (the array is in spawn order apart from the removals)
				int oldest = 0;
				for (int i = 1; i < count_; ++i) {
					if (puffs_[i].age / puffs_[i].life > puffs_[oldest].age / puffs_[oldest].life) {
						oldest = i;
					}
				}
				puffs_[oldest] = puffs_[--count_];
			}
			// the packed colour: r in the low byte, sRGB; a is the opacity the game worked out (temperature, quality)
			uint packed = wheel.smokeColor;
			Color color = new Color((packed & 255) / 255.0f, ((packed >> 8) & 255) / 255.0f, ((packed >> 16) & 255) / 255.0f, 1.0f).linear;
			color.a = ((packed >> 24) & 255) / 255.0f;
			if (color.a <= 0.0f) {
				return;
			}
			Vector3 up = (Vector3)wheel.up;
			if (up.sqrMagnitude < 0.5f) {
				up = Vector3.up;
			}
			// carried along with the car for a moment, thrown back by the spinning tyre, rising, and a little of everything
			float spin = Mathf.Clamp(Mathf.Abs(wheel.angularSpeed) * wheel.radius * 0.1f, 0.0f, 5.0f);
			Vector3 velocity = (Vector3)wheel.velocity * 0.3f - (Vector3)wheel.dir * spin + up * UnityEngine.Random.Range(0.3f, 1.1f) + UnityEngine.Random.insideUnitSphere * 0.7f;
			float a = wheel.emitSizeA, b = wheel.emitSizeB;
			float size = a > 0.02f && b > 0.02f && a < 4.0f && b < 4.0f ? UnityEngine.Random.Range(Mathf.Min(a, b), Mathf.Max(a, b)) : UnityEngine.Random.Range(0.3f, 0.55f);
			puffs_[count_++] = new Puff {
				position = (Vector3)spot.contactPoint + up * (size * 0.3f) + UnityEngine.Random.insideUnitSphere * 0.08f,
				velocity = velocity,
				age = 0,
				life = lifetime_ * UnityEngine.Random.Range(0.75f, 1.25f),
				size = size,
				growth = size * 4.0f + UnityEngine.Random.Range(0.8f, 1.6f),
				turn = UnityEngine.Random.Range(0.0f, 6.283f),
				spin = UnityEngine.Random.Range(-0.5f, 0.5f),
				color = color,
			};
		}

		private void Simulate(float dt) {
			float drag = Mathf.Exp(-1.4f * dt);
			for (int i = 0; i < count_;) {
				ref Puff p = ref puffs_[i];
				p.age += dt;
				if (p.age >= p.life) {
					puffs_[i] = puffs_[--count_];
					continue;
				}
				p.velocity *= drag;
				p.velocity.y += 0.3f * dt; // warm smoke rises
				p.position += p.velocity * dt;
				p.turn += p.spin * dt;
				++i;
			}
		}

		public void Clear() {
			count_ = 0;
			config_ = null; // (looked up again: another scene has its own settings objects)
			nextLookup_ = 0;
		}
	}
}
