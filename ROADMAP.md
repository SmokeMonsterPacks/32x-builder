# Backrooms 32X — roadmap of unresolved / future work

Each item below is something we attempted, hit a wall on, or deliberately
deferred. Listed in roughly the order I think they'd be productive to
revisit.

## Story / triggered lighting  (requested 2026-07-24)

**Status:** feature request — not started. Mike wants lighting to become a
narrative/scripting tool, not just static ambiance.

The vision, smallest-first:

- **Walk-on trigger lights.** A cell (or a tagged space) is dark until the
  player steps onto/into it, then its light turns on. The first building block:
  event-driven, per-cell light activation tied to player position.
- **Sequenced cell lighting.** Once single-cell triggers work, chain them: a
  corridor that lights up cell-by-cell ahead of (or behind) you, timed light
  sequences, a room that powers on in a pattern. Lighting as a scripted beat.
- Beyond that: "lighting all around" — this is meant to grow into a whole
  system (reactive dark rooms, flicker-to-life, follow/lead lights, maybe
  scripted light cues that pair with the audio work).

**Infra we already have to build on:** `cell_light[MAP_H][MAP_W]` (per-cell
light level + the CELL_DARK top-bit for dark rooms), `init_lights` that seeds
it per map, and the gen-gated secondary purge so both CPUs stay coherent when
it changes ([[reference_secondary_cpu_cache_coherency]]). The NEW part is a
*runtime* mutation path: mark cells for trigger/sequence activation at map load,
then flip their `cell_light` (and re-fog / re-shade) as the player crosses
tiles, with the secondary purging the changed lines. A small per-map "light
script" (cell + condition + timing) is the likely data model — mirrors how the
crawlspace/dark-room rects are authored per map.

Open design questions to settle when we start: authored (per-map light-script
table, like decals/dark rects) vs procgen-tagged; instant vs flicker-to-life
ramp; one-shot vs re-armable; how a sequence is timed (frame counter vs
player-crossing chained triggers).

## Visual / atmospheric

### Ceiling lights as actual grid-tile illumination
**Status:** ✅ done — scanline trapezoid fill from 4 projected corners
of each axis-aligned ceiling tile. Per-edge slope precomputed once,
per-row left/right reconstructed by linear interpolation, fill row
with z-test against walls. Plus a 2-bulb fluorescent troffer pattern
inside each tile (dim outer frame, two bright bulb bands, medium
gap), and a grid of fixtures populated at init from `world_map` at
every 2nd cell. Per-light flicker stays as a brightness offset on top
of the bulb pattern, gated by the LIGHTING_FLICKER toggle in the menu.

### SH-2 dual-CPU split
**Status:** ✅ done — multiple iterations. Current architecture is the
column-split: each CPU owns a vertical half of the screen and does its
own clear + ceiling grid + carpet + walls in parallel; one COMM4 sync
per frame before sprites/lights. Shared state uses the `| 0x20000000`
cache-through alias as planned. The hand-rolled SH-2 asm wall pixel
loop (see Perf section) sits inside this split.

## Level / geometry

### Wall-profile system — carved architecture ("architecture by omission")  (2026-07-25)
**Status:** designed, not built. The cheap-path answer to arches/openings/doorways
— distinct from the MAP_RES fork (that's for free-standing thin geometry + floor
heights). Everything here stays on the grid-wall DDA path, so **zero partition
tax**; the only new per-frame cost is "continue the ray past a carved cell,"
which void/exit cells already pay.

**The primitive:** a wall cell stops being a boolean (wall/void/exit) and carries
a small per-cell **profile** — where the solid part of the column lives:
- floor-anchored partial (floor->H) — ALREADY EXISTS as `part_height` (see-over dividers)
- **ceiling-anchored partial (ceiling->D) — the missing inverse; the key new primitive**
- mid-carve (header + opening + optional sill)

Reuses already in the tree: void/exit ray-continue (template for "draw a band,
keep marching"), the crawlspace bulkhead/mouth cap (a ceiling-anchored band
draw), `FRAME_BASE` (jamb/casing palette), `g_door_target` (swinging door), the
crawlspace eye-height gate (walk-under).

