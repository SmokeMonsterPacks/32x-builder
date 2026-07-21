#!/usr/bin/env python3
"""Bake the chair billboard from a rendered 3/4 PNG into chair_tex.h.

Source has a TRANSPARENT background (Blender film_transparent), so background
= alpha<threshold -> index 0. Foreground is quantized by luminance into the
DARK door-brown ramp: index 1..LEVELS -> DOOR_BASE + (v-1). Matches the
darkened 3D box chair so the near-3D / far-billboard LOD swap doesn't pop.

    tools/bake_chair.py <src.png> <out_h> <chair_tex.h> [preview.png]
"""
import sys
from PIL import Image

LEVELS = 4   # -> DOOR_BASE+0..3 (dark browns; avoids the washed-out +4)
# 32X 5-bit door ramp (for the preview only), *8 to 8-bit:
DOOR_RGB = [(15,12,9),(18,15,11),(22,18,14),(25,21,16)]

def quant(r,g,b):
    lum = 0.299*r + 0.587*g + 0.114*b
    v = int(lum * LEVELS / 256) + 1
    return max(1, min(LEVELS, v))

src = Image.open(sys.argv[1]).convert("RGBA")
out_h = int(sys.argv[2]); out_path = sys.argv[3]
preview = sys.argv[4] if len(sys.argv) > 4 else None

bbox = src.split()[3].getbbox()          # tight crop to non-transparent chair
src = src.crop(bbox)
cw, ch = src.size
out_w = max(1, round(out_h * cw / ch))
small = src.resize((out_w, out_h), Image.BOX)

grid = [[0]*out_w for _ in range(out_h)]
for y in range(out_h):
    for x in range(out_w):
        r,g,b,a = small.getpixel((x,y))
        grid[y][x] = 0 if a < 110 else quant(r,g,b)

with open(out_path,"w") as f:
    f.write("#ifndef CHAIR_TEX_H_INCLUDED\n#define CHAIR_TEX_H_INCLUDED\n#include <stdint.h>\n\n")
    f.write(f"#define CHAIR_TEX_WIDTH  {out_w}\n#define CHAIR_TEX_HEIGHT {out_h}\n\n")
    f.write("/* 3/4 chair billboard, quantized to DOOR_BASE+(v-1), 0=transparent.\n")
    f.write(" * Column-major: tex[x][y] so per-column sampling walks contiguous bytes. */\n")
    f.write(f"static const uint8_t chair_tex[CHAIR_TEX_WIDTH][CHAIR_TEX_HEIGHT] = {{\n")
    for x in range(out_w):
        col = [grid[y][x] for y in range(out_h)]
        f.write("    {" + ",".join(str(v) for v in col) + "},\n")
    f.write("};\n\n#endif\n")
print(f"wrote {out_path}  ({out_w}x{out_h})")

if preview:
    scale = 5
    pv = Image.new("RGB",(out_w*scale, out_h*scale),(30,30,34))
    for y in range(out_h):
        for x in range(out_w):
            v = grid[y][x]
            if v == 0: continue
            r,g,b = DOOR_RGB[v-1]
            for dy in range(scale):
                for dx in range(scale):
                    pv.putpixel((x*scale+dx, y*scale+dy), (r*8,g*8,b*8))
    pv.save(preview); print(f"wrote preview {preview}")
