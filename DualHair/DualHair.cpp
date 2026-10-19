// DualHair: two-tone hair (Omega style) for player characters.
//
// Rendering: the native creature renderer queues the hair model at one call
// site with a material colour of base * hair colour. For a two-tone character
// this mod queues a recoloured copy of that hair model instead: the copy is
// made with the game's own Sprite copy constructor, its voxels get a
// top-to-bottom gradient from the character's hair colour into the second
// colour, and the game's own mesher rebuilds it. The hair tint is divided out
// of the queued colour, so the top of the hair matches vanilla. Everything
// after the queue (lighting, shadows, TAA, other mods) sees an ordinary native
// model draw.
//
// GUI: a "Hair color 2" board drawn with the native widget text/rectangle
// calls beside the character-creation panel (in game: /hair2). Multiplayer: the look is packed into three appearance bytes that the
// game sends raw and never renders (see DualHairRules.h).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "DualHairRules.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dualhair {
// Native addresses (RVAs) in the inspected Steam build.
constexpr size_t HAIR_QUEUE_CALL = 0x2813a; // call QueueModel for the hair sprite
constexpr size_t QUEUE_MODEL = 0xee2f0;
constexpr size_t SPRITE_COPY = 0x16e790; // Sprite::Sprite(const Sprite &)
constexpr size_t SPRITE_MESH = 0x170700; // Sprite::BuildMesh(this, 0); replaces its buffers
constexpr size_t SPRITE_VTABLE = 0x44e790;
constexpr size_t VOXEL_MARKER = 0x16f230; // bool(void *, const Rgb *, bool primaries off)
constexpr size_t GAME_NEW = 0x392bac; // operator new used by the native scalar deleter
constexpr size_t SPRITE_SIZE = 0xa8;
// Sprite fields: voxel colours (3 bytes each, x fastest, then y, then z) and size.
constexpr size_t SPRITE_VOXELS = 0x68, SPRITE_SIZE_X = 0x88, SPRITE_SIZE_Y = 0x8c,
                 SPRITE_SIZE_Z = 0x90, SPRITE_GFX = 0x60;
// Creature: Appearance::hair_color (RGBA bytes), then two padding bytes. The
// native creature update reads the whole 0xB0-byte appearance raw (0x1390a4).
constexpr size_t CREATURE_HAIR_COLOR = 0x7a, CREATURE_SPARE = 0x7d;
constexpr size_t GAME_POINTER = 0x551a80, CHAT_PRINT = 0x26bf10;
// Widgets: class vtables (Draw is slot 1), native draw/size/mouse helpers and
// BaseWidget text state (font size, outline, fill/outline colour, alignment).
constexpr size_t STYLE_WIDGET_VTABLE = 0x46dbe8, STYLE_WIDGET_DRAW = 0x271030;
constexpr size_t WIDGET_SIZE = 0x32a110, WIDGET_MOUSE = 0x32b180, WIDGET_TEXT = 0x269210;
constexpr size_t WIDGET_RENDERER = 0x8, WIDGET_FONT = 0x1a8, WIDGET_NODE = 0x178;
constexpr size_t TEXT_STATE = 0x1b0, TEXT_STATE_SIZE = 0x38;
constexpr size_t STYLE_CURRENT_HAIR = 0x254; // CharacterStyleWidget current hair colour
// A clone unused this long cannot be in a pending frame queue and is freed.
constexpr ULONGLONG RETIRE_MS = 10000;
constexpr size_t CACHE_LIMIT = 64;
static const char PENDING[] = "_NewCharacter"; // Look chosen at character creation.

using QueueModel = void (*)(void *, void *, void *, float *, float, float, float, float, float);
using SpriteCopy = void *(*)(void *, void *);
using SpriteMesh = void (*)(void *, int);
using GameNew = void *(*)(size_t);
using WidgetDraw = void (*)(void *);

static const wchar_t *INI = L".\\Mods\\DualHair.ini";
static bool enabled = true, network = true;
static Shape defaults;
static float offsetX = 0, offsetY = 0;
static void *creatureSlot = nullptr; // Filled by the stub: r14 (renderer state; [r14] = creature).

