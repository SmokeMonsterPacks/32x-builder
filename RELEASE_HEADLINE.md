## Rest-mode supersampling

- Standing still (~350 ms, no buttons held) renders the scene twice — once normally, once with sampling shifted half a column — and merges the pair on a 1-pixel checkerboard into a single parked frame. On a CRT the dither fuses into sub-pixel detail on wall edges, textures, and floor/ceiling patterns.
- The parked frame is static: no page flipping while parked. Fluorescent palette flicker continues; ambience playback is unaffected.
- Any input exits immediately and the next frame renders normally. Adaptive resolution while moving is unchanged.
- Rooms with a powered monitor keep their live static instead of parking.
- Toggle: TESTING > ULTRA. Default on.

## Desk console

- The console now appears in every procedurally generated level. Placement required both an open 3x3 around the cell and a wall to back onto, which cannot both be true, so no generated level had ever received one.
- Activating the console requires facing it, within about fifteen degrees. Walking past and pressing A no longer starts it.
- Procgen PVMs are the desk-mounted composite; placement requires walkable neighbor cells. The canonical map's console sits at its authored nook, facing east, and boots the Master System.
- Console geometry is imported from the GLB as an authored 3-step ziggurat. The bake format gains a wedge primitive (a box with an inset top) for the sloped face, which points toward the player.
- New hand-edited bezel and rear-panel art. A single-file web editor round-trips the front and rear textures through editable PNGs, matched to the engine's mirrored sampling.
- Per-box shade ramps: desk, PVM, and Master System each carry their own gradient; the Master System is on a charcoal ramp.
- Monitor power-on settle flicker fixed.

## Asset viewer

- Composites (desk set, console) are first-class viewer entries; every asset opens the same way.
- Size control moved to the bare d-pad.

## Movement and audio

- Footsteps require actual displacement: walking into a wall no longer steps in place. Head-bob cadence follows the footstep rate, including sprint.
- Exit passage: dedicated corridor slide loop at native rate; landing scuff unchanged; both slide levels doubled.
- Crouched movement replaces footsteps with a drag sound.
- Walking into a bulkhead ducks automatically and passes through at walking speed. Crawl spaces still require the crawl.

## Master System

- Boot menu screen: TEST PATTERN banner wipe, startup chime reversed.
- TEST PATTERN: the minigame presents as a diagnostic screen; the level name appears in the title. Procedurally generated levels supply their own name, so each reads as its own specimen.
- Generative PSG score: a sparse G-minor melody with an echo channel and a walking bass, seeded from the level so no two play the same line.
- The title card no longer shows leftovers from a previous boot.

## Master System on the monitor

- Activating the desk console boots the Master System on the PVM itself. The title card and its chime play in the room, on the monitor, and after a short beat the screen takes over full-screen.
- The handoff keeps the same program running, so the chime and music do not restart across the cut.
- The 32X mix fades out while the Master System has the stage and returns on exit.
