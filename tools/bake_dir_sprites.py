#!/usr/bin/env python3
"""Bake the directional chair billboard set from the IN-GAME box model.

Renders the 7-box chair (parsed live from sh_src/chair3d.h, so model edits
are picked up automatically) at N viewer poses with the engine's exact
transform math, quantizes to the door-brown ramp, and emits
sh_src/chair_dir_tex.h plus a validation strip PNG to eyeball BEFORE any
ROM build.

The workflow: pick a pose in the in-ROM asset viewer (GAME variant), read
the Y/X coordinates off the screen, and run:

    tools/bake_dir_sprites.py --pitch 246 --yaws 0,238,218,194,180,156,128

Yaws should cover the half circle 0..128 (front..back, uneven spacing is
fine); the engine's 12-sector picker mirrors them for the other half.
Values: 0 transparent, 1..5 -> DOOR_BASE+(v-1) via the sprite vmap (the
same fog-aware decode the in-game paint path uses).
"""
import argparse, math, os, re, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FACES = [((1,5,7,3),4),((0,2,6,4),4),((2,3,7,6),6),((0,4,5,1),2),
         ((4,6,7,5),3),((0,1,3,2),3)]   # corner-bit quads + chair_face_shade axsh


def load_boxes(header="sh_src/chair3d.h", symbol="chair_boxes"):
    """Parse a cbox_t list out of a model header: 6 values per box in struct
    order x0,y0,z0,x1,y1,z1.

    Two spellings exist and both are model space with height 1.0:
      - hand-authored (chair3d.h) writes CM(0.26) — already model units
      - bake_boxes.py output (desk3d.h) writes raw 8.8 ints — /256
    """
    src = open(os.path.join(REPO, header)).read()
    body = src.split(symbol + "[")[1]
    boxes = []
    for m in re.finditer(r"\{([^}]*)\}", body):
        vals = re.findall(r"CM\(\s*(-?[0-9.]+)\s*\)", m.group(1))
        if len(vals) == 6:
            boxes.append(tuple(float(v) for v in vals))
            continue
        raw = re.findall(r"-?\d+", m.group(1))
        if len(raw) == 6:
            boxes.append(tuple(int(v) / 256.0 for v in raw))
    if not boxes:
        sys.exit("no %s parsed from %s" % (symbol, header))
    return boxes


def build_mesh(boxes):
    """Verts plus QUAD faces. The engine rasterizes triangles, but this baker
    fills whole quads: splitting a face into two triangles left a hairline along
    their shared edge where the box's back face showed through, and on the
    desk's large tabletop that baked in as a diagonal scar. One polygon, no
    internal edge. Depth sorting is per-face either way."""
    verts, quads = [], []
    for (x0, y0, z0, x1, y1, z1) in boxes:
        base = len(verts)
        for v in range(8):
            verts.append((x1 if v & 1 else x0,
                          y1 if v & 2 else y0,
                          z1 if v & 4 else z0))
        for (c, s) in FACES:
            quads.append((base+c[0], base+c[1], base+c[2], base+c[3], s))
    return verts, quads


def project(verts, rotY, rotX, zoom, big):
    """Shared yaw/pitch/ortho transform — raycast_model_view's, exactly."""
    th = rotY*2*math.pi/256; ph = rotX*2*math.pi/256
    cy, sy = math.cos(th), math.sin(th); cx, sx = math.cos(ph), math.sin(ph)
    P, Z = [], []
    for (x, y, z) in verts:
        y -= 0.5
        x1 = x*cy + z*sy; z1 = -x*sy + z*cy
        y2 = y*cx - z1*sx; z2 = y*sx + z1*cx
        P.append((big/2 + x1*zoom, big/2 - y2*zoom)); Z.append(z2)
    return P, Z


def fit_zoom(verts, yaws, rotX, big, margin=0.94):
    """One zoom for the WHOLE set, sized so the widest pose still fits.

    The old hardcoded big*0.62 assumed a chair-sized footprint; a desk is 2.15
    model units wide and got clipped at the cardinal yaws. Sharing a single
    fitted zoom across views also keeps the scale stable as the engine's picker
    swaps sectors — a per-view fit would make the sprite breathe as you circle
    it."""
    legacy = big*0.62
    worst = 0.0
    for yw in yaws:
        P, _ = project(verts, yw, rotX, 1.0, 0.0)
        for (px, py) in P:
            worst = max(worst, abs(px), abs(py))
    if worst <= 0:
        return legacy
    # Only ever SHRINK. Enlarging a model that already fit would change the
    # intermediate render resolution and so re-quantize an asset that is
    # already shipped — the chair must re-bake bit-identical.
    return min(legacy, (big*margin*0.5)/worst)


