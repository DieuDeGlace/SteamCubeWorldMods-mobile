# WaterLilies — native scenery test build

Adds stable, randomly scattered clusters of Steam's existing green lily pads and pink flowers to exposed freshwater. This mod was made by DieuDeGlace with OpenAI ChatGPT/Codex assistance. It targets CubeModLoader API 7.3 and the inspected Steam executable, and lives on the `test/water-lilies` branch.

Lilies are ordinary 64-byte native scenery records in the zone's decoration array. They are not creatures, health-bearing props, loot, interactive actors or voxel terrain edits. Steam's own scenery renderer draws them. The mod adds no graphics hooks, shaders, fullscreen passes, AI updates or per-frame population scans. Additional geometry still has a rendering cost; no live millisecond measurement has been made.

The installed game supplies `water-lily01.cub` and `water-lily02.cub`. Model bindings, dimensions and the native decoration append entry are checked before use. Native append handles allocation; the mod never allocates a replacement game vector or frees native scenery. Proprietary assets and native executable/disassembly data are not distributed.

Plans use the world seed and signed chunk coordinates, with four possible clusters and six to ten candidates in each. Default patch density is 50%, flowers are 25%, and the cap is 18 selected pads per 64×64 chunk (hard ceiling 32). Pads vary in size and rotation. Existing native lilies and previously added pads are retained and counted toward selected slots; repeated remesh/reload checks do not fill more slots. Unloaded zones are not generated to place lilies. A bounded cache avoids repeating native water searches on every remesh; generation/destruction callbacks invalidate it.

Placement runs before native chunk meshing, after zone generation and TerrainVariance's column translation. Nonblocking native zone/mesh locks protect scenery storage; a busy chunk is left for a later native remesh. The native block getter respects translated or implicit water instead of assuming global sea level zero. Water must be shallow, have a matching exposed 3×3 footprint, and have three clear blocks above it. Dry banks, low bridges, stepped water surfaces, poison and lava fail those checks. Ocean, frozen, mountain, deadland and fireland biomes are excluded. Rivers and lake shores in the remaining biomes can receive clusters, including water near towns. Native per-column mesh light is calculated during the normal remesh.

The bare-pad model has an empty bottom voxel layer, so its position compensates for that padding. The flower model's base begins in its bottom layer. Both receive a small surface clearance. Exact visible surface height, native scenery rendering and interaction with installed TAA/shadows still require an in-game check; the rule tests do not establish those visual outcomes.

## Build and install

Use the root build requirements, with a Windows x64 clang-cl/MSVC tools environment:

```bat
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target WaterLilies WaterLiliesRulesTest
ctest --test-dir build -R WaterLiliesRules --output-on-failure
```

With Steam Cube World closed, copy `build/WaterLilies.dll` and `config/WaterLilies.ini` into `Mods`. Only this new DLL/config is needed; existing gameplay and rendering DLLs can remain installed. Remove the DLL or set `Enabled=0` and restart to stop additions. This build has no persistent mod state file or save migration; already-loaded decorations last until the zone unloads. Restart after changing settings.

Visit several shallow rivers or lake shores, preferably in a plains, hills, woodland or wetland region. Check scattered green pads/pink flowers sit on the water, have no name/health/map marker, and do not obstruct walking, boats or combat. Leave the area and return: the pattern/count should stay stable. The log in `Mods/WaterLilies.log` records startup and cumulative decorated chunks/pads. Native scenery geometry and actual FPS are pending live verification.

`WaterLiliesRules` covers negative-coordinate ownership, deterministic plans, bounded caps, model selection, changing water heights, banks, bridges, deep water, poison, spacing, repeated population and preservation of unrelated decorations. These are algorithm tests using fixtures, not injected game calls.
