#include <bedrock/world/WorldIterators.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace bedrock {
namespace {

int32_t signOf(double value) {
    return value > 0.0 ? 1 : (value < 0.0 ? -1 : 0);
}

double disabledAxis() {
    return std::numeric_limits<double>::max();
}

} // namespace

WorldBlockPosition WorldVec3::floored() const {
    return {
        static_cast<int32_t>(std::floor(x)),
        static_cast<int32_t>(std::floor(y)),
        static_cast<int32_t>(std::floor(z))
    };
}

WorldVec3 WorldVec3::offset(double dx, double dy, double dz) const {
    return {x + dx, y + dy, z + dz};
}

WorldVec3 WorldVec3::plus(const WorldVec3& other) const {
    return {x + other.x, y + other.y, z + other.z};
}

WorldVec3 WorldVec3::minus(const WorldVec3& other) const {
    return {x - other.x, y - other.y, z - other.z};
}

WorldVec3 WorldVec3::scaled(double scale) const {
    return {x * scale, y * scale, z * scale};
}

ManhattanIterator::ManhattanIterator(double x, double z, double maxDistance)
    : maxDistance_(static_cast<int64_t>(std::floor(maxDistance))),
      startX_(x),
      startZ_(z) {}

std::optional<WorldVec3> ManhattanIterator::next() {
    if (leg_ == -1) {
        leg_ = 0;
        return WorldVec3 {startX_, 0.0, startZ_};
    }
    if (leg_ == 0) {
        if (maxDistance_ <= 1) {
            return std::nullopt;
        }
        --x_;
        ++z_;
        if (x_ == 0) {
            leg_ = 1;
        }
    } else if (leg_ == 1) {
        --x_;
        --z_;
        if (z_ == 0) {
            leg_ = 2;
        }
    } else if (leg_ == 2) {
        ++x_;
        --z_;
        if (x_ == 0) {
            leg_ = 3;
        }
    } else if (leg_ == 3) {
        ++x_;
        ++z_;
        if (z_ == 0) {
            ++x_;
            leg_ = 0;
            ++layer_;
            if (layer_ == maxDistance_) {
                return std::nullopt;
            }
        }
    }
    return WorldVec3 {
        startX_ + static_cast<double>(x_),
        0.0,
        startZ_ + static_cast<double>(z_)
    };
}

OctahedronIterator::OctahedronIterator(WorldVec3 start, double maxDistance)
    : maxDistance_(maxDistance) {
    const auto floored = start.floored();
    start_ = {
        static_cast<double>(floored.x),
        static_cast<double>(floored.y),
        static_cast<double>(floored.z)
    };
}

std::optional<WorldVec3> OctahedronIterator::next() {
    if (static_cast<double>(apothem_) > maxDistance_) {
        return std::nullopt;
    }
    --right_;
    if (right_ < 0) {
        --left_;
        if (left_ < 0) {
            z_ += 2;
            if (z_ > 1) {
                y_ += 2;
                if (y_ > 1) {
                    x_ += 2;
                    if (x_ > 1) {
                        ++apothem_;
                        x_ = -1;
                    }
                    y_ = -1;
                }
                z_ = -1;
            }
            left_ = apothem_;
        }
        right_ = left_;
    }

    const int64_t x = x_ * right_;
    const int64_t y = y_ * (apothem_ - left_);
    const int64_t z = z_ * (apothem_ - (std::abs(x) + std::abs(y)));
    return start_.offset(
        static_cast<double>(x),
        static_cast<double>(y),
        static_cast<double>(z)
    );
}

SpiralIterator2d::SpiralIterator2d(WorldVec3 start, double maxDistance)
    : start_(start) {
    const double side = (std::floor(maxDistance) - 0.5) * 2.0;
    numberOfPoints_ = static_cast<int64_t>(std::floor(side * side));
}

std::optional<WorldVec3> SpiralIterator2d::next() {
    if (iteration_ >= numberOfPoints_) {
        return std::nullopt;
    }
    const WorldVec3 output = start_.offset(
        static_cast<double>(i_),
        0.0,
        static_cast<double>(j_)
    );

    i_ += di_;
    j_ += dj_;
    ++segmentPassed_;
    if (segmentPassed_ == segmentLength_) {
        segmentPassed_ = 0;
        const int64_t buffer = di_;
        di_ = -dj_;
        dj_ = buffer;
        if (dj_ == 0) {
            ++segmentLength_;
        }
    }
    ++iteration_;
    return output;
}

