using System;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using Unity.Collections;
using Unity.Mathematics;
using UnityEngine;

namespace Aether {
	/// <summary>
	/// Tyre smoke without the game's renderer.
	///
	/// How the game does it (GpuExtensions.dll, read from the decompiled code): every car has a TireSmokeGpuEmitter that
	/// only works when WheelSmokeGpuSimulation raises its static event OnGpuEmissionStep; the emitter then writes, per
	/// wheel, where the tyre is and how it moves (SmokeWheel), per car its velocity and three spheres along its body
	/// (SmokeCar: front axle, middle, rear axle), and one EmittingSpot for every particle to spawn this frame (from the
	/// tyre's temperature, slip and the surface). The simulation and the drawing are compute shaders in an HDRP custom
	/// pass. Those shaders have no Direct3D 12 kernels and the pass is not run by Aether's pipeline, so the simulation
	/// object never exists and nobody raises the event.
	///
	/// What Aether does: raises that event itself once per frame (through the event's backing field), with a state
	/// object of its own, and so gets the game's own emission: the same spawn points, counts and colours (the player's
	/// smoke colour, the surface's dust colour, the quality setting's opacity). What the smoke does after that is
	/// Aether's own model:
	///
	/// 1. Round the tyre. Part of the smoke stays in the layer of air the tread drags along: it is carried from the
	///    contact patch up the back of the tyre and over its top, and leaves it somewhere on the way, thrown off along
	///    the tread's direction there.
	/// 2. The car's wake. Smoke moves with the air, and the air round a moving car is not at rest: under and behind
	///    the body it is dragged along with the car, drawn in towards the middle of the wake, lifted, and rolled up by
	///    the two vortices that trail from a car's rear corners. The body is the game's three spheres; "behind" is
	///    where the car comes from, not where its tail points (a drifting car's wake is at its side). So the smoke of
	///    both rear tyres gathers under the tail and leaves it as one plume with the wake's shape.
	/// 3. Heat. Every tyre has a temperature of Aether's own, from how fast its tread slides over the road. A tyre that
	///    has only just begun to slide gives thin, bluish wisps that are gone in a second or two; a hot one gives dense
	///    white smoke that rises and hangs for many seconds, with wisps mixed in.
	/// 4. Going. Nothing fades by the clock: a puff spreads (turbulent mixing, faster at first and in a wake), and gets
	///    thinner as it spreads; the wind (stronger away from the ground, in gusts) carries and shears it. It is gone
	///    when it is too thin to see.
	///
	/// The numbers are chosen by eye. A development aid: the file dev-smoke.txt in the config folder ("x y z radius
	/// speed slipDegrees") drives an invisible car round a circle there, smoking, without the game's emitters.
	/// </summary>
	internal sealed class SmokeSimulation {
		public static readonly SmokeSimulation Instance = new SmokeSimulation();

		// (Puff, WheelAir and Body are read by the native side too: Smoke.cpp has the same layouts)
		[StructLayout(LayoutKind.Sequential)]
		private struct Puff {
			public Vector3 position, velocity;
			public float age, life, size, spread, turn, spin;
			public float heat, density, ground, seed;
			public Color color; // a: opacity at birth
			public int wheel;   // >= 0: carried round this tyre (index in wheels_); -1: free
			public float angle, releaseAngle, side, lift;
		}

		private const int MaxPuffs = 12000;
		private const int SpawnCapacity = 65536; // (the emitters write spawn entries without a bound check: far more room than they use)
		private const int MaxCars = 64;

