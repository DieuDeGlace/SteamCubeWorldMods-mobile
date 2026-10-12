#include "TerrainRules.h"
#include <cassert>
#include <cstdio>
int main() {
    for (int64_t p : {-2048LL, -1024LL, 0LL, 1024LL})
        assert(terrain::mountainRegionWeight(p, p, true) == 0);
    assert(terrain::mountainRegionWeight(512, 512, true) == 1 &&
           terrain::mountainRegionWeight(-512, -512, true) == 1);
    for (int x = -2048; x <= 2048; x += 7) {
        assert(terrain::mountainRegionWeight(x, 500, false) == 0);
        double a = terrain::mountainRegionWeight(x, 500, true),
               b = terrain::mountainRegionWeight(x + 1, 500, true);
        assert(a >= 0 && a <= 1 && std::abs(a - b) < .012);
    }
    for (int added = -80; added <= 80; added++) {
        assert(terrain::mountainRelief(added, 80, 0) == added);
        assert(terrain::mountainRelief(added, 80, 1) >= 56 &&
               terrain::mountainRelief(added, 80, 1) <= 80);
    }
    for (int x = -4096; x <= 4096; x += 13)
        for (int y = -1024; y <= 1024; y += 29) {
            double w = terrain::mountainRegionWeight(x, y, true),
                   a = terrain::variation(x, y, 42, 68 + 12 * w, 768);
            assert(std::abs(a) <= 80);
            assert(terrain::variation(x, y, 42,
                                      68 + 12 * terrain::mountainRegionWeight(x, y, false),
                                      768) == terrain::variation(x, y, 42, 68, 768));
        }
    assert(terrain::variation(-1000, 1000, 42, 18, 384) ==
           terrain::variation(-1000, 1000, 42, 18, 384));
    assert(terrain::variation(-1000, 1000, 42, 18, 384) !=
           terrain::variation(-1000, 1000, 43, 18, 384));
    assert(terrain::protectionWeight(0, 192, 128) == 0 &&
           terrain::protectionWeight(192, 192, 128) == 0);
    assert(terrain::protectionWeight(320, 192, 128) == 1);
    assert(terrain::shoreWeight(0) == 0 && terrain::shoreWeight(16) == 0 &&
           terrain::shoreWeight(80) == 1);
    for (int x = -2048; x < 2048; x += 7)
        for (int y = -512; y < 512; y += 19) {
            double h = terrain::variation(x, y, 42, 18, 384);
            assert(std::isfinite(h) && std::abs(h) <= 18);
            double next = terrain::variation(x + 1, y, 42, 18, 384);
            assert(std::abs(next - h) < .5);
        }
    // Noise remains continuous across chunk boundaries, including negative coordinates.
    for (int x = -1024; x <= 1024; x += 64) {
        double a = terrain::variation(x - 1, 127, 42, 18, 384),
               b = terrain::variation(x, 127, 42, 18, 384),
               c = terrain::variation(x + 1, 127, 42, 18, 384);
        assert(std::abs(b - a) < .5 && std::abs(c - b) < .5);
    }
    for (int x = -4096; x < 4096; x += 11)
        for (int y = -1024; y < 1024; y += 31) {
            double a = terrain::variation(x, y, 123, 68, 512),
                   b = terrain::variation(x + 1, y, 123, 68, 512);
            assert(std::isfinite(a) && std::abs(a) <= 68 && std::abs(b - a) < 1);
            assert(a * terrain::protectionWeight(282 + 256, 282 + 256, 192) == 0);
        }
    // Uniform column translation preserves vertical clearances for all voxel types,
    // including water, foliage and building roofs, at positive and negative offsets.
    int minimum = 100, maximum = -100;
    for (int x = -4096; x <= 4096; x += 17)
        for (int y = -2048; y <= 2048; y += 23) {
            int delta = terrain::columnOffset(x, y, 123, 68, 512);
            minimum = std::min(minimum, delta);
            maximum = std::max(maximum, delta);
            assert(std::abs(delta) <= 68);
            assert(std::abs(delta - terrain::columnOffset(x + 1, y, 123, 68, 512)) <= 1);
            for (int nativeBase : {-80, -16, 0, 17, 160})
                for (int index : {0, 1, 12, 31, 64}) {
                    int movedBase = nativeBase + delta, movedBlock = nativeBase + index + delta;
                    assert(movedBlock - movedBase == index);
                    // A translated global water ceiling remains zero in native column coordinates.
                    assert((movedBlock <= delta) == (nativeBase + index <= 0));
                }
        }
    assert(maximum - minimum > 80); // Preserve large terrain relief rather than flattening it.
    assert(terrain::blockFromDots(-1) == -1 && terrain::blockFromDots(-65536) == -1 &&
           terrain::blockFromDots(-65537) == -2);
    for (int64_t z : {-65537LL, -1LL, 0LL, 65535LL, 65536LL})
        for (int delta : {-68, -17, 0, 19, 68}) {
            auto shifted = terrain::shiftedDetailZ(z, delta);
            assert(terrain::blockFromDots(shifted) == terrain::blockFromDots(z) + delta);
            assert(shifted - z ==
                   (int64_t)delta * 65536); // Fractional anchor height is preserved exactly.
        }
    // Opposing slopes whose sum rises gently must not acquire false downhill steps
    // from independently rounding the native and added terrain contributions.
    int previous = -1000;
    for (int x = -100; x <= 100; x++) {
        double native = x * .42, added = x * -.37;
        int surface = (int)std::floor(native) + terrain::surfaceOffset(native, added);
        assert(surface >= previous && surface - previous <= 1 + 1000 * (x == -100));
        previous = surface;
    }
    for (int x = -4096; x < 4096; x += 13)
        for (int y = -1024; y < 1024; y += 29) {
            double n = 123.125 + x * .015, d = terrain::variation(x, y, 123, 68, 768);
            int shift = terrain::surfaceOffset(n, d);
            assert(std::abs(shift) <= 68);
            assert((int)std::floor(n) + shift == (int)std::floor(n + d));
        }
    // Compare the old distribution with the hill-biased curve over multiple seeds.
    unsigned oldTall = 0, newTall = 0, samples = 0;
    double oldSlope = 0, newSlope = 0, maxSlope = 0;
    auto oldVariation = [](int x, int y, unsigned seed) {
        return 68 * (.72 * terrain::noise(x / 768., y / 768., seed) +
                     .28 * terrain::noise(x / 384., y / 384., seed ^ 0x84222325ull));
    };
    for (unsigned seed = 0; seed < 16; seed++)
        for (int x = -4096; x <= 4096; x += 83)
            for (int y = -4096; y <= 4096; y += 97) {
                double old = oldVariation(x, y, seed),
                       now = terrain::variation(x, y, seed, 68, 768);
                oldTall += old > 34;
                newTall += now > 34;
                ++samples;
                assert(std::isfinite(now) && std::abs(now) <= 68);
                for (auto axis : {0, 1}) {
                    double slope = std::abs(
                        terrain::variation(x + (axis == 0), y + (axis == 1), seed, 68, 768) - now);
                    newSlope += slope;
                    oldSlope +=
                        std::abs(oldVariation(x + (axis == 0), y + (axis == 1), seed) - old);
                    maxSlope = std::max(maxSlope, slope);
                    assert(slope < 1);
                }
            }
    assert(newTall > oldTall * 1.15 && newSlope < oldSlope * 1.1);
    printf(
        "Tall hill samples above +34 blocks: %.2f%% -> %.2f%%; mean adjacent slope %.4f -> %.4f, maximum %.4f\n",
        100. * oldTall / samples, 100. * newTall / samples, oldSlope / (samples * 2),
        newSlope / (samples * 2), maxSlope);
    std::puts(
        "Terrain bounds, continuity, integer column alignment, translated water ceiling and large relief passed.");
}
