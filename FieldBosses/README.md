# FieldBosses

Made with the aid of AI (OpenAI ChatGPT/Codex), by DieuDeGlace.

Deterministic region sites promote existing hostile native mobs into named bosses. No spawn announcements. At most three active bosses by default. Defeated-site state persists; no new creature pool is injected.

Native named bosses and promoted field bosses use the native named-boss light with a stronger purple RGB (`0.6, 0.04, 1.2`), visually checked on both body and nearby ground. Its native 16-block radius is retained. There are no added lights, shadow maps or rendering passes. The hook targets only the native named-boss light append call, so lanterns and ordinary lights retain their original colors. Boss recoloring does not depend on tracked IDs or a promotion tick, and remains consistent after reload. The hook checks the supported executable and original call target before installation; conflicting hooks leave the default native color in place. GPU performance has not been benchmarked.

Native creature IDs are treated as unsigned values even when the SDK exposes them as signed. This fixes eligible wild mobs with high-bit IDs being incorrectly excluded from field-boss promotion.

See [root build instructions](../README.md#build-on-windows-x64). Build only this mod with `cmake --build build --target FieldBosses` from the repository root. Output: `build/FieldBosses.dll`. Configuration: `Mods/FieldBosses.ini`. Close the game to install or remove hooks; restart after binary/config changes.
