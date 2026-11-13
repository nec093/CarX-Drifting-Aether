// The tyre smoke's motion: one step for every puff, and the puffs as quads for the particle pass.
// The model is described in src/Aether.Managed/SceneCapture/SmokeSimulation.cs, which owns the puffs, decides what
// is born and prepares this frame's wheels and cars; this is its inner loop (in C# on Mono a puff took a
// microsecond: 2000 puffs 2 ms of the main thread; here some tens of nanoseconds).
#include "../include/AetherApi.h"
#include "../include/SceneContract.h"
#include "SceneRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

struct V3 {
	float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Clamp01(float v) { return std::min(std::max(v, 0.0f), 1.0f); }

// (the same layouts as the C# structs of the same names)
struct SmokePuff {
	V3 position, velocity;
	float age, life, size, spread, turn, spin;
	float heat, density, ground, seed;
	float color[4];
	int32_t wheel;
	float angle, releaseAngle, side, lift;
};
struct SmokeWheelAir {
	V3 pos;
	float radius; // 0: not there this frame
	V3 dir;
	float angularSpeed;
	V3 up;
	float rolling; // 1 when the top of the tyre moves the way the wheel points, else -1
	V3 outside;
	float pad0;
	V3 carVelocity;
	float pad1;
};
struct SmokeBody {
	V3 velocity;
	float speed;
	V3 heading;
	float r0;
	V3 lateral; // (all 0: the car moves straight up or down)
	float r1;
	V3 c0;
	float r2;
	V3 c1;
	float pad0;
	V3 c2;
	float pad1;
	// the body as a box (floor to roof, bumper to bumper): smoke does not get into it
	V3 boxCentre;
	float hx; // half its width
	V3 forward;
	float hy; // half its height
	V3 up;
	float hz; // half its length
	V3 right;
	float hasBox;
};
static_assert(sizeof(SmokePuff) == 100, "SmokePuff layout");
static_assert(sizeof(SmokeWheelAir) == 80, "SmokeWheelAir layout");
static_assert(sizeof(SmokeBody) == 160, "SmokeBody layout");

uint32_t g_random = 0x9E3779B9u;
inline float Random01() {
	g_random ^= g_random << 13;
	g_random ^= g_random >> 17;
	g_random ^= g_random << 5;
	return float(g_random >> 8) * (1.0f / 16777216.0f);
}

inline float Smooth(float from, float to, float x) {
	const float t = Clamp01((x - from) / (to - from));
	return t * t * (3.0f - 2.0f * t);
}

// How much one puff hides. Little: the smoke is many puffs over one another (the game gives some 500 a second per
// car), and it is their sum that is dense. A puff holds the smoke it was born with: thinner as it spreads, though by
// less than its size grows (it loses to its neighbours what they lose to it).
inline float Opacity(const SmokePuff& p) {
	const float thin = std::min(std::pow(0.4f / std::max(p.size, 0.05f), 0.8f), 1.0f);
	const float fadeIn = Clamp01(p.age / 0.15f);
	const float last = 1.0f - Smooth(0.8f, 1.0f, p.age / p.life);
	// Fewer puffs as the smoke gets old: by then each is metres across and they lie dozens deep, which is what the
	// drawing costs. Half of them (by their own number) go at a second and a half, half of the rest at three and a
	// half; the ones that stay take over their share.
	float merge;
	if (p.seed < 0.5f) {
		merge = 1.0f - Smooth(1.0f, 1.5f, p.age);
	} else {
		merge = 1.0f + Smooth(1.0f, 1.5f, p.age);
		merge *= p.seed < 0.75f ? 1.0f - Smooth(3.0f, 3.5f, p.age) : 1.0f + Smooth(3.0f, 3.5f, p.age);
	}
	return Clamp01(p.color[3] * p.density * 0.22f * fadeIn * thin * last * merge);
}

// The air at a place: the wind, and what the cars do to the air round them. `mixing`: how hard the air is stirred
// there (0 still air .. 1 the middle of a wake).
V3 Air(V3 p, float ground, float seed, float time, V3 wind, const SmokeBody* bodies, int bodyCount, float& mixing) {
	// the wind in gusts, less of it near the ground
	const float gust = 1.0f + 0.45f * std::sin(time * 0.37f + p.x * 0.04f) + 0.25f * std::sin(time * 1.13f + p.z * 0.07f);
	const float height = Clamp01((p.y - ground) / 2.5f);
	V3 air = wind * (gust * (0.3f + 0.7f * height));
	mixing = 0;
	for (int c = 0; c < bodyCount; ++c) {
		const SmokeBody& car = bodies[c];
		const float mx = p.x - car.c1.x, mz = p.z - car.c1.z;
		if (mx * mx + mz * mz > 35.0f * 35.0f) {
			continue;
		}
		// the body: three spheres. Inside one the air goes with the car; behind one (where the car came from) is its
		// wake, wider and weaker with the distance.
		float best = 0, bestRadius = 1;
		V3 bestRadial = {0, 0, 0}, bestCentre = car.c1;
		if (car.hasBox > 0.5f) {
			// under the floor: the air there is dragged along with the car
			const V3 rel = p - car.boxCentre;
			if (std::fabs(Dot(rel, car.right)) < car.hx && std::fabs(Dot(rel, car.forward)) < car.hz && Dot(rel, car.up) <= -car.hy) {
				best = 0.6f;
			}
		}
		for (int s = 0; s < 3; ++s) {
			const float radius = s == 0 ? car.r0 : s == 1 ? car.r1 : car.r2;
			const V3 centre = s == 0 ? car.c0 : s == 1 ? car.c1 : car.c2;
			const V3 rel = p - centre;
			const float behind = -Dot(rel, car.heading);
			if (behind < -radius) {
				continue; // in front of it
			}
			const V3 radial = rel + car.heading * behind;
			float share;
			if (Dot(rel, rel) < radius * radius) {
				share = 0.6f;
			} else {
				const float reach2 = std::max(behind, 0.0f);
				const float width = radius * (0.9f + 0.22f * reach2 / radius);
				const float off2 = Dot(radial, radial) / (width * width);
				share = off2 > 12.0f ? 0.0f : 0.7f * std::exp(-reach2 / (5.0f * radius) - off2);
			}
			if (share > best) {
				best = share;
				bestRadius = radius;
				bestRadial = radial;
				bestCentre = centre;
			}
		}
		if (best < 0.01f) {
			continue;
		}
		air = air + car.velocity * best; // dragged along
		const float off = std::sqrt(Dot(bestRadial, bestRadial));
		if (off > 1e-3f) {
			// drawn in towards the middle of the wake (the pressure there is low), and lifted
			air = air - bestRadial * (car.speed * 0.10f * best * std::min(off / bestRadius, 1.0f) / off);
		}
		air.y += car.speed * 0.07f * best;
		// the two vortices trailing from the rear corners: up in the middle, down outside
		if (Dot(car.lateral, car.lateral) > 0.5f) {
			const float core = 0.35f * bestRadius;
			for (int side = -1; side <= 1; side += 2) {
				V3 q = p - (bestCentre + car.lateral * (float(side) * 0.55f * bestRadius) + V3{0, 0.2f * bestRadius, 0});
				q = q - car.heading * Dot(q, car.heading);
				air = air - Cross(car.heading, q) * (float(side) * car.speed * 0.22f * best * core / (Dot(q, q) + core * core));
			}
		}
		mixing = std::max(mixing, best * Clamp01(car.speed / 20.0f));
	}
	// eddies: the same for neighbours, different from puff to puff
	const float eddy = 0.3f + 1.6f * mixing;
	air.x += eddy * (std::sin(p.z * 1.7f + time * 1.3f + seed * 6.0f) + std::sin(p.y * 2.3f - time * 0.9f)) * 0.5f;
	air.y += eddy * std::sin(p.x * 1.9f + time * 1.1f + seed * 5.0f) * 0.35f;
	air.z += eddy * (std::sin(p.x * 1.5f - time * 1.7f + seed * 4.0f) + std::sin(p.y * 2.1f + time * 0.7f)) * 0.5f;
	return air;
}

// ---- the smoke's own shade. How much smoke there is, in cells of a metre (a hash table: the smoke is anywhere),
// and from that, per puff, how much smoke lies between it and the sun and between it and the sky above. Without it
// a cloud of many puffs is one flat white shape: every puff is lit as if it were alone.
const int kCells = 1 << 14;
struct Cell {
	int32_t x, y, z;
	float amount; // < 0: free
};
Cell g_cells[kCells];

inline uint32_t CellHash(int x, int y, int z) {
	return (uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u) & uint32_t(kCells - 1);
}

void CellAdd(V3 p, float amount) {
	const int x = int(std::floor(p.x)), y = int(std::floor(p.y)), z = int(std::floor(p.z));
	uint32_t h = CellHash(x, y, z);
	for (int probe = 0; probe < 16; ++probe, h = (h + 1) & uint32_t(kCells - 1)) {
		Cell& c = g_cells[h];
		if (c.amount < 0) {
			c = {x, y, z, amount};
			return;
		}
		if (c.x == x && c.y == y && c.z == z) {
			c.amount += amount;
			return;
		}
	}
}

float CellAt(V3 p) {
	const int x = int(std::floor(p.x)), y = int(std::floor(p.y)), z = int(std::floor(p.z));
	uint32_t h = CellHash(x, y, z);
	for (int probe = 0; probe < 16; ++probe, h = (h + 1) & uint32_t(kCells - 1)) {
		const Cell& c = g_cells[h];
		if (c.amount < 0) {
			return 0;
		}
		if (c.x == x && c.y == y && c.z == z) {
			return c.amount;
		}
	}
	return 0;
}

// The smoke on the way from a place in a direction (a few steps that grow: about six metres).
float SmokeTowards(V3 p, V3 d) {
	float sum = 0, reach = 0.7f, at = 0.5f;
	for (int k = 0; k < 5; ++k) {
		sum += CellAt(p + d * at) * reach;
		at += reach;
		reach *= 1.45f;
	}
	return sum;
}

} // namespace

