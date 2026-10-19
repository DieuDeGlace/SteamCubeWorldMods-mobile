#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "../NativeScene.h"
#include "PlacementRules.h"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace waterLilies {
using lilies::Decoration;

// ===========================================================================
// Native layout and entry points (inspected Steam executable)
// ===========================================================================

// Zone +0x18: std::vector<Decoration> of the chunk's scenery records.
constexpr size_t DecorationsOffset = 0x18;
// Native sprite indices of the two lily models.
constexpr size_t FlowerSprite = 0xd23, PadSprite = 0xd24;
constexpr uintptr_t BlockAt = 0xeb90;           // (zone, out block, x, y, z)
constexpr uintptr_t RegionOwner = 0x2ada20;     // (world, out region, block x, block y)
constexpr uintptr_t RegionBiome = 0x2b9080;     // (world, region x, region y) -> biome
constexpr uintptr_t AppendDecoration = 0x69b30; // (vector *, const Decoration *)
constexpr unsigned char AppendEntry[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
                                         0xec, 0x20, 0x48, 0x8b, 0x41, 0x08, 0x48, 0x8b};

struct Array {
    Decoration *begin, *end, *capacity;
};
static_assert(offsetof(cube::Zone, gap18) == DecorationsOffset);
static_assert(offsetof(cube::SpriteManager, environment_sprites) == 0x38);
static_assert(offsetof(Decoration, x) == 8 && offsetof(Decoration, flags) == 0x38);
static_assert(sizeof(Array) == 24);

static uintptr_t base;

static unsigned char *decorations(cube::Zone *zone) {
    return reinterpret_cast<unsigned char *>(zone) + DecorationsOffset;
}
static Array array(cube::Zone *zone) {
    Array value{};
    std::memcpy(&value, decorations(zone), sizeof(value));
    return value;
}
static bool valid(const Array &a) {
    auto b = uintptr_t(a.begin), e = uintptr_t(a.end), c = uintptr_t(a.capacity);
    if (!b)
        return !e && !c;
    return e >= b && c >= e && (e - b) % sizeof(Decoration) == 0 &&
           (c - b) % sizeof(Decoration) == 0 && c - b <= 16384 * sizeof(Decoration);
}
static int blockType(cube::Zone *zone, int x, int y, int z) {
    // The native getter respects translated or implicit water; never assume sea level 0.
    cube::Block value{};
    reinterpret_cast<cube::Block *(*)(cube::Zone *, cube::Block *, int, int, int)>(base + BlockAt)(
        zone, &value, x, y, z);
    return int(value.type);
}
static int biomeAt(cube::World *world, int x, int y) {
    IntVector2 owner{};
    reinterpret_cast<void (*)(cube::World *, IntVector2 *, int, int)>(base + RegionOwner)(
        world, &owner, x, y);
    return reinterpret_cast<int (*)(cube::World *, int, int)>(base + RegionBiome)(world, owner.x,
                                                                                  owner.y);
}
static void append(cube::Zone *zone, const Decoration &pad) {
    // Steam's POD decoration-vector append, including its own allocator. No
    // creature, block edit, occupancy/collision or render hook is involved.
    reinterpret_cast<void (*)(void *, const Decoration *)>(base + AppendDecoration)(
        decorations(zone), &pad);
}

// The two lily models must be the native sprites with their known dimensions.
static bool assets(cube::World *world) {
    auto manager = world->spritemanager;
    if (!manager || manager->environment_sprites.size() <= lilies::PadModel ||
        manager->sprites.size() <= PadSprite)
        return false;
    for (unsigned model : {lilies::FlowerModel, lilies::PadModel}) {
        bool flower = model == lilies::FlowerModel;
        auto sprite = manager->environment_sprites[model];
        if (!sprite || sprite != manager->sprites[flower ? FlowerSprite : PadSprite] ||
            sprite->lowering_this_makes_plants_go_fast != 4 ||
            sprite->something_with_offset_1 != (flower ? 13 : 9) ||
            sprite->something_with_offset_2 != (flower ? 14 : 9))
            return false;
    }
    return true;
}

// ===========================================================================
// Settings, statistics and the per-zone "already decorated" cache
// ===========================================================================

static bool enabled = false;
static unsigned density = 50, flowers = 25, maximum = 18, depth = 10;
static std::atomic<uint64_t> decorated{0}, totalPads{0};

struct Stamp {
    cube::World *world;
    IntVector2 position;
    int seed;
    bool matches(cube::Zone *zone) const {
        return world == zone->world && position == zone->position && seed == zone->world->seed;
    }
};
static std::mutex cacheMutex;
static std::unordered_map<cube::Zone *, Stamp> processed;

