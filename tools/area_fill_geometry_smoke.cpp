#include <bedrock/relay/AreaFillGeometry.hpp>
#include <iostream>
#include <optional>
using namespace bedrock::area_fill;
int main() {
    bool ok = true;
    auto check = [&](bool value) { if (!value) { ok = false; std::cerr << "area-fill geometry failed\n"; } };
    check(intersectsPlayer({0.85, 2.62, 0.5}, {1, 1, 0}));
    check(!intersectsPlayer({0.69, 2.62, 0.5}, {1, 1, 0}));
    check(!intersectsPlayer({0.5, 2.62, 0.5}, {0, 0, 0}));
    check(intersectsPlayer({-1008897.15, 2.62, -223773.5}, {-1008897, 1, -223774}));
    check(std::abs(localInput(1, 0, 0).x - 1) < 1e-8);
    check(std::abs(localInput(0, 1, 90).x - 1) < 1e-8);
    check(std::abs(localInput(-1, 0, 90).z - 1) < 1e-8);
    check(localInput(0, 0, 0).z == 0);
    auto flat = [](double, double, double) { return std::optional<double>(2.62); };
    check(safeSegment({0.5,2.62,0.5}, {1.5,2.62,0.5}, flat));
    check(!safeSegment({0.5,2.62,0.5}, {1.5,2.62,0.5},
        [](double x, double, double) -> std::optional<double> {
            if (x > 0.8 && x < 1.2) return std::nullopt;
            return 2.62;
        }));
    check(!safeSegment({0.5,2.62,0.5}, {5.5,2.62,0.5}, flat));
    // The actual player's reach is different from the centre of their cell.
    check(distanceSquared({0.01,2.62,0.5}, {3.5,1.5,0.5}) > 9);
    return ok ? 0 : 1;
}
