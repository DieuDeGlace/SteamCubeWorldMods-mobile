// TerrainVariance: adds gentle large-scale hills (and taller relief inside
// native "Mountain" locations) to Cube World's terrain without regenerating it.
//
// How it works:
//   1. The native height function is hooked so every height query includes our
//      per-column integer offset (from TerrainRules.h).
//   2. After a zone is generated natively, translate() shifts each column's
//      voxel storage up or down by that offset, plus sprite/spawn anchors.
//   3. The native block getter is hooked so lookups inside a translated column
//      still report water/air/filler relative to the shifted column.
// A few exported Audit*/Snapshot* functions are read-only diagnostics.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "TerrainRules.h"
#include "../NativeScene.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace terrainmod {

// --- Native addresses (RVAs in the supported cubeworld.exe) -----------------
constexpr uintptr_t HEIGHT_RVA = 0x2b4450;       // hooked: terrain height
constexpr uintptr_t GENERATE_RVA = 0x2ff6f0;     // hooked: zone generation
constexpr uintptr_t LOOKUP_RVA = 0xeb90;         // hooked: Block* (Zone*, Block*, x, y, z)
constexpr uintptr_t REGION_AT_RVA = 0x2ada20;    // void (World*, IntVector2* out, x, y)
constexpr uintptr_t BIOME_AT_RVA = 0x2b9080;     // int (World*, regionX, regionY)
constexpr uintptr_t LOCATION_AT_RVA = 0x35ce0;   // void (World*, x, y, Location* out)
constexpr uintptr_t GROUND_HEIGHT_RVA = 0x35ea0; // float (World*, x, y, mode)
constexpr uintptr_t BUILD_BOUNDS_RVA = 0x31d470; // void (Zone*): collision bounds, effect origins
constexpr uintptr_t BUILD_OCCUPANCY_RVA = 0x31d700; // void (Zone*): object occupancy, spatial keys
constexpr uintptr_t FILLER_BLOCK_RVA = 0x54dcb0;    // Block returned below a column
constexpr uintptr_t AIR_BLOCK_RVA = 0x54dcb8;       // Block returned above the water ceiling
constexpr uintptr_t WATER_BLOCK_RVA = 0x54dcc0;     // Block returned at/below the water ceiling
constexpr uintptr_t LOCATION_JUMP_TABLE_RVA = 0x16b7dc;
constexpr uint32_t MOUNTAIN_NAME_BRANCH = 0x16b454; // Branch that resolves the name "Mountain".
constexpr int LOCATION_TYPE_COUNT = 47;

constexpr int ZONE_SIZE = 64;            // Columns per zone side.
constexpr unsigned char DRY_AIR = 2;     // Block::breakable bit: air that should not become water.
constexpr unsigned char OCCUPIED = 0x40; // Block::breakable bit: object occupancy mark.
constexpr const wchar_t *INI = L".\\Mods\\TerrainVariance.ini";

static uintptr_t base;
static bool installed = false;
static double amplitude = 68, scale = 768, mountainAmplitude = 80;
static int mountainLocationType = 0;
// Re-entrancy counters: while non-zero our height hook defers to native code.
static thread_local unsigned generating = 0, recursion = 0;

struct Location {
    int type;
    unsigned char ocean, pad[3];
    int subtype, radius, x, y;
    float distanceSquared, influence;
};
using Height = float (*)(cube::World *, int, int, float, float, float, float, float, float, float,
                         float, Location *, int);
using Generate = void (*)(cube::Zone *);
using Lookup = cube::Block *(*)(cube::Zone *, cube::Block *, int, int, int);
static Height originalHeight;
static Generate originalGenerate;
static Lookup originalLookup;

// Per-zone column offsets, indexed [localX * 64 + localY].
struct Columns {
    std::array<int8_t, ZONE_SIZE * ZONE_SIZE> delta{};
};
static std::unordered_map<cube::Zone *, std::shared_ptr<Columns>> columns;
static std::shared_mutex columnsMutex;
static std::atomic<uint64_t> epoch{1}, chunks{0};
static std::atomic<uint64_t> details{0};

// Increments a counter for the lifetime of the scope.
struct ScopedCount {
    unsigned &count;
    explicit ScopedCount(unsigned &c) : count(c) {
        ++count;
    }
    ~ScopedCount() {
        --count;
    }
};