**Staged deliverables (shared foundation first):**
1. **Foundation** — per-cell profile field + column draw clips to it + DDA
   continues past a partial (void-cell path is the template). Everything below rides this.
2. **Bulkhead** — ceiling-anchored partial (hang a soffit/beam, walk under). It's
   `part_height` mirrored top-down; highest reuse, first to land. NOTE: we do NOT
   currently do top-down — we do floor-up (`part_height`) and lowered *ceilings*
   (`CEIL_H`). This is the new one.
3. **Arch** — walk-through opening: ceiling-anchored header + open-below + passable
   cell. Flat lintel first; curve is polish.
4. **Doorway** — mid-carve + jamb REVEAL (draw the wall-thickness side faces in
   `FRAME_BASE` so the opening has depth = "the interior cell") + the door leaf
   recessed in it. Ties into the "different door types" list.
5. **Curve LUT** — round/pointed arch tops (polish).

### Bigger / more authentic map
**Status:** ✅ done — settled on a hand-tuned 32×32.

Tried three map sources in order: original Sketchfab lobby (22×22,
felt like "one big room"), movie.blend extraction at 32×32 (lost all
the doorways), movie.blend at 64×64 (had doorways but felt like one
long corridor in any direction). Final answer:
`tools/gen_backrooms_map.py` — a hand-designed 32×32 with five
distinct zones (NW office cubicles, NE nested rooms, central band
with pillars, SW twisty maze, SE lounge with stub walls) all meeting
at the spawn. Plays well on real hardware.

### In-game settings menu
**Status:** ✅ done — START opens a nix-terminal-style overlay with
AMBIENCE and FOOTSTEPS sliders (0–100). Implementation in
`sh_src/menu.c` + hand-rolled 8×8 bitmap font in `sh_src/font.c`. The
audio decoupling (ambient slider scales buzz/neon/hello but not steps)
ships with this. Still in the not-done pile from the original spec:
turn/walk speed, view distance, head bob amplitude — none are blocking
anyone, easy to add later.

### Sega 32X boot logo
**Status:** designed by research agent, not implemented. Candidate
catalog in the session transcript; recommended option is **Candidate
2: palette-cycle shimmer** — static 32X panel (orange/yellow rounded
rect + blue SEGA + red 32X), animated yellow border via CRAM rotation
(same trick `raycast_shimmer()` already uses on the lights). ~190 LOC,
~70KB ROM. Natural pairing with the start menu below.

### Start menu / map selection
**Status:** planned

Boot screen that lets the player choose between the shipped hand-tuned
map and a procedurally-generated one. The procedural option leans
directly into the "AI dreamt this place" Backrooms vibe — each run is
a different layout, never the same place twice.

#### Menu UX

Title screen (`MARS BACKROOMS` or similar), three options:
1. **EXPLORE THE LOBBY** — boots into the hand-tuned 32×32 from
   `gen_backrooms_map.py`.
2. **WANDER A NEW PLACE** — generates a fresh 32×32 layout at boot
   using a PRNG seeded from the framecounter at the moment START is
   pressed. Same player presses START at slightly different times →
   never the same map.
3. **CONTINUE WHERE I WAS** (later) — restore last seed + position
   from save RAM.

D-pad to cycle, START to confirm. Background of the menu = static
ceiling-grid render with the title overlaid; reuse the raycaster's
existing palette and texture pipeline so it's almost free to render.

#### Procedural-generator design (the meat)

The pure-random "carve some rooms, drill some corridors" approach
will produce maps that feel like a maze game, not Backrooms. To
preserve the iconic Backrooms feel we want **zone-templated
generation**:

- Map is partitioned into a grid of 4 quadrants (16×16 each).
- Each quadrant gets assigned ONE template at random from a pool:
  `office_cubicles`, `nested_rooms`, `twisty_maze`, `pillar_lounge`,
  `long_hallway`, `dead_end_warren`, `false_partitions`, etc.
- Each template is a procedural sub-generator with its own parameters
  (e.g. cubicle grid 3×3 or 4×3, nested rooms 2 or 3 levels deep).