RaycastIterator::RaycastIterator(
    WorldVec3 pos,
    WorldVec3 direction,
    double maxDistance
) : pos_(pos),
    direction_(direction),
    maxDistance_(maxDistance) {
    const auto floored = pos.floored();
    block_ = {floored.x, floored.y, floored.z, BlockFace::Unknown};

    inverseDirectionX_ = direction.x == 0.0 ? disabledAxis() : 1.0 / direction.x;
    inverseDirectionY_ = direction.y == 0.0 ? disabledAxis() : 1.0 / direction.y;
    inverseDirectionZ_ = direction.z == 0.0 ? disabledAxis() : 1.0 / direction.z;
    stepX_ = signOf(direction.x);
    stepY_ = signOf(direction.y);
    stepZ_ = signOf(direction.z);
    deltaX_ = direction.x == 0.0 ? disabledAxis() : std::abs(1.0 / direction.x);
    deltaY_ = direction.y == 0.0 ? disabledAxis() : std::abs(1.0 / direction.y);
    deltaZ_ = direction.z == 0.0 ? disabledAxis() : std::abs(1.0 / direction.z);
    maxX_ = direction.x == 0.0
        ? disabledAxis()
        : std::abs((block_.x + (direction.x > 0.0 ? 1 : 0) - pos.x) / direction.x);
    maxY_ = direction.y == 0.0
        ? disabledAxis()
        : std::abs((block_.y + (direction.y > 0.0 ? 1 : 0) - pos.y) / direction.y);
    maxZ_ = direction.z == 0.0
        ? disabledAxis()
        : std::abs((block_.z + (direction.z > 0.0 ? 1 : 0) - pos.z) / direction.z);
}

std::optional<RaycastBlockStep> RaycastIterator::next() {
    if (std::min({maxX_, maxY_, maxZ_}) > maxDistance_) {
        return std::nullopt;
    }

    if (maxX_ < maxY_) {
        if (maxX_ < maxZ_) {
            block_.x += stepX_;
            maxX_ += deltaX_;
            block_.face = stepX_ > 0 ? BlockFace::West : BlockFace::East;
        } else {
            block_.z += stepZ_;
            maxZ_ += deltaZ_;
            block_.face = stepZ_ > 0 ? BlockFace::North : BlockFace::South;
        }
    } else if (maxY_ < maxZ_) {
        block_.y += stepY_;
        maxY_ += deltaY_;
        block_.face = stepY_ > 0 ? BlockFace::Bottom : BlockFace::Top;
    } else {
        block_.z += stepZ_;
        maxZ_ += deltaZ_;
        block_.face = stepZ_ > 0 ? BlockFace::North : BlockFace::South;
    }
    return block_;
}

std::optional<RaycastIntersection> RaycastIterator::intersect(
    const std::vector<BlockShape>& shapes,
    const WorldVec3& offset
) const {
    double nearest = disabledAxis();
    BlockFace nearestFace = BlockFace::Unknown;
    const WorldVec3 localPosition = pos_.minus(offset);

    for (const auto& shape : shapes) {
        double minimum =
            (shape[inverseDirectionX_ > 0.0 ? 0 : 3] - localPosition.x) *
            inverseDirectionX_;
        double maximum =
            (shape[inverseDirectionX_ > 0.0 ? 3 : 0] - localPosition.x) *
            inverseDirectionX_;
        const double minimumY =
            (shape[inverseDirectionY_ > 0.0 ? 1 : 4] - localPosition.y) *
            inverseDirectionY_;
        const double maximumY =
            (shape[inverseDirectionY_ > 0.0 ? 4 : 1] - localPosition.y) *
            inverseDirectionY_;
        BlockFace face = stepX_ > 0 ? BlockFace::West : BlockFace::East;

        if (minimum > maximumY || minimumY > maximum) {
            continue;
        }
        if (minimumY > minimum) {
            minimum = minimumY;
            face = stepY_ > 0 ? BlockFace::Bottom : BlockFace::Top;
        }
        if (maximumY < maximum) {
            maximum = maximumY;
        }

        const double minimumZ =
            (shape[inverseDirectionZ_ > 0.0 ? 2 : 5] - localPosition.z) *
            inverseDirectionZ_;
        const double maximumZ =
            (shape[inverseDirectionZ_ > 0.0 ? 5 : 2] - localPosition.z) *
            inverseDirectionZ_;
        if (minimum > maximumZ || minimumZ > maximum) {
            continue;
        }
        if (minimumZ > minimum) {
            minimum = minimumZ;
            face = stepZ_ > 0 ? BlockFace::North : BlockFace::South;
        }
        if (maximumZ < maximum) {
            maximum = maximumZ;
        }

        if (minimum < nearest) {
            nearest = minimum;
            nearestFace = face;
        }
    }

    if (nearest == disabledAxis()) {
        return std::nullopt;
    }
    return RaycastIntersection {
        pos_.plus(direction_.scaled(nearest)),
        nearestFace
    };
}

const WorldVec3& RaycastIterator::position() const {
    return pos_;
}

const WorldVec3& RaycastIterator::direction() const {
    return direction_;
}

} // namespace bedrock
