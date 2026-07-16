#include "mars.h"
#include "menu.h"
#include "raycast.h"
#include "sin_table.h"   /* COS_FX/SIN_FX for the automap player arrow */
#include "font.h"
#include "version.h"
#include "shared.h"
#include "procgen.h"
#include "custom_maps.h"
#include "box3d.h"
#include "box_hero.h"
#include "sound.h"

/* Non-static so box3d.c can drive the same swap state during the
 * title screen — keeps the front/back buffer bookkeeping in one
 * place. */
uint32_t lastTick = 0;
uint16_t currentFB = 0;

/* On-screen debug metrics — off by default, toggled by the six-button
 * controller's MODE button (edge-detected once per frame from any loop). */
uint8_t g_metrics_on = 0;
uint8_t g_padtest_on = 0;   /* MODE+Z: raw controller register overlay */
/* Automap overlay (MODE+B cycles): 0 = off, 1 = FULL (whole map, north-up,
 * arrow rotates), 2 = ROTATE (player fixed at center pointing screen-up, the
 * world rotating around the reticle in real time). d32xr-style vectors, but
 * composited in red OVER the live yellow view instead of a dedicated screen. */
static uint8_t g_automap_on = 0;
/* Continuous zoom, px-per-cell in 16.16: HOLDING MODE+UP/DOWN ramps the
 * target exponentially (~2s across the full range), and the drawn scale
 * eases a quarter of the gap per frame — phone-style pinch feel on a d-pad.
 * Range: 2 px/cell (tiny) .. 64 px/cell (one cell ~30% of the screen). */
static fx_t am_s_tgt = 4 << 16;
static fx_t am_s_cur = 4 << 16;

/* Which custom_maps[] entry is currently loaded; -1 = procgen/fixed/lobby.
 * The exit-door portal reads its next_map to walk story chains. */
static int g_custom_current = -1;
/* Seed of the procgen level currently loaded — its entire identity. The
 * automap footer derives the level's "name" from it (stable per level). */
static uint32_t g_procgen_seed = 0;


/* Pause-menu MAPS tab -> warp request. -1 = none; else a custom_maps[] index.
 * The menu (menu.c) sets it; the main loop drains it into portal_to_custom. */
volatile int g_warp_request = -1;
/* ── Controller input tester (MODE+Z) ────────────────────────────────────
 * Shows the exact word the 68K bridge delivers: RAW = MARS_SYS_COMM8 read
 * right now, SNP = the frame's snapshot the game logic is using (a diff
 * between them means the 68K rewrote COMM8 mid-frame). The lamp row is in
 * BIT ORDER — bit11..bit0 = M X Y Z S A C B R L D U — so the register can
 * be read straight off the screen. HIST logs the last 4 distinct words so
 * one-frame glitches leave evidence. */
static void pad_hex4(char *p, uint16_t v) {
    for (int i = 0; i < 4; i++) {
        int n = (v >> (12 - 4 * i)) & 15;
        p[i] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
    }
}
static void pad_lamps(char *p, uint16_t v) {
    static const char L[12] = { 'M','X','Y','Z','S','A','C','B','R','L','D','U' };
    for (int i = 0; i < 12; i++)
        p[i] = (v & (1u << (11 - i))) ? L[i] : '.';
}
static void pad_test_draw(uint8_t *fb, uint16_t snap) {
    static uint16_t hist[4] = {0,0,0,0};
    static uint16_t last = 0xFFFF;
    uint16_t raw = MARS_SYS_COMM8;
    if (raw != last) {
        hist[3] = hist[2]; hist[2] = hist[1]; hist[1] = hist[0]; hist[0] = raw;
        last = raw;
    }
    uint16_t ty = raw & SEGA_CTRL_TYPE;
    char l1[28], l2[20], l3[28], l4[28];
    /* RAW:XXXX 6BTN */
    l1[0]='R';l1[1]='A';l1[2]='W';l1[3]=':'; pad_hex4(l1+4, raw);
    l1[8]=' ';
    l1[9]  = (ty == SEGA_CTRL_SIX) ? '6' : (ty == SEGA_CTRL_THREE) ? '3' : '?';
    l1[10]='B';l1[11]='T';l1[12]='N';l1[13]=0;
    pad_lamps(l2, raw); l2[12]=0;
    l3[0]='S';l3[1]='N';l3[2]='P';l3[3]=':'; pad_hex4(l3+4, snap);
    l3[8]=' ';
    pad_lamps(l3+9, snap); l3[21]=0;
    l4[0]='H';l4[1]=':';
    for (int i = 0; i < 4; i++) { pad_hex4(l4+2+i*5, hist[i]); l4[6+i*5] = ' '; }
    l4[21]=0;
    font_draw_string(fb, 4, 40, l1, AMAP_RED_BRIGHT);   /* debug text: red reads on screenshots */
    font_draw_string(fb, 4, 52, l2, AMAP_RED_BRIGHT);   /* debug text: red reads on screenshots */
    font_draw_string(fb, 4, 64, l3, AMAP_RED_BRIGHT);   /* debug text: red reads on screenshots */
    font_draw_string(fb, 4, 76, l4, AMAP_RED_BRIGHT);   /* debug text: red reads on screenshots */
}

static void metrics_mode_check(uint16_t pad) {
    static uint16_t prev = 0xFFFF;
    /* Debug shortcuts live behind a HELD-MODE modifier: bare X used to cycle
     * the wall res while ALSO acting as a menu commit, and emulators with
     * loose six-button mappings fire phantom X/MODE singles. MODE alone now
     * does nothing (pure modifier); the combo edge-triggers on the second
     * button. The VISUALS pause-menu tab remains the discoverable path. */
    if (pad & SEGA_CTRL_MODE) {
        if ((pad & SEGA_CTRL_X) && !(prev & SEGA_CTRL_X))
            SHARED_UC->wall_res_mode = (uint8_t)((SHARED_UC->wall_res_mode + 1) % 4);
        if ((pad & SEGA_CTRL_Y) && !(prev & SEGA_CTRL_Y))
            g_metrics_on ^= 1;
        if ((pad & SEGA_CTRL_B) && !(prev & SEGA_CTRL_B))
            g_automap_on = (uint8_t)((g_automap_on + 1) % 3);
        if ((pad & SEGA_CTRL_Z) && !(prev & SEGA_CTRL_Z))
            g_padtest_on ^= 1;
        if (g_automap_on) {
            if (pad & SEGA_CTRL_UP) {                    /* held: ramp in */
                am_s_tgt += am_s_tgt >> 4;
                if (am_s_tgt > (64 << 16)) am_s_tgt = 64 << 16;
            }
            if (pad & SEGA_CTRL_DOWN) {                  /* held: ramp out */
                am_s_tgt -= am_s_tgt >> 4;
                if (am_s_tgt < (2 << 16)) am_s_tgt = 2 << 16;
            }
        }
    }
    prev = pad;
}

/* Frame-time profiler. Reads the SH-2 free-running timer at Φ/32
 * (~720kHz, 1.39μs per tick) once per frame and displays the delta
 * since the previous frame in the top-right corner. 60fps ≈ 12000
 * ticks, 30fps ≈ 24000, 15fps ≈ 48000. Single-stage rolling EMA
 * smooths jitter; the display updates every frame so changes are
 * immediate without being visually noisy. Remove this block when
 * we're done with the optimization pass. */
static uint16_t prof_prev_frt = 0;
static uint32_t prof_smoothed = 0;   /* 32-bit: a sub-15fps frame exceeds the 16-bit FRT range */
static uint16_t prof_secondary_smoothed = 0;
static uint16_t prof_half_smoothed = 0;

extern volatile uint16_t prof_primary_half_ticks;  /* written by raycast_render */

