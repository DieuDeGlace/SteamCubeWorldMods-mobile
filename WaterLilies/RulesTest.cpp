#include "PlacementRules.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    using namespace lilies;
    static_assert(offsetof(Decoration, x) == 8 && offsetof(Decoration, scale) == 0x20 &&
                  offsetof(Decoration, flags) == 0x38);
    assert(block(-1) == -1 && block(-Dots) == -1 && block(Dots - 1) == 0);
    for (int zx : {-8192, -1, 0, 8192})
        for (int zy : {-8192, -1, 0, 8192}) {
            auto first = candidates(17, zx, zy, 100, 25);
            auto repeat = candidates(17, zx, zy, 100, 25);
            unsigned enabled = 0, flowerCount = 0;
            for (unsigned i = 0; i < first.size(); ++i) {
                const auto &p = first[i];
                assert(p.x == repeat[i].x && p.y == repeat[i].y && p.model == repeat[i].model &&
                       p.scale == repeat[i].scale && p.yaw == repeat[i].yaw);
                assert(block(p.x) - int64_t(zx) * 64 >= 1 && block(p.x) - int64_t(zx) * 64 < 63);
                assert(block(p.y) - int64_t(zy) * 64 >= 1 && block(p.y) - int64_t(zy) * 64 < 63);
                assert(p.scale >= .055f && p.scale <= .075f && p.yaw >= 0 && p.yaw < 360);
                assert(lily(p.model));
                enabled += p.enabled;
                flowerCount += p.enabled && p.model == FlowerModel;
            }
            assert(enabled >= 24 && enabled <= 40);
            assert(flowerCount < enabled);
        }
    for (auto p : candidates(17, 0, 0, 0, 25))
        assert(!p.enabled);
    for (auto p : candidates(17, 0, 0, 100, 0))
        assert(p.model == PadModel);
    for (auto p : candidates(17, 0, 0, 100, 100))
        assert(p.model == FlowerModel);
    auto one = candidates(17, 0, 0, 100, 25), other = candidates(18, 0, 0, 100, 25);
    assert(one[0].x != other[0].x || one[0].y != other[0].y);
    for (int biome = 0; biome <= 13; ++biome)
        assert(inlandBiome(biome) == (biome != 1 && biome != 6 && biome != 7 && biome != 11 &&
                                      biome != 12 && biome != 13));
    assert(!inlandBiome(-1) && !inlandBiome(14));

    int top = -1000;
    for (int water : {-68, 0, 68}) {
        auto column = [water](int z) { return z <= water - 4 ? 1 : z <= water ? 2 : 0; };
        assert(findSurface(water - 4, 10, top, column) && top == water);
        assert(footprint(top, [&](int, int, int z) { return column(z); }));
        assert(!footprint(top, [&](int x, int, int z) {
            return x == 1 && z == water ? 1 : column(z); // Bank clips one edge.
        }));
        assert(!footprint(top, [&](int x, int, int z) {
            return column(z - (x == 1 ? 1 : 0)); // Neighbor's water is a step higher.
        }));
        assert(!footprint(top, [&](int, int y, int z) {
            return y == 1 && z == water + 3 ? 6 : column(z); // Low bridge.
        }));
    }
    assert(!findSurface(-30, 10, top, [](int z) { return z <= -30 ? 1 : z <= 0 ? 2 : 0; }));
    assert(!findSurface(-4, 10, top, [](int z) { return z <= -4 ? 1 : z <= 0 ? 19 : 0; }));
    assert(!findSurface(-4, 10, top, [](int) { return 0; }));
    // The column cache returns the same surface with each height looked up once.
    for (int water : {-68, 0, 68})
        for (unsigned maxDepth : {1u, 10u, 16u}) {
            auto column = [water](int z) { return z <= water - 4 ? 1 : z <= water ? 2 : 0; };
            unsigned lookups = 0;
            std::vector<int> seen;
            auto counted = [&](int z) {
                ++lookups;
                seen.push_back(z);
                return column(z);
            };
            int plainTop = 0, cachedTop = 0;
            bool plain = findSurface(water - 4, maxDepth, plainTop, column);
            CachedColumn<decltype(counted)> cached(counted, water - 4 - 2 - int(maxDepth));
            bool memo = findSurface(water - 4, maxDepth, cachedTop, cached);
            assert(plain == memo && (!plain || plainTop == cachedTop));
            for (size_t i = 0; i < seen.size(); ++i)
                for (size_t j = i + 1; j < seen.size(); ++j)
                    assert(seen[i] != seen[j]); // Every height queried at most once.
            assert(lookups <= 2 * maxDepth + 8);
        }

    // Run the production population loop against a relocating vector. Repeated
    // mesh builds preserve other decorations and cannot add the same pads twice.
    std::vector<Decoration> scenery(1);
    scenery[0].model = 35;
    scenery[0].x = 123456;
    const auto original = scenery[0];
    auto occupied = [&](const Candidate &p) {
        for (const auto &existing : scenery)
            if (overlaps(existing, p))
                return true;
        return false;
    };
    auto locate = [](const Candidate &, int &z) {
        z = 68;
        return true;
    };
    auto append = [&](const Decoration &p) {
        scenery.push_back(p);
        return true;
    };
    auto added = populate(one, 18, occupied, locate, append);
    assert(added > 0 && added <= 18 && scenery.size() == added + 1);
    for (unsigned repeat = 0; repeat < 20; ++repeat)
        assert(populate(one, 18, occupied, locate, append) == 0);
    // A cached chunk skips the loop. Even a cache reset is idempotent when the
    // same uncapped plan is replayed against every existing native lily.
    auto rest = populate(one, MaximumPads, occupied, locate, append);
    assert(rest <= MaximumPads);
    auto settled = scenery.size();
    for (unsigned repeat = 0; repeat < 20; ++repeat)
        assert(populate(one, MaximumPads, occupied, locate, append) == 0);
    assert(scenery.size() == settled);
    assert(std::memcmp(&scenery[0], &original, sizeof(original)) == 0);
    for (size_t i = 1; i < scenery.size(); ++i) {
        auto baseOffset = scenery[i].model == PadModel ? int64_t(scenery[i].scale * Dots) : 0;
        assert(scenery[i].z + baseOffset == 69 * Dots + int64_t(.01 * Dots));
        assert(scenery[i].flags == 2);
        for (size_t j = i + 1; j < scenery.size(); ++j) {
            Candidate p;
            p.x = scenery[j].x;
            p.y = scenery[j].y;
            assert(!overlaps(scenery[i], p));
        }
    }
    assert(populate(one, 0, occupied, locate, append) == 0);
    assert(populate(
               other, 1000, [](const Candidate &) { return false; }, locate,
               [](const Decoration &) { return false; }) == 0);
    puts("Stable native scenery plans, freshwater/clearance checks, caps and deduplication: PASS");
}
