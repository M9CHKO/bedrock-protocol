#pragma once
#include <algorithm>
#include <cmath>

namespace bedrock::area_fill {
struct Point { double x, y, z; };

inline double distanceSquared(Point a, Point b) noexcept {
    const double x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x * x + y * y + z * z;
}
inline bool intersectsPlayer(Point eye, Point block) noexcept {
    constexpr double halfWidth = 0.3, eyeHeight = 1.62, height = 1.8;
    const double feet = eye.y - eyeHeight;
    return eye.x + halfWidth > block.x + 0.001 &&
        eye.x - halfWidth < block.x + 0.999 &&
        eye.z + halfWidth > block.z + 0.001 &&
        eye.z - halfWidth < block.z + 0.999 &&
        feet < block.y + 0.999 && feet + height > block.y + 0.001;
}
// Packet movement axes: +X is strafe-left, +Z is forward.
// Preserve the camera yaw instead of rotating it at every path corner.
inline Point localInput(double dx, double dz, double yawDegrees) noexcept {
    const double length = std::hypot(dx, dz);
    if (!std::isfinite(length) || !std::isfinite(yawDegrees) || length < 1e-8)
        return {0, 0, 0};
    const double yaw = yawDegrees * 0.017453292519943295;
    return {(dx * std::cos(yaw) + dz * std::sin(yaw)) / length, 0,
        (-dx * std::sin(yaw) + dz * std::cos(yaw)) / length};
}
// Every sample including the endpoint must be walkable. A route between two
// individually safe cells can still cross an unsupported edge or an obstacle.
template <class StandingHeight>
inline bool safeSegment(Point from, Point to, StandingHeight&& standingHeight) {
    const double length = std::hypot(to.x - from.x, to.z - from.z);
    if (!std::isfinite(length) || length > 2.0) return false;
    const int count = std::max(1, static_cast<int>(std::ceil(length / 0.1)));
    double eyeY = from.y;
    for (int step = 1; step <= count; ++step) {
        const double t = static_cast<double>(step) / count;
        auto next = standingHeight(from.x + (to.x - from.x) * t,
            from.z + (to.z - from.z) * t, eyeY);
        if (!next.has_value()) return false;
        eyeY = *next;
    }
    return std::abs(eyeY - to.y) <= 0.05;
}
} // namespace bedrock::area_fill