		private readonly Puff[] puffs_ = new Puff[MaxPuffs];
		private int count_;
		private WheelGpuEmissionState state_;
		private NativeArray<EmittingSpot> spawn_;
		private NativeArray<SmokeCar> cars_;
		private NativeArray<SmokeWheel> wheels_;
		private readonly float[] heat_ = new float[MaxCars * 4];
		// where each wheel's smoke came from the frame before, and this frame (the game gives one place per wheel and
		// frame for all of that frame's puffs)
		private readonly Vector3[] lastContact_ = new Vector3[MaxCars * 4], contact_ = new Vector3[MaxCars * 4];
		private readonly int[] lastContactFrame_ = new int[MaxCars * 4], contactFrame_ = new int[MaxCars * 4];
		private int frame_;
		private int carCount_;
		// this frame's moving cars as the air model needs them (read once: 12000 puffs ask)
		[StructLayout(LayoutKind.Sequential)]
		private struct Body {
			public Vector3 velocity;
			public float speed;
			public Vector3 heading;
			public float r0;
			public Vector3 lateral;
			public float r1;
			public Vector3 c0;
			public float r2;
			public Vector3 c1;
			public float pad0;
			public Vector3 c2;
			public float pad1;
			// the body as a box (floor to roof, bumper to bumper): smoke does not get into it
			public Vector3 boxCentre;
			public float hx;
			public Vector3 forward;
			public float hy;
			public Vector3 up;
			public float hz;
			public Vector3 right;
			public float hasBox;
		}
		[StructLayout(LayoutKind.Sequential)]
		private struct WheelAir {
			public Vector3 pos;
			public float radius; // 0: not there this frame
			public Vector3 dir;
			public float angularSpeed;
			public Vector3 up;
			public float rolling;
			public Vector3 outside;
			public float pad0;
			public Vector3 carVelocity;
			public float pad1;
		}
		private readonly WheelAir[] wheelAir_ = new WheelAir[MaxCars * 4];
		private readonly Body[] bodies_ = new Body[MaxCars];
		private int bodyCount_;
		private Vector3 wind_;
		private TireEmissionConfig config_;
		private float nextLookup_;
		private FieldInfo stepField_, clearField_;
		private bool announced_;
		private float opacity_ = 1.0f, nextOpacityRead_; // Aether's own option (overlay)
		private float time_;
		// what the game emits, for the log (every few seconds while there is smoke)
		private float lastGameAlpha_, nextEmissionLog_;
		private int spawnedSinceLog_;
		// the development emitter
		private string devFile_;
		private float nextDevRead_, devAngle_, devRate_;
		private bool dev_;
		private readonly float[] devValues_ = new float[6];

		/// <summary>One frame: the game's emission, the motion, the quads. Returns the number of puffs alive. Main thread, once per frame.</summary>
		public int Step(ParticleCapture sink) {
			float dt = Mathf.Min(Time.deltaTime, 0.05f);
			time_ += dt;
			EnsureArrays();
			EnsureConfig();
			// (which wheels the emitters write this frame: the ones with a radius afterwards)
			for (int i = 0; i < carCount_ * 4; ++i) {
				SmokeWheel w = wheels_[i];
				w.radius = 0;
				wheels_[i] = w;
			}
			carCount_ = 0;
			int spawned = 0;
			if (config_ != null && dt > 0) {
				spawned = Emit();
			}
			ReadDev();
			if (dev_ && dt > 0) {
				DevCar(dt);
			}
			Heat(dt);
			++frame_;
			for (int i = 0; i < spawned; ++i) {
				EmittingSpot spot = spawn_[i];
				if (spot.wheelID < (uint)wheels_.Length) {
					SpawnAlong((Vector3)spot.contactPoint, (int)spot.wheelID, dt);
				}
			}
			if (dev_ && dt > 0) {
				DevSpawn(dt);
			}
			for (int i = 0; i < contact_.Length; ++i) {
				if (contactFrame_[i] == frame_) {
					lastContact_[i] = contact_[i];
					lastContactFrame_[i] = frame_;
				}
			}
			if (Time.unscaledTime >= nextOpacityRead_) {
				nextOpacityRead_ = Time.unscaledTime + 0.5f;
				opacity_ = Mathf.Clamp((float)Native.Aether_SettingNumber("smokeOpacity", 1.0), 0.0f, 2.0f);
			}
			long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
			Bodies();
			// the motion and the quads: on the native side (Smoke.cpp)
			if (count_ > 0) {
				sink.BeginBatch(0, AeConst.ParticleLit | AeConst.ParticleDisc);
				AeParticle[] quads = opacity_ > 0.0f ? sink.Reserve(count_, out int first) : null;
				unsafe {
					Vector3 wind = wind_;
					fixed (Puff* puffs = puffs_)
					fixed (WheelAir* wheels = wheelAir_)
					fixed (Body* bodies = bodies_)
					fixed (AeParticle* q = quads) {
						count_ = Native.Aether_SmokeStep(puffs, count_, wheels, carCount_ * 4, bodies, bodyCount_, dt, time_, &wind, opacity_, quads != null ? q + sink.Count : null);
					}
				}
				if (quads != null) {
					sink.Commit(count_);
				}
				sink.EndBatch();
			}
			if (spawnedSinceLog_ > 0 && Time.unscaledTime >= nextEmissionLog_) {
				// (the game's own numbers, to tune against: nobody can read them out of its settings objects from outside)
				float hottest = 0, tread = 0, sizeA = 0, sizeB = 0;
				for (int i = 0; i < carCount_ * 4; ++i) {
					SmokeWheel w = wheels_[i];
					if (w.radius > 0 && heat_[i] >= hottest) {
						hottest = heat_[i];
						tread = Mathf.Abs(w.angularSpeed) * w.radius;
						sizeA = w.emitSizeA;
						sizeB = w.emitSizeB;
					}
				}
				Native.Log(1, "Tyre smoke: " + (spawnedSinceLog_ / 5.0f).ToString("F0") + " puffs per second from the game, " + count_ + " alive, the game's opacity " + lastGameAlpha_.ToString("F2")
					+ ", emit size " + sizeA.ToString("F2") + " .. " + sizeB.ToString("F2") + ", hottest tyre " + hottest.ToString("F2") + " (tread " + tread.ToString("F0") + " m/s), "
					+ carCount_ + " cars" + (carCount_ > 0 ? ", first car " + cars_[0].carSpeed.ToString("F0") + " km/h, body spheres " + cars_[0].bs0.w.ToString("F2") + " / " + cars_[0].bs1.w.ToString("F2") + " / " + cars_[0].bs2.w.ToString("F2") : ""));
				spawnedSinceLog_ = 0;
				nextEmissionLog_ = Time.unscaledTime + 5.0f;
			}
			if (dev_ && Time.frameCount % 300 == 0) {
				double toMs = 1000.0 / System.Diagnostics.Stopwatch.Frequency;
				Native.Log(1, "Tyre smoke (dev): " + count_ + " puffs, motion and quads " + ((System.Diagnostics.Stopwatch.GetTimestamp() - t0) * toMs).ToString("F2") + " ms");
			}
			return count_;
		}

