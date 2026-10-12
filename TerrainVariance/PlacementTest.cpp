#include "TerrainVariance.cpp"
#include <cassert>
#include <vector>
int main() {
    // Raw fixture: only native position and array descriptors are accessed.
    alignas(cube::Zone) unsigned char storage[sizeof(cube::Zone)]{};
    auto zone = reinterpret_cast<cube::Zone *>(storage);
    zone->position = IntVector2(-100, 200);
    terrainmod::Columns columns;
    columns.delta.fill(-37);
    std::vector<std::vector<unsigned char>> records;
    std::vector<std::vector<unsigned char>> expected;
    for (auto layout : terrainmod::placements) {
        records.emplace_back(layout.stride, 0x5a);
        auto &record = records.back();
        int64_t p[] = {(-6400LL + 12) * 65536 + 123, (12800LL + 21) * 65536 + 456,
                       70LL * 65536 + 789};
        memcpy(record.data() + layout.position, p, sizeof(p));
        expected.push_back(record);
        p[2] -= 37LL * 65536;
        memcpy(expected.back().data() + layout.position + 16, p + 2, 8);
    }
    for (size_t i = 0; i < records.size(); i++) {
        auto &r = records[i];
        terrainmod::DetailArray a{r.data(), r.data() + r.size(), r.data() + r.size()};
        memcpy(storage + terrainmod::placements[i].array, &a, sizeof(a));
    }
    terrainmod::translateDetails(zone, columns);
    for (size_t i = 0; i < records.size(); i++)
        assert(records[i] == expected[i]);
    puts(
        "All six placement arrays: exact fixed-point Z translation, all other bytes preserved: PASS");
}
