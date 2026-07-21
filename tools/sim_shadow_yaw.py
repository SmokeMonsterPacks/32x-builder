#!/usr/bin/env python3
"""Sim-verify the yaw-matrix shadow picker + quad orientation against ground truth.

Ground truth: take the real box model, place it in the world at facing fa
(the engine's model->world rotation), shear every solid point along the world
cast direction by height*k (same light geometry as the bake). That world
footprint is what the shadow MUST look like.

Engine side: replicate draw_chair_shadow exactly (sector pick, quad corners,
u/v mapping, mirror flag), rasterize which world points the stencil paints.

Compare with IoU on a fine world-space raster. Asymmetric yaws (45/135) are
the mirror detectors: if u-handedness is wrong, IoU collapses.
"""
import math, sys, importlib.util, os

REPO = "/Users/mikeholzinger/src/32x-builder"
spec = importlib.util.spec_from_file_location("bake", os.path.join(REPO, "tools/bake_chair_shadow.py"))
bake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bake)

WORLD_SCALE = 0.375
KS = [0.5, 0.9, 1.4]
YAWS = [0.0, 45.0, 90.0, 135.0, 180.0]

def eng_cos(a): return math.cos(2*math.pi*(a % 256)/256.0)
def eng_sin(a): return math.sin(2*math.pi*(a % 256)/256.0)

# sector tables, exactly as the bake emits them
sect = []
for yi, yw in enumerate(YAWS):
    sect.append((round(yw*256/360) % 256, yi, 0))
for yi, yw in enumerate(YAWS):
    m = round((360-yw)*256/360) % 256
    if m != round(yw*256/360) % 256:
        sect.append((m, yi, 1))
sect.sort()

stencils = {}   # (yi, li) -> (grid, W, H, meta)
for yi, yw in enumerate(YAWS):
    for li, k in enumerate(KS):
        stencils[(yi, li)] = bake.bake(yw, k)

boxes = bake.load_boxes()

def ground_truth(fa, sdx, sdy, k, samples=14):
    """World footprint point cloud of the true sheared shadow."""
    fc, fs = eng_cos(fa+64), -eng_sin(fa+64)
    pts = []
    for (x0,y0,z0,x1,y1,z1) in boxes:
        for i in range(samples):
            for j in range(samples):
                for kk in range(samples):
                    x = x0 + (x1-x0)*i/(samples-1)
                    y = y0 + (y1-y0)*j/(samples-1)
                    z = z0 + (z1-z0)*kk/(samples-1)
                    wx, wy_h, wz = x*WORLD_SCALE, y*WORLD_SCALE, z*WORLD_SCALE
                    rx = wx*fc + wz*fs
                    rz = -wx*fs + wz*fc
                    # shear along world cast by height*k (bake: z + y*k, model units)
                    px = rx + sdx * (y * k * WORLD_SCALE)
                    py = rz + sdy * (y * k * WORLD_SCALE)
                    pts.append((px, py))
    return pts

def engine_quad(fa, sdx, sdy, kind, lat_sign):
    """Replicate draw_chair_shadow: sector pick + quad corners + u/v -> stencil."""
    fc, fs = eng_cos(fa+64), -eng_sin(fa+64)
    best, bestd = 0, -1e9
    for kdx, (a, yi, mir) in enumerate(sect):
        ms, mc = eng_sin(a), eng_cos(a)
        wdx = -ms*fc + mc*fs
        wdy =  ms*fs + mc*fc
        d = sdx*wdx + sdy*wdy
        if d > bestd: best, bestd = kdx, d
    a, yi, mir = sect[best]
    grid, W, H, meta = stencils[(yi, kind)]
    anchor = meta["anchor_fx"]/65536.0
    ln     = meta["len_fx"]/65536.0
    wd     = meta["width_fx"]/65536.0
    lpx, lpy = lat_sign*(-sdy), lat_sign*(sdx)   # engine lateral axis (sign under test)
    hw = wd/2
    nx, ny = -sdx*anchor, -sdy*anchor
    fxp, fyp = nx + sdx*ln, ny + sdy*ln
    cw = [(nx-lpx*hw, ny-lpy*hw), (nx+lpx*hw, ny+lpy*hw),
          (fxp+lpx*hw, fyp+lpy*hw), (fxp-lpx*hw, fyp-lpy*hw)]
    pts = []
    for v in range(H):
        for u in range(W):
            uu = (u+0.5)/W if not mir else 1.0-(u+0.5)/W
            vv = (v+0.5)/H
            if not grid[v][u]:
                continue
            top = (cw[0][0]+(cw[1][0]-cw[0][0])*uu, cw[0][1]+(cw[1][1]-cw[0][1])*uu)
            bot = (cw[3][0]+(cw[2][0]-cw[3][0])*uu, cw[3][1]+(cw[2][1]-cw[3][1])*uu)
            pts.append((top[0]+(bot[0]-top[0])*vv, top[1]+(bot[1]-top[1])*vv))
    return pts, (a, yi, mir)

def iou(p1, p2, res=0.02):
    def rast(pts):
        return set((int(round(x/res)), int(round(y/res))) for x, y in pts)
    a, b = rast(p1), rast(p2)
    inter = len(a & b)
    return inter/len(a | b) if a | b else 0.0

cases = []
for fa in (0, 64, 128, 192, 32):
    for ang_deg in (0, 45, 90, 135, 180, 225, 270, 315):
        th = math.radians(ang_deg)
        cases.append((fa, math.cos(th), math.sin(th)))

for lat_sign in (+1, -1):
    scores = []
    for fa, sdx, sdy in cases:
        kind = 1
        gt = ground_truth(fa, sdx, sdy, KS[kind])
        ep, pick = engine_quad(fa, sdx, sdy, kind, lat_sign)
        scores.append(iou(gt, ep))
    print("lat_sign=%+d  mean IoU=%.3f  min=%.3f" %
          (lat_sign, sum(scores)/len(scores), min(scores)))