- A central spawn vestibule is carved in the middle, with four
  doorways into the four quadrants.
- A connectivity validator floodfills from spawn and re-rolls if any
  quadrant is unreachable. Cheap on a 32×32 grid (1024 cells).

Result: every map FEELS like Backrooms (because every zone is
recognizably Backrooms-y) but no two maps are the same.

#### Implementation notes

- PRNG: 32-bit xorshift, fits in ~10 SH-2 instructions, deterministic
  from seed → same seed = same map (good for "share a seed" and for
  debugging weird layouts).
- Seed source: free-running framecounter latched at START button press
  on the menu. ~60 unique seeds per second of menu time.
- Memory cost: zero — generation runs at boot, writes into the same
  `world_map[32][32]` buffer the hand-tuned map currently lives in.
  We'll need to change `world_map` from `const` to mutable plus a
  separate `const` table of templates.
- Time cost: budget ~50ms one-time at boot. Negligible.
- Save seed in save RAM so "Continue" can re-generate the exact same
  map without storing the whole grid.

#### Stretch: AI-generated map oracle

Way down the road — host-side tool that asks an LLM for "describe a
Backrooms layout as a 32×32 grid" and bakes the output into a
template pool the cart can pick from. Lets us seed the procedural
templates with actual AI imagination instead of hand-design. Pure
roadmap dreaming; needs nothing else first.

#### Procgen tuning knobs (post-redesign refinement)

**Status:** deferred — set up after the building-blocks-based generator
ships, to make the "feel" tunable without code changes.

Once the new generator is in place (spine corridor + side rooms +
clustered room pairs + pockets + partitions), build a small constants
block at the top of `procgen.c` that lets us dial the procgen "feel"
in one place:

- `PROC_NUM_SIDE_ROOMS`        — how many rooms attach to the spine
- `PROC_NUM_CLUSTER_PAIRS`     — connected room-pair count
- `PROC_ROOM_SIZE_MIN/MAX`     — room dimensions in cells
- `PROC_POCKET_DENSITY`        — fraction of corridor cells getting an alcove
- `PROC_PARTITION_DENSITY`     — fraction of rooms ≥ 4×4 getting a partition
- `PROC_CORRIDOR_WIDTH`        — 1 or 2 cells wide
- `PROC_SPINE_ORIENTATION`     — horizontal / vertical / both
- `PROC_PILLAR_BUDGET`         — explicit cap; default 0 (zero stray pillars)

Eventually wire these into the in-game menu's TUNING tab so the
player can A/B procgen feels without rebuilding. For now just expose
them as compile-time `#define`s once the generator is shipping —
makes A/B testing in iteration cycles ~one line of code each.

Also worth adding when this lands: a "validate / re-roll" pass that
checks min walkable cell count (e.g. >= 200), max isolated pillar
count (e.g. <= 4), and floodfill reachability from spawn. Re-rolls
the seed if any check fails. Cheap on a 1024-cell grid.

### Backrooms couch (8-angle directional billboard)
**Status:** designed, not implemented. Deep-research agent landed a
concrete recommendation.

A mustard-yellow vinyl 3-seater sofa in the SE lounge and NE nested
rooms — matches the "70s/80s waiting-room furniture, condition
slightly worn but not destroyed" canonical Backrooms vocabulary.
Mustard yellow uses the existing wall family palette so the couch
reads as "the wallpaper color but stained darker."

**Technique: 8-angle billboard with Doom-style mirroring** (4 unique
front-half textures + 1 side, mirror for back half). Same per-frame
cost as the existing neanderthal standup (~1.5 ms per visible couch).
Multi-angle gives "appears to rotate as you circle it" without the
~9-18 ms cost of true sprite stacking — which would bust the budget
on 23 MHz SH-2.

**Files to add:**
- `sh_src/couch_tex.h` — 5 × 64 × 32 × 1 byte = 10 KB of texture data
  (palette-indexed, baked by a new `tools/bake_couch.py`)
- New `couch_t` struct + `couches[]` array in `raycast.c` next to
  the existing `standups[]`
