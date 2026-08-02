BACKROOMS 32X — wall texture hand-off
=====================================

WHAT'S HERE
  SOURCE_walltile.jpg          the original photo the shipped wallpaper was
  SOURCE_square_composite.jpg  baked from — edit THESE for a torn variant
  wall_hi_64_ingame.png        the 64x64 texture exactly as the ROM shows it
  wall_hi_64_ingame_4x.png       (4x nearest for easy viewing)
  wall_hi_64_levels_4x.png     the raw 5 darkness levels as grays — this is
                               what the engine actually stores
  wall_hi_64_tiled2x2_big.png  the tile repeated 2x2 — check your edit here
  wall_lo_16_*                 the 16x16 far-wall version of the same

WHAT RESOLUTION TO WORK AT
  Any square size you like — 512x512 is comfortable. The bake LANCZOS-resizes
  down to 64x64 (near walls) and 16x16 (far walls), so:
  - Detail smaller than ~1/64 of your canvas disappears. A "torn" edge needs
    to be BOLD — think chunky ripped-paper shapes, not hairline cracks.
  - The texture stores only 5 DARKNESS levels (no color!). The wall's yellow
    comes from the palette; your image's luminance becomes shading. High
    contrast survives; subtle gradients don't.
  - It TILES 4x4 across every wall cell and continues cell to cell, so all
    four edges must wrap seamlessly. Check against the tiled2x2 preview.
    A torn patch will therefore repeat every quarter-wall — a single
    dramatic tear reads as a pattern, so keep tears smallish/organic, or
    we can bake a torn variant as a second wall style instead.

HOW IT GETS IN
  Send the edited square image back (or PR it to images/); the bake is:
    python3 tools/bake_wall.py 64 64 sh_src/wall_tex_hi.h --src your.png
    python3 tools/bake_wall.py 16 16 sh_src/wall_tex.h    --src your.png
