#include <bedrock/world/BedrockChunk.hpp>
#include <bedrock/world/WorldIterators.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

bool closeTo(double lhs, double rhs) {
    return std::abs(lhs - rhs) < 1e-9;
}

bool samePosition(const bedrock::WorldVec3& value, const std::array<double, 3>& expected) {
    return closeTo(value.x, expected[0]) &&
        closeTo(value.y, expected[1]) &&
        closeTo(value.z, expected[2]);
}

} // namespace

int main() {
    const std::vector<std::array<double, 3>> manhattanGolden {
        {10, 0, -3}, {11, 0, -3}, {10, 0, -2}, {9, 0, -3},
        {10, 0, -4}, {12, 0, -3}, {11, 0, -2}, {10, 0, -1},
        {9, 0, -2}, {8, 0, -3}, {9, 0, -4}, {10, 0, -5},
        {11, 0, -4}
    };
    bedrock::ManathanIterator manhattan(10, -3, 3);
    for (const auto& expected : manhattanGolden) {
        const auto value = manhattan.next();
        if (!value.has_value() || !samePosition(*value, expected)) {
            std::cerr << "[WORLD-ITERATORS-SMOKE] Manhattan Node sequence mismatch\n";
            return 1;
        }
    }
    if (manhattan.next().has_value()) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] Manhattan termination mismatch\n";
        return 1;
    }

    bedrock::OctahedronIterator octahedron({1.8, -2.2, 3.9}, 2);
    std::size_t octahedronCount = 0;
    std::optional<bedrock::WorldVec3> octahedronLast;
    const std::vector<std::array<double, 3>> octahedronPrefix {
        {0, -3, 3}, {1, -3, 2}, {1, -4, 3}, {0, -3, 3}
    };
    while (const auto value = octahedron.next()) {
        if (octahedronCount < octahedronPrefix.size() &&
            !samePosition(*value, octahedronPrefix[octahedronCount])) {
            std::cerr << "[WORLD-ITERATORS-SMOKE] Octahedron prefix mismatch\n";
            return 1;
        }
        octahedronLast = value;
        ++octahedronCount;
    }
    if (octahedronCount != 73 || !octahedronLast.has_value() ||
        !samePosition(*octahedronLast, {-2, -3, 3})) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] Octahedron Node sequence mismatch\n";
        return 1;
    }

    const std::vector<std::array<double, 3>> spiralGolden {
        {10, 5, -3}, {11, 5, -3}, {11, 5, -2}, {10, 5, -2}, {9, 5, -2},
        {9, 5, -3}, {9, 5, -4}, {10, 5, -4}, {11, 5, -4}, {12, 5, -4},
        {12, 5, -3}, {12, 5, -2}, {12, 5, -1}, {11, 5, -1}, {10, 5, -1},
        {9, 5, -1}, {8, 5, -1}, {8, 5, -2}, {8, 5, -3}, {8, 5, -4},
        {8, 5, -5}, {9, 5, -5}, {10, 5, -5}, {11, 5, -5}, {12, 5, -5}
    };
    bedrock::SpiralIterator2d spiral({10, 5, -3}, 3);
    for (const auto& expected : spiralGolden) {
        const auto value = spiral.next();
        if (!value.has_value() || !samePosition(*value, expected)) {
            std::cerr << "[WORLD-ITERATORS-SMOKE] spiral Node sequence mismatch\n";
            return 1;
        }
    }
    if (spiral.next().has_value()) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] spiral termination mismatch\n";
        return 1;
    }

    struct RayStepGolden {
        int32_t x;
        int32_t y;
        int32_t z;
        bedrock::BlockFace face;
    };
    const std::vector<RayStepGolden> rayGolden {
        {1, 0, 0, bedrock::BlockFace::West},
        {1, 1, 0, bedrock::BlockFace::Bottom},
        {2, 1, 0, bedrock::BlockFace::West},
        {2, 1, -1, bedrock::BlockFace::South},
        {3, 1, -1, bedrock::BlockFace::West},
        {3, 2, -1, bedrock::BlockFace::Bottom}
    };
    bedrock::RaycastIterator ray({0.5, 0.5, 0.5}, {1, 0.5, -0.25}, 3);
    for (const auto& expected : rayGolden) {
        const auto value = ray.next();
        if (!value.has_value() || value->x != expected.x || value->y != expected.y ||
            value->z != expected.z || value->face != expected.face) {
            std::cerr << "[WORLD-ITERATORS-SMOKE] ray traversal mismatch\n";
            return 1;
        }
    }
    if (ray.next().has_value()) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] ray termination mismatch\n";
        return 1;
    }

    bedrock::RaycastIterator intersectionRay({0.5, 0.5, 0.5}, {1, 0, 0}, 5);
    const auto intersection = intersectionRay.intersect(
        {bedrock::FullBlockShape},
        {2, 0, 0}
    );
    if (!intersection.has_value() || !samePosition(intersection->pos, {2, 0.5, 0.5}) ||
        intersection->face != bedrock::BlockFace::West ||
        intersectionRay.intersect({}, {2, 0, 0}).has_value()) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] ray shape intersection mismatch\n";
        return 1;
    }

    bedrock::BedrockChunkColumn column(0, 0);
    column.setBlockStateId({.x = 2, .y = 0, .z = 0}, 42);
    column.setBlockStateId({.x = 3, .y = 0, .z = 0}, 99);
    bedrock::BedrockWorld world;
    world.setLoadedColumn(0, 0, std::move(column));

    const auto firstHit = world.raycast({0.5, 0.5, 0.5}, {1, 0, 0}, 5);
    if (!firstHit.has_value() || firstHit->position.x != 2 || firstHit->stateId != 42 ||
        firstHit->face != bedrock::BlockFace::West ||
        !samePosition(firstHit->intersect, {2, 0.5, 0.5})) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] BedrockWorld default raycast mismatch\n";
        return 1;
    }

    const auto matchedHit = world.raycast(
        {0.5, 0.5, 0.5},
        {1, 0, 0},
        5,
        [](int32_t stateId, const bedrock::BlockPosition&) {
            return stateId == 99;
        },
        [](int32_t, const bedrock::BlockPosition&) {
            return std::vector<bedrock::BlockShape> {
                bedrock::BlockShape {0.25, 0, 0, 1, 1, 1}
            };
        }
    );
    if (!matchedHit.has_value() || matchedHit->position.x != 3 ||
        matchedHit->stateId != 99 || matchedHit->face != bedrock::BlockFace::West ||
        !samePosition(matchedHit->intersect, {3.25, 0.5, 0.5})) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] BedrockWorld filtered raycast mismatch\n";
        return 1;
    }

    bedrock::BedrockChunkColumn negativeColumn(-1, 0);
    negativeColumn.setBlockStateId({.x = -1, .y = 0, .z = 0}, 77);
    world.setLoadedColumn(-1, 0, std::move(negativeColumn));
    const auto negativeHit = world.raycast({0.5, 0.5, 0.5}, {-1, 0, 0}, 3);
    if (!negativeHit.has_value() || negativeHit->position.x != -1 ||
        negativeHit->stateId != 77 || negativeHit->face != bedrock::BlockFace::East ||
        !samePosition(negativeHit->intersect, {0, 0.5, 0.5})) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] negative-coordinate raycast mismatch\n";
        return 1;
    }

    bedrock::BedrockWorld emptyWorld;
    if (emptyWorld.raycast({0.5, 0.5, 0.5}, {1, 0, 0}, 3).has_value()) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] unloaded-column raycast mismatch\n";
        return 1;
    }

    bedrock::BedrockWorld eventWorld;
    eventWorld.setLoadedColumn(0, 0, bedrock::BedrockChunkColumn(0, 0));
    const bedrock::BlockPosition updatePosition {.x = 4, .y = 12, .z = 6};
    std::vector<int32_t> eventOrder;
    std::size_t globalUpdates = 0;
    std::size_t positionedUpdates = 0;
    std::size_t unrelatedUpdates = 0;
    bedrock::BedrockWorldBlockSnapshot lastOld;
    bedrock::BedrockWorldBlockSnapshot lastNew;
    eventWorld.onBlockUpdate([&](
        const bedrock::BedrockWorldBlockSnapshot& oldBlock,
        const bedrock::BedrockWorldBlockSnapshot& newBlock
    ) {
        ++globalUpdates;
        lastOld = oldBlock;
        lastNew = newBlock;
        eventOrder.push_back(1);
        if (eventWorld.getBlockStateId(newBlock.position) != newBlock.stateId) {
            throw std::runtime_error("block update fired before mutation");
        }
    });
    eventWorld.onBlockUpdate(updatePosition, [&](const auto&, const auto&) {
        ++positionedUpdates;
        eventOrder.push_back(2);
    });
    eventWorld.onBlockUpdate({.x = 5, .y = 12, .z = 6}, [&](const auto&, const auto&) {
        ++unrelatedUpdates;
    });

    eventWorld.setBlockStateId(updatePosition, 123);
    if (globalUpdates != 1 || positionedUpdates != 1 || unrelatedUpdates != 0 ||
        eventOrder != std::vector<int32_t>({1, 2}) || lastOld.stateId != 0 ||
        lastNew.stateId != 123 || lastNew.position.x != updatePosition.x) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] block state update event mismatch\n";
        return 1;
    }
    eventWorld.setBlockLight(updatePosition, 9);
    eventWorld.setSkyLight(updatePosition, 14);
    eventWorld.setBiomeId(updatePosition, 7);
    const auto eventBlock = eventWorld.getBlock(updatePosition);
    if (!eventBlock.has_value() || eventBlock->stateId != 123 ||
        eventBlock->blockLight != 9 || eventBlock->skyLight != 14 ||
        eventBlock->biomeId != 7 || globalUpdates != 4 || positionedUpdates != 4 ||
        unrelatedUpdates != 0 || eventOrder !=
            std::vector<int32_t>({1, 2, 1, 2, 1, 2, 1, 2}) ||
        emptyWorld.getBlock(updatePosition).has_value()) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] block metadata update event mismatch\n";
        return 1;
    }

    std::unordered_map<std::string, bedrock::BedrockChunkColumn> storedColumns;
    std::size_t generatedColumns = 0;
    std::size_t loadedColumns = 0;
    std::size_t savedColumns = 0;
    std::size_t doneSavingEvents = 0;
    std::size_t storageLoadEvents = 0;
    std::size_t storageUnloadEvents = 0;
    std::vector<int32_t> storageEventOrder;
    auto storageKey = [](int32_t x, int32_t z) {
        return std::to_string(x) + "," + std::to_string(z);
    };

    bedrock::BedrockWorldOptions storageOptions;
    storageOptions.chunkGenerator = [&](int32_t x, int32_t z) {
        ++generatedColumns;
        bedrock::BedrockChunkColumn generated(x, z);
        generated.setBlockStateId({.x = 1, .y = 0, .z = 1}, 50);
        return generated;
    };
    storageOptions.loadColumn = [&](int32_t x, int32_t z)
        -> std::optional<bedrock::BedrockChunkColumn> {
        ++loadedColumns;
        const auto it = storedColumns.find(storageKey(x, z));
        return it == storedColumns.end()
            ? std::nullopt
            : std::optional<bedrock::BedrockChunkColumn>(it->second);
    };
    storageOptions.saveColumn = [&] (
        int32_t x,
        int32_t z,
        const bedrock::BedrockChunkColumn& saved
    ) {
        ++savedColumns;
        storedColumns.insert_or_assign(storageKey(x, z), saved);
    };

    bedrock::BedrockWorld storageWorld(std::move(storageOptions));
    storageWorld.onColumnLoad([&](const auto&) {
        ++storageLoadEvents;
    });
    storageWorld.onColumnUnload([&](const auto&) {
        ++storageUnloadEvents;
        storageEventOrder.push_back(1);
    });
    storageWorld.onDoneSaving([&] {
        ++doneSavingEvents;
        storageEventOrder.push_back(2);
    });

    auto* generatedColumn = storageWorld.getColumn(1, 2);
    if (generatedColumn == nullptr || generatedColumns != 1 || loadedColumns != 1 ||
        storageLoadEvents != 1 || storageWorld.savingQueueSize() != 1 ||
        storageWorld.getBlockStateId({.x = 17, .y = 0, .z = 33}) != 50 ||
        storageWorld.getColumn(1, 2) != generatedColumn || generatedColumns != 1) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] generated column lifecycle mismatch\n";
        return 1;
    }
    storageWorld.waitSaving();
    if (savedColumns != 1 || doneSavingEvents != 1 ||
        storageWorld.savingQueueSize() != 0 || storedColumns.size() != 1) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] initial column saving mismatch\n";
        return 1;
    }

    storageWorld.unloadColumn(1, 2);
    if (storageWorld.hasColumn(1, 2) || storageUnloadEvents != 1) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] immediate column unload mismatch\n";
        return 1;
    }
    auto* loadedColumn = storageWorld.getColumnAt({.x = 17, .y = 0, .z = 33});
    if (loadedColumn == nullptr || loadedColumns != 2 || generatedColumns != 1 ||
        storageLoadEvents != 2 || storageWorld.savingQueueSize() != 0 ||
        loadedColumn->getBlockStateId({.x = 1, .y = 0, .z = 1}) != 50) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] stored column loading mismatch\n";
        return 1;
    }

    storageWorld.setBlockStateId({.x = 17, .y = 0, .z = 33}, 75);
    storageWorld.unloadColumn(1, 2);
    if (!storageWorld.hasColumn(1, 2) || storageWorld.savingQueueSize() != 1 ||
        storageWorld.unloadQueueSize() != 1 || storageUnloadEvents != 1) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] deferred column unload mismatch\n";
        return 1;
    }
    storageEventOrder.clear();
    storageWorld.saveNow();
    if (storageWorld.hasColumn(1, 2) || savedColumns != 2 || doneSavingEvents != 2 ||
        storageUnloadEvents != 2 || storageWorld.savingQueueSize() != 0 ||
        storageWorld.unloadQueueSize() != 0 ||
        storageEventOrder != std::vector<int32_t>({1, 2}) ||
        storedColumns.at("1,2").getBlockStateId({.x = 1, .y = 0, .z = 1}) != 75) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] save-before-unload ordering mismatch\n";
        return 1;
    }

    bedrock::BedrockChunkColumn manuallyLoaded(5, 5);
    storageWorld.setColumn(5, 5, std::move(manuallyLoaded), false);
    if (storageWorld.savingQueueSize() != 0) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] setColumn save=false mismatch\n";
        return 1;
    }
    storageWorld.queueSaving(5, 5);
    if (storageWorld.savingQueueSize() != 1) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] manual saving queue mismatch\n";
        return 1;
    }
    storageWorld.saveNow();
    if (storedColumns.find("5,5") == storedColumns.end()) {
        std::cerr << "[WORLD-ITERATORS-SMOKE] manual column saving mismatch\n";
        return 1;
    }

    std::cout << "[WORLD-ITERATORS-SMOKE] ok\n";
    return 0;
}
