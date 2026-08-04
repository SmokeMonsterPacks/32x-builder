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

/* HUD text now lives on the GENESIS tile layer (Name Table B via HwMdPuts),
 * composited over the 3D — every glyph is a framebuffer store the SH-2 no
 * longer makes (the measured win: offload to the idle 68K, not FB tricks).
 * 0x4000 = palette line 2 = red (CRAM entry 33). */
#define HUD_TILE_COLOR 0x4000

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
/* GAME tab requests: whole-screen flows the main loop owns. VIEWER opens the
 * asset viewer over the paused game; LOBBY breaks the game loop back to the
 * start list (the "exit to main menu" that used to need a console reset). */
volatile int g_viewer_request = 0, g_lobby_request = 0;

/* GAME-tab automap hooks (menu.c): the same state the MODE+B combo and the
 * MODE+UP/DOWN zoom drive, reachable without MODE -- full parity for
 * 3-button pads and MODE+ABC hybrid layouts (gameplay itself never needed
 * X/Y/Z: run/crawl/activate/look all live on A/B/C). */
uint8_t m_main_automap_get(void) { return g_automap_on; }
void m_main_automap_cycle(int dir) {
    g_automap_on = (uint8_t)((g_automap_on + (dir < 0 ? 2 : 1)) % 3);
}
void m_main_automap_zoom(int dir) {
    if (dir > 0) { am_s_tgt += am_s_tgt >> 3; if (am_s_tgt > (64 << 16)) am_s_tgt = 64 << 16; }
    else         { am_s_tgt -= am_s_tgt >> 3; if (am_s_tgt < (2 << 16))  am_s_tgt = 2 << 16; }
}
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
    (void)fb;                                 /* text is on the Genesis layer now */
    HwMdPuts(l1, HUD_TILE_COLOR, 0, 5);
    HwMdPuts(l2, HUD_TILE_COLOR, 0, 6);
    HwMdPuts(l3, HUD_TILE_COLOR, 0, 7);
    HwMdPuts(l4, HUD_TILE_COLOR, 0, 8);
}

/* The nametable is single-buffered, so HUD tiles persist after MODE+Y off —
 * blank the rows the HUD used. One-time on toggle-off; ~one frame of COMM. */
void hud_genesis_blank(void) {   /* non-static: the menu's METRICS toggle blanks too */
    static char blank[41] = "                                        ";
    HwMdPuts(blank, 0, 0, 0);    /* X/Y + T/H/S */
    HwMdPuts(blank, 0, 0, 2);    /* A */
    HwMdPuts(blank, 0, 0, 23);   /* H/TX/P6 (chair A/B + pad type) — was MISSING:
                                  * turning metrics off left this row hovering */
    HwMdPuts(blank, 0, 0, 24);   /* D/Q/N/V/M (partition campaign) */
    HwMdPuts(blank, 0, 0, 25);   /* O/P/K/E */
    HwMdPuts(blank, 0, 0, 26);   /* C/G/R/W/F */
    HwMdPuts(blank, 0, 0, 27);   /* build stamp */
}