		// 0 below `from`, 1 above `to`, smooth between (Mathf.SmoothStep is something else: it blends its first two arguments)
		private static float Smooth(float from, float to, float x) {
			float t = Mathf.Clamp01((x - from) / (to - from));
			return t * t * (3.0f - 2.0f * t);
		}

		// How much one puff hides (as in Smoke.cpp).
		private static float Opacity(ref Puff p) {
			float thin = Mathf.Min(Mathf.Pow(0.4f / Mathf.Max(p.size, 0.05f), 0.8f), 1.0f);
			float fadeIn = Mathf.Clamp01(p.age / 0.15f);
			float last = 1.0f - Smooth(0.8f, 1.0f, p.age / p.life);
			float merge;
			if (p.seed < 0.5f) {
				merge = 1.0f - Smooth(1.0f, 1.5f, p.age);
			} else {
				merge = 1.0f + Smooth(1.0f, 1.5f, p.age);
				merge *= p.seed < 0.75f ? 1.0f - Smooth(3.0f, 3.5f, p.age) : 1.0f + Smooth(3.0f, 3.5f, p.age);
			}
			return Mathf.Clamp01(p.color.a * p.density * 0.22f * fadeIn * thin * last * merge);
		}

		private void EnsureArrays() {
			if (!spawn_.IsCreated) {
				spawn_ = new NativeArray<EmittingSpot>(SpawnCapacity, Allocator.Persistent);
				cars_ = new NativeArray<SmokeCar>(MaxCars, Allocator.Persistent);
				wheels_ = new NativeArray<SmokeWheel>(MaxCars * 4, Allocator.Persistent);
			}
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
				Native.Log(1, "Tyre smoke: the game's emitters are driven by Aether (settings " + config_.name + (stepField_ == null ? "; the emission event was not found: no smoke" : "") + ")");
			}
		}

		// The game's emission for this frame: the wheels, the cars and the spawn entries. Returns the number of entries.
		private int Emit() {
			if (stepField_ == null) {
				return 0;
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
				return 0; // no car with a smoke emitter
			}
			state_.ActiveId = 0;
			state_.CarId = 0;
			step(state_);
			carCount_ = Mathf.Clamp(state_.CarId, 0, MaxCars);
			return Mathf.Min(state_.ActiveId, spawn_.Length);
		}

