// A small 3-D vector. Positions are in millimetres in the stage frame:
// x to the audience's right, y away from the audience (upstage), z up from the deck.
#pragma once

#include <cmath>

namespace leota {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;

    constexpr Vec3() = default;
    constexpr Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(double k) const { return {x * k, y * k, z * k}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }

    constexpr double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    constexpr Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double norm() const { return std::sqrt(dot(*this)); }
};

constexpr Vec3 operator*(double k, const Vec3& v) { return v * k; }

// Rotates v by a rotation vector: its direction is the axis, its length the angle in
// radians. The ball's lean and turn are kept this way.
inline Vec3 rotate(const Vec3& rv, const Vec3& v) {
    const double th = rv.norm();
    if (th < 1e-12) return v + rv.cross(v);
    const Vec3 k = rv * (1.0 / th);
    const double c = std::cos(th), s = std::sin(th);
    return v * c + k.cross(v) * s + k * (k.dot(v) * (1.0 - c));
}

}  // namespace leota
