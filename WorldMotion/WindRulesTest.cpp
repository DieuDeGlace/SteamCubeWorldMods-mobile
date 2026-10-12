#include "WindRules.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
int main() {
    for (unsigned vertex : {0u, 1u, 262143u})
        for (bool pinned : {false, true}) {
            auto packed = wind::windIndex(vertex, pinned);
            assert(wind::windVertex(packed) == vertex);
            assert(bool(packed & 0x80000000u) == pinned);
        }
    assert(wind::anchorMaterial(7) && wind::anchorMaterial(6) && !wind::anchorMaterial(8) &&
           !wind::anchorMaterial(11) && !wind::anchorMaterial(0));
    assert(wind::windTime(3300, 1) == wind::windTime(3320, 1));
    assert(wind::foliage(8) && wind::foliage(11));
    for (int type : {0, 1, 2, 4, 5, 6, 7, 10, 13})
        assert(!wind::foliage(type));
    for (int i = 0; i < 20000; i++) {
        double t = i * .073;
        auto a = wind::leaf(-102856630, -16965650, t, .35f, 1);
        auto b = wind::leaf(-102856630, -16965650, t + .016, .35f, 1);
        assert(std::abs(a.x) <= .35f && std::abs(a.y) <= .35f * .65f);
        assert(std::abs(a.x - b.x) < .02);
        auto c = wind::cloud(t, 128);
        auto d = wind::cloud(t + .016, 128);
        assert(std::abs(c.x) <= 128 && std::abs(c.y) <= 128 * .4f);
        assert(std::abs(c.x - d.x) < .03);
        auto zero = wind::leaf(1, 2, t, .35f, 0);
        assert(zero.x == 0 && zero.y == 0);
    }
    // Shared world coordinates on either side of negative chunk borders have identical motion.
    for (int zone = -100; zone < 100; zone++) {
        auto a = wind::leaf(double(zone) * 64 + 64, 100, 13, .16f, 1),
             b = wind::leaf(double(zone + 1) * 64, 100, 13, .16f, 1);
        assert(a.x == b.x && a.y == b.y);
    }
    puts("Leaf targeting, motion bounds, temporal continuity, fade and chunk continuity passed.");
}
