BACKROOMS 32X — wall texture hand-off
=====================================

MAKING A SPRITE INSTEAD? (a standee, a poster, a wall sign, a prop)
  You don't need any of this. Open the map editor at
  https://backrooms-32x-project.fly.dev/ — the "Add a sprite" panel walks
  you through it: upload a transparent PNG, place it in a map, walk up to
  it in first person, submit. Full guide: SPRITES.md in the repo root.

  THIS folder is for editing the yellow WALLPAPER itself — the repeating
  texture on every wall. Different beast, rules below.

WHAT'S IN THIS FOLDER
  SOURCE_walltile.jpg          the original photo the shipped wallpaper
  SOURCE_square_composite.jpg  was made from — edit THESE for your variant
  wall_hi_64_ingame.png        the 64x64 texture exactly as the game shows it
  wall_hi_64_ingame_4x.png       (same thing, 4x bigger for easy viewing)
  wall_hi_64_levels_4x.png     the 5 brightness steps the game actually
                               stores, shown as grays
  wall_hi_64_tiled2x2_big.png  the tile repeated 2x2 — check your edit here
  wall_lo_16_*                 the 16x16 far-away version of the same

THE THREE RULES
  1. Work at any square size (512x512 is comfortable). Your image gets
     shrunk to 64x64, so detail must be BOLD to survive — for a torn edge,
     think chunky ripped-paper shapes, not hairline cracks.
  2. The game keeps only 5 brightness steps and NO color — the yellow is
     added separately. Your image's light-and-dark becomes the pattern.
     Strong contrast survives; subtle gradients vanish.
  3. The tile repeats 4 times across every wall, endlessly. All four edges
     must wrap seamlessly (compare with the tiled2x2 preview), and anything
     distinctive repeats every quarter-wall — one dramatic tear becomes
     visible wallpaper-pattern. Keep tears small and organic, OR tell us
     you want it as a separate "torn wall" style mappers can paint onto
     specific walls — we can set that up.

SENDING IT BACK
  Just send the edited square image (PNG preferred) back however you got
  this folder — we'll take it from there. If you're comfortable with
  GitHub, a pull request adding it to images/ works too.
