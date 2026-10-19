#pragma once
// Pure colour rules for DualHair, shared by the mod and its rule test.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>

namespace dualhair {
struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb &o) const {
        return r == o.r && g == o.g && b == o.b;
    }
};

// Gradient shape: the second colour begins `start` of the way down the hair
// and is fully reached after `blend` more (both 0-1; blend 0 = hard split).
struct Shape {
    float start = .4f, blend = .35f;
};

// Accepts "R,G,B", "R G B" (0-255 each) or "#RRGGBB"/"RRGGBB".
inline bool parseColor(const wchar_t *text, Rgb &out) {
    while (*text && iswspace(*text))
        ++text;
    if (*text == L'#')
        ++text;
    size_t hexDigits = 0;
    while (iswxdigit(text[hexDigits]))
        ++hexDigits;
    if (hexDigits == 6 && (text[6] == 0 || iswspace(text[6]))) {
        auto value = wcstoul(std::wstring(text, 6).c_str(), nullptr, 16);
        out = {uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
        return true;
    }
    int parts[3];
    for (int i = 0; i < 3; i++) {
        while (*text && (iswspace(*text) || *text == L','))
            ++text;
        if (!iswdigit(*text))
            return false;
        wchar_t *end = nullptr;
        long value = wcstol(text, &end, 10);
        if (value < 0 || value > 255)
            return false;
        parts[i] = (int)value;
        text = end;
    }
    while (*text && iswspace(*text))
        ++text;
    if (*text)
        return false;
    out = {uint8_t(parts[0]), uint8_t(parts[1]), uint8_t(parts[2])};
    return true;
}

// A saved look: "R,G,B" or "#RRGGBB", optionally followed by ",START,BLEND"
// percentages. Missing percentages keep `shape` (the defaults).
inline bool parseLook(const wchar_t *text, Rgb &color, Shape &shape) {
    std::wstring s(text);
    size_t cut = std::wstring::npos;
    int commas = 0;
    bool hex = s.find(L'#') != std::wstring::npos;
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == L',' && ++commas == (hex ? 1 : 3)) {
            cut = i;
            break;
        }
    if (!parseColor(s.substr(0, cut).c_str(), color))
        return false;
    if (cut == std::wstring::npos)
        return true;
    int start = -1, blend = -1;
    if (swscanf_s(s.c_str() + cut + 1, L"%d , %d", &start, &blend) != 2 || start < 0 ||
        start > 100 || blend < 0 || blend > 100)
        return false;
    shape = {start * .01f, blend * .01f};
    return true;
}

// Share of the second colour at `depth` (0 = top of the hair, 1 = lowest voxel).
inline float secondShare(float depth, const Shape &shape) {
    if (shape.blend <= 0)
        return depth >= shape.start ? 1.f : 0.f;
    float s = std::clamp((depth - shape.start) / shape.blend, 0.f, 1.f);
    return s * s * (3 - 2 * s);
}

// A hair voxel shaded by the gradient. The native draw multiplies the whole
// model by the hair colour; the mod removes that tint for a recoloured model,
// so the first colour is baked in here too and the top matches vanilla.
// Results never become empty (0,0,0) or a pure marker colour (a single 255
// channel with the others 0), which the native mesher treats specially.
inline Rgb shadeVoxel(Rgb voxel, Rgb first, Rgb second, float share) {
    auto mix = [&](uint8_t v, uint8_t a, uint8_t b) {
        float tint = (a + (b - a) * share) / 255.f;
        return (int)std::lround(v * tint);
    };
    int c[3] = {mix(voxel.r, first.r, second.r), mix(voxel.g, first.g, second.g),
                mix(voxel.b, first.b, second.b)};
    for (int &x : c)
        x = std::clamp(x, 1, 254);
    return {uint8_t(c[0]), uint8_t(c[1]), uint8_t(c[2])};
}

// ---------------------------------------------------------------------------
// Multiplayer packing. The appearance block is sent raw (all 0xB0 bytes), and
// the renderer never reads the hair colour's alpha byte or the two padding
// bytes after it, so those three bytes carry the second colour:
//   byte 0: 0b10 tag, 3-bit start, 3-bit blend (eighths of 7)
//   bytes 1-2: second colour as RGB565 (little endian)
// Anything without the tag (native alpha, other mods) means "one colour".
// ---------------------------------------------------------------------------
inline int quantize7(float v) {
    return (int)std::lround(std::clamp(v, 0.f, 1.f) * 7);
}
inline void pack(Rgb second, const Shape &s, uint8_t out[3]) {
    out[0] = uint8_t(0x80 | quantize7(s.start) << 3 | quantize7(s.blend));
    uint16_t c = uint16_t((second.r >> 3) << 11 | (second.g >> 2) << 5 | second.b >> 3);
    out[1] = uint8_t(c), out[2] = uint8_t(c >> 8);
}
inline bool unpack(const uint8_t in[3], Rgb &second, Shape &s) {
    if ((in[0] & 0xc0) != 0x80)
        return false;
    s.start = ((in[0] >> 3) & 7) / 7.f;
    s.blend = (in[0] & 7) / 7.f;
    uint16_t c = uint16_t(in[1] | in[2] << 8);
    auto expand = [](int v, int bits) { return uint8_t((v << (8 - bits)) | (v >> (2 * bits - 8))); };
    second = {expand(c >> 11, 5), expand((c >> 5) & 63, 6), expand(c & 31, 5)};
    return true;
}

// The native character-creation hair palette: 16 columns (15 hues and grey),
// 7 rows (three darker, the pure colour, three lighter), 17-unit cells.
constexpr int PALETTE_COLUMNS = 16, PALETTE_ROWS = 7;
inline Rgb paletteColor(int column, int row) {
    static const float bases[PALETTE_COLUMNS][3] = {
        {1, 0, 0},     {1, .25f, 0},  {1, .5f, 0},  {1, 1, 0},     {.5f, 1, 0},   {0, 1, 0},
        {0, 1, .5f},   {0, 1, 1},     {0, .5f, 1},  {0, .25f, 1},  {0, 0, 1},     {.25f, 0, 1},
        {.5f, 0, 1},   {.75f, 0, 1},  {1, 0, 1},    {.5f, .5f, .5f}};
    float c[3];
    for (int i = 0; i < 3; i++) {
        c[i] = bases[column][i];
        if (row < 3)
            c[i] *= row * .25f + .2f;
        else
            c[i] += (1 - c[i]) * (row - 3) * .25f;
    }
    return {uint8_t(c[0] * 255), uint8_t(c[1] * 255), uint8_t(c[2] * 255)};
}

inline bool emptyVoxel(Rgb v) {
    return v.r == 0 && v.g == 0 && v.b == 0;
}
// Pure red, green and blue voxels are markers the native mesher does not draw
// as hair (for example the face cut-out); they must stay exactly as they are.
inline bool markerVoxel(Rgb v) {
    return (v == Rgb{255, 0, 0}) || (v == Rgb{0, 255, 0}) || (v == Rgb{0, 0, 255});
}

// Removes the native hair tint from a final model colour. A zero hair channel
// leaves no information, so it uses the mean of the recoverable channels.
inline void untint(float color[3], Rgb hair) {
    const uint8_t h[3] = {hair.r, hair.g, hair.b};
    float sum = 0;
    int known = 0;
    for (int i = 0; i < 3; i++)
        if (h[i]) {
            color[i] *= 255.f / h[i];
            sum += color[i];
            known++;
        }
    float fallback = known ? sum / known : 1.f;
    for (int i = 0; i < 3; i++)
        if (!h[i])
            color[i] = fallback;
}
} // namespace dualhair