template <class Function> static Function nativeCall(uintptr_t rva) {
    return (Function)(base + rva);
}

static void log(const char *s) {
    FILE *f = nullptr;
    fopen_s(&f, "Mods\\TerrainVariance.log", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
}

// Converts world block coordinates to a column index within `zone`.
// Returns false when the column belongs to another zone.
static bool localColumn(const cube::Zone *zone, int64_t x, int64_t y, int64_t &lx, int64_t &ly) {
    lx = x - (int64_t)zone->position.x * ZONE_SIZE;
    ly = y - (int64_t)zone->position.y * ZONE_SIZE;
    return lx >= 0 && lx < ZONE_SIZE && ly >= 0 && ly < ZONE_SIZE;
}

// Integer offset for column (x, y) given its native surface height.
static int fromSurface(cube::World *w, int64_t x, int64_t y, float native) {
    if (!w || !std::isfinite(native) || x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX)
        return 0;
    // Use native biome ownership rather than temperature or the player's region.
    IntVector2 region{};
    nativeCall<void (*)(cube::World *, IntVector2 *, int, int)>(REGION_AT_RVA)(w, &region, (int)x,
                                                                               (int)y);
    int biome = nativeCall<int (*)(cube::World *, int, int)>(BIOME_AT_RVA)(w, region.x, region.y);
    double weight =
        terrain::mountainRegionWeight(x, y, biome == (int)cube::Zone::RegionType::Mountains);
    double limit = amplitude + (mountainAmplitude - amplitude) * weight;
    double added = terrain::variation(x, y, (uint32_t)w->seed, limit, scale);
    Location location{};
    nativeCall<void (*)(cube::World *, int, int, Location *)>(LOCATION_AT_RVA)(w, (int)x, (int)y,
                                                                               &location);
    if (mountainLocationType && location.type == mountainLocationType && location.influence > 0)
        added = terrain::mountainRelief(added, mountainAmplitude, location.influence);
    return terrain::landOffset(native, added);
}

// Offset for a column outside the zone being processed. Special height modes
// share the normal ground mode's integer translation, so query the native
// ground height (mode 0) with our height hook suppressed.
static int displacement(cube::World *w, int64_t x, int64_t y) {
    if (!w)
        return 0;
    ScopedCount guard(generating);
    float native = nativeCall<float (*)(cube::World *, int, int, int)>(GROUND_HEIGHT_RVA)(
        w, (int)x, (int)y, 0);
    return fromSurface(w, x, y, native);
}

// Returns the offsets recorded for `zone`, or null if it was not translated.
static const Columns *offsets(cube::Zone *zone) {
    // Nine neighbors serve mesh sampling. Epoch changes prevent recycled zone
    // addresses from reusing stale offsets; shared ownership covers destruction.
    struct Entry {
        cube::Zone *zone = nullptr;
        std::shared_ptr<Columns> data;
    };
    static thread_local std::array<Entry, 9> cache;
    static thread_local uint64_t seen = 0;
    static thread_local unsigned next = 0;
    static thread_local unsigned last = 0;
    uint64_t current = epoch.load(std::memory_order_acquire);
    if (current != seen) {
        for (auto &e : cache) {
            e.zone = nullptr;
            e.data.reset();
        }
        seen = current;
    }
    if (cache[last].zone == zone)
        return cache[last].data.get();
    for (unsigned i = 0; i < cache.size(); i++)
        if (cache[i].zone == zone) {
            last = i;
            return cache[i].data.get();
        }
    std::shared_lock lock(columnsMutex);
    auto found = columns.find(zone);
    auto data = found == columns.end() ? std::shared_ptr<Columns>{} : found->second;
    cache[next] = {zone, data};
    last = next;
    next = (next + 1) % cache.size();
    return cache[last].data.get();
}

// --- Hook replacements ------------------------------------------------------

static float height(cube::World *w, int x, int y, float a, float b, float c, float d, float e,
                    float f, float g, float h, Location *l, int flags) {
    if (generating || recursion)
        return originalHeight(w, x, y, a, b, c, d, e, f, g, h, l, flags);
    ScopedCount guard(recursion);
    float native = originalHeight(w, x, y, a, b, c, d, e, f, g, h, l, flags);
    if (!std::isfinite(native))
        return native;
    return native + (flags == 0 ? fromSurface(w, x, y, native) : displacement(w, x, y));
}

static void generate(cube::Zone *zone) {
    ScopedCount guard(generating);
    originalGenerate(zone); // Complete native water, objects and structures before translating.
}

static cube::Block *lookup(cube::Zone *zone, cube::Block *out, int x, int y, int z) {
    if (generating || !zone)
        return originalLookup(zone, out, x, y, z);
    int64_t lx, ly;
    if (!localColumn(zone, x, y, lx, ly))
        return originalLookup(zone, out, x, y, z);
    auto data = offsets(zone);
    if (!data)
        return originalLookup(zone, out, x, y, z);
    auto &field = zone->fields[lx * ZONE_SIZE + ly];
    int ceiling = data->delta[lx * ZONE_SIZE + ly];
    // Match the verified native getter, including solid filler and dry-air bit 2.
    // Implicit water uses this column's translated sea level instead of global zero.
    if (z < field.base_z) {
        memcpy(out, (void *)(base + FILLER_BLOCK_RVA), sizeof(*out));
        return out;
    }
    int64_t index = (int64_t)z - field.base_z;
    if (index >= (int64_t)field.blocks.size())
        memcpy(out, (void *)(base + (z > ceiling ? AIR_BLOCK_RVA : WATER_BLOCK_RVA)), sizeof(*out));
    else {
        *out = field.blocks[(size_t)index];
        if (out->type == cube::Block::Air && z <= ceiling && !(out->breakable & DRY_AIR))
            memcpy(out, (void *)(base + WATER_BLOCK_RVA), sizeof(*out));
    }
    return out;
}

// --- Detail (sprite/spawn/object) anchors -----------------------------------

struct DetailArray {
    unsigned char *begin, *end, *capacity;
};
// Where a zone keeps a std::vector of records: its offset in Zone, the record
// stride, and the offset of the record's int64 x/y/z position (in dots).
struct Placement {
    size_t array, stride, position;
};
static constexpr Placement placements[] = {{0x18, 0x40, 8},    {0x90, 0x40, 8}, {0x30, 0xe0, 0xa0},
                                           {0x48, 0xe0, 0xa0}, {0x60, 0xe0, 8}, {0x78, 0xe0, 8}};
constexpr size_t SPAWN_POSITION = 0xa0; // Position offset used by spawn records.

// Number of records in `a`, or 0 if the vector looks invalid.
static size_t detailCount(const DetailArray &a, size_t stride = 64) {
    uintptr_t begin = (uintptr_t)a.begin, end = (uintptr_t)a.end, capacity = (uintptr_t)a.capacity;
    if (!begin || end < begin || capacity < end || (end - begin) % stride ||
        (end - begin) > 16 * 1024 * 1024)
        return 0;
    return (end - begin) / stride;
}

static DetailArray detailArray(cube::Zone *zone, const Placement &layout) {
    DetailArray array;
    memcpy(&array, (unsigned char *)zone + layout.array, sizeof(array));
    return array;
}

static void translateDetails(cube::Zone *zone, const Columns &columns) {
    // Sprite arrays use 64-byte records; spawn/object arrays use 224-byte
    // records with different position offsets. Derived position caches still
    // require auditing before this expanded translation is deployed.
    constexpr int64_t Z_MARGIN = 96 * 65536LL; // Keep the shifted z from overflowing.
    for (auto layout : placements) {
        DetailArray array = detailArray(zone, layout);
        size_t count = detailCount(array, layout.stride);
        for (size_t i = 0; i < count; i++) {
            auto record = array.begin + i * layout.stride;
            int64_t p[3];
            memcpy(p, record + layout.position, sizeof(p));
            // Native spawn arrays contain empty records with a zero position.
            if (layout.position == SPAWN_POSITION && !record[0] && !p[0] && !p[1] && !p[2])
                continue;
            int64_t x = terrain::blockFromDots(p[0]), y = terrain::blockFromDots(p[1]);
            int64_t lx, ly;
            int delta = localColumn(zone, x, y, lx, ly) ? columns.delta[lx * ZONE_SIZE + ly]
                                                        : displacement(zone->world, x, y);
            if (p[2] < INT64_MIN + Z_MARGIN || p[2] > INT64_MAX - Z_MARGIN)
                continue;
            p[2] = terrain::shiftedDetailZ(p[2], delta);
            memcpy(record + layout.position + 16, &p[2], sizeof(p[2]));
            details.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// --- Zone lifecycle ---------------------------------------------------------

// Called after native generation: shift every column and its anchors.
static void translate(cube::Zone *zone) {
    if (!installed || !zone || !zone->world)
        return;
    auto data = std::make_shared<Columns>();
    for (int x = 0; x < ZONE_SIZE; x++)
        for (int y = 0; y < ZONE_SIZE; y++) {
            auto &field = zone->fields[x * ZONE_SIZE + y];
            int delta = fromSurface(zone->world, (int64_t)zone->position.x * ZONE_SIZE + x,
                                    (int64_t)zone->position.y * ZONE_SIZE + y, field.field_8);
            data->delta[x * ZONE_SIZE + y] = (int8_t)delta;
            // Do not reallocate or rewrite the game's voxel storage. All materials move together.
            field.base_z += delta;
            field.field_8 += delta;
            field.field_28 += delta;
        }
    translateDetails(zone, *data);
    {
        std::unique_lock lock(columnsMutex);
        columns[zone] = data;
        epoch.fetch_add(1, std::memory_order_release);
    }
    // Object occupancy is derived from object bounds, not part of the terrain.
    // Remove its old marks in their translated storage before the native builder
    // retires its old coordinate keys and marks the new bounds. Other bits survive.
    for (auto &field : zone->fields)
        for (auto &block : field.blocks)
            block.breakable &= ~OCCUPIED;
    nativeCall<void (*)(cube::Zone *)>(BUILD_BOUNDS_RVA)(zone);
    nativeCall<void (*)(cube::Zone *)>(BUILD_OCCUPANCY_RVA)(zone);
    auto n = ++chunks;
    if (n == 1 || n % 128 == 0) {
        char s[256];
        sprintf_s(
            s,
            "Column alignment chunks=%llu, translated detail anchors=%llu; amplitude=%.0f scale=%.0f",
            (unsigned long long)n, (unsigned long long)details.load(), amplitude, scale);
        log(s);
    }
}

static void forget(cube::Zone *zone) {
    std::unique_lock lock(columnsMutex);
    columns.erase(zone);
    epoch.fetch_add(1, std::memory_order_release);
}

// --- Inline hooks -----------------------------------------------------------

// True if the bytes at `rva` equal the hex string (two digits per byte).
static bool matches(uintptr_t rva, const char *hex) {
    for (size_t i = 0; hex[i * 2]; i++) {
        unsigned n = 0;
        if (sscanf_s(hex + i * 2, "%2x", &n) != 1 || *(unsigned char *)(base + rva + i) != n)
            return false;
    }
    return true;
}

// Writes a 14-byte absolute jump: jmp qword ptr [rip+0]; dq target.
static void jump(unsigned char *out, uintptr_t target) {
    out[0] = 0xff;
    out[1] = 0x25;
    memset(out + 2, 0, 4);
    memcpy(out + 6, &target, 8);
}

struct Hook {
    unsigned char *entry;
    unsigned size; // Bytes of whole instructions moved into the trampoline.
    void *replacement;
    unsigned char *trampoline = nullptr;
    DWORD protection = 0;
};

// Builds the trampoline (copied prologue + jump back) and makes the entry
// writable. Nothing in the game's code is modified yet.
static bool prepare(Hook &h) {
    // Nearby allocation permits relocating the lookup's RIP-relative cookie load.
    uintptr_t start = ((uintptr_t)h.entry) & ~uintptr_t(65535);
    for (uintptr_t d = 65536; d < 0x70000000; d += 65536) {
        h.trampoline = (unsigned char *)VirtualAlloc((void *)(start + d), 64,
                                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (h.trampoline)
            break;
    }
    if (!h.trampoline)
        return false;
    memcpy(h.trampoline, h.entry, h.size);
    if (h.size == 16) {
        // The lookup prologue loads the security cookie with a rel32 at byte 9.
        int32_t old;
        memcpy(&old, h.entry + 9, 4);
        int64_t relocated = (int64_t)(h.entry + 13 + old) - (int64_t)(h.trampoline + 13);
        if (relocated < INT32_MIN || relocated > INT32_MAX)
            return false;
        int32_t value = (int32_t)relocated;
        memcpy(h.trampoline + 9, &value, 4);
    }
    jump(h.trampoline + h.size, (uintptr_t)h.entry + h.size);
    DWORD unused;
    return VirtualProtect(h.trampoline, 64, PAGE_EXECUTE_READ, &unused) &&
           VirtualProtect(h.entry, h.size, PAGE_EXECUTE_READWRITE, &h.protection);
}

// Undoes whatever prepare() managed to do.
static void rollback(Hook &h) {
    if (h.protection) {
        DWORD unused;
        VirtualProtect(h.entry, h.size, h.protection, &unused);
    }
    if (h.trampoline)
        VirtualFree(h.trampoline, 0, MEM_RELEASE);
}

// Overwrites the entry with a jump to the replacement (NOP-padded).
static void patch(Hook &h) {
    unsigned char bytes[18];
    memset(bytes, 0x90, sizeof(bytes));
    jump(bytes, (uintptr_t)h.replacement);
    memcpy(h.entry, bytes, h.size);
    DWORD unused;
    VirtualProtect(h.entry, h.size, h.protection, &unused);
    FlushInstructionCache(GetCurrentProcess(), h.entry, h.size);
    FlushInstructionCache(GetCurrentProcess(), h.trampoline, 64);
}

static double setting(const wchar_t *key, int fallback) {
    return (double)GetPrivateProfileIntW(L"Terrain", key, fallback, INI);
}

static void install() {
    base = (uintptr_t)GetModuleHandleW(nullptr);
    if (!nativeScene::supported()) {
        log("Unsupported executable; terrain disabled.");
        return;
    }
    if (!GetPrivateProfileIntW(L"Terrain", L"Enabled", 1, INI)) {
        log("TerrainVariance disabled.");
        return;
    }
    amplitude = std::clamp(setting(L"Amplitude", 68), 0., 96.);
    scale = std::clamp(setting(L"Scale", 768), 256., 2048.);
    mountainAmplitude = std::clamp(setting(L"MountainAmplitude", 80), amplitude, 96.);
    // Verified native name branch 16b454 resolves "Mountain". Read its jump
    // table index instead of assuming a location ID from branch order.
    for (int i = 0; i < LOCATION_TYPE_COUNT; i++)
        if (*(uint32_t *)(base + LOCATION_JUMP_TABLE_RVA + i * 4) == MOUNTAIN_NAME_BRANCH)
            mountainLocationType = i + 1;
    if (!mountainLocationType) {
        log("Mountain location dispatch differs; terrain disabled.");
        return;
    }
    {
        char status[256];
        sprintf_s(
            status,
            "Mountain relief: ordinary amplitude=%.0f, mountain amplitude=%.0f; native location ID=%d; smooth 160-block region-border taper.",
            amplitude, mountainAmplitude, mountainLocationType);
        log(status);
    }
    // Refuse to patch unless every target still has the expected prologue.
    if (!matches(HEIGHT_RVA, "4c8bdc5553415541564157498d6ba8") ||
        !matches(GENERATE_RVA, "40554154415541564157488dac24507dffff") ||
        !matches(LOOKUP_RVA, "40534883ec40488b05f3a053004833c4")) {
        log("Column alignment: unsupported or hooked native code; disabled.");
        return;
    }
    if (!matches(BUILD_BOUNDS_RVA, "48895c242056574154415641574883ec40") ||
        !matches(BUILD_OCCUPANCY_RVA, "40555356574156488d6c24c94881ece0000000")) {
        log("Object cache builders differ; terrain disabled.");
        return;
    }
    Hook hooks[] = {{(unsigned char *)(base + HEIGHT_RVA), 15, (void *)height},
                    {(unsigned char *)(base + GENERATE_RVA), 18, (void *)generate},
                    {(unsigned char *)(base + LOOKUP_RVA), 16, (void *)lookup}};
    // All-or-nothing: prepare every hook before patching any of them.
    bool ready = true;
    for (auto &h : hooks)
        if (!prepare(h)) {
            ready = false;
            break;
        }
    if (!ready) {
        for (auto &h : hooks)
            rollback(h);
        log("Column alignment: hook preparation failed; no patches applied.");
        return;
    }
    originalHeight = (Height)hooks[0].trampoline;
    originalGenerate = (Generate)hooks[1].trampoline;
    originalLookup = (Lookup)hooks[2].trampoline;
    for (auto &h : hooks)
        patch(h);
    installed = true;
    log("TerrainVariance column alignment enabled; native water and shore columns preserved, inland terrain translated, all height modes uniform; API 7.3.");
}

// --- Read-only diagnostics (exported below) ---------------------------------

static DWORD auditLookup() {
    // This export never installs hooks or changes a game's fields. Compare the
    // replacement at zero displacement against the actual native getter byte for byte.
    if (installed) {
        log("Read-only block audit skipped: already installed.");
        return 0;
    }
    base = (uintptr_t)GetModuleHandleW(nullptr);
    if (!matches(LOOKUP_RVA, "40534883ec40488b05f3a053004833c4")) {
        log("Read-only block audit: native getter signature differs.");
        return 0;
    }
    originalLookup = (Lookup)(base + LOOKUP_RVA);
    auto game = nativeScene::game();
    auto world = game ? game->world : nullptr;
    nativeScene::Locks locks(world);
    if (!locks.held) {
        log(world ? "Read-only block audit: generation locks busy."
                  : "Read-only block audit: no supported world.");
        return 0;
    }
    uint64_t checked = 0, mismatches = 0;
    unsigned sampled = 0;
    for (auto &item : world->zones) {
        auto zone = item.second;
        if (!zone)
            continue;
        {
            std::unique_lock lock(columnsMutex);
            columns[zone] = std::make_shared<Columns>(); // All-zero offsets.
            epoch.fetch_add(1, std::memory_order_release);
        }
        for (int x = 0; x < ZONE_SIZE; x += 7)
            for (int y = 0; y < ZONE_SIZE; y += 11) {
                auto &f = zone->fields[x * ZONE_SIZE + y];
                int top = f.base_z + (int)f.blocks.size();
                int zs[] = {f.base_z - 1, f.base_z, top - 1, top, -1, 0, 1};
                for (int z : zs) {
                    cube::Block expected{}, actual{};
                    int bx = zone->position.x * ZONE_SIZE + x,
                        by = zone->position.y * ZONE_SIZE + y;
                    originalLookup(zone, &expected, bx, by, z);
                    lookup(zone, &actual, bx, by, z);
                    checked++;
                    if (memcmp(&actual, &expected, sizeof(actual)))
                        mismatches++;
                }
            }
        forget(zone);
        if (++sampled == 32)
            break;
    }
    char s[256];
    sprintf_s(
        s,
        "Read-only native block audit: zones=%u samples=%llu mismatches=%llu; no game fields modified.",
        sampled, (unsigned long long)checked, (unsigned long long)mismatches);
    log(s);
    return checked && mismatches == 0 ? 1 : 0;
}

static DWORD auditDetails() {
    base = (uintptr_t)GetModuleHandleW(nullptr);
    auto g = nativeScene::game();
    auto world = g ? g->world : nullptr;
    nativeScene::Locks locks(world);
    if (!locks.held) {
        log("Read-only detail audit: generation locks busy or no world.");
        return 0;
    }
    uint64_t count = 0, inZone = 0;
    unsigned zones = 0;
    double gap = 0;
    // (array offset, record id) -> (records, records inside their owning zone)
    std::map<std::pair<size_t, unsigned>, std::pair<uint64_t, uint64_t>> inventory;
    for (auto &item : world->zones) {
        auto zone = item.second;
        if (!zone)
            continue;
        for (auto layout : placements) {
            DetailArray array = detailArray(zone, layout);
            size_t n = detailCount(array, layout.stride);
            for (size_t i = 0; i < n; i++) {
                auto record = array.begin + i * layout.stride;
                unsigned id = 0;
                memcpy(&id, record, layout.position == SPAWN_POSITION ? 1 : 4);
                auto &group = inventory[{layout.array, id}];
                group.first++;
                int64_t p[3];
                memcpy(p, record + layout.position, sizeof(p));
                int64_t x, y;
                bool owned = localColumn(zone, terrain::blockFromDots(p[0]),
                                         terrain::blockFromDots(p[1]), x, y);
                count++;
                if (owned) {
                    inZone++;
                    group.second++;
                    gap = std::max(
                        gap, std::abs(p[2] / 65536. - zone->fields[x * ZONE_SIZE + y].field_8));
                }
            }
        }
        ++zones;
    }
    FILE *csv = nullptr;
    fopen_s(&csv, ".mod-work\\TerrainVariance\\placement-id-audit.csv", "w");
    if (csv) {
        fprintf(csv, "array_offset,id,count,in_owner_zone\n");
        for (auto &item : inventory)
            fprintf(csv, "%zx,%u,%llu,%llu\n", item.first.first, item.first.second,
                    (unsigned long long)item.second.first, (unsigned long long)item.second.second);
        fclose(csv);
    }
    char s[256];
    sprintf_s(
        s,
        "Read-only detail audit: zones=%u anchors=%llu in-owner-zone=%llu largest ground gap=%.3f; no positions modified.",
        zones, (unsigned long long)count, (unsigned long long)inZone, gap);
    log(s);
    return count && inZone * 100 / count >= 95 ? 1 : 0;
}

static DWORD snapshotColumns() {
    auto g = nativeScene::game();
    auto w = g ? g->world : nullptr;
    nativeScene::Locks locks(w);
    if (!locks.held || !w->local_creature)
        return 0;
    int64_t px = terrain::blockFromDots(w->local_creature->entity_data.position.x),
            py = terrain::blockFromDots(w->local_creature->entity_data.position.y);
    FILE *file = nullptr;
    fopen_s(&file, ".mod-work\\TerrainVariance\\live-columns.csv", "w");
    if (!file)
        return 0;
    fprintf(file,
            "x,y,height,delta,native_height,base_z,count,type_m2,type_m1,type_0,type_p1,type_p2\n");
    unsigned count = 0;
    for (int dx = -256; dx <= 256; dx += 4)
        for (int dy = -256; dy <= 256; dy += 4) {
            int64_t x = px + dx, y = py + dy;
            int zx = (int)std::floor(x / 64.), zy = (int)std::floor(y / 64.);
            auto found = w->zones.find(IntVector2(zx, zy));
            if (found == w->zones.end() || !found->second)
                continue;
            auto &f = found->second->fields[(x - (int64_t)zx * ZONE_SIZE) * ZONE_SIZE + y -
                                            (int64_t)zy * ZONE_SIZE];
            int delta = fromSurface(w, x, y, f.field_8);
            int z = (int)std::floor(f.field_8);
            fprintf(file, "%lld,%lld,%.6f,%d,%.6f,%d,%zu", x, y, f.field_8, delta,
                    f.field_8 - delta, f.base_z, f.blocks.size());
            // Block types two below to two above the surface (1 = below storage, 0 = above).
            for (int dz = -2; dz <= 2; dz++) {
                int64_t index = (int64_t)z + dz - f.base_z;
                int type = index < 0                           ? 1
                           : index >= (int64_t)f.blocks.size() ? 0
                                                               : f.blocks[(size_t)index].type;
                fprintf(file, ",%d", type);
            }
            fprintf(file, "\n");
            count++;
        }
    fclose(file);
    char s[160];
    sprintf_s(
        s, "Read-only local column snapshot: %u columns around (%lld,%lld); no terrain modified.",
        count, px, py);
    log(s);
    return count;
}

} // namespace terrainmod

class TerrainVariance : public GenericMod {
  public:
    TerrainVariance() {
        OnZoneGeneratedPriority = VeryLowPriority;
    }
    void Initialize() override {
        terrainmod::install();
    }
    void OnZoneGenerated(cube::Zone *zone) override {
        terrainmod::translate(zone);
    }
    void OnZoneDestroy(cube::Zone *zone) override {
        terrainmod::forget(zone);
    }
};
static_assert(sizeof(GenericMod) == 0x428);
EXPORT GenericMod *MakeMod() {
    return new TerrainVariance();
}
EXPORT void ModPreInitialize() {}
EXPORT int ModMajorVersion() {
    return 7;
}
EXPORT int ModMinorVersion() {
    return 3;
}

// Retries a read-only diagnostic for up to ~0.5 s while the world is busy.
template <class Diagnostic> static DWORD retry(Diagnostic diagnostic) {
    for (unsigned i = 0; i < 100; i++) {
        auto result = diagnostic();
        if (result)
            return result;
        Sleep(5);
    }
    return 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI AuditColumnLookup(void *) {
    return terrainmod::auditLookup();
}
extern "C" __declspec(dllexport) DWORD WINAPI AuditDetailPositions(void *) {
    return retry(terrainmod::auditDetails);
}
extern "C" __declspec(dllexport) DWORD WINAPI SnapshotTerrainColumns(void *) {
    return retry(terrainmod::snapshotColumns);
}
