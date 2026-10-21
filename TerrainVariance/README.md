# TerrainVariance

Made with the aid of AI (OpenAI ChatGPT/Codex), by DieuDeGlace.

Smooth added height variation, stronger hills, and up to 80-block mountain displacement. Underwater and low shore columns retain their native heights so water surfaces remain visible. Displacement fades in between native heights 16 and 80; higher inland terrain retains the full configured variation. Columns and six separately placed object arrays move together; placement indexes are adjusted. PC and mobile use identical terrain implementation, rules and regression tests. Collision/placement behavior has not been exhaustively tested for every object or world.

See [root build instructions](../README.md#build-on-windows-x64). Build only this mod with `cmake --build build --target TerrainVariance` from the repository root. Output: `build/TerrainVariance.dll`. Configuration: `Mods/TerrainVariance.ini`. Close the game to install or remove hooks; restart after binary/config changes.