- `draw_couches` function modeled on `draw_standups`, with angle
  quantization (atan2 → 8 slots, mirror slots 5/6/7 → 3/2/1)
- `COUCH_BASE = 72` in the palette (8 shades, slot 72-79; existing
  layout has slot 80+ free)
- `point_in_couches(px, py)` AABB collision wrap in `player_update`

**Asset pipeline:** Blender or MagicaVoxel low-poly model →
fixed-camera render at 8 yaw angles → `tools/bake_couch.py`
quantizes each to the 8-color couch palette → emits `couch_tex.h`.
Mirrors the existing `tools/extract_floorplan.py` pattern.

**Scalability:** texture data is shared, only per-instance placement
adds bytes (~20 bytes per couch). 5-6 visible couches comfortable
per frame; 10+ would tighten budget. In practice 1-2 per major room
(~4-8 total in the map but usually 0-2 visible).

Full report from the deep-mine agent is in the session transcript;
the recommendation maps cleanly to the existing infrastructure with
~150 LOC + the texture data.

### Sprites populating the rooms
- Folding chair cardboard cutout (sprite pipeline already in place)
- Vent grate (could be drawn on a wall face or a free-standing standup)
- TV / pile of papers / other office detritus

### Door/exit silhouette
Single dark rectangle on one wall hinting at "the way out."

### Mind-bending anomalies
- ✅ partial — distant fluorescent strobe on walls past `FOG_RAMP_DIST`.
  Per-cell hash + shared frame counter make distant dark cells
  occasionally flicker to dim-yellow ("a fluorescent panel trying
  to start in the haze"). Lives in `draw_walls`.
- Watcher figure REMOVED — the silhouette standup that vanished on
  approach was alluding to something we hadn't built. The infra
  (`standup_t.silhouette` field + draw_standups branch + per-standup
  vanish-on-distance check) is still in place for future reuse.
- Open: occasional 1-frame full-screen palette shift (chromatic
  glitch)
- Open: corridor that loops you back where you started (loop-warp
  zone)

## Audio

### PWM ambient fluorescent drone
✅ done — secondary SH-2 mixes a 30s buzz loop + occasional neon sting on
the PWM mono channel via DMA1 ping-pong buffers. Plus the Voyager
Golden Record neanderthal-positional hello (distance-attenuated) and
carpet footsteps gated on `is_walking`. See `sh_src/sound.c`.

### Footstep sounds on carpet
✅ done — shipped with the audio buildout. Sample baked from
`sound/ES_Footsteps...`; runtime `step_volume` is independent of the
ambient slider.

### Kane Parsons-style ambient score
After ambient drone is in place. Slow swells, sub-bass, distant rumbles.
Still pending — would layer over the existing buzz/hum bed.

## Performance

### Partition parity — deferred items + the cost wall  (2026-07-24)
**Status:** parity batch banked (commit "Partition parity: near-slab LOD…").
The slab/overlay path is a second-class render citizen: it re-implements, by
hand, every system the wall path gets for free. This session ported the
high-impact ones (texture LOD, per-column LOD, see-over PART_TOP, cap-plane
occlusion, topple line-of-sight). **Deferred, low-impact, cosmetic — do NOT
invest pre-pivot:**
- **#8 SEAMS** — overlay records no silhouette into `seam_top/seam_bot`, so
  counter top edges staircase at coarse res and the smoother can chew the wall
  edge behind a slab.
- **#9 DITHER** — slab bands never dithered (keyed on the #8 seam buffers).
- **#12 dark-room / crawlspace shade** — overlay shade block lacks the
  `g_lowceil` / `g_dark` modifiers, so a counter glows like a lit plank on dark
  maps / under low headers.
- **Top-cap lip dropout** — the 0.35-cell lip clamp (`raycast.c` countertop
  block) foreshortens to sub-pixel, so half-height counters go topless at
  distance. Relaxing it reintroduces "THE seam" (build-127). Needs a real fix,
  not just a bigger clamp.
- **Topple-through-end-cap** — `standup_wall_reach` marches only the fall
  centerline; a glancing topple parallel to a counter's end-cap isn't capped and
  the flat body draws beside/through the thin end slab. Test-fixture edge case.
- **#2 `cap<0` guard** (1-line safety), **#11 vert-mode branch** (wasted odd-row
  writes, invisible), **#13 far strobe** (cosmetic).

