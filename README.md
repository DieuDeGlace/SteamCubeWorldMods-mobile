# SteamCubeWorldMods-mobile

Mobile compatibility version. See [tested setup, changes and installation](MOBILE.md).

# Steam Cube World Mods

These mods were made by DieuDeGlace with the aid of AI: OpenAI ChatGPT/Codex. Original standalone code is MIT licensed; SDK attribution is in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

| Mod | Purpose |
| --- | --- |
| [WorldMotion](WorldMotion) | Cloud movement and gently swaying leaves |
| [TerrainVariance](TerrainVariance) | Taller, smoother hills and mountains with aligned objects/water |
| [MoonandShadow](https://github.com/DieuDeGlace/MoonandShadow) | Moon and shadows, moved to their own combined mod repository |
| [FieldBosses](FieldBosses) | Named bosses based on existing native enemies |
| [WaterLilies](WaterLilies) | Sparse native lily-pad/flower scenery on exposed freshwater |

These sources target Windows x64, CubeModLoader API 7.3 and the inspected Steam executable (PE timestamp `0x5d93ab0d`, image size `0x5b7000`). Omega and Alpha executables are not supported. No proprietary game assets, saves, compiled game binaries or development tools are bundled. SDK headers are vendored so builds do not depend on a changing upstream revision.

## Build on Windows x64

Install Git, CMake 3.20+, Ninja, and Visual Studio 2022 Build Tools with Desktop development with C++, a Windows SDK, and LLVM/Clang tools. Open an **x64 Native Tools Command Prompt for VS 2022**. Ensure `clang-cl`, `cmake`, and `ninja` are on PATH.

```bat
git clone https://github.com/DieuDeGlace/SteamCubeWorldMods.git
cd SteamCubeWorldMods
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

The three DLLs are written to `build/`. To build one mod, use e.g. `cmake --build build --target WorldMotion`. Embedded HLSL shaders are compiled at runtime; no separate shader compiler step is required. Optional graphics tests require D3D11 support: configure with `-DENABLE_GPU_TESTS=ON`, build and run CTest again. One shadow-resource smoke test uses a hardware device; other graphics tests use WARP.

## Install

Close Steam Cube World. Install a compatible CubeModLoader separately, then copy the desired DLLs into the game's `Mods` directory. Optional example settings are in `config/`; preserve your existing settings if you prefer. WorldMotion's example may be disabled, reflecting performance tuning. Do not install duplicate copies of the same mod. Restart the game after changes. Moon and shadow features are now one DLL in [MoonandShadow](https://github.com/DieuDeGlace/MoonandShadow); animated leaf/cloud shadow coordination uses WorldMotion's optional exports.

Terrain generation affects loaded world content: keep backups of your saves before experimenting. `FieldBossesState.ini` is gameplay data and is not included. Performance in dense forests and terrain/object alignment across all locations are known areas needing further testing. Build/rule tests and targeted graphics/live checks are not exhaustive gameplay verification.

The separately edited upstream projects are [CubeMegaMod](https://github.com/DieuDeGlace/CubeMegaMod) and [PyroProgression](https://github.com/DieuDeGlace/PyroProgression), published as forks rather than copied into this repository.

