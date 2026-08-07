#!/usr/bin/env python3
"""Sim-render draw_exit_hole() at a replayable camera, so the LOOK can be
judged (and A/B'd) without a hardware round trip.

Replicates raycast.c:draw_exit_hole exactly — same fixed point, same divides,
same dither — plus a stand-in wall pass for the surrounding face so the hole is
judged in context rather than floating on black. The wall surround is
approximate (flat plane, no DDA); the HOLE itself is the real code path.

    python3 tools/sim_exit_hole.py                 # baseline, 3 distances
    python3 tools/sim_exit_hole.py --dist 0.8      # one camera
    python3 tools/sim_exit_hole.py --off 0.35      # step sideways off-axis

Output: /private/tmp/.../sim_exit_hole_<tag>.png (path printed).
"""
import argparse, math, os, sys, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SH = os.path.join(REPO, "sh_src")

spec = importlib.util.spec_from_file_location(
    "export_assets", os.path.join(REPO, "tools/export_assets.py"))
ea = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ea)

# ---------------------------------------------------------------- fixed point
FX_SHIFT = 16
FX_ONE = 1 << FX_SHIFT


def FX(d):
    return int(d * FX_ONE)


def FX_MUL(a, b):
    return (a * b) >> FX_SHIFT          # C: (int64 a*b) >> 16, arithmetic


def trunc_div(n, d):
    """SH-2 DIVU/DIV1 truncates toward zero; Python // floors."""
    q = abs(n) // abs(d)
    return q if (n < 0) == (d < 0) else -q


def fx_div_hw(a, b):
    return trunc_div(a << FX_SHIFT, b)


def divu(a, b):
    return a // b                        # both operands unsigned at the C site


# ------------------------------------------------------------ engine constants
SCREEN_W, SCREEN_H = 320, 224
WALL_BASE, SHADE_LEVELS = 1, 16
DOOR_DARK_BASE = 104                 # 4 warm near-black greys, bottoms #201810
# The hole's own shade ramp: the wall ramp (luma 223..73) continued into
# DOOR_DARK's bottom three (60, 43, 25). Monotonic, so a fade can run the whole
# way from lit wallpaper to near-black without a palette seam.
HOLE_RAMP = [1 + v for v in range(16)] + [DOOR_DARK_BASE + 2]
HOLE_DARKEST = len(HOLE_RAMP) - 1
DARK_ROOM_SHADE = 6
FOG_RAMP_DIST = FX(6)
HOLE_HW = FX(0.30)
HOLE_Z0, HOLE_Z1 = 100, 212
HOLE_REVEAL_D = FX(0.12)             # proposed: the wall's cut thickness
HOLE_FADE_START = FX(0.5)            # cavity holds its lip shade this far in
WALL_TILE_HI_X, WALL_TILE_HI_Y = 4, 4


def cos_fx(a):
    return int(round(math.cos(2 * math.pi * (a % 256) / 256.0) * FX_ONE))


def sin_fx(a):
    return int(round(math.sin(2 * math.pi * (a % 256) / 256.0) * FX_ONE))


PAL = ea.build_palette(open(os.path.join(SH, "raycast.c")).read(),
                       os.path.join(SH, "raycast.c"))
_t = ea.parse_tex(os.path.join(SH, "wall_tex_hi.h"))
TEX_W, TEX_H, TEX = _t["w"], _t["h"], _t["data"]      # x-major: [x*h + y]


def base_shade(t):
    """The face's fog shade — identical expression to the C."""
    if t < FX(2.5):
        return (t * 2) // FX(2.5)
    past = t - FX(2.5)
    span = FOG_RAMP_DIST - FX(2.5)
    return 2 + (past * 13) // span


# --------------------------------------------------------------- the sim frame
class Scene:
    """Hole on a y-plane (axis=1) at y=8, centred on x=8.5, cavity toward +y."""
    plane = FX(8.0)
    c0 = FX(8.5)
    axis = 1
    dir = 1