def render_idx(verts, tris, rotY, rotX, big=448, zoom=None):
    """Ramp-index image: 0 transparent, 1..5 = door ramp index+1. Same
    yaw/pitch/ortho/painter math as raycast_model_view."""
    from PIL import Image, ImageDraw
    img = Image.new('L', (big, big), 0); d = ImageDraw.Draw(img)
    ZOOM = zoom if zoom else big*0.62
    P, Z = project(verts, rotY, rotX, ZOOM, big)
    quads = tris
    for t in sorted(range(len(quads)),
                    key=lambda t: -(Z[quads[t][0]] + Z[quads[t][1]]
                                  + Z[quads[t][2]] + Z[quads[t][3]])):
        a, b, c, e, s = quads[t]
        r = max(0, min(4, (s-1)*5//7))
        d.polygon([P[a], P[b], P[c], P[e]], fill=r+1)
    return img


def main():
    from PIL import Image, ImageDraw
    ap = argparse.ArgumentParser()
    ap.add_argument("--yaws", default="0,238,218,194,180,156,128",
                    help="comma list of model yaws (engine 0..255), front..back half circle")
    ap.add_argument("--pitch", type=int, default=246, help="camera pitch (engine 0..255)")
    ap.add_argument("--height", type=int, default=56, help="sprite height in pixels")
    ap.add_argument("--out", default="")
    ap.add_argument("--strip", default="", help="optional validation strip PNG path")
    ap.add_argument("--png", default="", help="write each pose as RGBA PNG (%y = yaw); feeds bake_sprite.py")
    ap.add_argument("--header", default="sh_src/chair3d.h",
                    help="model header holding the cbox_t list")
    ap.add_argument("--symbol", default="chair_boxes", help="box array symbol in --header")
    ap.add_argument("--prefix", default="CHAIR", help="emitted macro/symbol prefix")
    args = ap.parse_args()
    yaws = [int(y) & 255 for y in args.yaws.split(",")]
    H = args.height
    PRE = args.prefix.upper()
    pre = PRE.lower()
    if not args.out:
        args.out = "sh_src/%s_dir_tex.h" % pre

    verts, tris = build_mesh(load_boxes(args.header, args.symbol))
    BIG = 448
    zoom = fit_zoom(verts, yaws, args.pitch, BIG)
    views = []
    for yw in yaws:
        im = render_idx(verts, tris, yw, args.pitch, BIG, zoom)
        bb = im.getbbox()
        if bb is None:
            sys.exit("pose %d rendered empty" % yw)
        im = im.crop(bb)
        w = max(1, round(H*im.width/im.height))
        views.append((yw, w, im.resize((w, H), Image.NEAREST)))

    out = os.path.join(REPO, args.out)
    with open(out, "w") as f:
        f.write("/* Auto-generated by tools/bake_dir_sprites.py from the in-game box model\n")
        f.write(" * (%s). Poses: pitch %d, yaws %s. Do not edit.\n" % (args.header, args.pitch, yaws))
        f.write(" * Values: 0 transparent, 1..5 -> DOOR_BASE+(v-1) via the sprite vmap\n")
        f.write(" * (distance fog applies). ROW-MAJOR tex[y][x]; X-mirror covers the\n")
        f.write(" * other half circle in the engine's 12-sector picker. */\n")
        f.write("#ifndef %s_DIR_TEX_H_INCLUDED\n#define %s_DIR_TEX_H_INCLUDED\n#include <stdint.h>\n\n" % (PRE, PRE))
        wmax = max(w for _, w, _ in views)
        f.write("#define %s_DIR_VIEWS  %d\n#define %s_DIR_H      %d\n" % (PRE, len(views), PRE, H))
        f.write("/* Widest view — the engine sizes its decode scratch from this so a\n")
        f.write(" * re-bake at any --height stays in bounds (no fixed-40 overflow). */\n")
        f.write("#define %s_DIR_WMAX   %d\n\n" % (PRE, wmax))
        for i, (yw, w, im) in enumerate(views):
            f.write("/* view %d: model yaw %d */\n" % (i, yw))
            f.write("#define %s_DIR_W%d %d\n" % (PRE, i, w))
            f.write("static const uint8_t %s_dir_tex%d[%s_DIR_H][%d] = {\n" % (pre, i, PRE, w))
            for y in range(H):
                f.write("    {" + ",".join(str(im.getpixel((x, y))) for x in range(w)) + "},\n")
            f.write("};\n\n")
        f.write("typedef struct { const uint8_t *tex; uint8_t w; } %s_dir_view_t;\n" % pre)
        f.write("static const %s_dir_view_t %s_dir_views[%s_DIR_VIEWS] = {\n" % (pre, pre, PRE))
        for i, (yw, w, im) in enumerate(views):
            f.write("    { (const uint8_t*)%s_dir_tex%d, %s_DIR_W%d },\n" % (pre, i, PRE, i))
        f.write("};\n\n")

        # Sector tables: every baked yaw is a direct sector; every non-self-mirror
        # yaw also serves its reflection (256-yaw) with texX reversed. The engine
        # picker just argmaxes dot products over these — view count is data.
        sectors = [(yw % 256, i, 0) for i, (yw, w, im) in enumerate(views)]
        for i, (yw, w, im) in enumerate(views):
            m = (256 - yw) % 256
            if m != yw % 256:
                sectors.append((m, i, 1))
        sectors.sort()
        f.write("/* Bearing sectors: normalized view yaw, sprite index, mirror flag.\n")
        f.write(" * The picker maximizes dot(chair->player, dir(facing+128+v)). */\n")
        f.write("#define %s_DIR_SECTORS %d\n" % (PRE, len(sectors)))
        f.write("static const uint8_t %s_dir_sect_v[%s_DIR_SECTORS] = {\n    %s };\n"
                % (pre, PRE, ",".join(str(v) for v, s, m in sectors)))
        f.write("static const uint8_t %s_dir_sect_view[%s_DIR_SECTORS] = {\n    %s };\n"
                % (pre, PRE, ",".join(str(s) for v, s, m in sectors)))
        f.write("static const uint8_t %s_dir_sect_mirror[%s_DIR_SECTORS] = {\n    %s };\n"
                % (pre, PRE, ",".join(str(m) for v, s, m in sectors)))
        f.write("\n#endif\n")
    print("wrote %s  views: %s" % (args.out, [(yw, w) for yw, w, _ in views]))

    if args.png:
        # Emit each pose as an RGBA PNG in the door ramp, so the SAME box model
        # and the SAME engine pose can feed bake_sprite.py's standee path. A
        # standee rendered from some other camera won't sit on the floor the way
        # the 3D model does, and the LOD swap pops.
        DOOR = [(15*8,12*8,9*8),(18*8,15*8,11*8),(22*8,18*8,14*8),
                (25*8,21*8,16*8),(29*8,25*8,20*8)]
        for yw, w, im in views:
            rgba = Image.new('RGBA', im.size, (0, 0, 0, 0))
            for y in range(im.height):
                for x in range(im.width):
                    v = im.getpixel((x, y))
                    if v:
                        rgba.putpixel((x, y), DOOR[v-1] + (255,))
            path = args.png.replace("%y", str(yw))
            rgba.save(path)
            print("wrote pose png:", path)

    if args.strip:
        DOOR = [(15*8,12*8,9*8),(18*8,15*8,11*8),(22*8,18*8,14*8),(25*8,21*8,16*8),(29*8,25*8,20*8)]
        sc = 4
        tot = sum(w for _, w, _ in views)
        sheet = Image.new('RGB', ((tot+len(views)*3)*sc, H*sc+16), (40, 40, 44))
        dd = ImageDraw.Draw(sheet)
        x0 = 0
        for yw, w, im in views:
            for y in range(H):
                for x in range(w):
                    v = im.getpixel((x, y))
                    if v:
                        for dy in range(sc):
                            for dx in range(sc):
                                sheet.putpixel((x0+x*sc+dx, y*sc+dy), DOOR[v-1])
            dd.text((x0, H*sc+2), str(yw), fill=(255, 230, 0))
            x0 += (w+3)*sc
        sheet.save(args.strip)
        print("wrote strip:", args.strip)


if __name__ == "__main__":
    main()
