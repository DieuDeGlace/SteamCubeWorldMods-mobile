# DualHair — two-tone hair

Gives chosen player characters Omega-style two-colour hair. The top of the hair stays the normal in-game hair colour and fades down into a second colour toward the tips. Targets CubeModLoader API 7.3 and the inspected Steam executable. It also runs on the GameNative copy of that build, which has the same code with the SteamStub `.bind` section stripped.

## How it works

The native creature renderer queues the hair model from one call site, with a material colour of base × hair colour. For a configured character, DualHair queues a recoloured copy of that hair model instead:

- The copy is made with the game's own `Sprite` copy constructor.
- Its voxels get a top-to-bottom gradient, from the character's hair colour into the second colour.
- The game's own mesher rebuilds the copy's vertex and index buffers.
- The hair tint is divided back out of the queued colour, so the top of the hair matches vanilla.

Everything after the queue (lighting, shadows, TAA, other mods) sees an ordinary native model draw. The mod adds no shaders, D3D hooks or per-frame scans.

Each hairstyle and colour pair is built once and cached. A copy that has gone unused for 10 seconds is freed through the native destructor; the cache holds at most 64 copies. Recoloured voxels never become empty or a pure marker colour. The hook checks the bytes at the call site, the hair-sprite read, and the copy, mesh and allocator prologues. If any of them differ (another build, or another mod changed them), DualHair stays off and says so in `Mods/DualHair.log`.

## Use

**Character creation:** a "Hair color 2" board appears beside the native panel. It has the same 16 × 7 palette as the native hair colour board, plus:

- **TWO TONE** ON/OFF
- **FADE AT:** where the second colour starts, as a percentage of the way down the hair
- **FADE SIZE:** how long the fade is (0 = a hard split)
- a preview strip

The preview character updates as you click. The look is saved for the new character when you enter the world.

**In game:** use `/hair2` (below); the board is only shown at character creation, never on the inventory screen. If the board overlaps something, `PanelOffsetX`/`PanelOffsetY` move it.

Both boards are drawn with the game's own widget text and rectangle calls. Clicks on the board are not passed to the game.

**Chat** still works: `/hair2 R G B`, `/hair2 #RRGGBB`, `/hair2 off`, `/hair2 start N`, `/hair2 blend N`, and `/hair2` on its own shows the current look. Looks are saved by character name under `[Characters]` in `DualHair.ini`.

## Multiplayer

The native creature update sends the whole 0xB0-byte appearance block raw; the reader at `0x1390a4` copies all of it. The renderer never reads three bytes of that block: the hair colour's alpha and the two padding bytes after it. DualHair packs your look into them:

- byte 0: a tag, plus the fade start and fade size in eighths;
- bytes 1–2: the second colour as RGB565.

So other players who also run DualHair see your two-tone hair, slightly quantised. Players without the mod see normal one-colour hair. `Multiplayer=0` turns this off; a local `[Characters]` entry for another player's name is used if they don't send a look.

**Not yet confirmed:** whether the sender's change check notices a change to only these bytes. If it doesn't, others see a new look the next time your character's full appearance is sent (for example when they join or come into range).

## Build

```bat
cmake --build build --target DualHair DualHairRulesTest
ctest --test-dir build -R DualHairRules --output-on-failure
```

`DualHairRules` covers colour and saved-look parsing, the gradient shape, voxel shading (the top matches the native tint, and the result is never empty or a marker colour), tint removal, the multiplayer packing (round trip, untagged bytes) and the native palette colours. The in-game look, the board positions, the gradient direction on every hairstyle, multiplayer between two DualHair players, and the interaction with installed shadow/TAA mods still need a live check.