		// ---- 3. heat: how hot each tyre is, from how fast its tread slides over the road
		private void Heat(float dt) {
			for (int i = 0; i < heat_.Length; ++i) {
				float target = 0;
				if (i < carCount_ * 4) {
					SmokeWheel w = wheels_[i];
					if (w.radius > 0) {
						Vector3 v = (Vector3)cars_[i >> 2].carVelocity;
						float along = Vector3.Dot(v, (Vector3)w.dir), across = Vector3.Dot(v, (Vector3)w.outside);
						float spinning = Mathf.Abs(w.angularSpeed) * w.radius - Mathf.Abs(along);
						float slide = Mathf.Sqrt(spinning * spinning + across * across); // m/s between tread and road
						target = Mathf.Clamp01((slide - 1.5f) / 16.0f);
					}
				}
				// (rubber heats in about a second of hard sliding and takes a few to cool)
				float rate = target > heat_[i] ? 1.4f : 0.3f;
				heat_[i] += (target - heat_[i]) * (1.0f - Mathf.Exp(-rate * dt));
			}
		}

		// Which way the tread runs: 1 when the top of the tyre moves the way the wheel points.
		private float Rolling(ref SmokeWheel w, Vector3 carVelocity) {
			float along = Vector3.Dot(carVelocity, (Vector3)w.dir);
			if (Mathf.Abs(w.angularSpeed) * w.radius > Mathf.Abs(along) + 1.0f) {
				return w.angularSpeed >= 0 ? 1.0f : -1.0f; // (spinning on the spot: the wheel's own sense)
			}
			return along >= 0 ? 1.0f : -1.0f;
		}

		// One puff of this frame's emission of a wheel. The game names one place for all of a frame's puffs: at 100 km/h
		// that is nine puffs in one spot and the next nine half a metre on: a string of lumps. The puffs are laid along
		// the way the contact patch came since the frame before, each as old as it would be by now.
		private void SpawnAlong(Vector3 contact, int wheelIndex, float dt) {
			contact_[wheelIndex] = contact;
			contactFrame_[wheelIndex] = frame_;
			float u = 1.0f;
			Vector3 from = contact;
			if (lastContactFrame_[wheelIndex] == frame_ - 1 && (lastContact_[wheelIndex] - contact).sqrMagnitude < 9.0f) {
				u = UnityEngine.Random.value;
				from = lastContact_[wheelIndex];
			}
			if (Spawn(Vector3.Lerp(from, contact, u), wheelIndex)) {
				ref Puff p = ref puffs_[count_ - 1];
				float early = (1.0f - u) * dt;
				p.age = early;
				if (p.wheel < 0) {
					p.position += p.velocity * early;
				} else {
					p.angle += early * 4.0f;
				}
			}
		}