static inline uint16_t prof_read_frt(void) {
    /* Hitachi SH-2 FRT quirk: reading FRCH latches FRCL into a
     * temporary register so the 16-bit value stays atomic. */
    uint8_t hi = SH2_FRT_FRCH;
    uint8_t lo = SH2_FRT_FRCL;
    return ((uint16_t)hi << 8) | lo;
}

static inline void prof_init(void) {
    SH2_FRT_TIER  = 0x01;  /* default — no interrupts enabled */
    SH2_FRT_TCR   = 0x02;  /* Φ/128 prescaler ≈ 180kHz — heavy diagnostic frames
                          * (SERL ~120ms) were wrapping the 16-bit window at Φ/32,
                          * poisoning every T/H/S/W read above 91ms. 364ms window
                          * now; ~5.6us/tick is ample for pass-level metrics. */
    SH2_FRT_FTCSR = 0;     /* clear OVF/OCF; free-running */
    prof_prev_frt = prof_read_frt();
}

static void prof_sample_and_draw(uint8_t *fb) {
    uint16_t now = prof_read_frt();
    uint16_t raw = (uint16_t)(now - prof_prev_frt);
    prof_prev_frt = now;
    /* The 16-bit FRT wraps once per ~91ms. A frame under ~15fps (>48000 ticks)
     * still fits, but a sub-11fps frame (>65536) wraps and reads tiny. Unwrap
     * the single overflow the same way the FPS calc does: a "frame" shorter
     * than 12000 ticks (>60fps) can't be real here, so it's a wrapped long one. */
    uint32_t delta = (raw < 3000) ? (uint32_t)raw + 65536u : raw;
    /* EMA: 7/8 old + 1/8 new — ~8-frame time constant. */
    prof_smoothed = (prof_smoothed - (prof_smoothed >> 3)) + (delta >> 3);
    uint16_t secondary = SHARED_UC->secondary_render_ticks;
    prof_secondary_smoothed = (uint16_t)((prof_secondary_smoothed - (prof_secondary_smoothed >> 3)) + (secondary >> 3));
    uint16_t half = prof_primary_half_ticks;
    prof_half_smoothed = (uint16_t)((prof_half_smoothed - (prof_half_smoothed >> 3)) + (half >> 3));

    /* "T:NNNNN H:NNNNN S:NNNNN" — frame total, primary half-render,
     * secondary half-render. Higher of H/S is the parallel bottleneck.
     * (Effective FPS rides the bottom line next to the per-pass breakdown.) */
    char text[24];
    text[0] = 'T'; text[1] = ':';
    uint16_t v = prof_smoothed;
    text[6] = '0' + (v % 10); v /= 10;
    text[5] = '0' + (v % 10); v /= 10;
    text[4] = '0' + (v % 10); v /= 10;
    text[3] = '0' + (v % 10); v /= 10;
    text[2] = '0' + v;
    text[7] = ' '; text[8] = 'H'; text[9] = ':';
    v = prof_half_smoothed;
    text[14] = '0' + (v % 10); v /= 10;
    text[13] = '0' + (v % 10); v /= 10;
    text[12] = '0' + (v % 10); v /= 10;
    text[11] = '0' + (v % 10); v /= 10;
    text[10] = '0' + v;
    text[15] = ' '; text[16] = 'S'; text[17] = ':';
    v = prof_secondary_smoothed;
    text[22] = '0' + (v % 10); v /= 10;
    text[21] = '0' + (v % 10); v /= 10;
    text[20] = '0' + (v % 10); v /= 10;
    text[19] = '0' + (v % 10); v /= 10;
    text[18] = '0' + v;
    text[23] = 0;
    /* Top-right corner. LIGHT_BASE[0] (palette idx 49) is the brightest
     * fixture-white, reads on every background. */
    font_draw_string(fb, SCREEN_W - 8 * 23 - 4, 4, text, AMAP_RED_BRIGHT);   /* debug text: red reads on screenshots */
    /* Build stamp on the debug HUD: every metrics screenshot self-identifies
     * (no more guessing which ROM produced a capture). */
    font_draw_string(fb, SCREEN_W - 8 * 14 - 4, SCREEN_H - 36, "B" VERSION_BUILD_STR " " VERSION_SHA_STR, AMAP_RED_BRIGHT);

    /* Second line: primary-half per-pass breakdown — Clear / ceiling-Grid /
     * caRpet / Walls (raw FRT ticks), then F = effective FPS. Per-pass tells
     * us which pass to optimize; F is the bottom-line score it rolls up to. */
    {
        extern volatile uint16_t prof_pass_clear, prof_pass_ceil,
                                 prof_pass_carpet, prof_pass_walls;
        static const char lbl[4] = {'C', 'G', 'R', 'W'};
        uint16_t pv[4] = { prof_pass_clear, prof_pass_ceil,
                           prof_pass_carpet, prof_pass_walls };
        char t2[40];
        int pos = 0;
        for (int i = 0; i < 4; i++) {
            t2[pos++] = lbl[i];
            t2[pos++] = ':';
            uint16_t x = pv[i];
            for (int d = 4; d >= 0; d--) { t2[pos + d] = '0' + (x % 10); x /= 10; }
            pos += 5;
            t2[pos++] = ' ';
        }
        /* Effective FPS = 720000 / frame_period (FRT is ~720kHz). The 16-bit
         * FRT wraps at 65536 (~91ms); a per-frame delta below one vblank
         * (12000 ticks) wrapped once, so add 65536 — honest down to ~10fps. */
        uint32_t ft = delta ? delta : 1;
        if (ft < 3000) ft += 65536;
        uint32_t fps = (180000u + ft / 2) / ft;
        if (fps > 99) fps = 99;
        t2[pos++] = 'F'; t2[pos++] = ':';
        t2[pos++] = '0' + (fps / 10);
        t2[pos++] = '0' + (fps % 10);
        t2[pos] = 0;
        font_draw_string(fb, 4, SCREEN_H - 12, t2, AMAP_RED_BRIGHT);   /* debug text: red reads on screenshots */
    }

    /* Third line: the SERIAL TAIL — primary-only post-sync work that the
     * C/G/R/W line does NOT cover. L = low-ceiling slab + bulkheads
     * (crawlspace, scene-dependent), P = lights + standups sprites. This is
     * the ~25%-of-frame block that was invisible until now. */
    {
        extern volatile uint16_t prof_pass_ovl, prof_pass_sprite;
        extern volatile uint16_t prof_split_col, prof_dda_fat;
        static const char lbl[4] = {'O', 'P', 'K', 'E'};
        uint16_t pv[4] = { prof_pass_ovl, prof_pass_sprite,
                           prof_split_col, prof_dda_fat };
        char t3[36];
        int pos = 0;
        for (int i = 0; i < 4; i++) {
            t3[pos++] = lbl[i];
            t3[pos++] = ':';
            uint16_t x = pv[i];
            for (int d = 4; d >= 0; d--) { t3[pos + d] = '0' + (x % 10); x /= 10; }
            pos += 5;
            t3[pos++] = ' ';
        }
        t3[pos] = 0;
        font_draw_string(fb, 4, SCREEN_H - 24, t3, AMAP_RED_BRIGHT);   /* debug text: red reads on screenshots */
    }
}

/* ---- Automap overlay ------------------------------------------------- *
 * Vector map over the live render: red boundary lines for every wall face
 * that touches open floor (voids stay open = exits read as gaps), partitions
 * as their true segments, the player in bright red.
 *   FULL:   whole 32x32 grid, north-up, fixed 4px/cell, arrow rotates.
 *   ROTATE: player pinned at screen center pointing screen-up; every segment
 *           is translated player-relative and rotated by (192 - angle) --
 *           the rotation that maps the facing vector onto -Y (up). */
