#!/usr/bin/env python3
"""Bake a COMMUNITY sprite: any transparent PNG -> a palette-indexed standee
the engine, editor and build all understand, in ONE command.

    tools/bake_sprite.py --id traffic_cone --src cone.png --height 0.55

does all three registrations the pipeline needs:
  1. sh_src/spr_<id>_tex.h  — [H][W] row-major texels, 0 = transparent,
     v>0 -> COMM_BASE + (v-1): the shared 16-shade community ramp
     (build_palette in raycast.c; warm gray bright->near-black). One shared
     ramp = zero per-sprite CRAM cost and a unified backrooms look.
  2. registry.json assets.sprites — mount=billboard, decode=offset,
     flags=[standalone]: the codegen (gen_assets.py) then emits its
     sprite_defs[] row and the engine renders it with NO C edits.
  3. registry.json decals.kinds — the next free kind number, so the map
     format, lint, and the web editor all pick it up.

Then `make` regenerates sprite_defs.h + custom_maps.c and the standee is
placeable in any .map (and procgen-able later). The editor's /bake_sprite
route imports bake_image()/emit_header()/registry_entries() from here so the
web upload path and this CLI can never drift.
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MAX_W, MAX_H = 64, 96          # texel caps: a standee, not a wall texture
MAX_TEXELS   = 4096            # ROM budget per community sprite (4 KB)
RAMP_N       = 16              # COMM_BASE ramp length (raycast.c)

# Per-sprite COLOR palettes, the same model every first-party asset uses
# (the door's ramp holds browns AND greens AND white): median-cut quantize
# the upload to up to 7 colors, ordered darkest -> brightest, stored in the
# registry entry and painted into a managed CRAM arena at build time.
#
# WHY 7: the engine's standup shade LUT is vmap[8] — texel 0 is transparent
# and 1..7 index the sprite's ramp; distance fog walks the index DOWN toward
# the dark end (see draw_standups), which is why the luminance ordering is
# load-bearing, not cosmetic.
ARENA_BASE = 144               # first CRAM slot of the community arena
ARENA_END  = 256               # exclusive; 14 sprites x 8 slots fit
PAL_COLORS = 7                 # + texel 0 transparent = the vmap contract


def quantize_palette(img, colors=PAL_COLORS):
    """Median-cut the image's OPAQUE pixels to <= `colors` RGB entries
    (32X 0..31 per channel), ordered darkest -> brightest. Returns
    (palette_31, palette_rgb8) — the 5-bit entries and their 8-bit
    equivalents for previews."""
    from PIL import Image
    px = img.load()
    W, H = img.size
    opaque = [px[x, y][:3] for y in range(H) for x in range(W)
              if px[x, y][3] >= 128]
    if not opaque:
        opaque = [(128, 128, 128)]
    strip = Image.new("RGB", (len(opaque), 1))
    strip.putdata(opaque)
    q = strip.quantize(colors=colors, method=Image.Quantize.MEDIANCUT)
    flat = q.getpalette()[:colors * 3]
    cols = {tuple(flat[i:i + 3]) for i in range(0, len(flat), 3)}
    # 8-bit -> 5-bit CRAM, dedupe (quantize can emit near-duplicates that
    # collapse at 5 bits), order by luminance ascending (dark first).
    seen, pal31 = set(), []
    for r, g, b in sorted(cols, key=lambda c: c[0] * 299 + c[1] * 587 + c[2] * 114):
        c31 = (r * 31 // 255, g * 31 // 255, b * 31 // 255)
        if c31 not in seen:
            seen.add(c31)
            pal31.append(c31)
    pal31 = pal31[:colors]
    pal8 = [tuple(c * 255 // 31 for c in e) for e in pal31]
    return pal31, pal8

# 4x4 Bayer matrix for the optional ordered dither (0..15 thresholds).
BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def orient(img, rotate=0, mirror=False):
    """Pre-bake orientation: rotate CW in 90-degree steps, then mirror
    left/right. Shared by the CLI and the editor's /bake_sprite route."""
    from PIL import Image
    img = img.convert("RGBA")
    t = {90: Image.ROTATE_270, 180: Image.ROTATE_180, 270: Image.ROTATE_90}
    if rotate in t:
        img = img.transpose(t[rotate])
    if mirror:
        img = img.transpose(Image.FLIP_LEFT_RIGHT)
    return img