// One step for `count` puffs (in place; puffs that are gone are overwritten by the last ones), and the puffs that
// are left as quads in `out` (room for `count`). Returns how many are left.
// `wind`: three floats. `opacity`: Aether's smoke opacity option.
AETHER_API int Aether_SmokeStep(void* puffData, int count, const void* wheelData, int wheelCount, const void* bodyData, int bodyCount, float dt, float time, const float* wind, float opacity, AeParticle* out) {
	SmokePuff* puffs = static_cast<SmokePuff*>(puffData);
	const SmokeWheelAir* wheels = static_cast<const SmokeWheelAir*>(wheelData);
	const SmokeBody* bodies = static_cast<const SmokeBody*>(bodyData);
	if (!puffs || count <= 0 || !wind) {
		return 0;
	}
	const V3 windVector = {wind[0], wind[1], wind[2]};
	const float follow = 1.0f - std::exp(-2.2f * dt); // (smoke weighs next to nothing: it takes the air's speed in half a second)
	for (int i = 0; i < count;) {
		SmokePuff& p = puffs[i];
		p.age += dt;
		if (p.age >= p.life) {
			puffs[i] = puffs[--count];
			continue;
		}
		float mixing = 0;
		if (p.wheel >= 0) {
			// ---- carried round the tyre
			if (p.wheel >= wheelCount || !wheels || wheels[p.wheel].radius <= 0.05f) {
				p.wheel = -1; // (the car is gone)
				p.velocity = {0, 0, 0};
			} else {
				const SmokeWheelAir& w = wheels[p.wheel];
				// (the air on the tread is slower than the tread)
				// (and far slower than that: it is a layer of air that slips, not a part of the tyre)
				p.angle += std::min(std::max(std::fabs(w.angularSpeed) * 0.25f, 1.5f), 10.0f) * dt;
				const float sin = std::sin(p.angle), cos = std::cos(p.angle);
				const V3 radial = w.up * -cos - w.dir * (sin * w.rolling);
				// (it drifts off the tread as it goes, and outwards: the wheel arch is shut above and inside)
				const float outwards = p.side + p.angle * 0.08f;
				p.position = w.pos + radial * (w.radius + p.lift + p.age * 0.35f) + w.outside * outwards;
				if (p.angle >= p.releaseAngle) {
					const V3 tangent = w.up * sin - w.dir * (cos * w.rolling);
					const V3 scatter = {Random01() - 0.5f, Random01() - 0.5f, Random01() - 0.5f};
					p.velocity = w.carVelocity + tangent * std::min(std::fabs(w.angularSpeed) * w.radius * 0.15f, 4.0f) + w.outside * (0.3f + Random01() * 1.2f) + scatter * 0.6f;
					// (not thrown up: above the tyre is the wheel arch)
					p.velocity.y = std::min(p.velocity.y, w.carVelocity.y + 1.0f);
					p.wheel = -1;
				}
			}
		} else {
			const V3 air = Air(p.position, p.ground, p.seed, time, windVector, bodies, bodies ? bodyCount : 0, mixing);
			p.velocity = p.velocity + (air - p.velocity) * follow;
			p.velocity.y += (0.15f + 0.8f * p.heat) * std::exp(-p.age * 0.5f) * dt; // (hot smoke rises, for as long as it is hot)
			p.position = p.position + p.velocity * dt;
			// the car bodies: smoke goes round them, not through them. A puff that got into a body's box leaves it by
			// the nearest way out that there is: a side (the wheel arches), the tail, or down under the floor; hardly
			// ever the nose or the roof.
			for (int c = 0; c < (bodies ? bodyCount : 0); ++c) {
				const SmokeBody& car = bodies[c];
				if (car.hasBox < 0.5f) {
					continue;
				}
				const float margin = 0.08f;
				const V3 rel = p.position - car.boxCentre;
				float lx = Dot(rel, car.right), ly = Dot(rel, car.up), lz = Dot(rel, car.forward);
				if (std::fabs(lx) >= car.hx + margin || std::fabs(lz) >= car.hz + margin || ly <= -car.hy || ly >= car.hy + margin) {
					continue;
				}
				const float side = car.hx + margin - std::fabs(lx), rear = car.hz + margin + lz, bottom = ly + car.hy;
				const float front = (car.hz + margin - lz) * 3.0f, top = (car.hy + margin - ly) * 4.0f;
				const float least = std::min(std::min(side, rear), std::min(bottom, std::min(front, top)));
				V3 n;
				if (least == side) {
					const float sign = lx >= 0 ? 1.0f : -1.0f;
					lx = sign * (car.hx + margin);
					n = car.right * sign;
				} else if (least == rear) {
					lz = -(car.hz + margin);
					n = car.forward * -1.0f;
				} else if (least == bottom) {
					ly = -car.hy;
					n = car.up * -1.0f;
				} else if (least == front) {
					lz = car.hz + margin;
					n = car.forward;
				} else {
					ly = car.hy + margin;
					n = car.up;
				}
				p.position = car.boxCentre + car.right * lx + car.up * ly + car.forward * lz;
				// (it does not move into the body either: what it had of that, against the body's own motion, is gone)
				const float into = Dot(p.velocity - car.velocity, n);
				if (into < 0) {
					p.velocity = p.velocity - n * into;
				}
			}
			const float floor = p.ground + p.size * 0.12f;
			if (p.position.y < floor) {
				p.position.y = floor;
				if (p.velocity.y < 0) {
					p.velocity.y *= -0.1f;
				}
			}
		}
		// ---- spreading: fast at first, slower and slower (mixing with the air round it), faster in a wake
		const float rate = (p.density < 0.4f ? 1.5f : 0.95f) * 0.6f / std::sqrt(p.age + 0.15f) * (1.0f + 0.8f * mixing);
		p.size += rate * dt;
		p.turn += p.spin * dt;
		const float alpha = Opacity(p);
		if (p.age > 0.5f && alpha < 0.006f) {
			puffs[i] = puffs[--count]; // too thin to see
			continue;
		}
		if (out) {
			AeParticle& q = out[i];
			q.position[0] = p.position.x;
			q.position[1] = p.position.y;
			q.position[2] = p.position.z;
			q.size = p.size;
			q.color[0] = p.color[0];
			q.color[1] = p.color[1];
			q.color[2] = p.color[2];
			q.color[3] = Clamp01(alpha * opacity);
			q.rotation = p.turn;
			q.stretch[0] = q.stretch[1] = q.stretch[2] = 0;
			// (uv: two numbers that tell this puff's shape from the next one's, how dense it is, how much of the sky and the sun reach it)
			q.uv[0] = p.seed;
			q.uv[1] = std::fmod(p.seed * 7.31f, 1.0f);
			q.uv[2] = p.density;
			q.uv[3] = 100.99f; // (the smoke's own shade: filled in below)
		}
		++i;
	}
	if (out && count > 0) {
		for (Cell& c : g_cells) {
			c.amount = -1;
		}
		// (what a puff adds to its cell: its opacity, less the more cells it is spread over)
		for (int i = 0; i < count; ++i) {
			const float across = std::max(puffs[i].size, 1.0f);
			CellAdd(puffs[i].position, out[i].color[3] / (opacity > 0 ? opacity : 1.0f) / across);
		}
		const aether::Vec3 s = aether::SceneRenderer::Get().SunDirection();
		const V3 sun = {s.x, s.y, s.z};
		for (int i = 0; i < count; ++i) {
			const float toSun = std::exp(-2.0f * SmokeTowards(puffs[i].position, sun));
			const float toSky = std::exp(-1.5f * SmokeTowards(puffs[i].position, V3{0, 1, 0}));
			// (uv.w: the sky's share in hundredths, and the sun's share after the point)
			out[i].uv[3] = std::floor(Clamp01(toSky) * 100.0f) + Clamp01(toSun) * 0.99f;
		}
	}
	return count;
}