static void log(const char *text) {
    FILE *f = nullptr;
    fopen_s(&f, "Mods\\DualHair.log", "a");
    if (f) {
        fprintf(f, "%s\n", text);
        fclose(f);
    }
}
static unsigned char *base() {
    return (unsigned char *)GetModuleHandleW(nullptr);
}
template <typename T> static T field(void *object, size_t offset) {
    T value;
    memcpy(&value, (unsigned char *)object + offset, sizeof(T));
    return value;
}

static bool supported() {
    auto b = base();
    auto dos = (IMAGE_DOS_HEADER *)b;
    if (!b || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    auto nt = (IMAGE_NT_HEADERS64 *)(b + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == 0x5d93ab0d &&
           (nt->OptionalHeader.SizeOfImage == 0x5b7000 || // PC (SteamStub .bind section)
            nt->OptionalHeader.SizeOfImage == 0x58a000);  // GameNative (section stripped)
}
static cube::Game *game() {
    return *(cube::Game **)(base() + GAME_POINTER);
}
static cube::Creature *player() {
    auto g = game();
    return g && g->world ? g->world->local_creature : nullptr;
}
static std::string nameOf(cube::Creature *c) {
    char name[17]{};
    memcpy(name, c->entity_data.name, 16);
    return name;
}
static std::wstring wide(const std::string &s) {
    return std::wstring(s.begin(), s.end());
}

// ---------------------------------------------------------------------------
// Settings: per-character looks, keyed by character name.
// ---------------------------------------------------------------------------
struct Look {
    Rgb second;
    Shape shape;
};
struct Entry {
    std::string name;
    Look look;
};
static std::vector<Entry> characters;
static bool pendingThisSession = false;

static void loadSettings() {
    enabled = GetPrivateProfileIntW(L"DualHair", L"Enabled", 1, INI) != 0;
    network = GetPrivateProfileIntW(L"DualHair", L"Multiplayer", 1, INI) != 0;
    defaults.start =
        std::clamp((int)GetPrivateProfileIntW(L"DualHair", L"StartPercent", 40, INI), 0, 100) * .01f;
    defaults.blend =
        std::clamp((int)GetPrivateProfileIntW(L"DualHair", L"BlendPercent", 35, INI), 0, 100) * .01f;
    offsetX = (float)(int)GetPrivateProfileIntW(L"DualHair", L"PanelOffsetX", 0, INI);
    offsetY = (float)(int)GetPrivateProfileIntW(L"DualHair", L"PanelOffsetY", 0, INI);
    characters.clear();
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetPrivateProfileSectionW(L"Characters", buffer.data(), (DWORD)buffer.size(), INI);
    for (const wchar_t *line = buffer.data(); line < buffer.data() + length && *line;
         line += wcslen(line) + 1) {
        const wchar_t *equals = wcschr(line, L'=');
        if (!equals || *line == L';')
            continue;
        Look look{{}, defaults};
        if (!parseLook(equals + 1, look.second, look.shape))
            continue;
        std::string name;
        for (const wchar_t *c = line; c < equals; ++c)
            name += (char)*c;
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (!name.empty())
            characters.push_back({name, look});
    }
}
static const Look *lookFor(const std::string &name) {
    for (auto &e : characters)
        if (_stricmp(e.name.c_str(), name.c_str()) == 0)
            return &e.look;
    return nullptr;
}
static void saveLook(const std::string &name, const Look *look) {
    wchar_t value[48];
    if (look)
        swprintf_s(value, L"%d,%d,%d,%d,%d", look->second.r, look->second.g, look->second.b,
                   (int)std::lround(look->shape.start * 100), (int)std::lround(look->shape.blend * 100));
    WritePrivateProfileStringW(L"Characters", wide(name).c_str(), look ? value : nullptr, INI);
    loadSettings();
}

// ---------------------------------------------------------------------------
// Recoloured hair models.
// ---------------------------------------------------------------------------
struct Clone {
    void *original, *originalVoxels, *gfx;
    int size[3];
    Rgb first;
    Look look;
    void *sprite;
    ULONGLONG used;
};
static std::vector<Clone> clones;

// Voxels the native mesher treats specially (empty, or a marker colour with the
// sprite's own marker setting) keep their exact colour and do not count as hair.
static bool specialVoxel(void *sprite, Rgb v) {
    using Marker = bool (*)(void *, const uint8_t *, bool);
    const uint8_t bytes[3] = {v.r, v.g, v.b};
    return emptyVoxel(v) || markerVoxel(v) ||
           ((Marker)(base() + VOXEL_MARKER))(nullptr, bytes, field<bool>(sprite, 0x99));
}

static void recolor(void *sprite, Rgb first, const Look &look) {
    int sx = field<int>(sprite, SPRITE_SIZE_X), sy = field<int>(sprite, SPRITE_SIZE_Y),
        sz = field<int>(sprite, SPRITE_SIZE_Z);
    auto voxels = field<uint8_t *>(sprite, SPRITE_VOXELS);
    int top = -1, bottom = sz;
    for (int z = 0; z < sz; z++)
        for (int i = 0; i < sx * sy; i++) {
            auto v = voxels + 3 * ((size_t)z * sx * sy + i);
            if (!specialVoxel(sprite, {v[0], v[1], v[2]})) {
                top = std::max(top, z);
                bottom = std::min(bottom, z);
            }
        }
    if (top < 0)
        return;
    float span = (float)std::max(1, top - bottom);
    for (int z = 0; z < sz; z++) {
        float share = secondShare((top - z) / span, look.shape);
        for (int i = 0; i < sx * sy; i++) {
            auto v = voxels + 3 * ((size_t)z * sx * sy + i);
            Rgb voxel{v[0], v[1], v[2]};
            if (specialVoxel(sprite, voxel))
                continue;
            auto out = shadeVoxel(voxel, first, look.second, share);
            v[0] = out.r, v[1] = out.g, v[2] = out.b;
        }
    }
}

static void destroySprite(void *sprite) {
    // Native scalar deleting destructor: frees the buffers, voxels and object.
    auto vtable = *(void (***)(void *, int))sprite;
    vtable[0](sprite, 1);
}

static bool sameLook(const Look &a, const Look &b) {
    return a.second == b.second && a.shape.start == b.shape.start && a.shape.blend == b.shape.blend;
}

static void *cloneFor(void *original, Rgb first, const Look &look) {
    auto voxels = field<void *>(original, SPRITE_VOXELS);
    auto gfx = field<void *>(original, SPRITE_GFX);
    int size[3] = {field<int>(original, SPRITE_SIZE_X), field<int>(original, SPRITE_SIZE_Y),
                   field<int>(original, SPRITE_SIZE_Z)};
    if (*(void **)original != base() + SPRITE_VTABLE || !voxels || !gfx || size[0] <= 0 ||
        size[1] <= 0 || size[2] <= 0 || size[0] * size[1] * size[2] > 256 * 256 * 256)
        return nullptr;
    auto now = GetTickCount64();
    for (auto &c : clones)
        if (c.original == original && c.originalVoxels == voxels && c.gfx == gfx &&
            !memcmp(c.size, size, sizeof size) && c.first == first && sameLook(c.look, look)) {
            c.used = now;
            return c.sprite;
        }
    // Only clones idle long enough to be outside any pending draw queue are freed.
    for (size_t i = 0; i < clones.size();)
        if (now - clones[i].used > RETIRE_MS) {
            destroySprite(clones[i].sprite);
            clones.erase(clones.begin() + i);
        } else {
            ++i;
        }
    if (clones.size() >= CACHE_LIMIT)
        return nullptr;
    auto memory = ((GameNew)(base() + GAME_NEW))(SPRITE_SIZE);
    if (!memory)
        return nullptr;
    auto sprite = ((SpriteCopy)(base() + SPRITE_COPY))(memory, original);
    if (!field<void *>(sprite, SPRITE_VOXELS)) {
        destroySprite(sprite);
        return nullptr;
    }
    recolor(sprite, first, look);
    ((SpriteMesh)(base() + SPRITE_MESH))(sprite, 0);
    clones.push_back({original, voxels, gfx, {size[0], size[1], size[2]}, first, look, sprite, now});
    return sprite;
}

// ---------------------------------------------------------------------------
// Which look a creature wears.
// ---------------------------------------------------------------------------
static ULONGLONG creationSeen = 0; // Last frame the creation panel was drawn.
static bool creationActive() {
    return GetTickCount64() - creationSeen < 500;
}
static Look creationLook{{40, 110, 200}, {.4f, .35f}};
static bool creationOn = false;

static bool lookOf(cube::Creature *creature, Look &out) {
    if (creature->entity_data.hostility_type != 0) // players only
        return false;
    auto name = nameOf(creature);
    if (name.empty()) {
        // The creation preview has no name yet.
        if (creationActive() && creationOn) {
            out = creationLook;
            return true;
        }
        return false;
    }
    if (creature == player() || !network) {
        auto look = lookFor(name);
        if (look)
            out = *look;
        return look != nullptr;
    }
    // Other players: their packed look, else a local ini entry for that name.
    auto spare = (uint8_t *)creature + CREATURE_SPARE;
    if (unpack(spare, out.second, out.shape))
        return true;
    if (auto look = lookFor(name)) {
        out = *look;
        return true;
    }
    return false;
}

// Replaces the native hair queue call (same arguments; rdx = hair sprite,
// r9 = final material colour).
static void queueHair(void *queue, void *sprite, void *matrix, float *color, float a, float b,
                      float c, float d, float e) {
    auto original = (QueueModel)(base() + QUEUE_MODEL);
    void *state = creatureSlot;
    auto creature = state ? *(cube::Creature **)state : nullptr;
    Look look;
    if (enabled && sprite && color && creature && lookOf(creature, look)) {
        auto hair = (uint8_t *)creature + CREATURE_HAIR_COLOR;
        Rgb first{hair[0], hair[1], hair[2]};
        if (auto recolored = cloneFor(sprite, first, look)) {
            float untinted[4] = {color[0], color[1], color[2], color[3]};
            untint(untinted, first);
            original(queue, recolored, matrix, untinted, a, b, c, d, e);
            return;
        }
    }
    original(queue, sprite, matrix, color, a, b, c, d, e);
}

// Keeps the local player's packed look current (sent with native appearance
// updates) and adopts a look chosen during character creation.
static uint8_t nativeSpare[3];
static bool nativeSpareKnown = false;
static void tick() {
    auto self = player();
    if (!self)
        return;
    auto name = nameOf(self);
    if (name.empty())
        return;
    if (pendingThisSession && !lookFor(name)) {
        if (auto pending = lookFor(PENDING)) {
            Look adopted = *pending;
            saveLook(name, &adopted);
            saveLook(PENDING, nullptr);
        }
        pendingThisSession = false;
    }
    auto spare = (uint8_t *)self + CREATURE_SPARE;
    Rgb unused;
    Shape unusedShape;
    if (!unpack(spare, unused, unusedShape)) {
        memcpy(nativeSpare, spare, 3);
        nativeSpareKnown = true;
    }
    auto look = lookFor(name);
    if (look && network && enabled) {
        uint8_t packed[3];
        pack(look->second, look->shape, packed);
        memcpy(spare, packed, 3);
    } else if (nativeSpareKnown && unpack(spare, unused, unusedShape)) {
        memcpy(spare, nativeSpare, 3);
    }
}

// ---------------------------------------------------------------------------
// GUI: the second colour board.
// ---------------------------------------------------------------------------
struct V2 {
    float x, y;
};
enum Action { NONE, TOGGLE, START_DOWN, START_UP, BLEND_DOWN, BLEND_UP, CELL };
struct Hit {
    float x0, y0, x1, y1;
    Action action;
    int arg;
};
struct PanelState {
    void *widget = nullptr;
    std::vector<Hit> hits;
    int hovered = -1;
    ULONGLONG drawn = 0;
};
static PanelState panel;
static bool mouseWasDown = false;
static bool gameInFront() {
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

static V2 widgetOrigin(void *widget) {
    auto node = field<unsigned char *>(widget, WIDGET_NODE);
    auto transformation = node ? field<unsigned char *>(node, 0x60) : nullptr;
    if (!transformation)
        return {0, 0};
    int index = field<int>(transformation, 0x244);
    auto positions = field<V2 *>(transformation, 0x278);
    return positions ? positions[index] : V2{0, 0};
}
static void fill(void *widget, V2 origin, float x, float y, float w, float h, const float rgba[4]) {
    auto renderer = field<void *>(widget, WIDGET_RENDERER);
    if (!renderer)
        return;
    using Rect = void (*)(void *, V2 *, V2 *, const float *);
    V2 position{origin.x + x, origin.y + y}, size{w, h};
    (*(Rect **)renderer)[6](renderer, &position, &size, rgba);
}
static void fill(void *widget, V2 origin, float x, float y, float w, float h, Rgb c) {
    float rgba[4] = {c.r / 255.f, c.g / 255.f, c.b / 255.f, 1};
    fill(widget, origin, x, y, w, h, rgba);
}
static void text(void *widget, const wchar_t *s, float x, float y, int align) {
    if (!field<void *>(widget, WIDGET_FONT))
        return;
    auto w = (unsigned char *)widget;
    const float size = 12, outline = 3, white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 1};
    memcpy(w + 0x1b0, &size, 4);
    memcpy(w + 0x1b4, &outline, 4);
    memcpy(w + 0x1c4, white, 16);
    memcpy(w + 0x1d4, black, 16);
    memcpy(w + 0x1e4, &align, 4);
    using Text = void (*)(void *, V2 *, std::wstring *, float, float);
    std::wstring str(s);
    V2 out{};
    ((Text)(base() + WIDGET_TEXT))(widget, &out, &str, x, y);
}
// Native-style arrow: a small white triangle with a black edge.
static void arrow(void *widget, V2 o, float x, float y, bool right) {
    static const Rgb black{0, 0, 0}, white{255, 255, 255};
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < 5; k++) {
            float column = right ? x + k * 2 : x + (4 - k) * 2;
            float half = (float)(5 - k);
            float grow = pass == 0 ? 1.f : 0.f;
            fill(widget, o, column - grow, y - half - grow, 2 + 2 * grow, 2 * half + 2 * grow,
                 pass == 0 ? black : white);
        }
}
static void frame(void *widget, V2 o, float x, float y, float w, float h) {
    static const Rgb black{0, 0, 0}, white{255, 255, 255};
    // Black edge, white 2-unit border, notched corners like the native panels.
    fill(widget, o, x + 2, y, w - 4, 4, black);
    fill(widget, o, x + 2, y + h - 4, w - 4, 4, black);
    fill(widget, o, x, y + 2, 4, h - 4, black);
    fill(widget, o, x + w - 4, y + 2, 4, h - 4, black);
    fill(widget, o, x + 3, y + 1, w - 6, 2, white);
    fill(widget, o, x + 3, y + h - 3, w - 6, 2, white);
    fill(widget, o, x + 1, y + 3, 2, h - 6, white);
    fill(widget, o, x + w - 3, y + 3, 2, h - 6, white);
}

constexpr float PANEL_W = 300, PANEL_H = 330;

// The Hair color 2 board beside the character creation style panel. (In game
// the look is changed with /hair2; the board is not drawn on the inventory.)
static void drawPanel(void *widget) {
    V2 o = widgetOrigin(widget);
    V2 size{}, mouse{};
    using Size = V2 *(*)(void *, V2 *);
    ((Size)(base() + WIDGET_SIZE))(widget, &size);
    ((Size)(base() + WIDGET_MOUSE))(widget, &mouse);
    float px = size.x + 10 + offsetX;
    float py = size.y - PANEL_H + offsetY;

    // The look being edited.
    Look look = creationLook;
    bool on = creationOn;
    Rgb first = field<Rgb>(widget, STYLE_CURRENT_HAIR);

    auto saved = std::vector<unsigned char>(TEXT_STATE_SIZE);
    memcpy(saved.data(), (unsigned char *)widget + TEXT_STATE, TEXT_STATE_SIZE);
    std::vector<Hit> hits;
    auto hit = [&](float x, float y, float w, float h, Action a, int arg = 0) {
        hits.push_back({px + x, py + y, px + x + w, py + y + h, a, arg});
    };
    frame(widget, o, px, py, PANEL_W, PANEL_H);
    {
        float row = 27.f;
        auto option = [&](const wchar_t *label, const std::wstring &value, Action down, Action up) {
            text(widget, label, px + 15, py + row, 0);
            text(widget, value.c_str(), px + 205, py + row, 1);
            arrow(widget, o, px + 125, py + row - 1, false);
            arrow(widget, o, px + 276, py + row - 1, true);
            hit(115, row - 12, 30, 24, down);
            hit(266, row - 12, 30, 24, up);
            row += 30;
        };
        auto percent = [](float v) { return std::to_wstring((int)std::lround(v * 100)) + L"%"; };
        option(L"TWO TONE", on ? L"ON" : L"OFF", TOGGLE, TOGGLE);
        option(L"FADE AT", percent(look.shape.start), START_DOWN, START_UP);
        option(L"FADE SIZE", percent(look.shape.blend), BLEND_DOWN, BLEND_UP);
        // Preview strip: first colour at the top of the hair, fading down.
        text(widget, L"PREVIEW", px + 15, py + row, 0);
        for (int i = 0; i < 16; i++) {
            float share = on ? secondShare(i / 15.f, look.shape) : 0;
            Rgb c{uint8_t(first.r + (look.second.r - first.r) * share),
                  uint8_t(first.g + (look.second.g - first.g) * share),
                  uint8_t(first.b + (look.second.b - first.b) * share)};
            fill(widget, o, px + 112 + i * 11, py + row - 7, 11, 14, c);
        }
        text(widget, L"HAIR COLOR 2", px + PANEL_W * .5f, py + 182, 1);
        for (int column = 0; column < PALETTE_COLUMNS; column++)
            for (int r = 0; r < PALETTE_ROWS; r++) {
                float x = px + 13 + column * 17.f, y = py + 197 + r * 17.f;
                Rgb c = paletteColor(column, r);
                bool chosen = on && c == look.second;
                fill(widget, o, x, y, 17, 17, chosen ? Rgb{255, 255, 255} : Rgb{0, 0, 0});
                fill(widget, o, x + (chosen ? 2.f : 1.f), y + (chosen ? 2.f : 1.f),
                     chosen ? 13.f : 15.f, chosen ? 13.f : 15.f, c);
                hit(13 + column * 17.f, 197 + r * 17.f, 17, 17, CELL, column * 8 + r);
            }
    }
    memcpy((unsigned char *)widget + TEXT_STATE, saved.data(), TEXT_STATE_SIZE);

    int hovered = -1;
    for (size_t i = 0; i < hits.size(); i++)
        if (mouse.x >= hits[i].x0 && mouse.x < hits[i].x1 && mouse.y >= hits[i].y0 &&
            mouse.y < hits[i].y1)
            hovered = (int)i;
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (down && !mouseWasDown && hovered >= 0 && gameInFront()) {
        auto h = hits[hovered];
        auto step = [](float &v, float d) { v = std::clamp(std::round((v + d) * 20) / 20, 0.f, 1.f); };
        switch (h.action) {
        case TOGGLE: on = !on; break;
        case START_DOWN: step(look.shape.start, -.05f); break;
        case START_UP: step(look.shape.start, .05f); break;
        case BLEND_DOWN: step(look.shape.blend, -.05f); break;
        case BLEND_UP: step(look.shape.blend, .05f); break;
        case CELL:
            look.second = paletteColor(h.arg / 8, h.arg % 8);
            on = true;
            break;
        default: break;
        }
        creationLook = look;
        creationOn = on;
        saveLook(PENDING, on ? &look : nullptr);
        pendingThisSession = true;
    }
    mouseWasDown = down;
    panel.widget = widget;
    panel.hits = std::move(hits);
    panel.hovered = hovered;
    panel.drawn = GetTickCount64();
    creationSeen = panel.drawn;
}
static bool hoveringPanel() {
    return panel.hovered >= 0 && GetTickCount64() - panel.drawn < 250;
}

static WidgetDraw styleDraw = nullptr;
static void drawStyleWidget(void *widget) {
    styleDraw(widget);
    if (enabled)
        drawPanel(widget);
}
static bool hookDraw(size_t vtable, size_t expected, void *hook, WidgetDraw &original) {
    auto slot = (void **)(base() + vtable) + 1;
    if (*slot != base() + expected)
        return false; // another mod replaced it; leave that panel alone
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return false;
    original = (WidgetDraw)*slot;
    InterlockedExchangePointer(slot, hook);
    VirtualProtect(slot, sizeof(void *), old, &old);
    return true;
}

// ---------------------------------------------------------------------------
// Hair queue hook: a call-site stub near the module saves r14 for queueHair.
// ---------------------------------------------------------------------------
static bool install() {
    auto site = base() + HAIR_QUEUE_CALL;
    // movss xmm6,[rbp+0x2cc8]; movss [rsp+0x20],xmm6; call QueueModel
    static const unsigned char expected[] = {0xf3, 0x0f, 0x10, 0xb5, 0xc8, 0x2c, 0x00,
                                             0x00, 0xf3, 0x0f, 0x11, 0x74, 0x24, 0x20,
                                             0xe8, 0xb1, 0x61, 0x0c, 0x00};
    static const unsigned char hairSprite[] = {0x49, 0x8b, 0x96, 0xe0, 0x00, 0x00, 0x00};
    static const unsigned char copyPrologue[] = {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x4b, 0x08, 0x55};
    static const unsigned char meshPrologue[] = {0x48, 0x8b, 0xc4, 0x55, 0x56, 0x57, 0x41, 0x54};
    static const unsigned char newPrologue[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b};
    static const unsigned char markerPrologue[] = {0x45, 0x33, 0xc9, 0x4c, 0x8d, 0x1d};
    if (memcmp(site - 14, expected, sizeof expected) ||
        memcmp(base() + 0x280f2, hairSprite, sizeof hairSprite) ||
        memcmp(base() + SPRITE_COPY, copyPrologue, sizeof copyPrologue) ||
        memcmp(base() + SPRITE_MESH, meshPrologue, sizeof meshPrologue) ||
        memcmp(base() + GAME_NEW, newPrologue, sizeof newPrologue) ||
        memcmp(base() + VOXEL_MARKER, markerPrologue, sizeof markerPrologue)) {
        log("Native hair draw does not match the inspected build (or another mod changed it); "
            "DualHair is off.");
        return false;
    }
    // Find executable memory within rel32 range of the call site.
    unsigned char *stub = nullptr;
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    auto granularity = (uintptr_t)info.dwAllocationGranularity;
    for (uintptr_t distance = granularity; distance < 0x70000000 && !stub; distance += granularity)
        for (int sign : {-1, 1}) {
            auto at = ((uintptr_t)site + sign * (intptr_t)distance) & ~(granularity - 1);
            stub = (unsigned char *)VirtualAlloc((void *)at, 4096, MEM_RESERVE | MEM_COMMIT,
                                                 PAGE_EXECUTE_READWRITE);
            if (stub)
                break;
        }
    if (!stub) {
        log("No memory near the game for the hook stub; DualHair is off.");
        return false;
    }
    unsigned char code[] = {
        0x49, 0xbb, 0, 0, 0, 0, 0, 0, 0, 0, // mov r11, &creatureSlot
        0x4d, 0x89, 0x33,                   // mov [r11], r14
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, queueHair
        0xff, 0xe0,                         // jmp rax
    };
    auto slot = &creatureSlot;
    auto target = &queueHair;
    memcpy(code + 2, &slot, 8);
    memcpy(code + 15, &target, 8);
    memcpy(stub, code, sizeof code);
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof code);
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
        return false;
    int32_t rel = (int32_t)(stub - (site + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

// ---------------------------------------------------------------------------
// Chat: /hair2 R G B | #RRGGBB | off | start N | blend N
// ---------------------------------------------------------------------------
static void say(const std::wstring &text) {
    auto g = game();
    if (!g || !g->gui.chat_widget)
        return;
    using Print = void (*)(void *, std::wstring *, FloatRGBA *);
    std::wstring line = text + L"\n";
    FloatRGBA color(.85f, .75f, 1.f, 1.f);
    ((Print)(base() + CHAT_PRINT))(g->gui.chat_widget, &line, &color);
}
static bool chat(const std::wstring &message) {
    if (_wcsnicmp(message.c_str(), L"/hair2", 6) != 0 || (message.size() > 6 && message[6] != L' '))
        return false;
    auto self = player();
    if (!self)
        return true;
    auto name = nameOf(self);
    std::wstring args = message.size() > 7 ? message.substr(7) : L"";
    while (!args.empty() && args.back() == L' ')
        args.pop_back();
    auto current = lookFor(name);
    Look look = current ? *current : Look{{40, 110, 200}, defaults};
    wchar_t textBuffer[200];
    if (args.empty()) {
        if (current)
            swprintf_s(textBuffer,
                       L"DualHair: %ls has second hair colour %d,%d,%d (start %d%%, blend %d%%).",
                       wide(name).c_str(), look.second.r, look.second.g, look.second.b,
                       (int)std::lround(look.shape.start * 100), (int)std::lround(look.shape.blend * 100));
        else
            swprintf_s(textBuffer, L"DualHair: off for %ls. Use /hair2 R G B (or the Hair color 2 "
                                   L"board at character creation).",
                       wide(name).c_str());
        say(textBuffer);
        return true;
    }
    if (_wcsicmp(args.c_str(), L"off") == 0) {
        saveLook(name, nullptr);
        say(L"DualHair: second hair colour removed for " + wide(name) + L".");
        return true;
    }
    bool start = _wcsnicmp(args.c_str(), L"start ", 6) == 0,
         blend = _wcsnicmp(args.c_str(), L"blend ", 6) == 0;
    if (start || blend) {
        float v = std::clamp(_wtoi(args.c_str() + 6), 0, 100) * .01f;
        (start ? look.shape.start : look.shape.blend) = v;
        saveLook(name, &look);
        say(L"DualHair: updated for " + wide(name) + L".");
        return true;
    }
    if (!parseColor(args.c_str(), look.second)) {
        say(L"DualHair: use /hair2 R G B (0-255), /hair2 #RRGGBB, /hair2 off, "
            L"/hair2 start N or /hair2 blend N.");
        return true;
    }
    saveLook(name, &look);
    swprintf_s(textBuffer, L"DualHair: second hair colour for %ls set to %d,%d,%d.",
               wide(name).c_str(), look.second.r, look.second.g, look.second.b);
    say(textBuffer);
    return true;
}
} // namespace dualhair

class DualHair : public GenericMod {
  public:
    void Initialize() override {
        if (!dualhair::supported()) {
            dualhair::log("Unsupported Cube World executable; DualHair is off.");
            return;
        }
        dualhair::loadSettings();
        if (!dualhair::install())
            return;
        bool style = dualhair::hookDraw(dualhair::STYLE_WIDGET_VTABLE, dualhair::STYLE_WIDGET_DRAW,
                                        (void *)&dualhair::drawStyleWidget, dualhair::styleDraw);
        char line[200];
        sprintf_s(line, "DualHair attached to the native hair draw (creation board %s).",
                  style ? "on" : "unavailable");
        dualhair::log(line);
    }
    void OnGameTick(cube::Game *) override {
        dualhair::tick();
    }
    int OnChat(std::wstring *message) override {
        return message && dualhair::chat(*message) ? 1 : 0;
    }
    // Clicks on the board are not passed to the game.
    void OnGetMouseState(DIMOUSESTATE *mouse) override {
        if (mouse && dualhair::hoveringPanel())
            mouse->rgbButtons[0] = 0;
    }
    int OnWindowProc(HWND, UINT message, WPARAM, LPARAM) override {
        bool click = message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
                     message == WM_LBUTTONDBLCLK;
        return click && dualhair::hoveringPanel() ? 1 : 0;
    }
};
static_assert(sizeof(GenericMod) == 0x428);
EXPORT GenericMod *MakeMod() {
    return new DualHair();
}
EXPORT void ModPreInitialize() {}
EXPORT int ModMajorVersion() {
    return 7;
}
EXPORT int ModMinorVersion() {
    return 3;
}