		private bool Spawn(Vector3 contact, int wheelIndex) {
			if (count_ == MaxPuffs) {
				// full: the thinnest goes
				int least = 0;
				float leastOpacity = 2.0f;
				for (int i = 0; i < count_; i += 7) {
					float o = Opacity(ref puffs_[i]);
					if (o < leastOpacity) {
						leastOpacity = o;
						least = i;
					}
				}
				puffs_[least] = puffs_[--count_];
			}
			SmokeWheel wheel = wheels_[wheelIndex];
			// the packed colour: r in the low byte, sRGB; a is the opacity the game worked out (temperature, quality)
			uint packed = wheel.smokeColor;
			Color color = new Color((packed & 255) / 255.0f, ((packed >> 8) & 255) / 255.0f, ((packed >> 16) & 255) / 255.0f, 1.0f).linear;
			float gameAlpha = ((packed >> 24) & 255) / 255.0f;
			if (gameAlpha <= 0.0f) {
				return false;
			}
			// (The game's opacity is its own tyre temperature and its smoke quality setting. Aether has its own heat and
			// its own opacity option: every puff starts opaque, and what makes it thin is how hot the tyre is.)
			color.a = 1.0f;
			lastGameAlpha_ = gameAlpha;
			++spawnedSinceLog_;
			Vector3 up = (Vector3)wheel.up, dir = (Vector3)wheel.dir, outside = (Vector3)wheel.outside;
			if (up.sqrMagnitude < 0.5f) {
				up = Vector3.up;
			}
			Vector3 carVelocity = (Vector3)cars_[wheelIndex >> 2].carVelocity;
			float heat = Mathf.Clamp01(heat_[wheelIndex] + UnityEngine.Random.Range(-0.12f, 0.12f));
			// thin wisps or dense smoke: a cool tyre gives mostly wisps, a hot one mostly dense smoke with wisps among it
			bool wisp = UnityEngine.Random.value < Mathf.Lerp(0.55f, 0.1f, Smooth(0.1f, 0.6f, heat));
			// (as wide as the tyre and a little more: the smoke leaves the patch as a sheet of that width. The game's own
			// emit size, 8 to 10 cm, is for its point sprites.)
			float size = Mathf.Max(wheel.width, 0.2f) * UnityEngine.Random.Range(1.2f, 1.8f);
			Puff p = new Puff();
			p.heat = heat;
			if (wisp) {
				p.density = Mathf.Lerp(0.25f, 0.5f, heat) * UnityEngine.Random.Range(0.7f, 1.3f);
				p.life = Mathf.Lerp(1.4f, 3.5f, heat) * UnityEngine.Random.Range(0.8f, 1.25f);
				size *= 1.25f;
				// (thin smoke of fine droplets scatters blue more than red)
				color.r *= 0.84f;
				color.g *= 0.9f;
			} else {
				p.density = Mathf.Lerp(0.9f, 1.6f, heat) * UnityEngine.Random.Range(0.8f, 1.2f);
				p.life = Mathf.Lerp(5.0f, 11.0f, heat) * UnityEngine.Random.Range(0.8f, 1.25f);
				size *= Mathf.Lerp(0.85f, 1.2f, heat);
				float tint = Mathf.Lerp(0.92f, 1.0f, heat);
				color.r *= tint;
				color.g *= Mathf.Lerp(0.96f, 1.0f, heat);
			}
			p.color = color;
			p.size = size;
			p.spread = size;
			p.ground = contact.y;
			p.seed = UnityEngine.Random.value;
			p.turn = UnityEngine.Random.Range(0.0f, 6.283f);
			p.spin = UnityEngine.Random.Range(-0.5f, 0.5f);
			float tread = Mathf.Abs(wheel.angularSpeed) * wheel.radius; // m/s
			// ---- 1. on the tyre, or straight off the contact patch
			if (wheel.radius > 0.05f && UnityEngine.Random.value < (tread > 3.0f ? 0.45f : 0.2f)) {
				p.wheel = wheelIndex;
				p.angle = UnityEngine.Random.Range(0.0f, 0.3f);
				// (it leaves the tyre on the way up its back, below the height of the hub: above that is the wheel arch,
				// and the layer of air on the tread is torn off long before the top)
				p.releaseAngle = Mathf.Min(0.25f - Mathf.Log(1.0f - UnityEngine.Random.value * 0.999f) * 0.45f, 1.3f);
				p.side = UnityEngine.Random.Range(-0.5f, 0.5f) * Mathf.Max(wheel.width, 0.15f);
				p.size *= 0.65f; // (close on the tyre)
				p.lift = UnityEngine.Random.Range(0.01f, 0.07f);
				p.position = contact;
			} else {
				p.wheel = -1;
				float rolling = Rolling(ref wheel, carVelocity);
				p.position = contact + up * (size * 0.2f) + UnityEngine.Random.insideUnitSphere * 0.08f;
				// (off the patch: the way the tread runs there, backwards, and a little of everything)
				// Off the patch the way the tread slides over the road there (backwards under a spinning wheel, sideways
				// under a sliding one), carried a little with the car, and a little of everything.
				Vector3 slide = carVelocity - dir * (rolling * tread);
				float slideSpeed = slide.magnitude;
				if (slideSpeed > 6.0f) {
					slide *= 6.0f / slideSpeed;
				}
				p.velocity = carVelocity * 0.35f + slide * 0.5f + up * UnityEngine.Random.Range(0.1f, 0.5f)
					+ outside * UnityEngine.Random.Range(-0.3f, 0.8f) + UnityEngine.Random.insideUnitSphere * 0.6f;
			}
			puffs_[count_++] = p;
			return true;
		}