def bake_image(img, out_w, dither=True, max_h=MAX_H, pal8=None):
    """PIL RGBA image -> (rows, W, H, pal31, pal8): texel 0 transparent,
    1..7 index the sprite's own median-cut palette (darkest first). Crops
    to the alpha bbox, scales to out_w keeping aspect. Pass pal8 to reuse
    a palette already quantized (the hi-res bake shares the lo-res one so
    both LODs decode through the same CRAM block)."""
    from PIL import Image
    img = img.convert("RGBA")
    box = img.split()[3].getbbox()
    if box:
        img = img.crop(box)
    w0, h0 = img.size
    out_h = max(1, round(h0 * out_w / max(1, w0)))
    if out_h > max_h:
        out_h = max_h
        out_w = max(1, round(w0 * out_h / max(1, h0)))
    img = img.resize((out_w, out_h), Image.LANCZOS)
    if pal8 is None:
        pal31, pal8 = quantize_palette(img)
    else:
        pal31 = [(r * 31 // 255, g * 31 // 255, b * 31 // 255)
                 for r, g, b in pal8]
    px = img.load()
    rows = []
    for y in range(out_h):
        row = []
        for x in range(out_w):
            r, g, b, a = px[x, y]
            if a < 128:
                row.append(0)
                continue
            best, bd = 1, 1 << 30
            for i, (pr, pg, pb) in enumerate(pal8):
                d = (r - pr) * (r - pr) + (g - pg) * (g - pg) + (b - pb) * (b - pb)
                if d < bd:
                    bd, best = d, i + 1
            row.append(best)
        rows.append(row)
    return rows, out_w, out_h, pal31, pal8


def emit_header(sprite_id, rows, W, H, base="COMM_BASE", hi=False):
    """Texture header. Lo-res is ROW-major [H][W] (neanderthal-style); the
    optional hi-res variant is COLUMN-major [W][H], matching the engine's
    LOD path (sequential bytes down a sprite column)."""
    suffix = "_hi" if hi else ""
    up = "SPR_%s_TEX%s" % (sprite_id.upper(), suffix.upper())
    L = ["#ifndef %s_H_INCLUDED" % up,
         "#define %s_H_INCLUDED" % up,
         "",
         "#include <stdint.h>",
         "",
         "/* AUTO-GENERATED by tools/bake_sprite.py — community sprite '%s'%s." % (
             sprite_id, " (hi-res LOD)" if hi else ""),
         " * 0 = transparent, v (1..7) -> CRAM %s + v: the sprite's OWN" % base,
         " * median-cut palette (registry 'pal', darkest first). %s */" % (
             "Column-major [W][H] like the neanderthal hi-res." if hi
             else "Row-major like the neanderthal lo-res."),
         "#define %s_WIDTH  %d" % (up, W),
         "#define %s_HEIGHT %d" % (up, H),
         ""]
    if hi:
        L.append("static const uint8_t spr_%s_tex%s[%s_WIDTH][%s_HEIGHT] = {"
                 % (sprite_id, suffix, up, up))
        for x in range(W):
            L.append("    {" + ",".join(str(rows[y][x]) for y in range(H)) + "},")
    else:
        L.append("static const uint8_t spr_%s_tex[%s_HEIGHT][%s_WIDTH] = {"
                 % (sprite_id, up, up))
        for row in rows:
            L.append("    {" + ",".join(str(v) for v in row) + "},")
    L += ["};", "", "#endif", ""]
    return "\n".join(L)


def next_arena_base(reg):
    """First free 8-slot CRAM block in the community arena. Deterministic
    from the registry alone, so the bake (CLI or web) can assign it and the
    codegen just paints what the registry says."""
    used = [s["base"] for s in reg["assets"]["sprites"]
            if isinstance(s.get("base"), int)]
    base = ARENA_BASE
    while base in used:
        base += 8
    if base + 8 > ARENA_END:
        raise SystemExit("community CRAM arena is full (%d sprites) — an "
                         "existing sprite must retire first"
                         % ((ARENA_END - ARENA_BASE) // 8))
    return base


def registry_entries(reg, sprite_id, W, H, world_h, pal31, author="",
                     mount="billboard", z=0.5, hi=False):
    """Compute (sprites_entry, kinds_entry). Kind = the next number after
    every existing decal kind, so sprite_defs stays kind-indexed (gen_assets
    pads any non-sprite kinds, e.g. exit_hole). Two mounts:
      billboard — free-standing standee like the neanderthal: camera-facing,
                  stands on the floor, collides. z ignored.
      wall      — painted FLAT on a wall face like the outlet: placed on a
                  wall edge, z = the plate's centre height up the wall.
    """
    kind = max(k["kind"] for k in reg["decals"]["kinds"]) + 1
    world_hw = round(world_h * (W / float(H)) / 2.0, 4)
    flags = ["standalone"] if mount == "billboard" else []
    sprite = {
        "id": sprite_id, "kind": kind, "mount": mount,
        "tex": "spr_%s_tex.h" % sprite_id, "sym": "spr_%s_tex" % sprite_id,
        "base": next_arena_base(reg), "pal": [list(c) for c in pal31],
        "world_h": world_h, "world_hw": world_hw,
        "z": (z if mount == "wall" else 0.0),
        "transparent": 1, "decode": "offset",
        "flags": flags,
    }
    if hi and mount == "billboard":
        sprite["tex_hi"] = "spr_%s_tex_hi.h" % sprite_id
        sprite["sym_hi"] = "spr_%s_tex_hi" % sprite_id
        flags.insert(0, "lod")
    # Everything baked through this path is a community upload: tier it so the
    # flagship ROM never compiles it in (gen_assets --profile core drops it).
    # Promotion is a one-word registry edit when the maintainer wants it in the
    # main game.
    sprite["tier"] = "community"
    if author:
        sprite["author"] = author
    noun = "standee" if mount == "billboard" else "wall decal"
    kindent = {
        "id": sprite_id, "kind": kind,
        # A billboard's decal z is its CENTRE height, so it must track the
        # sprite's own world height — every first-party asset sits at h/2 (door
        # 0.49 of 0.98, chair 0.19 of 0.375). The old flat 0.45 was the
        # neanderthal's value hardcoded, which floated any standee shorter than
        # a person: the 0.31-high desk hovered nearly its own height off the
        # floor. Wall mounts still take the caller's plate height.
        # (`world_h` — the parameter's name. This read `height`, so EVERY
        # free-standing bake died with a NameError: the editor's standee upload,
        # the exact path a contributor takes, 500'd on submit.)
        "z": (z if mount == "wall" else round(world_h / 2.0, 4)), "glyph": "⧉",
        "color": "#b8b0a4",
        "label": "%s (community %s)" % (sprite_id.replace("_", " "), noun),
    }
    if mount == "billboard":
        kindent["standalone"] = True
    return sprite, kindent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--id", required=True,
                    help="C-identifier name, e.g. traffic_cone")
    ap.add_argument("--src", required=True, help="source image (PNG/WebP/JPG)")
    ap.add_argument("--height", type=float, default=1.0,
                    help="world height in cells (1.0 = about a person)")
    ap.add_argument("--width", type=int, default=48,
                    help="texel width (aspect keeps height; caps %dx%d)" % (MAX_W, MAX_H))
    ap.add_argument("--author", default="", help="credited in the registry")
    ap.add_argument("--mount", choices=["billboard", "wall"], default="billboard",
                    help="billboard = free-standing standee (neanderthal-style); "
                         "wall = painted flat on a wall face (outlet-style)")
    ap.add_argument("--z", type=float, default=0.5,
                    help="wall mount only: plate centre height up the wall 0..1")
    ap.add_argument("--hi", action="store_true",
                    help="also bake a 2x hi-res LOD texture (billboard only; "
                         "~5x the ROM cost — for hero pieces)")
    ap.add_argument("--rotate", type=int, choices=[0, 90, 180, 270], default=0,
                    help="rotate the source clockwise before baking")
    ap.add_argument("--mirror", action="store_true",
                    help="flip the source left/right before baking")
    ap.add_argument("--no-dither", action="store_true")
    ap.add_argument("--dry", action="store_true",
                    help="print what would be written, touch nothing")
    args = ap.parse_args()

    if not re.match(r"^[a-z][a-z0-9_]{1,15}$", args.id):
        sys.exit("--id must be 2-16 chars of [a-z0-9_], starting with a letter")
    if not (0.1 <= args.height <= 1.0):
        sys.exit("--height must be 0.1..1.0 cells (the room is 1.0 tall)")
    if not (8 <= args.width <= MAX_W):
        sys.exit("--width must be 8..%d texels" % MAX_W)

    reg_path = os.path.join(ROOT, "registry.json")
    reg = json.load(open(reg_path))
    ids = ([s["id"] for s in reg["assets"]["sprites"]] +
           [k["id"] for k in reg["decals"]["kinds"]])
    if args.id in ids:
        sys.exit("id %r already exists in the registry" % args.id)

    from PIL import Image
    src_img = orient(Image.open(args.src), args.rotate, args.mirror)
    rows, W, H, pal31, pal8 = bake_image(src_img, args.width,
                                         dither=not args.no_dither)
    if W * H > MAX_TEXELS:
        sys.exit("baked %dx%d = %d texels exceeds the %d budget — "
                 "use a smaller --width" % (W, H, W * H, MAX_TEXELS))
    hi = args.hi and args.mount == "billboard"
    rows_hi = W_hi = H_hi = None
    if hi:
        rows_hi, W_hi, H_hi, _p31, _p8 = bake_image(
            src_img, W * 2, dither=not args.no_dither,
            max_h=MAX_H * 2, pal8=pal8)      # SAME palette as the lo-res
    tex_path = os.path.join(ROOT, "sh_src", "spr_%s_tex.h" % args.id)
    sprite, kindent = registry_entries(reg, args.id, W, H, args.height,
                                       pal31, args.author, args.mount,
                                       args.z, hi)
    base = sprite["base"]
    header = emit_header(args.id, rows, W, H, base)
    if args.dry:
        print("would write %s (%dx%d%s) and register kind %d"
              % (tex_path, W, H,
                 " + hi %dx%d" % (W_hi, H_hi) if hi else "",
                 sprite["kind"]))
        print(json.dumps(sprite, indent=1))
        return
    open(tex_path, "w").write(header)
    if hi:
        open(os.path.join(ROOT, "sh_src", "spr_%s_tex_hi.h" % args.id),
             "w").write(emit_header(args.id, rows_hi, W_hi, H_hi, base, hi=True))
    reg["assets"]["sprites"].append(sprite)
    reg["decals"]["kinds"].append(kindent)
    json.dump(reg, open(reg_path, "w"), indent=1, ensure_ascii=False)
    print("baked %s (%dx%d, kind %d) + registry entries — run `make` and the"
          " standee is placeable in maps and the editor" % (tex_path, W, H,
                                                            sprite["kind"]))


if __name__ == "__main__":
    main()