def render(dist, off_x, pitch=0, eye=128, fix=0):
    px = Scene.c0 + FX(off_x)
    py = Scene.plane - FX(dist)
    angle = 64                                   # facing +y
    dirX, dirY = cos_fx(angle), sin_fx(angle)
    planeX, planeY = FX_MUL(-dirY, FX(0.66)), FX_MUL(dirX, FX(0.66))
    horizon_y = SCREEN_H // 2 - pitch
    PAR = FX(0.01)
    fb = [0] * (SCREEN_W * SCREEN_H)

    for col in range(SCREEN_W):
        camX = ((2 * col - SCREEN_W) << FX_SHIFT) // SCREEN_W
        rdx = dirX + FX_MUL(planeX, camX)
        rdy = dirY + FX_MUL(planeY, camX)
        rdp = rdy if Scene.axis else rdx
        rda = rdx if Scene.axis else rdy
        if -PAR < rdp < PAR:
            continue
        pp = py if Scene.axis else px
        pa = px if Scene.axis else py
        t = fx_div_hw(Scene.plane - pp, rdp)
        if t <= PAR:
            continue
        off = pa + FX_MUL(t, rda) - Scene.c0

        # --- stand-in wall pass: the face this hole is cut into -------------
        bsh = base_shade(t)
        lh = divu(SCREEN_H << FX_SHIFT, t)
        wb = horizon_y + ((lh * eye) >> 8)
        wtop = wb - lh
        wx = (pa + FX_MUL(t, rda)) & (FX_ONE - 1)
        tx = ((wx * (TEX_W * WALL_TILE_HI_X)) >> FX_SHIFT) & (TEX_W - 1)
        vstep = divu((TEX_H * WALL_TILE_HI_Y) << FX_SHIFT, lh) if lh else 0
        for y in range(max(0, wtop), min(SCREEN_H, wb + 1)):
            v = (((y - wtop) * vstep) >> FX_SHIFT) & (TEX_H - 1)
            ws = bsh + (TEX[tx * TEX_H + v] >> 1)
            fb[y * SCREEN_W + col] = WALL_BASE + min(ws, SHADE_LEVELS - 1)

        # --- draw_exit_hole, verbatim ---------------------------------------
        if off < -HOLE_HW or off > HOLE_HW:
            continue

        side_hit = 0
        t2 = fx_div_hw(Scene.plane + Scene.dir * FX_ONE - pp, rdp)
        if rda > 64 or rda < -64:
            adrift = -rda if rda < 0 else rda
            edge = (HOLE_HW - off) if rda > 0 else (off + HOLE_HW)
            edge = max(edge, 0)
            ts = t + fx_div_hw(edge, adrift)
            if ts < t2:
                t2, side_hit = ts, 1

        lh_n = divu(SCREEN_H << FX_SHIFT, t)
        lh_f = divu(SCREEN_H << FX_SHIFT, t2)
        wb_n = horizon_y + ((lh_n * eye) >> 8)
        wb_f = horizon_y + ((lh_f * eye) >> 8)
        hn, hf = wb_n - ((lh_n * HOLE_Z1) >> 8), wb_f - ((lh_f * HOLE_Z1) >> 8)
        sn, sf = wb_n - ((lh_n * HOLE_Z0) >> 8), wb_f - ((lh_f * HOLE_Z0) >> 8)
        head_lo, head_hi = min(hn, hf), max(hn, hf)
        sill_lo, sill_hi = min(sn, sf), max(sn, sf)

        # The interior is UNLIT: its darkness is absolute, not "N steps below
        # the face". The old relative murk left a hole you stood next to
        # reading mid-brown, i.e. lit. Only haze between you and the opening
        # lifts it, hence the small fog term.
        murk = HOLE_DARKEST - (bsh >> 2)
        if murk < HOLE_DARKEST - 2:
            murk = HOLE_DARKEST - 2
        depth_in = max(t2 - t, 0)
        if side_hit and rda < 0:
            s_start, reach = bsh + 1, 9
        elif side_hit:
            s_start, reach = bsh + 5, 10
        else:
            s_start, reach = bsh + 2, 9
        s_start = min(s_start, murk)
        fade_in = max(depth_in - HOLE_FADE_START, 0)
        sv8 = (s_start << 8) + (((fade_in * ((murk - s_start) << (reach - 8))) << 8) >> FX_SHIFT)
        # Nothing but the BACK panel is allowed to reach murk: every other
        # surface stops two steps short, so each corner is a visible edge
        # instead of two darks dithering into each other.
        sv8 = min(sv8, (murk - 2) << 8)

        def dith(acc8, y):
            v = (acc8 + ((((y ^ col) & 1)) << 7)) >> 8
            return HOLE_RAMP[max(0, min(v, HOLE_DARKEST))]

        def put(y, c):
            if 0 <= y < SCREEN_H:
                fb[y * SCREEN_W + col] = c

        # head underside: HOLD then fall, same rule as the side walls
        s0 = min(bsh + 4, sv8 >> 8)
        h = head_hi - head_lo
        hold = h >> 1
        run = h - hold
        step = divu(sv8 - (s0 << 8), run) if run > 0 else 0
        y0, y1 = max(head_lo, 0), min(head_hi, SCREEN_H - 1)
        for y in range(y0, y1 + 1):
            k = y - head_lo - hold
            put(y, dith((s0 << 8) + (step * k if k > 0 else 0), y))
        # core
        y0, y1 = max(head_hi + 1, 0), min(sill_lo - 1, SCREEN_H - 1)
        reveal = fix and side_hit and depth_in < HOLE_REVEAL_D
        if reveal:
            # JAMB: the wall's own cut thickness. Near-face bright so it reads
            # continuous with the wallpaper, darkening inward (depth AO), with
            # a contact darkening where it meets the head and the sill.
            q = (depth_in * (4 * FX_ONE // HOLE_REVEAL_D)) >> FX_SHIFT
            q = max(0, min(q, 3))
            pa_ = bsh + 1 + (q >> 1)
            pb_ = pa_ + (q & 1)
            span = max(sill_lo - head_hi, 1)
            edge = max(span >> 3, 1)
            for y in range(y0, y1 + 1):
                near = (y - head_hi) < edge or (sill_lo - y) < edge
                s = (pa_ + 1) if near else (pb_ if ((y ^ col) & 1) else pa_)
                put(y, HOLE_RAMP[min(s, HOLE_DARKEST)])
        elif not side_hit and y0 <= y1:
            ca, cb = HOLE_RAMP[murk], HOLE_RAMP[max(murk - 1, 0)]
            for y in range(y0, y1 + 1):
                put(y, cb if ((y ^ col) & 1) else ca)
        else:
            # Cavity side wall: same fade, one step darker for the corner. No
            # chevron -- the wallpaper is the room's skin, not the cut's.
            acc = sv8 + ((1 << 8) if fix else 0)
            for y in range(y0, y1 + 1):
                put(y, dith(acc, y))
        # sill
        s0 = min(bsh + 1, sv8 >> 8)
        h = sill_hi - sill_lo
        hold = h >> 1
        run = h - hold
        step = divu(sv8 - (s0 << 8), run) if run > 0 else 0
        y0, y1 = max(sill_lo, 0), min(sill_hi, SCREEN_H - 1)
        for y in range(y0, y1 + 1):
            k = sill_hi - y - hold
            put(y, dith((s0 << 8) + (step * k if k > 0 else 0), y))
    return fb


def save(fb, path, scale=2):
    rows = []
    for y in range(SCREEN_H):
        row = bytearray()
        for x in range(SCREEN_W):
            r, g, b = PAL[fb[y * SCREEN_W + x]]
            row += bytes((r, g, b)) * scale
        for _ in range(scale):
            rows.append(bytes(row))
    try:
        from PIL import Image
        im = Image.frombytes("RGB", (SCREEN_W * scale, SCREEN_H * scale),
                             b"".join(rows))
        im.save(path)
    except ImportError:
        path = path.rsplit(".", 1)[0] + ".ppm"
        with open(path, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (SCREEN_W * scale, SCREEN_H * scale))
            f.write(b"".join(rows))
    return path


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--dist", type=float, action="append",
                    help="camera distance from the face, in cells (repeatable)")
    ap.add_argument("--off", type=float, default=0.0,
                    help="lateral offset from the hole centre, in cells")
    ap.add_argument("--pitch", type=int, default=0)
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--tag", default="base")
    ap.add_argument("--out", default=os.environ.get("SCRATCH", "/tmp"))
    a = ap.parse_args()
    for d in (a.dist or [0.7, 1.2, 2.5]):
        p = os.path.join(a.out, "sim_exit_hole_%s_d%.1f_o%.2f.png"
                         % (a.tag, d, a.off))
        print(save(render(d, a.off, a.pitch, fix=a.fix), p))