**The wall behind all of it:** measured `F:` in a slab-heavy view is F:05
standing / F:09 walking — *below the 15 floor regardless*, and near-independent
of standup count (partitions dominate, the 3 neanderthals added ~0 standing).
Parity makes partitions **correct, not fast**. Getting above the floor with
partitions in view needs the structural fork, not more parity patches:
decouple grid resolution from cell size (`MAP_RES`) so a thin divider becomes a
first-class DDA cell and inherits depth/LOD/occlusion natively — cost is DDA
steps + map RAM. See the doom-staircase / major-pivot discussion.

### Mine the Doom 32X Resurrection codebase for techniques
**Status:** strategic resource — deep-mine report landed; concrete
adopt-list below in ranked-ROI order.

[viciious/d32xr](https://github.com/viciious/d32xr) is years of
optimization work to make Doom run smoothly on the actual 23 MHz
SH-2s. We've already borrowed:
- the `| 0x20000000` cache-through SDRAM alias for shared state
- the COMM4 doorbell + COMM6 arg-word convention
- the `MARS_SECCMD_*` command enum + secondary polling dispatcher pattern

#### Ranked adopt-list (from the deep-mine research agent)

**1. SH-2 hardware DIVU latency-hiding for `1/perpDist`.** ✅ done.
Wired up via `divu_start_u32` / `divu_read` in `sh_src/sh2_asm.h`.
Wall column code starts the divide, then computes `wall_shade` +
texture coordinate setup during the 39-cycle latency.

**2. Hand-roll SH-2 asm wall column inner draw loop.** ✅ done.
4-pixel-per-iter inline asm block in `draw_walls` — keeps `tex_pos`,
`p`, `shade_lut`, `step`, `mask` in registers, uses indexed byte load
via `@(R0,Rm)` and `dt`/`bf` for the count-down. Measured on
hardware: primary half-render time dropped from 44000→33500 FRT ticks
(24%), secondary from 44000→25000 (43%). Frame crossed a vsync boundary
in the wall scene (15fps → ~20fps).

**3. Work-stealing wall split via COMM6.** ✅ done, then reverted.
Implemented per d32xr's pattern. Reverted in favor of the column-
ownership split (each CPU owns a half), which has no per-column TAS
overhead and gives natural load balance since walls cluster predict-
ably with view direction. Kept the COMM6 infrastructure in shared.h
for future use.

**4. GBR thread-local-storage for per-CPU state.**
Still open. Now unblocked by #2 — the inline asm in `draw_walls` would
benefit. Would let each CPU's `shade_lut`/`screen_w`/`tex_h_mask`
fetches become single `mov.l @(disp,gbr),r0` instead of stack-passed
arguments. Estimated ~5% additional inner-loop win.

**5. Compact sine LUT.** ✅ done. `sh_src/sin_table.h` is a
`uint16_t[256]` quarter-wave; `COS_FX`/`SIN_FX` macros do the
quadrant folding via `swap.w` + sign flip.

**6. Cache-line invalidate macro.** ✅ done. `Mars_ClearCacheLine`
and `Mars_ClearCacheLines` in `sh_src/sh2_asm.h`.

**7. SH-2 DMA + completion-interrupt audio mixer.** ✅ done.
`sh_src/sound.c` mixes a ping-pong `amb_pwm_buf[2][1024]` on the secondary
(64 ms per buffer + amb_pump checkpoints between the secondary's render
passes — so render chunks can't starve the ping-pong; underruns counted
on the HUD as AU:, old 16 ms arm kept as a menu A/B);
DMA1 streams the active buffer to `MARS_PWM_MONO`; `amb_dma_handler`
swaps + re-arms. Mixes buzz + neon + positional hello + footsteps.

**8. Other clever tricks worth piecemeal adoption:**
- `0xFFFFFF00` 2-instruction materialization — used in `divu_*` helpers
- `muls.w` over `dmuls.l` — opportunistic; `mul_hi32_s` helper uses
  `dmuls.l` where 64-bit precision is actually needed
- 4bpp textures with pre-swapped nibbles — not adopted; our 8bpp
  framebuffer already fits the use case
- Sort drawables by texture identity — not adopted; standup count
  is small enough that the cache hit pattern doesn't dominate

#### Critical correction from the research

SH-2 cache is **write-through**, not write-back. So writes via the
cache-through alias AND writes via the cached alias both reach
memory immediately. The "explicit flush before another CPU reads"
concern from our earlier work is unfounded for WRITES — flushes
only matter when one CPU previously *read* a value into its cache
and needs the next read to see the *other* CPU's update.

This means we can be smarter: shared-write-only state can use the
cached alias for speed, only the reader side needs occasional
`Mars_ClearCacheLine` calls.

### Texture mipmaps for walls
✅ partial — distance-based LOD swap between 16×16 lo-res and 64×64
hi-res wall_tex, threshold `WALL_LOD_THRESHOLD = FX(2)`. Hi-res is
column-major for cache-friendly per-stripe scans. Same LOD pattern
also applies to the neanderthal sprite (32×64 lo-res ↔ 128×256
hi-res column-major, threshold `FX(3)`). Three or more bands could be
added later; not currently a bottleneck.

### Floor-cast carpet at proper LOD
Still open — currently every-4th-column stamp covers the full bottom
half. Could compress further with a sparser near-row pattern and skip
the horizon-band entirely (already half-done — we skip max-dark rows).

## Polish

### Higher-res chair billboard via `hero_scratch` reuse
The directional chair sprite is baked at 56px (`tools/bake_dir_sprites.py
--height`). An 84px bake is crisper and the sprite data is free (it's
`const`/ROM), but the viewer's `chair_pv` decode scratch scales with
sprite size and, as a static `.bss` buffer, steals from the primary
stack. At 84px it left only ~1100 bytes of stack headroom (the same
class of silent-overflow trap as the audio 2×2048 buffer), so we kept
56px. It's academic at the current 3.5-cell LOD swap (chair is ~24px on
screen there, already 2.3× oversampled), but if the perf pass ever
pushes the swap *closer* the billboard gets big enough to want the
detail. Safe path when we do: host `chair_pv` in the idle `hero_scratch`
buffer (71 KB, dark during the asset viewer) instead of its own static —
zero new `.bss`, full-res sprites. Cost: couples box_hero into the
renderer with a "hero_scratch is free during the viewer" lifetime
assumption. The bake tool already emits `CHAIR_DIR_WMAX` so the scratch
auto-sizes to any `--height`.