static void metrics_mode_check(uint16_t pad) {
    static uint16_t prev = 0xFFFF;
    /* Debug shortcuts live behind a HELD-MODE modifier: bare X used to cycle
     * the wall res while ALSO acting as a menu commit, and emulators with
     * loose six-button mappings fire phantom X/MODE singles. MODE alone now
     * does nothing (pure modifier); the combo edge-triggers on the second
     * button. The VISUALS pause-menu tab remains the discoverable path. */
    /* MODE must be held across TWO consecutive frames before any combo is
     * honored: a deliberate human MODE-hold always is, while a single-frame
     * phantom MODE (mis-read pad on real hardware) never toggles anything. */
    if ((pad & SEGA_CTRL_MODE) && (prev & SEGA_CTRL_MODE)) {
        if ((pad & SEGA_CTRL_X) && !(prev & SEGA_CTRL_X))
            SHARED_UC->wall_res_mode = (uint8_t)((SHARED_UC->wall_res_mode + 1) % 5);
        if ((pad & SEGA_CTRL_Y) && !(prev & SEGA_CTRL_Y)) {
            g_metrics_on ^= 1;
            if (!g_metrics_on) hud_genesis_blank();
        }
        if ((pad & SEGA_CTRL_B) && !(prev & SEGA_CTRL_B))
            g_automap_on = (uint8_t)((g_automap_on + 1) % 3);
        if ((pad & SEGA_CTRL_C) && !(prev & SEGA_CTRL_C))   /* partition diag (HUD J) */
            SHARED_UC->part_diag = (uint8_t)((SHARED_UC->part_diag + 1) % 3);
        if ((pad & SEGA_CTRL_A) && !(prev & SEGA_CTRL_A))   /* chair flat/textured A/B (HUD TX) */
            SHARED_UC->chair_tex ^= 1;
        if ((pad & SEGA_CTRL_Z) && !(prev & SEGA_CTRL_Z)) {
            g_padtest_on ^= 1;
            if (!g_padtest_on) {
                static char blank[41] = "                                        ";
                HwMdPuts(blank, 0, 0, 5); HwMdPuts(blank, 0, 0, 6);
                HwMdPuts(blank, 0, 0, 7); HwMdPuts(blank, 0, 0, 8);
            }
        }
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

/* Escape hatch: opening the pause menu clears EVERY debug overlay. Field
 * report (smokemonster, real hardware): phantom MODE combos from a mis-read
 * pad turned overlays ON that a three-button pad could never turn off —
 * START is the one button every controller has. Called from menu_update. */
void debug_overlays_clear(void) {
    if (g_metrics_on) { g_metrics_on = 0; hud_genesis_blank(); }
    g_automap_on = 0;
    if (g_padtest_on) {
        static char blank[41] = "                                        ";
        g_padtest_on = 0;
        HwMdPuts(blank, 0, 0, 5); HwMdPuts(blank, 0, 0, 6);
        HwMdPuts(blank, 0, 0, 7); HwMdPuts(blank, 0, 0, 8);
    }
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
    HwMdPuts(text, HUD_TILE_COLOR, 16, 0);   /* T/H/S, top-right, GENESIS layer */
    /* Build stamp: every metrics screenshot self-identifies. GENESIS layer. */
    HwMdPuts((char *)("B" VERSION_BUILD_STR " " VERSION_SHA_STR),
             HUD_TILE_COLOR, 25, 27);

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
        HwMdPuts(t2, HUD_TILE_COLOR, 0, 26);   /* C/G/R/W/F, GENESIS layer */
    }

    /* Third line: the SERIAL TAIL — primary-only post-sync work that the
     * C/G/R/W line does NOT cover. L = low-ceiling slab + bulkheads
     * (crawlspace, scene-dependent), P = lights + standups sprites. This is
     * the ~25%-of-frame block that was invisible until now. */
    {
        extern volatile uint16_t prof_pass_ovl, prof_pass_sprite, prof_pass_slab;
        extern volatile uint16_t prof_split_col, prof_dda_fat, prof_ovl_px;
        static const char lbl[6] = {'L', 'O', 'U', 'P', 'K', 'E'};
        uint16_t pv[6] = { prof_pass_slab, prof_pass_ovl, prof_ovl_px,
                           prof_pass_sprite, prof_split_col, prof_dda_fat };
        char t3[52];
        int pos = 0;
        for (int i = 0; i < 6; i++) {
            t3[pos++] = lbl[i];
            t3[pos++] = ':';
            uint16_t x = pv[i];
            for (int d = 4; d >= 0; d--) { t3[pos + d] = '0' + (x % 10); x /= 10; }
            pos += 5;
            t3[pos++] = ' ';
        }
        t3[pos] = 0;
        HwMdPuts(t3, HUD_TILE_COLOR, 0, 25);   /* O/P/K/E, GENESIS layer */
    }

    /* Fourth line — the PARTITION CAMPAIGN counters (primary half, per frame):
     * D = DDA steps walked, Q = run-extent cells re-scanned (per contact per
     * column!), N = partial contacts kept, V = columns on the slow overlay
     * path, M = columns promoted to the fast main path. Together with O
     * (overlay ticks) these say whether the wall pass's time goes to the DDA
     * walk, the run re-scans, or the overlay draw. */
    {
        extern volatile uint16_t prof_dda_steps, prof_runwalk, prof_efg_kept,
                                 prof_ovl_cols, prof_promote_cols;
        static const char lbl[5] = {'D', 'Q', 'N', 'V', 'M'};
        static const uint8_t wid[5] = {5, 5, 5, 3, 3};   /* V/M are column counts <= 320 */
        uint16_t pv[5] = { prof_dda_steps, prof_runwalk, prof_efg_kept,
                           prof_ovl_cols, prof_promote_cols };
        char t4[42];
        int pos = 0;
        for (int i = 0; i < 5; i++) {
            t4[pos++] = lbl[i];
            t4[pos++] = ':';
            uint16_t x = pv[i];
            for (int d = wid[i] - 1; d >= 0; d--) { t4[pos + d] = '0' + (x % 10); x /= 10; }
            pos += wid[i];
            t4[pos++] = ' ';
        }
        t4[pos++] = 'J';                     /* partition diag mode (MODE+C) */
        t4[pos++] = ':';
        t4[pos++] = (char)('0' + SHARED_UC->part_diag);
        t4[pos] = 0;
        HwMdPuts(t4, HUD_TILE_COLOR, 0, 24);   /* D/Q/N/V/M + J, GENESIS layer */
    }

    /* Fifth line — chair fill A/B (MODE+A). H = primary-half chair fill ticks
     * this frame (the flat/textured cost we're measuring); TX = 0 flat /
     * 1 textured. Face a rendered chair and toggle: watch H jump. */
    {
        extern volatile uint16_t prof_pass_chair;
        char t5[48];   /* H/TX/P6/AU + E + FQ — 38 cols of the 40-col layer */
        int pos = 0;
        t5[pos++] = 'H';
        t5[pos++] = ':';
        uint16_t x = prof_pass_chair;
        for (int d = 4; d >= 0; d--) { t5[pos + d] = '0' + (x % 10); x /= 10; }
        pos += 5;
        t5[pos++] = ' ';
        t5[pos++] = 'T';
        t5[pos++] = 'X';
        t5[pos++] = ':';
        t5[pos++] = (char)('0' + (SHARED_UC->chair_tex & 1));
        /* P6: what the 68K thinks the pad is (1 = six-button handshake
         * validated / sticky-latched, 0 = three-button). Field diagnostic for
         * "my MODE combos don't work" — reachable via pause menu -> VISUALS ->
         * METRICS, which needs no MODE press. */
        t5[pos++] = ' ';
        t5[pos++] = 'P'; t5[pos++] = '6'; t5[pos++] = ':';
        t5[pos++] = (char)('0' + ((MARS_SYS_COMM8 >> 12) & 1));
        /* AU: audio underruns — DMA swaps into a buffer the pump never
         * refilled (each one = an audible stale-fragment replay). Frozen
         * = healthy; climbing = the ping-pong is starving. Pair with the
         * AUDIO tab's BUFFER A/B row: 16MS should climb in dense scenes,
         * 64MS should hold. */
        t5[pos++] = ' ';
        t5[pos++] = 'A'; t5[pos++] = 'U'; t5[pos++] = ':';
        {
            uint16_t u = amb_get_underruns();
            for (int d = 4; d >= 0; d--) { t5[pos + d] = '0' + (u % 10); u /= 10; }
            pos += 5;
        }
        /* E: eye height above the floor (SHARED_UC->eye_h, 128 = standing,
         * 40 = fully crouched, anything between = mid-ease).
         *
         * X/Y/A alone do not pin down a screenshot: eye height is exactly what
         * changes between a standing and a crouching shot, and without it a
         * reported render bug cannot be reproduced on the host — it has to be
         * guessed at from pixel positions in a photo, which is unreliable
         * enough to have sent this desk investigation down two dead ends.
         * X/Y/A + E is a replayable camera. */
        t5[pos++] = ' ';
        t5[pos++] = 'E'; t5[pos++] = ':';
        {
            uint16_t e = SHARED_UC->eye_h;
            for (int d = 2; d >= 0; d--) { t5[pos + d] = '0' + (e % 10); e /= 10; }
            pos += 3;
        }
        t5[pos] = 0;
        HwMdPuts(t5, HUD_TILE_COLOR, 0, 23);   /* H + TX, GENESIS layer */
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

/* Current level's display name into out[] (needs 18 bytes): the authored map's
 * real name, or a pronounceable 8-char name derived from the procgen SEED
 * (consonant-vowel syllables) — deterministic, so a level keeps its name for as
 * long as you wander it. Shared by the automap and the pause CREDITS tab. */
void cur_map_name(char *out) {
    int n = 0;
    if (g_custom_current >= 0) {
        for (const char *p = custom_maps[g_custom_current].name; *p && n < 16; p++)
            out[n++] = *p;
    } else {
        static const char CONS[] = "BDKLMNPRSTVZ";   /* 12 */
        static const char VOWS[] = "AEIOU";          /*  5 */
        uint32_t h = g_procgen_seed * 2654435761u;   /* Knuth mix */
        for (int i = 0; i < 4; i++) {
            out[n++] = CONS[h % 12]; h /= 12;
            out[n++] = VOWS[h % 5];  h /= 5;
            h ^= h >> 7;
        }
    }
    out[n] = 0;
}

/* Current level's author credit. Authored maps carry their own; a blank field
 * and every procgen level credit the project. */
const char *cur_map_author(void) {
    if (g_custom_current >= 0) {
        const char *a = custom_maps[g_custom_current].author;
        if (a && *a) return a;
    }
    return "-BACKROOMS-";
}

/* Automap credits: the map NAME bottom-center in bright red, the AUTHOR
 * top-center in dim red. Both slide clear of the metrics HUD when it's up. */
static void am_footer(uint8_t *fb) {
    char name[18];
    cur_map_name(name);
    int n = 0; while (name[n]) n++;
    int y = g_metrics_on ? (SCREEN_H - 36) : (SCREEN_H - 12);
    font_draw_string(fb, (SCREEN_W - n * 8) / 2, y, name, AMAP_RED_BRIGHT);

    const char *author = cur_map_author();
    int an = 0; while (author[an]) an++;
    int ay = g_metrics_on ? 40 : 8;
    font_draw_string(fb, (SCREEN_W - an * 8) / 2, ay, author, AMAP_RED);
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

    HwMdPuts(line1, HUD_TILE_COLOR, 0, 0);   /* X/Y, top-left, GENESIS layer */
    HwMdPuts(line2, HUD_TILE_COLOR, 0, 2);   /* A, GENESIS layer */
}

void swapBuffers(void) {
    while (lastTick == MARS_SYS_COMM12);
    /* In vblank now — safe palette-write window. */
    raycast_shimmer();
    raycast_pal_flush();     /* live COLOR-tab palette edits, repaint when dirty */
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

/* ---- Start-menu flip-out ------------------------------------------------
 * On a final selection the whole menu pane is grabbed off the framebuffer and
 * replayed as a rotating textured quad: it hinges up in perspective and recedes
 * to nothing over the live lobby, so the menu physically lifts off and you're
 * standing in the level. This is the payoff for keeping the start menu on the
 * 32X — a per-pixel warp the framebuffer does for free and tiles never could. */
#define PANE_X 48
#define PANE_Y 24
#define PANE_W 224
#define PANE_H 156
static uint8_t pane_buf[PANE_W * PANE_H];   /* SDRAM: captured menu pane */

static void capture_menu_pane(const uint8_t *fb) {
    for (int v = 0; v < PANE_H; v++) {
        const uint8_t *s = fb + (PANE_Y + v) * SCREEN_W + PANE_X;
        uint8_t *d = pane_buf + v * PANE_W;
        for (int u = 0; u < PANE_W; u++) d[u] = s[u];
    }
}

/* Which exit transform the commit plays. Cycled at the start menu with
 * MODE+A, which also fires an instant slowed preview — the exploration loop
 * needs no rebuilds. 0 hinge-up, 3 fall-forward, 4 fly-through. */
static uint8_t g_flip_style = 3;

static void menu_flip_out(int style, int NF) {
    const int D  = 220;            /* viewer distance, px */
    const int CX = SCREEN_W / 2;   /* pane centre x */
    const int CY0 = PANE_Y + PANE_H / 2;
    const int HY  = PANE_Y + PANE_H;             /* bottom hinge (fall-forward) */
    for (int f = 1; f <= NF; f++) {
        SHARED_UC->frame_count++;
        raycast_render();                            /* live lobby behind */
        uint8_t *fb = (uint8_t *)((uintptr_t)&MARS_FRAMEBUFFER + 0x200);

        if (style == 4) {
            /* FLY-THROUGH: zoom about the pane centre (scale 1..8) with a
             * progressive checker dissolve — the menu blows past the camera
             * and shreds as you punch through into the level. Per-pixel work
             * is one add + mask via the incremental u walk. */
            fx_t k   = FX_ONE + (fx_t)(((int64_t)f * FX(7)) / NF);
            fx_t inv = FX_DIV(FX_ONE, k);
            int diss = (f * 17) / NF;                /* dissolve 0..16 */
            for (int sy = 0; sy < SCREEN_H; sy++) {
                int v = PANE_H / 2 + (int)(((int64_t)(sy - CY0) * inv) >> FX_SHIFT);
                if (v < 0 || v >= PANE_H) continue;
                const uint8_t *srow = pane_buf + v * PANE_W;
                uint8_t *drow = fb + sy * SCREEN_W;
                fx_t u_fx = ((fx_t)(PANE_W / 2) << FX_SHIFT) - (fx_t)CX * inv;
                int hash = (sy * 13) & 15;
                for (int sx = 0; sx < SCREEN_W; sx++, u_fx += inv) {
                    hash = (hash + 7) & 15;
                    if (hash < diss) continue;       /* dissolved away */
                    int u = (int)(u_fx >> FX_SHIFT);
                    if ((unsigned)u < (unsigned)PANE_W) drow[sx] = srow[u];
                }
            }
        } else if (style == 3) {
            /* FALL-FORWARD: hinged at the pane's bottom edge, the top falls
             * away from the camera until it lies flat — the menu topples like
             * the neanderthal. v measured up from the hinge:
             * v = dy*D/(D*cos - dy*sin), width scale 1/s = (D + v*sin)/D. */
            uint8_t ang = (uint8_t)((f * 60) / NF);  /* 0..~84 deg */
            int32_t cs = COS_FX(ang), sn = SIN_FX(ang);
            for (int sy = 0; sy < SCREEN_H; sy++) {
                int dyp = HY - sy;                   /* px above the hinge */
                if (dyp < 0) continue;
                int32_t denom = (int32_t)D * cs - dyp * sn;
                if (denom <= 0) continue;
                int32_t vpx = (int32_t)((((int64_t)dyp * D) << 16) / denom);
                int v = PANE_H - 1 - vpx;            /* source row (top falls) */
                if (v < 0 || v >= PANE_H) continue;
                int32_t denom_s = ((int32_t)D << 16) + vpx * sn;  /* D + v*sin */
                int half_w = (int)((((int64_t)(PANE_W / 2) * D) << 16) / denom_s);
                int32_t inv_s = (int32_t)((int64_t)denom_s / D);
                const uint8_t *srow = pane_buf + v * PANE_W;
                uint8_t *drow = fb + sy * SCREEN_W;
                int x0 = CX - half_w; if (x0 < 0) x0 = 0;
                int x1 = CX + half_w; if (x1 > SCREEN_W) x1 = SCREEN_W;
                for (int sx = x0; sx < x1; sx++) {
                    int u = PANE_W / 2 + (int)(((int32_t)(sx - CX) * inv_s) >> 16);
                    if (u >= 0 && u < PANE_W) drow[sx] = srow[u];
                }
            }
        } else {
            /* HINGE-UP (the original): tilts back at the top and lifts away. */
            uint8_t ang = (uint8_t)((f * 56) / NF);      /* tilt: 0..~78 deg */
            int CY = CY0 - (int)((int32_t)f * 46 / NF);  /* drift up as it lifts */
            int32_t cs = COS_FX(ang);                    /* 16.16 */
            int32_t sn = SIN_FX(ang);                    /* 16.16 */
            for (int sy = 0; sy < SCREEN_H; sy++) {
                int dy = sy - CY;
                int32_t denom = (int32_t)D * cs + dy * sn;
                if (denom <= 0) continue;
                int32_t Y = (int32_t)((((int64_t)dy * D) << 16) / denom);
                int v = Y + PANE_H / 2;
                if (v < 0 || v >= PANE_H) continue;
                int32_t denom_s = ((int32_t)D << 16) - Y * sn;   /* (D - Y*sin) */
                if (denom_s <= 0) continue;
                int half_w = (int)((((int64_t)(PANE_W / 2) * D) << 16) / denom_s);
                int32_t inv_s = (int32_t)((int64_t)denom_s / D);  /* 16.16 = 1/s */
                const uint8_t *srow = pane_buf + v * PANE_W;
                uint8_t *drow = fb + sy * SCREEN_W;
                int x0 = CX - half_w; if (x0 < 0) x0 = 0;
                int x1 = CX + half_w; if (x1 > SCREEN_W) x1 = SCREEN_W;
                for (int sx = x0; sx < x1; sx++) {
                    int u = PANE_W / 2 + (int)(((int32_t)(sx - CX) * inv_s) >> 16);
                    if (u >= 0 && u < PANE_W) drow[sx] = srow[u];
                }
            }
        }
        swapBuffers();
    }
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

/* ---- Asset viewer screen (start menu) --------------------------------
 * Dedicated black screen for inspecting assets WITHOUT loading a level:
 * the chair renders as the live clustered 3D mesh, free-rotated on both
 * axes by the D-pad; other assets show as their baked sprites. Self-owned
 * loop like the controls screen — no raycast_render behind it, index 0 is
 * true black in the gameplay palette. MODE+START exits back to the menu. */
static void asset_viewer_screen(void) {
    /* The hero backdrop painted ALL 256 CRAM entries with its own palette,
     * so every asset previewed here was decoding through the WRONG colors
     * (chair read tan, outlet read blank cream, door looked broken). Load
     * the full gameplay palette at full brightness — the viewer's whole
     * job is showing assets as the game shows them. Handed back to the
     * hero palette on exit below. Index 0 stays black (backdrop). */
    raycast_set_brightness(FADE_STEPS);
    HwMdReadPad(0);
    uint16_t prev = MARS_SYS_COMM8;          /* seed: ignore the held commit button */
    int sel = 3;                             /* start on CHAIR — the one with a 3D mesh */
    int variant = 0;                         /* chair only: 0 hero MESH, 1 GAME boxes, 2 SPRITE */
    uint8_t rotY = 32, rotX = 12;            /* engine angle units, 0..255 */
    int zoom = 3;                            /* mesh views: screen scale notch */
    /* Sprite views: a live world quad (tex_tri, the neanderthal's path).
     * UP/DOWN glides the distance CONTINUOUSLY — smooth scaling through the
     * real rasterizer — LEFT/RIGHT yaws it (edge-on, cardboard back, LOD). */
    fx_t adist = FX(2.5);
    int wire = 1;                            /* Z toggles; default ON — the filled
                                              * 1,692-tri hero view crawls */
    for (;;) {
        HwMdReadPad(0);
        uint16_t pad = MARS_SYS_COMM8;
        uint16_t pressed = (uint16_t)(pad & ~prev);
        prev = pad;
        if ((pad & SEGA_CTRL_MODE) && (pressed & SEGA_CTRL_START)) break;
        /* Held D-pad = continuous independent rotation on both axes. */
        if (pad & SEGA_CTRL_LEFT)  rotY = (uint8_t)(rotY - 2);
        if (pad & SEGA_CTRL_RIGHT) rotY = (uint8_t)(rotY + 2);
        if (pad & SEGA_CTRL_UP)    rotX = (uint8_t)(rotX + 2);
        if (pad & SEGA_CTRL_DOWN)  rotX = (uint8_t)(rotX - 2);
        /* Assets that own real 3D geometry: the chair (hand-authored boxes +
         * a baked hero mesh) and the DESK (imported GLB -> 3 boxes via
         * tools/bake_boxes.py). Everything else is sprite-only. */
        int model_id = (sel == CHAIR_ASSET_KIND) ? MODEL_CHAIR
                     : (sel == DESK_ASSET_KIND)  ? MODEL_DESK : -1;
        int mesh_shown = (model_id >= 0 && variant < 2);
        if (pressed & SEGA_CTRL_A) {
            /* sprite_defs[] is kind-indexed and sparse — step over the null
             * padding rows or the viewer lands on an empty asset. */
            int n = raycast_asset_count();
            for (int t = 0; t < n; t++) {
                sel = (sel + 1) % n;
                if (raycast_asset_valid(sel)) break;
            }
        }
        if (pressed & SEGA_CTRL_B) {
            if (mesh_shown) zoom = (zoom >= 5) ? 2 : zoom + 1;
        }
        if (pressed & SEGA_CTRL_C) { rotY = 32; rotX = 12; adist = FX(2.5); }
        if (pressed & SEGA_CTRL_X) variant = (variant + 1) % 3;
        if (pressed & SEGA_CTRL_Z) wire ^= 1;
        if (!mesh_shown) {                    /* held: glide the quad in/out */
            if (pad & SEGA_CTRL_UP)   { adist -= FX(0.07); if (adist < FX(0.5)) adist = FX(0.5); }
            if (pad & SEGA_CTRL_DOWN) { adist += FX(0.07); if (adist > FX(8))   adist = FX(8); }
        }
        SHARED_UC->frame_count++;

        uint8_t *fb = (uint8_t *)((uintptr_t)&MARS_FRAMEBUFFER + 0x200);
        /* Clear with 32-bit stores: the 32X framebuffer IGNORES byte writes of
         * zero (hardware sprite-transparency quirk), so a fb[i]=0 byte loop
         * silently does nothing. Word writes always land — same trick as
         * raycast_clear_half. Index 0 = true black in the gameplay palette. */
        {
            uint32_t *fb32 = (uint32_t *)fb;
            for (int i = 0; i < (SCREEN_W * SCREEN_H) / 4; i++) fb32[i] = 0;
        }
        if (mesh_shown)
            raycast_model_view(fb, rotY, rotX, 60 + zoom * 22, variant, wire, model_id);
        else
            raycast_asset_preview(fb, sel, rotY, adist);

        /* HUD: name + dims, live rotation coordinates, controls. */
        char line[40]; int p = 0;
        const char *nm = raycast_asset_name(sel);
        while (*nm && p < 14) line[p++] = *nm++;
        line[p++] = ' ';
        int w = 0, h = 0; raycast_asset_dims(sel, &w, &h);
        if (w >= 100) line[p++] = (char)('0' + (w / 100) % 10);
        line[p++] = (char)('0' + (w / 10) % 10); line[p++] = (char)('0' + w % 10);
        line[p++] = 'X';
        if (h >= 100) line[p++] = (char)('0' + (h / 100) % 10);
        line[p++] = (char)('0' + (h / 10) % 10); line[p++] = (char)('0' + h % 10);
        line[p] = 0;
        font_draw_string(fb, 8, 8, "ASSET VIEWER", 49);
        font_draw_string(fb, 8, 22, line, 49);
        p = 0;
        line[p++]='Y'; line[p++]=':';
        line[p++]=(char)('0'+(rotY/100)%10); line[p++]=(char)('0'+(rotY/10)%10); line[p++]=(char)('0'+rotY%10);
        line[p++]=' '; line[p++]='X'; line[p++]=':';
        line[p++]=(char)('0'+(rotX/100)%10); line[p++]=(char)('0'+(rotX/10)%10); line[p++]=(char)('0'+rotX%10);
        line[p++]=' ';
        if (mesh_shown) {
            line[p++]='Z'; line[p++]=':'; line[p++]=(char)('0'+zoom);
        } else {                              /* live distance in cells, one decimal */
            int d10 = (int)(((int64_t)adist * 10) >> FX_SHIFT);
            line[p++]='D'; line[p++]=':';
            line[p++]=(char)('0' + (d10 / 10) % 10);
            line[p++]='.';
            line[p++]=(char)('0' + d10 % 10);
        }
        line[p]=0;
        font_draw_string(fb, 8, 36, line, 49);
        if (model_id >= 0) {
            /* An imported model has no hero tri-mesh, so variants 0/1 both show
             * its boxes — label them BOXES rather than lying about a MESH. */
            static const char *const vnames[3]  = { "MESH",  "GAME", "SPRITE" };
            static const char *const vimport[3] = { "BOXES", "BOXES", "SPRITE" };
            const char *const *vn = (model_id == MODEL_CHAIR) ? vnames : vimport;
            font_draw_string(fb, 8, 50, vn[variant], 49);
            if (variant < 2)
                font_draw_string(fb, 64, 50, wire ? "WIRE" : "FILL", 49);
        }
        /* SPRITE view of a directional asset: report which baked frame the
         * bearing picker landed on, so rotating can be checked to reach every
         * one of them rather than assumed to. */
        if (!mesh_shown) {
            int nv = 0, dv = raycast_asset_dir_view(sel, rotY, &nv);
            if (nv > 0) {
                char t[16]; int q = 0;
                t[q++]='V'; t[q++]=':';
                t[q++]=(char)('0' + (dv + 1) / 10); t[q++]=(char)('0' + (dv + 1) % 10);
                t[q++]='/';
                t[q++]=(char)('0' + nv / 10); t[q++]=(char)('0' + nv % 10);
                t[q]=0;
                font_draw_string(fb, 8, 50, t, 49);
            }
        }
        font_draw_string(fb, 8, SCREEN_H - 24, "DPAD ROTATE  A ASSET  B ZOOM  C RESET", 49);
        font_draw_string(fb, 8, SCREEN_H - 12, "X VARIANT  Z WIRE  MODE+START BACK", 49);
        swapBuffers();
    }
    /* Restore the palette of the screen we RETURN to: the start menu draws
     * over the LIVE LOBBY render (raycast_render, the "stationary lobby
     * view"), so it needs the full gameplay palette back — the same one we
     * loaded on entry. box3d_load_palette() was wrong here: it re-stamps the
     * cardboard ramp across CRAM 64..79, which survives on the walls/floor/
     * ceiling (those live at 1..52) but paints the outlet (OUTLET_BASE 72..76)
     * cardboard-orange. Loading the game palette at full brightness restores
     * the outlet (and the neanderthal/partition ramps that also sit at 64+). */
    raycast_set_brightness(FADE_STEPS);
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

    /* ---- SESSION loop: lobby start list -> level -> game; the GAME tab's
     * EXIT TO LOBBY breaks the game loop and lands back here. The landing
     * cinematic above plays once per power-on only. */
    for (;;) {

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
    enum { IT_MAP, IT_PROC, IT_SEP, IT_FOLD, IT_CTRL, IT_VIEW };
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
    int do_flip = 0;              /* selection committed -> play the flip-out */
    int preview_flip = 0;         /* MODE+A: play a slowed preview this frame */
    uint32_t frame = 0;           /* time-in-lobby — entropy for procgen */
    const uint16_t LOBBY_COMMIT = SEGA_CTRL_START | SEGA_CTRL_A | SEGA_CTRL_B |
                                  SEGA_CTRL_C | SEGA_CTRL_X | SEGA_CTRL_Y | SEGA_CTRL_Z;

    /* Phase A — frozen menu over the still photo-perspective. */
    {
        uint16_t prev_pad = 0xFFFF;
        int committing = 0;       /* map/proc chosen -> capture + break this frame */
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
                items[n_items].kind = IT_VIEW; items[n_items].map = 0;
                items[n_items].label = "ASSET VIEWER"; n_items++;

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
                if (items[cur].kind == IT_VIEW) {
                    asset_viewer_screen();
                    prev_pad = 0xFFFF;       /* swallow the still-held button */
                    continue;
                }
                if (items[cur].kind != IT_FOLD) committing = 1;
            }
            /* MODE+A: cycle the exit transform and preview it slowed,
             * right here, no rebuild — 0 hinge-up, 3 fall, 4 fly-through. */
            if ((pad & SEGA_CTRL_MODE) && (pressed & SEGA_CTRL_A)) {
                g_flip_style = (g_flip_style == 0) ? 3
                             : (g_flip_style == 3) ? 4 : 0;
                preview_flip = 1;
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
            if (committing || preview_flip) capture_menu_pane(fb_text);
            if (committing) do_flip = 1;
            swapBuffers();
            if (preview_flip) {
                preview_flip = 0;
                menu_flip_out(g_flip_style, 12);   /* slowed so it reads */
                prev_pad = 0xFFFF;                 /* swallow the held combo */
            }
            if (committing) break;
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
        int committing = 0;       /* START locks in -> capture + break this frame */
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
            if (pressed & SEGA_CTRL_START) committing = 1;   /* lock in, generate */
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
            if (committing) { capture_menu_pane(fb_text); do_flip = 1; }
            swapBuffers();
            if (committing) break;
        }
    }

    if (do_flip)
        menu_flip_out(g_flip_style, g_flip_style == 4 ? 4 : g_flip_style == 3 ? 3 : 2);

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
        /* GAME tab: 3D viewer over the paused game (self-owned screen; it
         * restores the gameplay palette on exit), or back to the lobby. */
        if (g_viewer_request) {
            g_viewer_request = 0;
            asset_viewer_screen();
            continue;
        }
        if (g_lobby_request) {
            g_lobby_request = 0;
            break;                     /* -> session loop's lobby return */
        }
        metrics_mode_check(pad);
        if (!menu_is_active()) {
            /* EXIT-HOLE climb: input frozen while raycast_exit_pullup drives
             * the three POV beats (glance up, pull to the belly, enter the
             * aperture) over PULLUP_FRAMES rendered frames — it owns pitch,
             * eye height AND position. Then the portal fade fires with the
             * hole's walls surrounding the view. */
            #define PULLUP_FRAMES 21
            static int g_pullup = 0;
            if (g_pullup > 0) {
                g_pullup--;
                raycast_exit_pullup(PULLUP_FRAMES - g_pullup, PULLUP_FRAMES);
                if (g_pullup == 0) {
                    SHARED_UC->eye_h = 128;
                    portal_to_procgen();                   /* holes are procgen-only */
                    continue;
                }
            } else {
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
            /* Standing centered under the EXIT HOLE: start the climb-out. */
            if (raycast_exit_hole_check()) g_pullup = PULLUP_FRAMES;
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

    /* EXIT TO LOBBY: fade the level out, level the camera (stale hold-C
     * tilt or climb pitch would survive), restore the lobby, fade up, and
     * loop back to the start list. */
    for (int lvl = FADE_STEPS; lvl >= 0; lvl -= 2) fade_step(lvl);
    raycast_exit_pullup(0, 1);        /* zero the pitch channel */
    SHARED_UC->eye_h = 128;
    SHARED_UC->pitch_y = 0;
    g_custom_current = -1;
    raycast_load_lobby();
    raycast_init();
    raycast_set_brightness(0);
    for (int lvl = 0; lvl <= FADE_STEPS; lvl += 2) fade_step(lvl);

    }   /* SESSION loop */
    return 0;
}