		private void Bodies() {
			bodyCount_ = 0;
			for (int c = 0; c < carCount_; ++c) {
				SmokeCar car = cars_[c];
				// the car from its wheels: the middles of its axles, its width
				Vector3 front = Vector3.zero, rear = Vector3.zero, rearLeft = Vector3.zero, rearRight = Vector3.zero, up = Vector3.up, forward = Vector3.forward;
				int fronts = 0, rears = 0;
				float radius = 0.32f, width = 0.25f;
				for (int i = 0; i < 4; ++i) {
					SmokeWheel w = wheels_[c * 4 + i];
					if (w.radius <= 0.05f) {
						continue;
					}
					if (w.isRear > 0.5f) {
						rear += (Vector3)w.pos;
						if (rears == 0) {
							rearLeft = (Vector3)w.pos;
						} else {
							rearRight = (Vector3)w.pos;
						}
						++rears;
						forward = (Vector3)w.dir; // (a rear wheel points where the car does)
						up = (Vector3)w.up;
						radius = w.radius;
						width = w.width;
					} else {
						front += (Vector3)w.pos;
						++fronts;
					}
				}
				if (fronts == 0 && rears == 0) {
					continue;
				}
				Vector3 v = (Vector3)car.carVelocity;
				float speed = v.magnitude;
				Body b = new Body();
				b.velocity = v;
				b.speed = speed;
				b.heading = speed > 0.5f ? v / speed : forward;
				Vector3 lateral = Vector3.Cross(Vector3.up, b.heading);
				float length = lateral.magnitude;
				b.lateral = length > 0.1f ? lateral / length : Vector3.zero;
				b.c0 = new Vector3(car.bs0.x, car.bs0.y, car.bs0.z);
				b.c1 = new Vector3(car.bs1.x, car.bs1.y, car.bs1.z);
				b.c2 = new Vector3(car.bs2.x, car.bs2.y, car.bs2.z);
				b.r0 = Mathf.Clamp(car.bs0.w, 0.6f, 1.3f);
				b.r1 = Mathf.Clamp(car.bs1.w, 0.6f, 1.3f);
				b.r2 = Mathf.Clamp(car.bs2.w, 0.6f, 1.3f);
				if (fronts == 2 && rears == 2 && forward.sqrMagnitude > 0.5f && up.sqrMagnitude > 0.5f) {
					front *= 0.5f;
					rear *= 0.5f;
					Vector3 right = Vector3.Cross(up, forward).normalized;
					float halfTrack = Mathf.Abs(Vector3.Dot(rearRight - rearLeft, right)) * 0.5f;
					float halfBase = Vector3.Distance(front, rear) * 0.5f;
					// (the floor some 15 cm over the road, the roof at 1.3 m; the bumpers 0.9 m beyond the axles; the
					// body as wide as the tyres' outer faces)
					const float floor = 0.15f, roof = 1.3f, overhang = 0.9f;
					Vector3 road = (front + rear) * 0.5f - up * radius;
					b.boxCentre = road + up * ((floor + roof) * 0.5f);
					b.hx = Mathf.Clamp(halfTrack + width * 0.5f, 0.6f, 1.3f);
					b.hy = (roof - floor) * 0.5f;
					b.hz = Mathf.Clamp(halfBase + overhang, 1.2f, 3.5f);
					b.forward = forward;
					b.up = up;
					b.right = right;
					b.hasBox = 1.0f;
				}
				bodies_[bodyCount_++] = b;
			}
			for (int i = 0; i < carCount_ * 4; ++i) {
				SmokeWheel w = wheels_[i];
				Vector3 carVelocity = (Vector3)cars_[i >> 2].carVelocity;
				wheelAir_[i] = new WheelAir {
					pos = (Vector3)w.pos, radius = w.radius, dir = (Vector3)w.dir, angularSpeed = w.angularSpeed, up = (Vector3)w.up,
					rolling = w.radius > 0 ? Rolling(ref w, carVelocity) : 1.0f, outside = (Vector3)w.outside, carVelocity = carVelocity,
				};
			}
			// the wind: a steady breeze that swings slowly
			float swing = 0.6f * Mathf.Sin(time_ * 0.05f);
			wind_ = new Vector3(Mathf.Cos(0.5f + swing), 0, Mathf.Sin(0.5f + swing)) * 1.4f;
		}

