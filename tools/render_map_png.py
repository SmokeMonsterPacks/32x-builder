#!/usr/bin/env python3
"""Render a .map to a top-down PNG — the PR bot's map preview.

Registry-driven colors (the same palette the web editor uses), so the preview
in a community-map PR matches what the contributor drew. Pure Pillow.

  python3 tools/render_map_png.py maps/community/foo.map out.png [--cell 16]
"""
import argparse, json, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import mapfmt  # noqa: E402

from PIL import Image, ImageDraw  # noqa: E402


def render(model, reg, cell=16):
    w, h = model["w"], model["h"]
    img = Image.new("RGB", (w * cell + 1, h * cell + 1), "#101010")
    d = ImageDraw.Draw(img)
    colors = {c["value"]: c["color"] for c in reg["cells"]["palette"]}
    glyphs = reg["cells"]["glyphs"]
    kinds = {k["id"]: k for k in reg["decals"]["kinds"]}

    for y, row in enumerate(model["grid"]):
        for x, ch in enumerate(row):
            d.rectangle([x * cell, y * cell, (x + 1) * cell, (y + 1) * cell],
                        fill=colors.get(glyphs.get(ch, 1), "#f0f"))

    for c in model.get("crawls", []):                     # crawlspace tint
        dx, dy = reg["crawl"]["dir"].get(c.get("dir", "S"), (0, 1))
        for i in range(c.get("len", 1)):
            cx, cy = c["cx"] + dx * i, c["cy"] + dy * i
            d.rectangle([cx * cell, cy * cell, (cx + 1) * cell, (cy + 1) * cell],
                        fill="#2e4a5e")

    for y in range(h + 1):                                # grid lines
        d.line([0, y * cell, w * cell, y * cell], fill="#262215")
    for x in range(w + 1):
        d.line([x * cell, 0, x * cell, h * cell], fill="#262215")

    for p in model.get("partitions", []):                 # partitions
        col = "#9aa84a" if p.get("style") == "spotted" else "#caa84a"
        wd = 3 if p.get("height") == "full" else 2
        d.line([p["x1"] * cell, p["y1"] * cell, p["x2"] * cell, p["y2"] * cell],
               fill=col, width=wd)

    for l in model.get("lights", []):                     # ceiling lights
        x0, y0 = l["cx"] * cell, l["cy"] * cell
        pad = cell * 0.28
        d.rectangle([x0 + pad, y0 + pad, x0 + cell - pad, y0 + cell - pad],
                    fill="#fff7d0")

    for dec in model.get("decals", []):                   # decals
        k = kinds.get(dec.get("kind"), {})
        r = cell * 0.28
        d.ellipse([dec["x"] * cell - r, dec["y"] * cell - r,
                   dec["x"] * cell + r, dec["y"] * cell + r],
                  fill=k.get("color", "#f0f"), outline="#000")

    sp = model["spawn"]                                   # spawn arrow
    sx, sy = sp["x"] * cell, sp["y"] * cell
    r = cell * 0.34
    d.ellipse([sx - r, sy - r, sx + r, sy + r], fill="#39d353", outline="#000")
    ang = {"E": 0, "S": 90, "W": 180, "N": 270}.get(sp.get("facing", "N"), 270)
    rad = math.radians(ang)
    d.line([sx, sy, sx + math.cos(rad) * cell * 0.6, sy + math.sin(rad) * cell * 0.6],
           fill="#000", width=2)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map_path")
    ap.add_argument("out_png")
    ap.add_argument("--cell", type=int, default=16)
    ap.add_argument("--registry", default=os.path.join(ROOT, "registry.json"))
    args = ap.parse_args()
    with open(args.registry) as fh:
        reg = json.load(fh)
    model = mapfmt.parse(open(args.map_path).read())
    img = render(model, reg, args.cell)
    img.save(args.out_png)
    print("render_map_png: %s (%dx%d) -> %s" %
          (model.get("name"), model["w"], model["h"], args.out_png))


if __name__ == "__main__":
    main()
