# Mobile compatibility

Derived from https://github.com/DieuDeGlace/SteamCubeWorldMods with its Git history and license retained. GitHub does not permit a same-owner fork, so this is a separate source-derived repository. Original PC repository unchanged.

Tested 2026-10-10: Pixel 10 Pro / Android 17, GameNative 1.2.1, proton-11.0-2-arm64ec-1, System graphics driver, Wrapper-gamenative, DXVK 2.7.1-1-gplasync-0, ALSA, 1280x720. This is a Windows x64 DLL running through the phone runtime, not a native Android library.

## Changes

- NativeScene requires the tested phone executable image size 0x58a000 and retains timestamp 0x5d93ab0d. Unknown builds remain rejected.
- TerrainVariance checks the complete object-cache builder prologue at RVA 0x31d700 including its verified leading 0x40 REX instruction prefix. The old signature omitted that byte. Both builder checks and all hook-entry checks remain enabled; no terrain algorithm changes.

## Installation

Close the game. Back up Mods/TerrainVariance.dll and TerrainVariance.ini. Install the mobile release DLL as Mods/TerrainVariance.dll (one copy only). Set Enabled=1 in TerrainVariance.ini. Tested PC values: Amplitude=68, Scale=768, MountainAmplitude=80, ProtectionPadding=256, ProtectionBlend=192.

Disable CubeMegaMod WorldGen (ID6); its enabled state produced broken world terrain on this testbed. The tested PC CubeMegaMod enabled list also disables Combat ID3 and Beginner ID7. TerrainVariance is separate from WorldGen.

Existing character coordinates may be below/above the newly displaced terrain after enabling the mod. Use a backed-up test save when changing generation settings. Game files, saves, Steam client, credentials and Microsoft .NET are not distributed here.

## Build and validation

Use a Windows x64 Visual Studio developer shell with clang-cl and the Windows SDK. Run build-mobile.cmd for TerrainVariance and its CPU tests. The original CMake build remains available for all projects, but only TerrainVariance in this mobile repository has been built and phone-tested so far.

TerrainRulesTest passed: bounds, continuity, integer column alignment, translated water ceiling, large relief. PlacementTest passed: all six placement arrays retain every non-Z byte and translate exact fixed-point Z values.

Phone log confirmed installation and generated chunks1,128,256,384, translating57,13954,27178,39790 detail anchors respectively. World/map rendered. This is a short gameplay test; long-session stability and every biome are not yet established. Geo native .NET and the other installed mods remained in the isolated test setup.

## WorldMotion mobile75
WorldMotion built from the same phone-guard source. Reduced motion config is config/mobile75/WorldMotion.ini: leaf amplitude12, wind75, cloud drift48, cloud speed94; clouds/leaves stay enabled and animation range96 retained. Companion shadows preset: https://github.com/DieuDeGlace/MoonandShadow-mobile .
