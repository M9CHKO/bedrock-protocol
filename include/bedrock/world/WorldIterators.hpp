#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace bedrock {

enum class BlockFace : int32_t {
    Unknown = -999,
    Bottom = 0,
    Top = 1,
    North = 2,
    South = 3,
    West = 4,
    East = 5
};

struct WorldBlockPosition {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

struct WorldVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    WorldBlockPosition floored() const;
    WorldVec3 offset(double dx, double dy, double dz) const;
    WorldVec3 plus(const WorldVec3& other) const;
    WorldVec3 minus(const WorldVec3& other) const;
    WorldVec3 scaled(double scale) const;
};

using BlockShape = std::array<double, 6>;
inline constexpr BlockShape FullBlockShape {0.0, 0.0, 0.0, 1.0, 1.0, 1.0};

struct RaycastBlockStep {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    BlockFace face = BlockFace::Unknown;
};

struct RaycastIntersection {
    WorldVec3 pos;
    BlockFace face = BlockFace::Unknown;
};

class ManhattanIterator {
public:
    ManhattanIterator(double x, double z, double maxDistance);

    std::optional<WorldVec3> next();

private:
    int64_t maxDistance_ = 0;
    double startX_ = 0.0;
    double startZ_ = 0.0;
    int64_t x_ = 2;
    int64_t z_ = -1;
    int64_t layer_ = 1;
    int32_t leg_ = -1;
};

// prismarine-world exported this misspelling historically.
using ManathanIterator = ManhattanIterator;

class OctahedronIterator {
public:
    OctahedronIterator(WorldVec3 start, double maxDistance);

    std::optional<WorldVec3> next();

private:
    WorldVec3 start_;
    double maxDistance_ = 0.0;
    int64_t apothem_ = 1;
    int64_t x_ = -1;
    int64_t y_ = -1;
    int64_t z_ = -1;
    int64_t left_ = 1;
    int64_t right_ = 2;
};

class SpiralIterator2d {
public:
    SpiralIterator2d(WorldVec3 start, double maxDistance);

    std::optional<WorldVec3> next();

private:
    WorldVec3 start_;
    int64_t numberOfPoints_ = 0;
    int64_t di_ = 1;
    int64_t dj_ = 0;
    int64_t segmentLength_ = 1;
    int64_t i_ = 0;
    int64_t j_ = 0;
    int64_t segmentPassed_ = 0;
    int64_t iteration_ = 0;
};

class RaycastIterator {
public:
    RaycastIterator(WorldVec3 pos, WorldVec3 direction, double maxDistance);

    std::optional<RaycastBlockStep> next();
    std::optional<RaycastIntersection> intersect(
        const std::vector<BlockShape>& shapes,
        const WorldVec3& offset
    ) const;

    const WorldVec3& position() const;
    const WorldVec3& direction() const;

private:
    RaycastBlockStep block_;
    WorldVec3 pos_;
    WorldVec3 direction_;
    double inverseDirectionX_ = 0.0;
    double inverseDirectionY_ = 0.0;
    double inverseDirectionZ_ = 0.0;
    int32_t stepX_ = 0;
    int32_t stepY_ = 0;
    int32_t stepZ_ = 0;
    double deltaX_ = 0.0;
    double deltaY_ = 0.0;
    double deltaZ_ = 0.0;
    double maxX_ = 0.0;
    double maxY_ = 0.0;
    double maxZ_ = 0.0;
    double maxDistance_ = 0.0;
};

} // namespace bedrock
