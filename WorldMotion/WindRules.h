#pragma once
#include <cmath>
// Pure wind rules for WorldMotion (shared with its tests).
namespace wind {
struct Offset {
    float x, y;
};
// Horizontal leaf displacement at world (x, y): two sine waves whose phase
// varies across the world so neighbouring trees do not move in lockstep.
inline Offset leaf(double x, double y, double time, float amplitude, float fade) {
    double phase = std::fmod(x * .035 + y * .025, 6.283185307179586),
           detail = std::fmod(x * .022 + y * .014, 6.283185307179586);
    return {amplitude * fade *
                (float)(.7 * std::sin(time * 1.7 + phase) + .3 * std::sin(time * 3.1 + detail)),
            amplitude * fade * .65f * (float)std::sin(time * 1.35 + phase + .8)};
}
// Slow cloud drift within +/- distance (x) and +/- 0.4 * distance (y).
inline Offset cloud(double time, float distance) {
    return {distance * (float)std::sin(time * .012), distance * .4f * (float)std::sin(time * .009)};
}
// Leaf block types (Steam build).
inline bool foliage(int type) {
    return type == 8 || type == 11;
}
// Solid non-leaf blocks hold neighbouring leaf vertices still.
inline bool anchorMaterial(int type) {
    return type != 0 && !foliage(type);
}
// Packs a vertex index with an "anchored" flag in the top bit.
inline unsigned windIndex(unsigned vertex, bool anchored) {
    return vertex | (anchored ? 0x80000000u : 0u);
}
inline unsigned windVertex(unsigned packed) {
    return packed & 0x7fffffffu;
}
// Animation time in seconds, quantised to ~30 steps per second.
inline double windTime(unsigned long long elapsed, float speed) {
    return (elapsed / 33) * .033 * speed;
}
} // namespace wind