		// ---- the development emitter: a car that is not there, sliding round a circle
		private void ReadDev() {
			if (devFile_ == null && ParticleCapture.ConfigDir != null) {
				devFile_ = Path.Combine(ParticleCapture.ConfigDir, "dev-smoke.txt");
			}
			if (devFile_ == null || Time.unscaledTime < nextDevRead_) {
				return;
			}
			nextDevRead_ = Time.unscaledTime + 1.0f;
			dev_ = false;
			try {
				if (File.Exists(devFile_)) {
					string[] t = File.ReadAllText(devFile_).Split(new[] { ' ', ',', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
					if (t.Length >= 6) {
						for (int i = 0; i < 6; ++i) {
							devValues_[i] = float.Parse(t[i], CultureInfo.InvariantCulture);
						}
						dev_ = devValues_[3] > 0.5f;
					}
				}
			} catch (Exception) {
				dev_ = false;
			}
		}

		private void DevCar(float dt) {
			if (carCount_ >= MaxCars) {
				return;
			}
			float radius = devValues_[3], speed = devValues_[4], slip = devValues_[5] * Mathf.Deg2Rad;
			devAngle_ += speed / radius * dt;
			Vector3 centre = new Vector3(devValues_[0] + radius * Mathf.Cos(devAngle_), devValues_[1], devValues_[2] + radius * Mathf.Sin(devAngle_));
			Vector3 heading = new Vector3(-Mathf.Sin(devAngle_), 0, Mathf.Cos(devAngle_));
			// (the nose points into the turn by the slip angle; the turn is to the right, seen from above)
			Vector3 forward = Quaternion.AngleAxis(-slip * Mathf.Rad2Deg, Vector3.up) * heading;
			Vector3 right = Vector3.Cross(Vector3.up, forward);
			const float wheelRadius = 0.32f, halfTrack = 0.76f, halfBase = 1.3f;
			int car = carCount_++;
			SmokeCar c = new SmokeCar();
			c.carVelocity = heading * speed;
			c.carSpeed = speed * 3.6f;
			Vector3 front = centre + forward * halfBase + Vector3.up * wheelRadius, rear = centre - forward * halfBase + Vector3.up * wheelRadius;
			c.bs0 = new float4(front.x, front.y + 0.2f, front.z, halfTrack);
			c.bs2 = new float4(rear.x, rear.y + 0.2f, rear.z, halfTrack);
			c.bs1 = (c.bs0 + c.bs2) * 0.5f;
			cars_[car] = c;
			for (int i = 0; i < 4; ++i) {
				bool isRear = i >= 2, left = (i & 1) == 0;
				SmokeWheel w = new SmokeWheel();
				w.radius = wheelRadius;
				w.width = 0.26f;
				w.up = Vector3.up;
				w.dir = isRear ? forward : heading; // (the front wheels steer into the slide)
				w.outside = right * (left ? -1.0f : 1.0f);
				w.pos = (isRear ? rear : front) + right * (left ? -halfTrack : halfTrack);
				w.velocity = heading * speed;
				w.isRear = isRear ? 1 : 0;
				// (the rear wheels spin faster than the road goes by)
				w.angularSpeed = (speed * Mathf.Cos(slip) + (isRear ? 14.0f : 0.0f)) / wheelRadius;
				w.smokeColor = 0xFFFFFFFFu;
				w.emitSizeA = 0.3f;
				w.emitSizeB = 0.5f;
				wheels_[car * 4 + i] = w;
			}
		}

		private void DevSpawn(float dt) {
			int car = carCount_ - 1;
			if (car < 0) {
				return;
			}
			devRate_ += 260.0f * dt; // per rear wheel and second (the game: some 520 a second per car)
			int n = (int)devRate_;
			devRate_ -= n;
			for (int i = 2; i < 4; ++i) {
				SmokeWheel w = wheels_[car * 4 + i];
				for (int k = 0; k < n; ++k) {
					SpawnAlong((Vector3)w.pos - Vector3.up * w.radius, car * 4 + i, dt);
				}
			}
		}

		public void Clear() {
			count_ = 0;
			config_ = null; // (looked up again: another scene has its own settings objects)
			nextLookup_ = 0;
			Array.Clear(heat_, 0, heat_.Length);
		}
	}
}
