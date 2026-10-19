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

## FieldBosses mobile update — 2026-10-11
Upstream3123774 (including d1f8452) merged. Built against retained mobile executable guard timestamp5d93ab0d/image58a000; no changes to boss rules/glow algorithm. Inspected phone executable named-boss light call RVA d1882 opcodeE8 targets e2bd0 as required. RulesTest PASS: deterministic anchors, unsigned IDs, filtering, RGB consistency and light position/radius/sort preservation.
Installed only FieldBosses.dll into actual GameNative Steam Cube World Mods; existing Enabled1/BossesPerRegion3/ActiveCap3 and progress retained. Copy result0; SHA2569699EBDCD6CA7976B73DB2FF65B7EA35E87371784836F9CF748C46178E30FCC8 matched phone. Log: Named-boss purple glow hook: installed. World loaded; no boss visual/reload encounter or GPU benchmark performed. Prior DLL backed up FieldBosses-before-3123774-mobile.dll; normal Steam-folder launcher restored. Existing terrain/wind/shadow/Geo unchanged. Copied fallback game not updated.

## Coordinated rendering update (2026-10-12)
Merged upstream test/cascaded-shadows at 5084188, TemporalAA test/taa-edge-stability at 8f3c561 (code 44b12e0), and WorldMotion test/cascaded-shadows at 59c8a89. Palette has no newer source update and its installed companion stays unchanged.

Shadows retain the phone strength 45, full far 4096/radius 120, cloud and moon settings. Upstream now enables a near 2048/radius 32 map by default, even with older INIs; it adds roughly 16 MiB depth storage. Cascades=0 in [Shadows] disables the near map. The new model receiver preserves ambient and local illumination, fixes close contact/self-shadow comparisons, and stabilizes grazing model faces. WorldMotion shares animation time across native rendering and shadow capture. TAA aligns native backgrounds with world jitter and improves edge history rejection without adding samples or fullscreen passes. Phone wind settings and all INIs were preserved byte for byte during installation.

Built Windows x64 DLLs with clang-cl /O2. All nine shadow tests passed, including160 model-plane cases, native shader clip equivalence (4608 values, zero ULP discrepancy on the local PC fixture), ambient/point/lantern preservation, cascade filtering and state restoration. TemporalTest passed 720p/1080p/square cases; TemporalStabilityTest static-edge variance changed from 67.8996 to 14.7805 on WARP, with moving silhouette/uniform-color checks passing. This synthetic variance reduction is not an in-game percentage claim. Wind CPU/GPU tests passed including frame-time synchronization.

Installed into the actual GameNative Steam folder with a rollback installer; three DLLs byte-compared successfully. Pixel 10 Pro / Android 17 / GameNative 1.2.1 loaded into Groron Hills; HUD and terrain rendered. Logs confirm model receiver/cascades active and TAA resolves. User reported: "Looks steadier; speed feels similar." No matched gameplay FPS benchmark or long-session test was performed. Wine's shader compiler reports a potential-uninitialized warning for modelPlaneVisibility; all return paths are defined, compilation completes, and rendering tests/device initialization succeed. This warning is retained in diagnostics rather than suppressed.

Rollback: close GameNative and restore all three backed-up DLLs together. Keep the existing INIs. The normal Steam-folder launcher was restored after the one-time installer. Geo/.NET, saves, Steam auth, driver/DXVK/Proton and other installed mods were not replaced.


## Rendering performance sync — 2026-10-15

Applied directly to mobile main from desktop `f7a9905218bcace214b9f7aaf3347d4c22b1e31e`. Ports early rejection of unrelated model draws, matrix-only shadow copies and selective state restoration. Wind shader, speed, range and update interval are unchanged. The mobile executable identity guard, cached validation and mobile configuration presets are retained. Builds remain Windows x64 DLLs for GameNative, not Android shared libraries. Desktop CPU/GPU fixture results are not phone performance measurements. Preserve existing phone INIs when installing. PC regression checks and phone deployment verification are recorded with the release artifacts; gameplay performance/visual confirmation remains required.

## PC mod-list sync — 2026-10-18
Merged upstream main e028150, including current FieldBosses/terrain rules, WaterLilies, and DualHair's creation-only panel. The mobile PE identity cache and corrected leading 0x40 byte at the object-occupancy function are retained. WorldMotion keeps the accepted mobile draw-hook optimization and shared frame timestamp rather than regressing to the older main implementation during the readability merge. Mobile graphics presets are unchanged.

Built five Windows x64 DLLs with clang-cl /O2. Seven suites passed: water-lily rules, wind rules, terrain bounds, fixed-point placement, boss rules/glow, DualHair rules and GPU wind/state restoration. DualHair character colors and existing state files are preserved during installation. FieldBosses, WaterLilies and DualHair now live in this mobile fork alongside TerrainVariance and WorldMotion. Device integration and rollback results are recorded with the phone-sync manifest.