## Tools / infra

### `make deploy` to MiSTer
✅ done — auto-scp's the .32x to `root@mister.office.local`.

### Squash-before-push workflow
✅ done — agreed protocol: WIP commits stay local, only squashed
commits push to GitHub.

### Blender floor-plan extractor
✅ done — `tools/extract_floorplan.py` runs in headless Blender and
emits both an ASCII visualization and a ready-to-paste C array.

### Wallpaper + sprite baker tools
✅ done — `tools/bake_wall.py` and `tools/bake_neander.py` quantize
PNGs to palette-indexed C headers. Both emit column-major output by
default for SH-2 cache friendliness.

### `make deploy-tv` to second MiSTer
✅ done — `make deploy-tv` SSHes the TV MiSTer (`mister.tv.local`)
and probes `/media/usb0` then `/media/usb1` for the S32X dir before
scp'ing, so USB renumber doesn't break the push.

### FRT-based on-screen profiler
✅ done — top-right overlay shows `T:NNNNN H:NNNNN S:NNNNN` (frame
total, primary half-render time, secondary half-render time) sampled from
SH-2 free-running timer at Φ/32 (1.39μs per tick). Both CPUs init
their own FRT; secondary publishes its delta via `SHARED_UC->secondary_render_ticks`.
Remove the overlay before shipping a release build.