#define AM_CX     (SCREEN_W / 2)
#define AM_CY     (SCREEN_H / 2)

static void am_line(uint8_t *fb, int x0, int y0, int x1, int y1, uint8_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        if ((unsigned)x0 < SCREEN_W && (unsigned)y0 < SCREEN_H)
            fb[y0 * SCREEN_W + x0] = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Per-frame transform state (set by automap_draw, read by am_pt). */
static fx_t am_rc, am_rs, am_px, am_py;
static int  am_rotate, am_ax, am_ay;

/* World FX * scale FX -> screen pixels ((2^16 x)(2^16 s) >> 32 = x*s). */
#define AM_PX(v) ((int)(((int64_t)(v) * am_s_cur) >> 32))

static void am_pt(fx_t wx, fx_t wy, int *ox, int *oy) {
    if (!am_rotate) {
        *ox = am_ax + AM_PX(wx);
        *oy = am_ay + AM_PX(wy);
        return;
    }
    fx_t dx = wx - am_px, dy = wy - am_py;
    fx_t rx = FX_MUL(dx, am_rc) - FX_MUL(dy, am_rs);
    fx_t ry = FX_MUL(dx, am_rs) + FX_MUL(dy, am_rc);
    *ox = AM_CX + AM_PX(rx);
    *oy = AM_CY + AM_PX(ry);
}

static void am_emit(uint8_t *fb, fx_t wx0, fx_t wy0, fx_t wx1, fx_t wy1, uint8_t c) {
    int x0, y0, x1, y1;
    am_pt(wx0, wy0, &x0, &y0);
    am_pt(wx1, wy1, &x1, &y1);
    am_line(fb, x0, y0, x1, y1, c);
}

/* Automap footer: the current map's name, static, bottom-center, in red.
 * Authored maps use their real name; a procgen level derives a pronounceable
 * 8-char name from its SEED (consonant-vowel syllables) — deterministic, so
 * a level keeps its name for as long as you wander it. */
static void am_footer(uint8_t *fb) {
    char name[18];
    int n = 0;
    if (g_custom_current >= 0) {
        for (const char *p = custom_maps[g_custom_current].name; *p && n < 16; p++)
            name[n++] = *p;
    } else {
        static const char CONS[] = "BDKLMNPRSTVZ";   /* 12 */
        static const char VOWS[] = "AEIOU";          /*  5 */
        uint32_t h = g_procgen_seed * 2654435761u;   /* Knuth mix */
        for (int i = 0; i < 4; i++) {
            name[n++] = CONS[h % 12]; h /= 12;
            name[n++] = VOWS[h % 5];  h /= 5;
            h ^= h >> 7;
        }
    }
    name[n] = 0;
    int y = g_metrics_on ? (SCREEN_H - 36) : (SCREEN_H - 12);
    font_draw_string(fb, (SCREEN_W - n * 8) / 2, y, name, AMAP_RED_BRIGHT);
}

static void automap_draw(uint8_t *fb) {
    am_rotate = (g_automap_on == 2);
    /* Glide the drawn scale toward the target (quarter-gap per frame). */
    fx_t d = am_s_tgt - am_s_cur;
    am_s_cur += (d > 255 || d < -255) ? (d >> 2) : d;
    if (am_rotate) {
        uint8_t th = (uint8_t)(192 - (uint8_t)player.angle);
        am_rc = COS_FX(th); am_rs = SIN_FX(th);
        am_px = player.x;   am_py = player.y;
    } else {
        /* North-up camera, per-axis: centered while that axis of the map fits
         * the screen; once it outgrows it, follow the player, clamped so the
         * map edge never leaves a gap. The clamp range collapses exactly at
         * the fits/doesn't boundary, so zoom glides through with no pop. */
        int map_w = AM_PX((fx_t)MAP_W << FX_SHIFT);
        int map_h = AM_PX((fx_t)MAP_H << FX_SHIFT);
        if (map_w <= SCREEN_W) {
            am_ax = (SCREEN_W - map_w) / 2;
        } else {
            am_ax = AM_CX - AM_PX(player.x);
            if (am_ax > 0) am_ax = 0;
            if (am_ax < SCREEN_W - map_w) am_ax = SCREEN_W - map_w;
        }
        if (map_h <= SCREEN_H) {
            am_ay = (SCREEN_H - map_h) / 2;
        } else {
            am_ay = AM_CY - AM_PX(player.y);
            if (am_ay > 0) am_ay = 0;
            if (am_ay < SCREEN_H - map_h) am_ay = SCREEN_H - map_h;
        }
    }
    /* Grid: each wall cell's faces that border walkable floor. A few hundred
     * short segments, only while the overlay is on. */
    for (int cy = 0; cy < MAP_H; cy++) {
        for (int cx = 0; cx < MAP_W; cx++) {
            if (world_map[cy][cx] != 1) continue;
            fx_t x0 = (fx_t)cx << FX_SHIFT,  y0 = (fx_t)cy << FX_SHIFT;
            fx_t x1 = x0 + FX_ONE,           y1 = y0 + FX_ONE;
            if (cy > 0         && world_map[cy - 1][cx] == 0) am_emit(fb, x0, y0, x1, y0, AMAP_RED);
            if (cy < MAP_H - 1 && world_map[cy + 1][cx] == 0) am_emit(fb, x0, y1, x1, y1, AMAP_RED);
            if (cx > 0         && world_map[cy][cx - 1] == 0) am_emit(fb, x0, y0, x0, y1, AMAP_RED);
            if (cx < MAP_W - 1 && world_map[cy][cx + 1] == 0) am_emit(fb, x1, y0, x1, y1, AMAP_RED);
        }
    }
    /* The outer shell. It's solid — the DDA treats out of bounds as wall and
     * collision always has — but it lives nowhere in world_map, so the map has
     * to draw it explicitly or it stays the last system claiming the boundary
     * is open. Emit only where it faces walkable floor: a border cell that's
     * already a wall draws its own face and hides the shell, same as in 3D. */
    {
        const fx_t xe = (fx_t)MAP_W << FX_SHIFT, ye = (fx_t)MAP_H << FX_SHIFT;
        for (int cx = 0; cx < MAP_W; cx++) {
            fx_t x0 = (fx_t)cx << FX_SHIFT, x1 = x0 + FX_ONE;
            if (world_map[0][cx] == 0)          am_emit(fb, x0,  0, x1,  0, AMAP_RED);
            if (world_map[MAP_H - 1][cx] == 0)  am_emit(fb, x0, ye, x1, ye, AMAP_RED);
        }
        for (int cy = 0; cy < MAP_H; cy++) {
            fx_t y0 = (fx_t)cy << FX_SHIFT, y1 = y0 + FX_ONE;
            if (world_map[cy][0] == 0)          am_emit(fb,  0, y0,  0, y1, AMAP_RED);
            if (world_map[cy][MAP_W - 1] == 0)  am_emit(fb, xe, y0, xe, y1, AMAP_RED);
        }
    }
    /* Partitions are cell-edge flags now — each flagged edge draws as its
     * one-cell segment; contiguous runs read as continuous lines. */
    if (g_pedge_any) {
        for (int ey = 0; ey < MAP_H; ey++)
            for (int ex = 0; ex <= MAP_W; ex++)
                if (pedge_w[ey][ex] & CM_PEDGE_PRESENT)
                    am_emit(fb, (fx_t)ex << FX_SHIFT, (fx_t)ey << FX_SHIFT,
                                (fx_t)ex << FX_SHIFT, (fx_t)(ey + 1) << FX_SHIFT,
                            AMAP_RED);
        for (int ey = 0; ey <= MAP_H; ey++)
            for (int ex = 0; ex < MAP_W; ex++)
                if (pedge_n[ey][ex] & CM_PEDGE_PRESENT)
                    am_emit(fb, (fx_t)ex << FX_SHIFT, (fx_t)ey << FX_SHIFT,
                                (fx_t)(ex + 1) << FX_SHIFT, (fx_t)ey << FX_SHIFT,
                            AMAP_RED);
    }

    if (am_rotate) {
        /* Fixed reticle: center arrow always pointing screen-up. */
        am_line(fb, AM_CX, AM_CY + 3, AM_CX, AM_CY - 5, AMAP_RED_BRIGHT);
        am_line(fb, AM_CX, AM_CY - 5, AM_CX - 3, AM_CY - 1, AMAP_RED_BRIGHT);
        am_line(fb, AM_CX, AM_CY - 5, AM_CX + 3, AM_CY - 1, AMAP_RED_BRIGHT);
    } else {
        /* FULL mode: the arrow lives on the map and rotates with the view. */
        int px = am_ax + AM_PX(player.x);
        int py = am_ay + AM_PX(player.y);
        int fx = (int)((COS_FX((uint8_t)player.angle) * 6) >> FX_SHIFT);
        int fy = (int)((SIN_FX((uint8_t)player.angle) * 6) >> FX_SHIFT);
        am_line(fb, px - fx / 2, py - fy / 2, px + fx, py + fy, AMAP_RED_BRIGHT);
        int bx = (-fx - fy) / 3, by = (fx - fy) / 3;
        am_line(fb, px + fx, py + fy, px + fx + bx, py + fy + by, AMAP_RED_BRIGHT);
        bx = (-fx + fy) / 3; by = (-fx - fy) / 3;
        am_line(fb, px + fx, py + fy, px + fx + bx, py + fy + by, AMAP_RED_BRIGHT);
    }
    am_footer(fb);
}

/* Top-left position + angle overlay for debugging map locations.
 * Line 1: "X:NN.N Y:NN.N" — integer cell + one decimal.
 * Line 2: "A:NNN"          — raw uint8 angle.
 * Two lines so A: doesn't collide with the top-right T/H/S timer. */
static void pos_draw(uint8_t *fb) {
    char line1[14];
    char line2[6];
    int32_t px = player.x;
    int32_t py = player.y;
    int px_i = (int)(px >> 16);
    int px_f = (int)(((uint32_t)(px & 0xFFFF) * 10) >> 16);
    int py_i = (int)(py >> 16);
    int py_f = (int)(((uint32_t)(py & 0xFFFF) * 10) >> 16);
    int angle = (int)player.angle;
    if (px_i < 0)  px_i = 0;
    if (px_i > 99) px_i = 99;
    if (py_i < 0)  py_i = 0;
    if (py_i > 99) py_i = 99;

    line1[0] = 'X'; line1[1] = ':';
    line1[2] = '0' + (px_i / 10);
    line1[3] = '0' + (px_i % 10);
    line1[4] = '.';
    line1[5] = '0' + px_f;
    line1[6] = ' '; line1[7] = 'Y'; line1[8] = ':';
    line1[9]  = '0' + (py_i / 10);
    line1[10] = '0' + (py_i % 10);
    line1[11] = '.';
    line1[12] = '0' + py_f;
    line1[13] = 0;

    line2[0] = 'A'; line2[1] = ':';
    line2[4] = '0' + (angle % 10); angle /= 10;
    line2[3] = '0' + (angle % 10); angle /= 10;
    line2[2] = '0' + (angle % 10);
    line2[5] = 0;

    font_draw_string(fb, 4,  4, line1, AMAP_RED_BRIGHT);   /* debug text: red reads on screenshots */
    font_draw_string(fb, 4, 16, line2, AMAP_RED_BRIGHT);   /* debug text: red reads on screenshots */
}

void swapBuffers(void) {
    while (lastTick == MARS_SYS_COMM12);
    /* In vblank now — safe palette-write window. */
    raycast_shimmer();
    MARS_VDP_FBCTL = currentFB ^ 1;
    while ((MARS_VDP_FBCTL & MARS_VDP_FS) == currentFB);
    currentFB ^= 1;
    lastTick = MARS_SYS_COMM12;
}

/* One brightness-fade step with its own vblank flip (bypasses raycast_shimmer,
 * which would reset the bright palette mid-fade). Shared by the lobby walk-
 * through and the door portal. */
static void fade_step(int lvl) {
    SHARED_UC->frame_count++;
    raycast_render();
    while (lastTick == MARS_SYS_COMM12);
    raycast_set_brightness(lvl);
    MARS_VDP_FBCTL = currentFB ^ 1;
    while ((MARS_VDP_FBCTL & MARS_VDP_FS) == currentFB);
    currentFB ^= 1;
    lastTick = MARS_SYS_COMM12;
}

/* Fill an 8px-tall selection bar into the text framebuffer — the same muted
 * LIGHT_BASE+2 (idx 51) shade as the pause-menu highlight, so the lobby start
 * picker reads consistently. Caller gates the ~10 Hz blink. */
static void lobby_hl_bar(uint8_t *fb, int x, int y, int w) {
    for (int yy = 0; yy < 8; yy++) {
        uint8_t *row = fb + (y + yy) * SCREEN_W + x;
        for (int xx = 0; xx < w; xx++) row[xx] = 51;
    }
}

/* Start-menu picker row: "> [ NAME ]" — brackets always (so the current pick
 * reads), cursor + blinking bar when selected. Drawn at box-pixel (x,y). */
/* Start-menu action row: "> LABEL" — cursor + blinking bar when selected. */
/* Translucent dark panel behind the start-menu text: a 50% checkerboard of
 * the pause menu's dark eggshell (index 46) over the live lobby render — on a
 * CRT/scaler the dither reads as a smoked-glass box, and the halved contrast
 * behind the glyphs makes the white text pop. Solid would hide the lobby;
 * this keeps it breathing through. */
static void lobby_menu_panel(uint8_t *fb, int x0, int y0, int x1, int y1) {
    for (int yy = y0; yy < y1; yy++) {
        uint8_t *row = fb + yy * SCREEN_W;
        for (int xx = x0 + (yy & 1); xx < x1; xx += 2) row[xx] = 46;
    }
}

static void lobby_action_row(uint8_t *fb, int x, int y, int sel, const char *label) {
    char line[20]; int p = 0;
    line[p++] = sel ? '>' : ' ';
    line[p++] = ' ';
    for (const char *nm = label; *nm; nm++) line[p++] = *nm;
    line[p] = '\0';
    if (sel && (SHARED_UC->frame_count % 6) < 3) {
        int nl = 0; while (label[nl]) nl++;
        lobby_hl_bar(fb, x + 2 * 8, y, nl * 8);
    }
    font_draw_string(fb, x, y, line, 49);   /* menu text — stays white */
}

/* SHOW CONTROLS sub-screen: the title and the controls legend over the frozen
 * lobby, until any face/START button sends you back to the start menu. */
static void show_controls_screen(void) {
    const uint16_t BTNS = SEGA_CTRL_START | SEGA_CTRL_A | SEGA_CTRL_B |
                          SEGA_CTRL_C | SEGA_CTRL_X | SEGA_CTRL_Y | SEGA_CTRL_Z;
    HwMdReadPad(0);
    uint16_t prev = MARS_SYS_COMM8;          /* seed: ignore the button still held */
    for (;;) {
        HwMdReadPad(0);
        uint16_t pad = MARS_SYS_COMM8;
        uint16_t pressed = (uint16_t)(pad & ~prev);
        prev = pad;
        if ((pressed & BTNS) && !(pad & SEGA_CTRL_MODE)) break;
        SHARED_UC->frame_count++;
        raycast_render();
        uint8_t *fb = (uint8_t *)((uintptr_t)&MARS_FRAMEBUFFER + 0x200);
        font_draw_string(fb, (SCREEN_W - 13 * 8) / 2, 32, "BACKROOMS 32X", 49);
        const int LEG_X = 92;
        font_draw_string(fb, LEG_X, 78,  "RUN / INTERACT: A", 49);
        font_draw_string(fb, LEG_X, 92,  "LOOK: C",           49);
        font_draw_string(fb, LEG_X, 106, "CROUCH: A+B",       49);
        font_draw_string(fb, LEG_X, 120, "DEBUG STATS: MODE+Y", 49);
        font_draw_string(fb, LEG_X, 134, "RESOLUTION: MODE+X",  49);
        font_draw_string(fb, LEG_X, 148, "AUTOMAP: MODE+B U/D ZOOM", 49);
        font_draw_string(fb, LEG_X, 162, "PAD TEST: MODE+Z",     49);
        font_draw_string(fb, (SCREEN_W - 16 * 8) / 2, 170, "ANY BUTTON: BACK", 49);
        swapBuffers();
    }
}

/* Walk-through-the-EXIT-door portal: fade to black, generate a fresh procedural
 * map, drop the player at the standard spawn, fade back up. The "way out" only
 * loops you deeper into the backrooms. Mirrors the lobby walk-through fade. */
static void portal_to_procgen(void) {
    g_custom_current = -1;
    for (int lvl = FADE_STEPS; lvl >= 0; lvl -= 2) fade_step(lvl);
    g_procgen_seed = SHARED_UC->frame_count * 1000003u + (uint32_t)player.x;
    procgen_run(g_procgen_seed);
    player.x = FX(16.5); player.y = FX(28.5); player.angle = 192;
    raycast_init();                 /* rebuilds full-bright palette... */
    raycast_set_brightness(0);      /* ...held black until the fade-in */
    for (int lvl = 0; lvl <= FADE_STEPS; lvl += 2) fade_step(lvl);
}

/* Pause-menu MAPS-tab warp: the same fade/load/fade as the procgen portal, but
 * loads a hand-authored custom map by index (it sets its own spawn). Lets you
 * jump to any compiled-in map mid-session — the editor test-loop + an escape
 * hatch when the player gets stuck or is done with a map. */
static void portal_to_custom(int idx) {
    g_custom_current = idx;
    for (int lvl = FADE_STEPS; lvl >= 0; lvl -= 2) fade_step(lvl);
    raycast_load_custom(idx);
    raycast_init();
    raycast_set_brightness(0);
    for (int lvl = 0; lvl <= FADE_STEPS; lvl += 2) fade_step(lvl);
}

int m_main(void) {
    /* Release the secondary SH-2. The crt0 (mars_start.s:271-273) intends
     * to do this after the init JSR but uses a stale r0 — the write
     * to "clear secondary status" goes to ROM and is silently dropped.
     * Without this, the secondary loops forever in its S_OK wait at
     * 0x20004024 (= MARS_SYS_COMM4) and never reaches s_main().
     *
     * Writing 0 to COMM4 changes the upper half of the 32-bit word
     * the secondary is polling for "S_OK" (0x535F4F4B) → cmp/eq fails →
     * secondary exits the wait and jumps to s_main. */
    MARS_SYS_COMM4 = 0;

    Hw32xInit(MARS_VDP_MODE_256, 0);
    Hw32xDelay(1);    /* wait for first vblank — palette is writable now */

    /* High-res "attic box" splash: the SEGA CORE label on the closed
     * carton, held until START. Then we hand off to the live low-res 3D
     * box for the open + dive. */
    box_hero_show();

    /* Cardboard box title screen — the box mesh + camera dive are
     * imported from box_model.h and rendered live by box3d (see
     * tools/export_box.py). It owns its own CRAM palette and a
     * shimmer-free flip, and runs BEFORE raycast_init so the gameplay
     * palette build reclaims CRAM after a map is chosen. */
    box3d_play();   /* loads the box palette in vblank on its first frame */

    /* No button needed: the box intro flows straight into the trap-door
     * fall and we plummet into the void. (box3d_play can still be skipped
     * with START.) The map is chosen later, down in the lobby. */
    box3d_play_fall();

    /* Land in the lobby — the open carpeted room from the HobbyTown
     * reference. Build the lobby map BEFORE raycast_init so init_lights
     * lays the ceiling-fixture grid over it. */
    raycast_load_lobby();
    raycast_init();
    prof_init();

    /* Backrooms ambience comes in as we stand up in the lobby (secondary
     * starts pumping from the top of the loop now). */
    amb_set_active(1);

    /* ---- Landing reveal --------------------------------------------- *
     * You fell through the box into darkness; now you come to from the
     * floor. Fade up looking straight DOWN at the carpet, hold a beat so
     * the floor perspective reads, then STAND UP — ease the camera from
     * face-down to the level photo view, decelerating into standing. */
    SHARED_UC->pitch_y = 80;                 /* face-down at the carpet */
    for (int lvl = 0; lvl <= FADE_STEPS; lvl++) {     /* fade up from black */
        SHARED_UC->frame_count++;
        raycast_render();
        while (lastTick == MARS_SYS_COMM12);
        raycast_set_brightness(lvl);
        MARS_VDP_FBCTL = currentFB ^ 1;
        while ((MARS_VDP_FBCTL & MARS_VDP_FS) == currentFB);
        currentFB ^= 1;
        lastTick = MARS_SYS_COMM12;
    }
    for (int i = 0; i < 14; i++) {                    /* hold on the carpet */
        SHARED_UC->frame_count++;
        raycast_render();
        swapBuffers();
    }
    while (SHARED_UC->pitch_y > 0) {                  /* stand up */
        int p = SHARED_UC->pitch_y; p -= (p >> 3) + 1; if (p < 0) p = 0;
        SHARED_UC->pitch_y = (int8_t)p;
        SHARED_UC->frame_count++;
        raycast_render();
        swapBuffers();
    }

    /* --- Lobby: frozen menu, then walk in ---------------------------- *
     * Phase A: the player is FROZEN at the photo vantage; only the text
     * menu is live (UP/DOWN pick the level, any button confirms and
     * dismisses the menu). Phase B: the menu is gone and the choice is
     * locked — you wander the lobby and walk forward into the backrooms
     * to enter the level you picked. */
    /* ---- Unified start list ----------------------------------------------
     * Every destination is ONE row in ONE list. COMMUNITY and STORIES are
     * FOLDING groups: their headers are selectable rows ('>' folded,
     * '|' unrolled); RIGHT unrolls, LEFT folds, confirm toggles. The list is
     * REBUILT whenever a fold changes, and the unroll is animated by a damped
     * integer spring — rows slide out from under the header (clipped until
     * they emerge) and the rows below visibly bounce as the spring settles. */
    enum { IT_MAP, IT_PROC, IT_SEP, IT_FOLD, IT_CTRL };
    struct { uint8_t kind; uint8_t map; const char *label; } items[40];
    int n_items = 0;
    const int n_core  = custom_core_count;
    const int n_comm  = custom_pick_count - custom_core_count;
    const int n_start = custom_start_count;

    /* Story chains (next_map links): a community map someone links TO is a
     * CHAPTER — hidden from the flat list (you start a story at its head, not
     * mid-book; the pause MAPS tab still warps anywhere as the escape hatch).
     * A community map with a next-link that nobody links to is a story HEAD —
     * listed under STORIES. Core maps always list normally. */
    uint8_t has_in[64] = {0};
    for (int i2 = 0; i2 < custom_pick_count; i2++) {
        int nm2 = custom_maps[i2].next_map;
        if (nm2 >= 0 && nm2 < 64) has_in[nm2] = 1;
    }

    int fold_grp[3] = { 1, 1, 1 };  /* 0=COMMUNITY, 1=STORIES, 2=TEST; 1 = folded */
    int rebuild_items = 1;          /* build on first frame + after toggles */
    int refocus_grp = -1;           /* after rebuild, park cursor on this header */
    int pending_open = -1;          /* group just unfolded: arm the unroll anim */
    int anim_hdr = -1, anim_n = 0;  /* animating header index + its child count */
    int gap_cur = 0, gap_vel = 0, gap_tgt = 0, anim_closing = 0;
    int anim_grp_closing = 0;
    int anim_ticks = 0;           /* failsafe: snap the spring after ~20 frames */
    int cur = 0;

    /* Smooth pixel scroll, smartphone-style: the window eases toward keeping
     * the cursor CENTERED, clamped at the list ends — so riding back up parks
     * the -- START MAPS -- header at the top instead of hiding it, and every
     * step glides instead of jumping a row. */
    const int VIS = 7;            /* visible rows in the window */
    const int ROW_H = 14, LIST_Y = 52, LIST_H = VIS * ROW_H;
    int scroll_px = 0;
    int nav_hold = 0;             /* frames UP/DOWN held, for key-repeat */
    uint32_t frame = 0;           /* time-in-lobby — entropy for procgen */
    const uint16_t LOBBY_COMMIT = SEGA_CTRL_START | SEGA_CTRL_A | SEGA_CTRL_B |
                                  SEGA_CTRL_C | SEGA_CTRL_X | SEGA_CTRL_Y | SEGA_CTRL_Z;

    /* Phase A — frozen menu over the still photo-perspective. */
    {
        uint16_t prev_pad = 0xFFFF;
        for (;;) {
            HwMdReadPad(0);
            uint16_t pad = MARS_SYS_COMM8;
            uint16_t pressed = (uint16_t)(pad & ~prev_pad);
            prev_pad = pad;

            if (rebuild_items) {
                rebuild_items = 0;
                n_items = 0;
                items[n_items].kind = IT_SEP; items[n_items].map = 0;
                items[n_items].label = "-- START MAPS --"; n_items++;
                for (int i = 0; i < n_start && n_items < 34; i++) {
                    items[n_items].kind = IT_MAP; items[n_items].map = (uint8_t)i;
                    items[n_items].label = custom_maps[i].name; n_items++;
                }
                items[n_items].kind = IT_PROC; items[n_items].map = 0;
                items[n_items].label = "PROCEDURAL"; n_items++;
                int any_plain = 0, any_head = 0;
                for (int i = 0; i < n_comm; i++) {
                    int mi = n_core + i;
                    if (has_in[mi]) continue;
                    if (custom_maps[mi].next_map >= 0) any_head = 1; else any_plain = 1;
                }
                if (any_plain) {
                    items[n_items].kind = IT_FOLD; items[n_items].map = 0;
                    items[n_items].label = "-- COMMUNITY --"; n_items++;
                    if (!fold_grp[0])
                        for (int i = 0; i < n_comm && n_items < 37; i++) {
                            int mi = n_core + i;
                            if (has_in[mi] || custom_maps[mi].next_map >= 0) continue;
                            items[n_items].kind = IT_MAP; items[n_items].map = (uint8_t)mi;
                            items[n_items].label = custom_maps[mi].name; n_items++;
                        }
                }
                if (any_head) {
                    items[n_items].kind = IT_FOLD; items[n_items].map = 1;
                    items[n_items].label = "-- STORIES --"; n_items++;
                    if (!fold_grp[1])
                        for (int i = 0; i < n_comm && n_items < 37; i++) {
                            int mi = n_core + i;
                            if (has_in[mi] || custom_maps[mi].next_map < 0) continue;
                            items[n_items].kind = IT_MAP; items[n_items].map = (uint8_t)mi;
                            items[n_items].label = custom_maps[mi].name; n_items++;
                        }
                }
                if (n_core > n_start) {
                    items[n_items].kind = IT_FOLD; items[n_items].map = 2;
                    items[n_items].label = "-- TEST --"; n_items++;
                    if (!fold_grp[2])
                        for (int i = n_start; i < n_core && n_items < 38; i++) {
                            items[n_items].kind = IT_MAP; items[n_items].map = (uint8_t)i;
                            items[n_items].label = custom_maps[i].name; n_items++;
                        }
                }
                items[n_items].kind = IT_SEP; items[n_items].map = 0;
                items[n_items].label = ""; n_items++;   /* gap before CONTROLS */
                items[n_items].kind = IT_CTRL; items[n_items].map = 0;
                items[n_items].label = "CONTROLS"; n_items++;

                if (refocus_grp >= 0) {
                    for (int i = 0; i < n_items; i++)
                        if (items[i].kind == IT_FOLD && items[i].map == refocus_grp) { cur = i; break; }
                    refocus_grp = -1;
                }
                if (cur >= n_items) cur = n_items - 1;
                while (cur < n_items - 1 && items[cur].kind == IT_SEP) cur++;

                if (pending_open >= 0) {    /* arm the unroll spring */
                    anim_hdr = -1;
                    for (int i = 0; i < n_items; i++)
                        if (items[i].kind == IT_FOLD && items[i].map == pending_open) { anim_hdr = i; break; }
                    anim_n = 0;
                    if (anim_hdr >= 0)
                        while (anim_hdr + 1 + anim_n < n_items &&
                               items[anim_hdr + 1 + anim_n].kind == IT_MAP) anim_n++;
                    gap_cur = 0; gap_vel = 0; gap_tgt = anim_n * ROW_H;
                    anim_closing = 0; anim_ticks = 0;
                    if (anim_n == 0) anim_hdr = -1;
                    pending_open = -1;
                }
            }

            /* UP/DOWN move the cursor through the list, hopping separators.
             * Holding a direction auto-repeats after ~a third of a second —
             * most of what makes a long list feel like flick-scrolling. */
            if (pad & (SEGA_CTRL_UP | SEGA_CTRL_DOWN)) nav_hold++; else nav_hold = 0;
            int rep = (nav_hold > 6 && (nav_hold & 1) == 0);
            if ((pressed & SEGA_CTRL_UP) || (rep && (pad & SEGA_CTRL_UP))) {
                int c = cur;
                do { c--; } while (c >= 0 && items[c].kind == IT_SEP);
                if (c >= 0) cur = c;
            }
            if ((pressed & SEGA_CTRL_DOWN) || (rep && (pad & SEGA_CTRL_DOWN))) {
                int c = cur;
                do { c++; } while (c < n_items && items[c].kind == IT_SEP);
                if (c < n_items) cur = c;
            }
            /* Folding-group headers: RIGHT unrolls, LEFT folds, confirm
             * toggles. Collapse keeps the children in the list and springs
             * the gap shut; the rebuild happens when the spring settles. */
            if (items[cur].kind == IT_FOLD && anim_hdr < 0) {
                int g = items[cur].map, open_now = -1;
                if ((pressed & SEGA_CTRL_RIGHT) && fold_grp[g])  open_now = 1;
                if ((pressed & SEGA_CTRL_LEFT)  && !fold_grp[g]) open_now = 0;
                if ((pressed & LOBBY_COMMIT) && !(pad & SEGA_CTRL_MODE))
                    open_now = fold_grp[g] ? 1 : 0;
                if (open_now == 1) {
                    fold_grp[g] = 0;
                    rebuild_items = 1; refocus_grp = g; pending_open = g;
                } else if (open_now == 0) {
                    anim_hdr = cur; anim_n = 0;
                    while (cur + 1 + anim_n < n_items &&
                           items[cur + 1 + anim_n].kind == IT_MAP) anim_n++;
                    gap_cur = anim_n * ROW_H; gap_vel = 0; gap_tgt = 0;
                    anim_closing = 1; anim_grp_closing = g; anim_ticks = 0;
                }
            } else if ((pressed & LOBBY_COMMIT) && !(pad & SEGA_CTRL_MODE)) {
                /* CONTROLS opens its sub-screen and returns here; a map row or
                 * PROCEDURAL confirms and starts. MODE-held presses are debug
                 * combos (MODE+X/Y), not commits. */
                if (items[cur].kind == IT_CTRL) {
                    show_controls_screen();
                    prev_pad = 0xFFFF;       /* swallow the still-held button */
                    continue;
                }
                if (items[cur].kind != IT_FOLD) break;
            }
            metrics_mode_check(pad);
            frame++;

            SHARED_UC->frame_count++;
            raycast_render();                    /* stationary lobby view */
            uint8_t *fb_text = (uint8_t *)((uintptr_t)&MARS_FRAMEBUFFER + 0x200);
            /* Smoked-glass panel first, then the text over it. Fixed bounds
             * cover title, the VIS-row window (+ overflow arrows) and the
             * hint line, so the box doesn't pump as the list scrolls. */
            lobby_menu_panel(fb_text, 48, 24, 272, 52 + 7 * 14 + 30);
            /* Title. */
            font_draw_string(fb_text, (SCREEN_W - 13 * 8) / 2, 32,
                             "BACKROOMS 32X", 49);

            /* The unified list, smooth-scrolled: ease scroll_px a quarter of
             * the remaining distance per frame toward centering the cursor
             * (clamped at the ends), then draw every row at its pixel offset,
             * culling rows outside the window. */
            const int MENU_X = 96;
            {
                int max_scroll = n_items * ROW_H - LIST_H;
                if (max_scroll < 0) max_scroll = 0;
                int tgt = cur * ROW_H - (LIST_H / 2 - ROW_H / 2);
                if (tgt < 0) tgt = 0;
                if (tgt > max_scroll) tgt = max_scroll;
                int d = tgt - scroll_px;
                scroll_px += (d > 3 || d < -3) ? (d >> 2) : (d > 0) - (d < 0);

                /* Unroll spring: stiffness 3/4, damping 5/8 per frame — ONE
                 * overshoot then settle. Damping was 3/8, which claimed a
                 * single overshoot but actually rang three times: simulating
                 * the integer spring at a 3-row target gives peaks at 44 (tgt
                 * 30), then 27, then back over — a visible double-bounce that
                 * took 11 frames to settle. 5/8 overshoots exactly once and
                 * settles in 5, so it reads calmer AND snappier. (3/4 kills
                 * the bounce entirely and feels dead; 1/2 still rings twice.)
                 * When a CLOSE settles, commit the fold and rebuild without
                 * the children. */
                int disp = 0, span = 0;
                if (anim_hdr >= 0) {
                    /* Symmetric integer spring: TRUE division truncates toward
                     * zero for both signs — arithmetic >> floors negatives,
                     * which let the CLOSING spring ring in a small limit cycle
                     * that never hit the settle window and left the header
                     * deaf to input (the expand-after-collapse lockup). The
                     * tick failsafe guarantees settle even so. */
                    gap_vel += (gap_tgt - gap_cur) * 3 / 4;
                    gap_vel -= gap_vel * 5 / 8;
                    gap_cur += gap_vel;
                    span = anim_n * ROW_H;
                    anim_ticks++;
                    int settled = (gap_tgt - gap_cur < 3 && gap_cur - gap_tgt < 3 &&
                                   gap_vel < 3 && gap_vel > -3) || anim_ticks > 20;
                    if (settled) {
                        gap_cur = gap_tgt;
                        if (anim_closing) {
                            fold_grp[anim_grp_closing] = 1;
                            rebuild_items = 1; refocus_grp = anim_grp_closing;
                        }
                        anim_hdr = -1;
                    }
                    disp = span - gap_cur;
                }
                int hdr_ry = (anim_hdr >= 0)
                           ? LIST_Y + anim_hdr * ROW_H - scroll_px : 0;
                for (int i = 0; i < n_items; i++) {
                    int ry = LIST_Y + i * ROW_H - scroll_px;
                    if (anim_hdr >= 0 && i > anim_hdr) {
                        ry -= disp;                       /* rows ride the gap */
                        if (i <= anim_hdr + anim_n && ry <= hdr_ry + 4)
                            continue;                     /* still under the header */
                    }
                    if (ry < LIST_Y - 3 || ry > LIST_Y + LIST_H - 8) continue;
                    if (items[i].kind == IT_SEP)
                        font_draw_string(fb_text, MENU_X + 2 * 8, ry, items[i].label, 49);
                    else if (items[i].kind == IT_FOLD) {
                        char fl[20]; int p = 0;
                        fl[p++] = fold_grp[items[i].map] ? '>' : '|';
                        fl[p++] = ' ';
                        for (const char *q = items[i].label; *q && p < 19; q++) fl[p++] = *q;
                        fl[p] = 0;
                        if (cur == i && (SHARED_UC->frame_count % 6) < 3)
                            lobby_hl_bar(fb_text, MENU_X, ry, p * 8);
                        font_draw_string(fb_text, MENU_X, ry, fl, 49);
                    } else
                        lobby_action_row(fb_text, MENU_X, ry, cur == i, items[i].label);
                }
                /* Up-overflow arrow only — the down arrow kept reading as a
                 * stray glyph, and the smooth scroll + peeking rows already
                 * say "there's more below". */
                if (scroll_px > 0)
                    font_draw_string(fb_text, 248, LIST_Y + 2, "^", 49);
            }
            font_draw_string(fb_text, (SCREEN_W - 26 * 8) / 2, LIST_Y + LIST_H + 16,
                             "U/D: PICK   ANY BUTTON: GO", 49);

            /* PAD-type readout (debug, top-left): 6/3/? button handshake. */
            uint16_t ptype = pad & SEGA_CTRL_TYPE;
            char padline[8] = { 'P','A','D',':',' ',
                (ptype == SEGA_CTRL_SIX) ? '6' : (ptype == SEGA_CTRL_THREE) ? '3' : '?',
                0, 0 };
            font_draw_string(fb_text, 8, 8, padline, 49);
            if (g_metrics_on) { prof_sample_and_draw(fb_text); pos_draw(fb_text); }
            if (g_padtest_on) pad_test_draw(fb_text, pad);
            swapBuffers();
        }
    }

    /* Phase A.5 — procedural weight tuning. Only when PROCEDURAL is chosen:
     * the player dials the generation mix (or leaves the balanced default)
     * before walking out. UP/DOWN pick a knob, LEFT/RIGHT adjust it, C resets
     * to defaults, START locks it in. Drawn over the live lobby view. */
    if (items[cur].kind == IT_PROC) {
        static const char *const labels[6] = {
            "OPENNESS    ", "PARTITIONS  ", "CRAWLSPACES ",
            "OUTLETS     ", "SPOTTED     ", "SEE-OVER    " };
        uint8_t *const wv[6] = {
            &g_procgen_params.openness,  &g_procgen_params.partitions,
            &g_procgen_params.crawlspaces, &g_procgen_params.outlets,
            &g_procgen_params.spotted,   &g_procgen_params.lowdivs };
        int row = 0;
        uint16_t prev_pad = 0xFFFF;
        for (;;) {
            HwMdReadPad(0);
            uint16_t pad = MARS_SYS_COMM8;
            uint16_t pressed = (uint16_t)(pad & ~prev_pad);
            prev_pad = pad;
            if ((pressed & SEGA_CTRL_UP)    && row > 0) row--;
            if ((pressed & SEGA_CTRL_DOWN)  && row < 5) row++;
            if ((pressed & SEGA_CTRL_LEFT)  && *wv[row] > 0) (*wv[row])--;
            if ((pressed & SEGA_CTRL_RIGHT) && *wv[row] < PROCGEN_MAX_W) (*wv[row])++;
            if (pressed & SEGA_CTRL_C) procgen_params_default();
            if (pressed & SEGA_CTRL_START) break;     /* lock in, generate */
            metrics_mode_check(pad);
            frame++;

            SHARED_UC->frame_count++;
            raycast_render();                          /* live lobby behind */
            uint8_t *fb_text = (uint8_t *)((uintptr_t)&MARS_FRAMEBUFFER + 0x200);
            font_draw_string(fb_text, (SCREEN_W - 15 * 8) / 2, 36,
                             "TUNE PROCEDURAL", 49);
            for (int i = 0; i < 6; i++) {
                /* "> OPENNESS    0 --|-- 4" — a slider | along a 0..MAX track,
                 * min on the left, max value on the right, so the level reads
                 * unambiguously (the old [##--] bar was unclear). */
                char line[32]; int n = 0;
                line[n++] = (i == row) ? '>' : ' ';
                line[n++] = ' ';
                for (const char *p = labels[i]; *p; p++) line[n++] = *p;
                line[n++] = '0';
                line[n++] = ' ';
                for (int b = 0; b <= PROCGEN_MAX_W; b++)
                    line[n++] = (b == *wv[i]) ? '|' : '-';
                line[n++] = ' ';
                line[n++] = (char)('0' + PROCGEN_MAX_W);
                line[n]   = 0;
                font_draw_string(fb_text, (SCREEN_W - n * 8) / 2, 64 + i * 14, line, 49);
            }
            font_draw_string(fb_text, (SCREEN_W - 23 * 8) / 2, SCREEN_H - 28,
                             "L/R ADJUST   C DEFAULTS", 49);
            font_draw_string(fb_text, (SCREEN_W - 14 * 8) / 2, SCREEN_H - 14,
                             "START GENERATE", 49);
            swapBuffers();
        }
    }

    /* Phase B — menu dismissed, choice locked. Walk up to the black void
     * (world_map cell == 2, the dark exit doorway along the east wall) and
     * step through it. */
    {
        for (;;) {
            HwMdReadPad(0);
            uint16_t pad = MARS_SYS_COMM8;
            metrics_mode_check(pad);
            /* MODE is a combo modifier: while held, UP/DOWN drive the automap
         * zoom, so they must not also walk the player. */
        player_update((pad & SEGA_CTRL_MODE)
                      ? (pad & ~(SEGA_CTRL_UP | SEGA_CTRL_DOWN)) : pad);
            /* Exit when the player's cell sits against a black-void cell (==2),
             * any side. Robust to where on the void edge you arrive — the old
             * fixed x>7 / y<5 box missed the bottom row of the doorway. */
            int pcx = FX_INT(player.x), pcy = FX_INT(player.y);
            if ((pcx + 1 < MAP_W && world_map[pcy][pcx + 1] == 2) ||
                (pcx - 1 >= 0    && world_map[pcy][pcx - 1] == 2) ||
                (pcy + 1 < MAP_H && world_map[pcy + 1][pcx] == 2) ||
                (pcy - 1 >= 0    && world_map[pcy - 1][pcx] == 2)) break;
            SHARED_UC->frame_count++;
            raycast_render();
            uint8_t *fb_text = (uint8_t *)((uintptr_t)&MARS_FRAMEBUFFER + 0x200);
            if (g_metrics_on) { prof_sample_and_draw(fb_text); pos_draw(fb_text); }
            if (g_padtest_on) pad_test_draw(fb_text, pad);
            swapBuffers();
        }
    }

    /* Walk-through transition: fade the lobby to black, swap in the chosen
     * map behind the black, fade it up — reads as the lobby sealing off
     * and the backrooms opening ahead. (Own vblank flip so it bypasses
     * raycast_shimmer, which would reset the bright palette mid-fade.) */
    for (int lvl = FADE_STEPS; lvl >= 0; lvl -= 2) {
        SHARED_UC->frame_count++;
        raycast_render();
        while (lastTick == MARS_SYS_COMM12);
        raycast_set_brightness(lvl);
        MARS_VDP_FBCTL = currentFB ^ 1;
        while ((MARS_VDP_FBCTL & MARS_VDP_FS) == currentFB);
        currentFB ^= 1;
        lastTick = MARS_SYS_COMM12;
    }

    if (items[cur].kind == IT_PROC) {
        g_custom_current = -1;
        g_procgen_seed = (uint32_t)frame * 1000003u + (uint32_t)player.x;
        procgen_run(g_procgen_seed);
        player.x = FX(16.5); player.y = FX(28.5); player.angle = 192;
    } else if (custom_pick_count > 0) {
        g_custom_current = items[cur].map;
        raycast_load_custom(items[cur].map);   /* core or community; sets its own spawn */
    } else {
        g_custom_current = -1;
        raycast_load_fixed();
    }
    raycast_init();                 /* rebuilds full-bright palette... */
    raycast_set_brightness(0);      /* ...but hold black until the fade-in */

    for (int lvl = 0; lvl <= FADE_STEPS; lvl += 2) {
        SHARED_UC->frame_count++;
        raycast_render();
        while (lastTick == MARS_SYS_COMM12);
        raycast_set_brightness(lvl);
        MARS_VDP_FBCTL = currentFB ^ 1;
        while ((MARS_VDP_FBCTL & MARS_VDP_FS) == currentFB);
        currentFB ^= 1;
        lastTick = MARS_SYS_COMM12;
    }

    for (;;) {
        /* Read the joypad up-front so the menu can both react to
         * START and tell player_update to skip movement when open. */
        HwMdReadPad(0);
        uint16_t pad = MARS_SYS_COMM8;

        menu_update(pad);
        /* MAPS tab asked to warp -> fade to the chosen custom map. */
        if (g_warp_request >= 0) {
            int t = g_warp_request; g_warp_request = -1;
            portal_to_custom(t);
            continue;
        }
        metrics_mode_check(pad);
        if (!menu_is_active()) {
            /* MODE is a combo modifier: while held, UP/DOWN drive the automap
         * zoom, so they must not also walk the player. */
        player_update((pad & SEGA_CTRL_MODE)
                      ? (pad & ~(SEGA_CTRL_UP | SEGA_CTRL_DOWN)) : pad);
            /* Stepped into the open EXIT door. On a story map (next: set) the
             * door is the CHAPTER TRANSITION — jump to the linked map. Anywhere
             * else it falls through to the endless procgen backrooms (which is
             * also how every story ultimately ends). */
            if (raycast_door_portal_check()) {
                int nx = (g_custom_current >= 0)
                       ? custom_maps[g_custom_current].next_map : -1;
                if (nx >= 0 && nx < custom_map_count) portal_to_custom(nx);
                else                                  portal_to_procgen();
                continue;
            }
        }
        /* Tick the shared frame counter before render so both CPUs
         * read the same value when computing the distant-wall strobe. */
        SHARED_UC->frame_count++;
        raycast_render();
        uint8_t *fb_text = (uint8_t *)((uintptr_t)&MARS_FRAMEBUFFER + 0x200);
        if (g_automap_on) automap_draw(fb_text);   /* red vectors under the text */
        menu_render(fb_text);
        if (g_metrics_on) {
            prof_sample_and_draw(fb_text);
            pos_draw(fb_text);
        }
        if (g_padtest_on) pad_test_draw(fb_text, pad);
        swapBuffers();
    }
    return 0;
}
