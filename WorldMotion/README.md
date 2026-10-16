# WorldMotion

Made with the aid of AI (OpenAI ChatGPT/Codex), by DieuDeGlace.

Bounded cloud drift and gentle leaf motion using GPU buffers. Cloud speed is independently configurable; touching leaf corners are anchored to reduce seams. Forest performance remains a known limitation; motion can be disabled with F8.

See [root build instructions](../README.md#build-on-windows-x64). Build only this mod with `cmake --build build --target WorldMotion` from the repository root. Output: `build/WorldMotion.dll`. Configuration: `Mods/WorldMotion.ini`. Close the game to install or remove hooks; restart after binary/config changes.

The supplied preset enables clouds and leaves by default (Enabled=1). F8 toggles motion; set Enabled=0 before restart to disable startup motion. Forest performance is not guaranteed.

On `test/cascaded-shadows`, BeginWorldMotionFrame provides a scene timestamp to synchronize moving leaves and cloud drift with MoonandShadow's cascade test. Geometry, sway amplitude and animation update rate remain unchanged. The timing test verifies that draws crossing an animation tick keep the same scene time. Original source and tests were modified with AI assistance.

On `perf/world-motion-draw-hooks`, unrelated model draws are rejected by cloud
buffer identity and terrain chunk membership before inspecting six constant
buffers or reading matrices. Matrix shadow copies are limited to the 64-byte
buffers actually consumed. Cloud draws restore only their world matrix; leaf
draws restore only vertex input. Animation geometry, shader, range, speed,
33 ms update interval and synchronization with shadow maps are unchanged.

`Mods/WorldMotion.log` includes `Draw setup/restore CPU`, sampled on one frame
in 60 and averaged across 30 samples. This excludes native draws, buffer-write
hooks and the final release of saved COM references; it is not total movement
cost or a GPU measurement. The wind GPU reference and binding-restoration
checks pass. A matched forest FPS comparison is still required.