static void log(const char *words) {
    FILE *file = nullptr;
    fopen_s(&file, "Mods/WaterLilies.log", "a");
    if (file) {
        fprintf(file, "%s\n", words);
        fclose(file);
    }
}
static void forget(cube::Zone *zone) {
    std::lock_guard<std::mutex> guard(cacheMutex);
    processed.erase(zone);
}
// Cheap pre-check before any native lock: most remeshes are of decorated zones.
static bool done(cube::Zone *zone) {
    std::lock_guard<std::mutex> guard(cacheMutex);
    auto found = processed.find(zone);
    return found != processed.end() && found->second.matches(zone);
}
static bool claim(cube::Zone *zone) {
    std::lock_guard<std::mutex> guard(cacheMutex);
    auto found = processed.find(zone);
    if (found != processed.end() && found->second.matches(zone))
        return false;
    // Lifecycle callbacks clear normal residency. Bound the cache even if a
    // different loader omits a destruction callback; placement remains deduped.
    if (processed.size() >= 4096)
        processed.clear();
    processed[zone] = {zone->world, zone->position, zone->world->seed};
    return true;
}
static void report(unsigned placed) {
    auto chunks = decorated.fetch_add(1, std::memory_order_relaxed) + 1;
    auto pads = totalPads.fetch_add(placed, std::memory_order_relaxed) + placed;
    if (chunks == 1 || chunks % 32 == 0) {
        char message[160];
        sprintf_s(message, "Native scenery: decorated chunks=%llu, lily pads=%llu; no actors spawned.",
                  (unsigned long long)chunks, (unsigned long long)pads);
        log(message);
    }
}

// ===========================================================================
// Placement
// ===========================================================================

static void place(cube::Zone *zone) {
    if (!enabled || !zone || !zone->world || !maximum || !density || done(zone) ||
        !assets(zone->world))
        return;
    // Native meshing/rendering share these locks. Never move scenery storage
    // while another thread is reading it, or wait on a busy generation worker;
    // a busy zone is left for a later native remesh.
    nativeScene::Locks locks(zone->world);
    if (!locks.held)
        return;
    auto records = array(zone);
    if (!valid(records) || !claim(zone))
        return;
    int64_t originX = int64_t(zone->position.x) * 64, originY = int64_t(zone->position.y) * 64;
    if (originX < INT_MIN + 64LL || originX > INT_MAX - 64LL || originY < INT_MIN + 64LL ||
        originY > INT_MAX - 64LL)
        return;
    if (!lilies::inlandBiome(biomeAt(zone->world, int(originX + 32), int(originY + 32))))
        return;
    const auto plan = lilies::candidates(uint32_t(zone->world->seed), zone->position.x,
                                         zone->position.y, density, flowers);
    // Existing lilies (native, or added on an earlier load) plus every pad added
    // below. One pass over the zone's scenery replaces a rescan of the whole
    // native array for each of the forty candidates.
    std::vector<Decoration> lilyRecords;
    for (auto p = records.begin; p && p != records.end; ++p)
        if (lilies::lily(p->model))
            lilyRecords.push_back(*p);
    auto occupied = [&](const lilies::Candidate &candidate) {
        for (const auto &lily : lilyRecords)
            if (lilies::overlaps(lily, candidate))
                return true;
        return false;
    };
    auto locate = [&](const lilies::Candidate &candidate, int &top) {
        auto x = lilies::block(candidate.x), y = lilies::block(candidate.y);
        int lx = int(x - originX), ly = int(y - originY);
        if (lx < 1 || ly < 1 || lx >= 63 || ly >= 63)
            return false;
        auto height = zone->fields[lx * 64 + ly].field_8;
        if (!std::isfinite(height) || std::abs(height) > 100000.f)
            return false;
        int ground = int(std::floor(height));
        auto native = [&](int z) { return blockType(zone, int(x), int(y), z); };
        lilies::CachedColumn<decltype(native)> column(native, ground - 2 - int(depth));
        return lilies::findSurface(ground, depth, top, column) &&
               lilies::footprint(top, [&](int dx, int dy, int z) {
                   return dx == 0 && dy == 0 ? column(z)
                                             : blockType(zone, int(x) + dx, int(y) + dy, z);
               });
    };
    auto add = [&](const Decoration &pad) {
        append(zone, pad);
        lilyRecords.push_back(pad);
        return true;
    };
    if (unsigned placed = lilies::populate(plan, maximum, occupied, locate, add))
        report(placed);
}

static void initialize() {
    base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto setting = [](const wchar_t *name, unsigned fallback) {
        return GetPrivateProfileIntW(L"WaterLilies", name, fallback, L".\\Mods\\WaterLilies.ini");
    };
    density = std::min(setting(L"PatchDensity", 50), 100u);
    flowers = std::min(setting(L"FlowerPercent", 25), 100u);
    maximum = std::min(setting(L"MaxPadsPerChunk", 18), lilies::MaximumPads);
    depth = std::clamp(setting(L"MaximumDepth", 10), 1u, 16u);
    enabled = setting(L"Enabled", 1) && nativeScene::supported() &&
              std::memcmp(reinterpret_cast<void *>(base + AppendDecoration), AppendEntry,
                          sizeof(AppendEntry)) == 0;
    log(enabled ? "WaterLilies enabled: native freshwater scenery, stable chunk clusters."
                : "WaterLilies disabled or native decoration append signature unavailable.");
}
} // namespace waterLilies

class WaterLilies : public GenericMod {
  public:
    void Initialize() override {
        waterLilies::initialize();
    }
    void OnChunkRemesh(cube::Zone *zone) override {
        waterLilies::place(zone);
    }
    void OnZoneGenerated(cube::Zone *zone) override {
        waterLilies::forget(zone);
    }
    void OnZoneDestroy(cube::Zone *zone) override {
        waterLilies::forget(zone);
    }
};
static_assert(sizeof(GenericMod) == 0x428);
EXPORT GenericMod *MakeMod() {
    return new WaterLilies();
}
EXPORT void ModPreInitialize() {}
EXPORT int ModMajorVersion() {
    return 7;
}
EXPORT int ModMinorVersion() {
    return 3;
}
