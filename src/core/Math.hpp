#pragma once
// Affine transform math in double precision. Xform maps a point p to L*p + t.
#include <array>
#include <cmath>

namespace nm {

struct Vec3 {
	double x = 0, y = 0, z = 0;
	Vec3() = default;
	Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
	Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
	Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
	double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
	Vec3 cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
	double length() const { return std::sqrt(dot(*this)); }
	Vec3 normalized() const {
		double l = length();
		return l > 1e-12 ? *this * (1.0 / l) : Vec3();
	}
};

struct Mat3 {
	// m[row][col]
	std::array<std::array<double, 3>, 3> m{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

	static Mat3 zero() {
		Mat3 r;
		for (auto& row : r.m) row = {0, 0, 0};
		return r;
	}
	Vec3 operator*(const Vec3& v) const {
		return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
				m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
				m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
	}
	Mat3 operator*(const Mat3& o) const {
		Mat3 r = zero();
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				for (int k = 0; k < 3; k++) r.m[i][j] += m[i][k] * o.m[k][j];
		return r;
	}
	Mat3 operator*(double s) const {
		Mat3 r = *this;
		for (auto& row : r.m)
			for (auto& v : row) v *= s;
		return r;
	}
	Mat3& operator+=(const Mat3& o) {
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++) m[i][j] += o.m[i][j];
		return *this;
	}
	Mat3 transposed() const {
		Mat3 r;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++) r.m[i][j] = m[j][i];
		return r;
	}
	double det() const {
		return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	}
	Mat3 inverse() const {
		double d = det();
		Mat3 r;
		r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d;
		r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d;
		r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
		r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d;
		r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d;
		r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
		r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d;
		r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d;
		r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
		return r;
	}
	Vec3 col(int c) const { return {m[0][c], m[1][c], m[2][c]}; }
	void setCol(int c, const Vec3& v) { m[0][c] = v.x; m[1][c] = v.y; m[2][c] = v.z; }
	// Nearest proper rotation (Gram-Schmidt on columns, X then Y), discarding scale and shear.
	Mat3 orthonormalized() const {
		Vec3 x = col(0).normalized();
		Vec3 y = (col(1) - x * x.dot(col(1))).normalized();
		Vec3 z = x.cross(y);
		Mat3 r;
		r.setCol(0, x);
		r.setCol(1, y);
		r.setCol(2, z);
		return r;
	}
};

// Unit rotation quaternion (x, y, z, w), w the scalar part. Same rotation convention as Mat3:
// toMat3() * v rotates v.
struct Quat {
	double x = 0, y = 0, z = 0, w = 1;
	Quat() = default;
	Quat(double x_, double y_, double z_, double w_) : x(x_), y(y_), z(z_), w(w_) {}
	double dot(const Quat& o) const { return x * o.x + y * o.y + z * o.z + w * o.w; }
	Quat normalized() const {
		double l = std::sqrt(dot(*this));
		return l > 1e-12 ? Quat(x / l, y / l, z / l, w / l) : Quat();
	}
	// Angle between the two rotations, radians (q and -q are the same rotation).
	double angleTo(const Quat& o) const { return 2.0 * std::acos(std::fmin(1.0, std::fabs(dot(o)))); }
	Mat3 toMat3() const {
		Mat3 r;
		r.m[0] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)};
		r.m[1] = {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)};
		r.m[2] = {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
		return r;
	}
	// From a proper rotation matrix (Shepperd's method: pivot on the largest diagonal term).
	static Quat fromMat3(const Mat3& r) {
		const auto& m = r.m;
		double tr = m[0][0] + m[1][1] + m[2][2];
		Quat q;
		if (tr > 0) {
			double s = std::sqrt(tr + 1.0) * 2;
			q = {(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25 * s};
		} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
			double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
			q = {0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
		} else if (m[1][1] > m[2][2]) {
			double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
			q = {(m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
		} else {
			double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
			q = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s, (m[1][0] - m[0][1]) / s};
		}
		return q.normalized();
	}
};

struct Xform {
	Mat3 L;
	Vec3 t;

	Vec3 point(const Vec3& p) const { return L * p + t; }
	Vec3 dir(const Vec3& d) const { return L * d; }
	Xform operator*(const Xform& o) const { return {L * o.L, L * o.t + t}; }
	Xform inverse() const {
		Mat3 inv = L.inverse();
		return {inv, (inv * t) * -1.0};
	}
	// Rigid part: rotation without scale, translation kept.
	Xform rigid() const { return {L.orthonormalized(), t}; }
	double maxDiff(const Xform& o) const {
		double d = 0;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++) d = std::fmax(d, std::fabs(L.m[i][j] - o.L.m[i][j]));
		Vec3 dt = t - o.t;
		return std::fmax(d, std::fmax(std::fabs(dt.x), std::fmax(std::fabs(dt.y), std::fabs(dt.z))));
	}
};

} // namespace nm
