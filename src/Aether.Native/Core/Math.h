// Small matrix / vector helpers in the scene contract's convention: 4x4 matrices as 16 floats, column-major
// (m[col * 4 + row]), column vectors (M * v). Unity's Matrix4x4 has the same memory layout, and HLSL's default
// (column_major) packing reads it back as the same matrix, so shaders use mul(M, v).
#pragma once
#include <cmath>
#include <cstring>

namespace aether {

struct Vec3 {
	float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a) {
	float l = Length(a);
	return l > 1e-12f ? a * (1.0f / l) : Vec3{0, 0, 1};
}

struct Mat4 {
	float m[16];
	float& at(int row, int col) { return m[col * 4 + row]; }
	float at(int row, int col) const { return m[col * 4 + row]; }
	static Mat4 Identity() {
		Mat4 r;
		std::memset(r.m, 0, sizeof(r.m));
		r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
		return r;
	}
	static Mat4 From(const float* p) {
		Mat4 r;
		std::memcpy(r.m, p, sizeof(r.m));
		return r;
	}
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
	Mat4 r;
	for (int c = 0; c < 4; ++c) {
		for (int row = 0; row < 4; ++row) {
			float s = 0;
			for (int k = 0; k < 4; ++k) {
				s += a.at(row, k) * b.at(k, c);
			}
			r.at(row, c) = s;
		}
	}
	return r;
}

inline Vec3 TransformPoint(const Mat4& m, Vec3 p) {
	return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
		m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
		m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}

inline Vec3 TransformDirection(const Mat4& m, Vec3 d) {
	return {m.at(0, 0) * d.x + m.at(0, 1) * d.y + m.at(0, 2) * d.z, m.at(1, 0) * d.x + m.at(1, 1) * d.y + m.at(1, 2) * d.z, m.at(2, 0) * d.x + m.at(2, 1) * d.y + m.at(2, 2) * d.z};
}

// Homogeneous transform with the divide (for unprojecting clip space corners).
inline Vec3 TransformProjective(const Mat4& m, Vec3 p) {
	float w = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
	Vec3 r = TransformPoint(m, p);
	return std::fabs(w) > 1e-12f ? r * (1.0f / w) : r;
}

inline float Determinant3x3(const Mat4& m) {
	return m.at(0, 0) * (m.at(1, 1) * m.at(2, 2) - m.at(1, 2) * m.at(2, 1)) - m.at(0, 1) * (m.at(1, 0) * m.at(2, 2) - m.at(1, 2) * m.at(2, 0)) +
		m.at(0, 2) * (m.at(1, 0) * m.at(2, 1) - m.at(1, 1) * m.at(2, 0));
}

// General 4x4 inverse (cofactors). Returns identity for a singular matrix.
inline Mat4 Inverse(const Mat4& a) {
	const float* m = a.m;
	float inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (std::fabs(det) < 1e-30f) {
		return Mat4::Identity();
	}
	Mat4 r;
	float s = 1.0f / det;
	for (int i = 0; i < 16; ++i) {
		r.m[i] = inv[i] * s;
	}
	return r;
}

// A view matrix for a light: right-handed, looking down -Z from `eye` along `forward` (like Unity's worldToCamera).
inline Mat4 LookAlong(Vec3 eye, Vec3 forward, Vec3 upHint) {
	Vec3 f = Normalize(forward);
	Vec3 r = Normalize(Cross(f, upHint)); // right-handed: right = forward x up
	if (Length(Cross(f, upHint)) < 1e-4f) {
		r = Normalize(Cross(f, Vec3{1, 0, 0}));
	}
	Vec3 u = Cross(r, f);
	Mat4 m = Mat4::Identity();
	m.at(0, 0) = r.x; m.at(0, 1) = r.y; m.at(0, 2) = r.z; m.at(0, 3) = -Dot(r, eye);
	m.at(1, 0) = u.x; m.at(1, 1) = u.y; m.at(1, 2) = u.z; m.at(1, 3) = -Dot(u, eye);
	m.at(2, 0) = -f.x; m.at(2, 1) = -f.y; m.at(2, 2) = -f.z; m.at(2, 3) = Dot(f, eye);
	return m;
}

// Orthographic projection for a right-handed view space (looking down -Z) to D3D clip space with standard depth
// (near = 0, far = 1). Used for the shadow maps. zNear / zFar are distances in front of the eye.
inline Mat4 OrthoRH(float left, float right, float bottom, float top, float zNear, float zFar) {
	Mat4 m = Mat4::Identity();
	m.at(0, 0) = 2.0f / (right - left);
	m.at(1, 1) = 2.0f / (top - bottom);
	m.at(2, 2) = -1.0f / (zFar - zNear);
	m.at(0, 3) = -(right + left) / (right - left);
	m.at(1, 3) = -(top + bottom) / (top - bottom);
	m.at(2, 3) = -zNear / (zFar - zNear);
	return m;
}

// Perspective projection in the scene contract's convention (what GL.GetGPUProjectionMatrix(p, false) gives on D3D):
// right-handed view space, y up, reversed Z. For the test host's own camera.
inline Mat4 PerspectiveReversedZ(float fovYRadians, float aspect, float zNear, float zFar) {
	float f = 1.0f / std::tan(fovYRadians * 0.5f);
	Mat4 m;
	std::memset(m.m, 0, sizeof(m.m));
	m.at(0, 0) = f / aspect;
	m.at(1, 1) = f;
	m.at(2, 2) = zNear / (zFar - zNear);
	m.at(2, 3) = zFar * zNear / (zFar - zNear);
	m.at(3, 2) = -1.0f;
	return m;
}

} // namespace aether
