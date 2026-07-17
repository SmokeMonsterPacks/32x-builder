#include "mars.h"
#include "font.h"
#include "menu.h"
#include "raycast.h"
#include "shared.h"
#include "version.h"
#include "custom_maps.h"

/* Owned by m_main.c — the metrics-overlay gate. Exposed so the LIGHTING tab can
 * toggle it: the MODE-button shortcut is 6-button-only, so this is the way to
 * reach the overlay on a 3-button pad. */
extern uint8_t g_metrics_on;
/* Owned by m_main.c — the MAPS tab writes the chosen custom-map index here and
 * the main loop drains it into the warp. -1 = no request. */
extern volatile int g_warp_request;

/* Two-tab pause menu. START opens/closes; tabs (AUDIO / LIGHTING) sit
 * on row 0 and LEFT/RIGHT switches between them when that row is
 * focused. UP/DOWN cycles between the tab row and the per-tab content
 * rows; LEFT/RIGHT on a content row adjusts the value (sliders) or
 * inverts the bool (toggles). Settings live in shared SDRAM so the
 * secondary's audio pump and the raycaster's effect gates see edits
 * immediately via cache-through. */

#define TAB_AUDIO    0
#define TAB_LIGHTING 1
#define TAB_VISUALS  2
#define TAB_CREDITS  3
#define TAB_MAPS     4
#define NUM_TABS     5

#define AUDIO_CONTENT_ROWS    2   /* AMBIENCE, FOOTSTEPS */
#define LIGHTING_CONTENT_ROWS 3   /* FLICKER, STROBES, SHIMMER */
#define VISUALS_CONTENT_ROWS  3   /* WALLS (h-res), VERT (v-res), METRICS */
#define CREDITS_CONTENT_ROWS  0   /* BUILD/DATE/SHA are read-only display */

static int      menu_active = 0;
static int      menu_dirty  = 1;   /* menu content changed -> rewrite tiles */
static int      menu_redraw = 0;   /* this frame's gate (set from menu_dirty) */
static int      menu_tab    = TAB_AUDIO;
static int      menu_row    = 0;   /* 0 = tab row, 1..N = content row */
static uint16_t menu_prev_pad = 0;

/* Layout — 22-char × 10-row box (176 × 80 px) centered on the 320×224
 * screen, wide enough for the "LIGHTING |CREDITS|" tab row and tall
 * enough for the LIGHTING tab's three toggle rows. */
#define MENU_W_PX      176
#define MENU_H_PX       80
#define MENU_X        ((SCREEN_W - MENU_W_PX) / 2)
#define MENU_Y        ((SCREEN_H - MENU_H_PX) / 2)

#define MENU_BG_COLOR  46   /* CEIL_BASE end = dark eggshell */
#define MENU_FG_COLOR  49   /* LIGHT_BASE[0] = bright white-ish */
/* Selection highlight bar: a muted shade off the same LIGHT_BASE white ramp the
 * text is drawn from (49=full text, 50/51/52 = 75/50/25%). Picking this existing
 * index only changes which color fills the bar — it never rewrites a palette
 * entry, so the live 3D view behind the menu is untouched, and the bright text
 * (49) reads cleanly on top. The bar blinks on/off at ~10 Hz (see hl_blink). */
#define MENU_HL_BAR    51   /* LIGHT_BASE+2 (50%) */
/* Genesis-tile HUD: menu text lives on Name Table B now. 0 = palette line 0
 * (CRAM entry 1 = light gray) — the white-ish menu look. */
#define MENU_TILE_COLOR 0x0000
/* px/py (framebuffer) -> Genesis tile x/y. MENU_X/Y are 8-aligned. */
#define TX(px) ((px) >> 3)
#define TY(py) ((py) >> 3)
/* Blank the menu box's tile rows so navigation/value changes leave no ghosts.
 * The menu box spans tile cols 9..30, rows 9..18. */
static void menu_genesis_blank(void) {
    static char blank[24] = "                       ";
    for (int r = TY(MENU_Y); r <= TY(MENU_Y + MENU_H_PX); r++)
        HwMdPuts(blank, 0, TX(MENU_X), r);
}

/* Write s at tile (col, MENU_Y+y_off), space-padded to `width` so it overwrites
 * whatever was on that row in ONE pass — no separate blank, so navigating never
 * shows a blanked row mid-scan on the single-buffered nametable (the per-move
 * flash). Gated by menu_redraw so tiles are touched only when content changes. */
static void menu_puts_pad(int col, int y_off, const char *s, int width) {
    if (!menu_redraw) return;
    char buf[24];
    if (width > 23) width = 23;
    int n = 0;
    while (s[n] && n < width) { buf[n] = s[n]; n++; }
    while (n < width) buf[n++] = ' ';
    buf[n] = 0;
    HwMdPuts(buf, MENU_TILE_COLOR, col, TY(MENU_Y + y_off));
}

#define VOL_STEP 16

int menu_is_active(void) {
    return menu_active;
}

static int content_rows_for(int tab) {
    switch (tab) {
    case TAB_AUDIO:    return AUDIO_CONTENT_ROWS;
    case TAB_LIGHTING: return LIGHTING_CONTENT_ROWS;
    case TAB_VISUALS:  return VISUALS_CONTENT_ROWS;
    case TAB_MAPS:     return custom_pick_count;   /* one row per compiled-in map */
    default:           return CREDITS_CONTENT_ROWS;
    }
}

void menu_update(uint16_t pad) {
    uint16_t pressed = (uint16_t)(pad & ~menu_prev_pad);
    menu_prev_pad = pad;

    if (pressed & SEGA_CTRL_START) {
        menu_active = !menu_active;
        if (menu_active) { menu_row = 0; menu_dirty = 1; }
        else menu_genesis_blank();   /* wipe the menu tiles on close */
        return;
    }
    if (!menu_active) return;
    if (pressed) menu_dirty = 1;     /* any input can change the tile content */

    int total_rows = 1 + content_rows_for(menu_tab);

    if (pressed & SEGA_CTRL_UP) {
        menu_row = (menu_row + total_rows - 1) % total_rows;
    }
    if (pressed & SEGA_CTRL_DOWN) {
        menu_row = (menu_row + 1) % total_rows;
    }

    /* MAPS tab: A on a map row warps there and closes the menu. */
    if (menu_tab == TAB_MAPS && menu_row >= 1 && (pressed & SEGA_CTRL_A)) {
        if (menu_row - 1 < custom_pick_count) {
            g_warp_request = menu_row - 1;
            menu_active = 0;
            menu_genesis_blank();    /* warp-close: wipe tiles like a START close */
        }
        return;
    }

    int dir = 0;
    if (pressed & SEGA_CTRL_LEFT)  dir = -1;
    if (pressed & SEGA_CTRL_RIGHT) dir = +1;
    if (dir == 0) return;

    if (menu_row == 0) {
        /* Tab row: LEFT/RIGHT cycles through the tabs (wraps both ways). */
        menu_tab = (menu_tab + dir + NUM_TABS) % NUM_TABS;
        return;
    }

    if (menu_tab == TAB_AUDIO) {
        volatile uint8_t *target =
            (menu_row == 1) ? &SHARED_UC->amb_volume
                            : &SHARED_UC->step_volume;
        int v = (int)*target + dir * VOL_STEP;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        *target = (uint8_t)v;
    } else if (menu_tab == TAB_LIGHTING) {
        /* LIGHTING tab: toggle the corresponding effect bit. dir doesn't
         * matter — LEFT and RIGHT both flip. */
        uint8_t bit;
        switch (menu_row) {
        case 1: bit = LIGHTING_FLICKER; break;
        case 2: bit = LIGHTING_STROBE;  break;
        default: bit = LIGHTING_SHIMMER; break;
        }
        SHARED_UC->lighting_flags ^= bit;
    } else if (menu_tab == TAB_VISUALS) {
        /* WALLS res mode cycles FULL/HALF/AUTO (LEFT/RIGHT step the cycle);
         * VERT (vertical half-res) and METRICS overlay are flips. */
        if (menu_row == 1) {
            int m = (int)SHARED_UC->wall_res_mode + dir;
            SHARED_UC->wall_res_mode = (uint8_t)((m + 4) % 4);
        } else if (menu_row == 2) SHARED_UC->vres_half ^= 1;
        else if (menu_row == 3) g_metrics_on ^= 1;
    }
}

static void fmt_pct(uint8_t v, char out[4]) {
    int pct = ((int)v * 100 + 127) / 255;
    out[0] = (pct >= 100) ? '1' : ' ';
    out[1] = (pct >=  10) ? ('0' + ((pct / 10) % 10)) : ' ';
    out[2] = ('0' + (pct % 10));
    out[3] = 0;
}

static void fill_bg(uint8_t *fb) {
    for (int yy = 0; yy < MENU_H_PX; yy++) {
        uint8_t *row = fb + (MENU_Y + yy) * SCREEN_W + MENU_X;
        for (int xx = 0; xx < MENU_W_PX; xx++) row[xx] = MENU_BG_COLOR;
    }
}

/* ~10 Hz blink: on for 3 frames, off for 3 (6-frame period at 60 fps). Tied to
 * frame_count, not wall-clock. The selection bar is drawn only while this is
 * true, so it flashes around the focused option. */
static int hl_blink(void) {
    return (SHARED_UC->frame_count % 6) < 3;
}

/* Concise highlight spanning `ncols` 8px character cells starting at character
 * column `col` (relative to the box left edge), 8px tall. Drawn before the text
 * so the glyphs (which only stamp set pixels) sit on top of it. Used to frame
 * just the selected option's word, padded by its leading/trailing space. */
static void draw_word_hl(uint8_t *fb, int y_off, int col, int ncols, uint8_t color) {
    int x0 = MENU_X + col * 8;
    for (int yy = 0; yy < 8; yy++) {
        uint8_t *row = fb + (MENU_Y + y_off + yy) * SCREEN_W + x0;
        for (int xx = 0; xx < ncols * 8; xx++) row[xx] = color;
    }
}

/* Render one content row with a "> LABEL  VALUE" layout. sel marks
 * which row is currently selected (shows the > prefix). */
static void draw_row(uint8_t *fb, int y_off, int sel,
                     const char *label, const char *value) {
    /* Blink a concise bar around just the selected row's label (the label sits
     * at column 3, after the "> " prefix; pad one space each side). FB side,
     * every frame — independent of the tile redraw. */
    if (sel && hl_blink()) {
        int ll = 0; while (label[ll]) ll++;
        draw_word_hl(fb, y_off, 2, ll + 2, MENU_HL_BAR);
    }
    /* One full-width line "> LABEL      VALUE" (label at col 2, value at col 12)
     * so the whole row is overwritten in place — no blank, no flash. */
    char line[22];
    for (int k = 0; k < 20; k++) line[k] = ' ';
    line[0] = sel ? '>' : ' ';
    for (int i = 0; label[i] && i < 9; i++)   line[2 + i]  = label[i];
    for (int j = 0; value[j] && 12 + j < 20; j++) line[12 + j] = value[j];
    line[20] = 0;
    menu_puts_pad(TX(MENU_X + 8), y_off, line, 20);
}

void menu_render(uint8_t *fb) {
    if (!menu_active) return;
    fill_bg(fb);              /* the dimming panel stays on the FB for now */
    menu_redraw = menu_dirty; menu_dirty = 0;   /* rewrite tiles only on change */

    const int X = MENU_X;

    /* Top + bottom rule. Every write below is full-width and overwrites its row
     * in place, so no blank pass is needed on a change — that blank-then-redraw
     * was the per-move flash. (The close path still blanks to clear the menu.) */
    menu_puts_pad(TX(X), 0,  "+--------------------+", 22);
    menu_puts_pad(TX(X), 72, "+--------------------+", 22);

    /* Tab row at y=16: the > cursor points at the active tab, with the next
     * tab in the cycle shown after a pipe separator — e.g. "> AUDIO |
     * LIGHTING |". Reads as "AUDIO is selected; LIGHTING is next". The >
     * shows only when the tab row is focused (menu_row == 0), so it doubles
     * as the focus marker; LEFT/RIGHT cycles tabs there. The active tab is
     * always leftmost, so it stays clear even with the cursor hidden. */
    static const char *const tab_names[NUM_TABS] = {
        "AUDIO", "LIGHTING", "VISUALS", "CREDITS", "MAPS" };
    int tab_sel  = (menu_row == 0);
    int next_tab = (menu_tab + 1) % NUM_TABS;
    char tab_text[24];
    int t = 0;
    tab_text[t++] = tab_sel ? '>' : ' ';
    tab_text[t++] = ' ';
    for (const char *p = tab_names[menu_tab]; *p; p++) tab_text[t++] = *p;
    tab_text[t++] = ' ';
    tab_text[t++] = '|';
    tab_text[t++] = ' ';
    for (const char *p = tab_names[next_tab]; *p; p++) tab_text[t++] = *p;
    tab_text[t++] = ' ';
    tab_text[t++] = '|';
    tab_text[t]   = 0;
    /* Blink a concise bar around just the active tab name (leading/trailing
     * space included) while the tab row is focused — name starts at column 2,
     * after the "> " prefix. */
    if (tab_sel && hl_blink()) {
        int wl = 0; while (tab_names[menu_tab][wl]) wl++;
        draw_word_hl(fb, 16, 1, wl + 2, MENU_HL_BAR);
    }
    menu_puts_pad(TX(X), 16, tab_text, 22);

    /* Content rows at y = 32, 40, 48. */
    char num[4];
    if (menu_tab == TAB_AUDIO) {
        fmt_pct(SHARED_UC->amb_volume, num);
        draw_row(fb, 32, menu_row == 1, "AMBIENCE",  num);
        fmt_pct(SHARED_UC->step_volume, num);
        draw_row(fb, 40, menu_row == 2, "FOOTSTEPS", num);
        draw_row(fb, 48, 0, "", "");                 /* 3rd row unused: keep it clear */
    } else if (menu_tab == TAB_LIGHTING) {
        uint8_t f = SHARED_UC->lighting_flags;
        draw_row(fb, 32, menu_row == 1, "FLICKER",
                 (f & LIGHTING_FLICKER) ? " ON" : "OFF");
        draw_row(fb, 40, menu_row == 2, "STROBES",
                 (f & LIGHTING_STROBE)  ? " ON" : "OFF");
        draw_row(fb, 48, menu_row == 3, "SHIMMER",
                 (f & LIGHTING_SHIMMER) ? " ON" : "OFF");
    } else if (menu_tab == TAB_VISUALS) {
        static const char *res_lbl[4] = { "FULL", "HALF", "AUTO", "SERL" };
        uint8_t m = SHARED_UC->wall_res_mode; if (m > 3) m = 1;
        draw_row(fb, 32, menu_row == 1, "WALLS", res_lbl[m]);
        draw_row(fb, 40, menu_row == 2, "VERT",
                 SHARED_UC->vres_half ? "HALF" : "FULL");
        draw_row(fb, 48, menu_row == 3, "METRICS",
                 g_metrics_on ? " ON" : "OFF");
    } else if (menu_tab == TAB_CREDITS) {
        /* CREDITS — read-only build stamp (no selection cursor). */
        menu_puts_pad(TX(X + 8), 32, "BUILD " VERSION_BUILD_STR, 20);
        menu_puts_pad(TX(X + 8), 40, "DATE  " VERSION_DATE_STR,  20);
        menu_puts_pad(TX(X + 8), 48, "SHA   " VERSION_SHA_STR,   20);
    } else { /* TAB_MAPS — scrolling list of the compiled-in custom maps */
        if (custom_pick_count == 0) {
            menu_puts_pad(TX(X + 8), 32, "  (NO MAPS)", 20);
            menu_puts_pad(TX(X + 8), 40, "", 20);
            menu_puts_pad(TX(X + 8), 48, "", 20);
        } else {
            int sel = menu_row - 1;            /* selected map, or -1 on the tab row */
            int off = 0;                       /* 3-row window scrolls with selection */
            if (custom_pick_count > 3) {
                off = (sel > 0 ? sel : 0) - 1;
                if (off < 0) off = 0;
                if (off > custom_pick_count - 3) off = custom_pick_count - 3;
            }
            /* Always paint all 3 window rows (blank when past the end) so the
             * list self-clears as it scrolls, no blank pass. */
            for (int i = 0; i < 3; i++) {
                int mi = off + i;
                if (mi < custom_pick_count) {
                    char line[22]; int p = 0;
                    line[p++] = (menu_row == mi + 1) ? '>' : ' ';
                    line[p++] = ' ';
                    for (const char *nm = custom_maps[mi].name; *nm && p < 19; ) line[p++] = *nm++;
                    line[p] = 0;
                    if (menu_row == mi + 1 && hl_blink())
                        draw_word_hl(fb, 32 + 8 * i, 2, p, MENU_HL_BAR);
                    menu_puts_pad(TX(X + 8), 32 + 8 * i, line, 20);
                } else {
                    menu_puts_pad(TX(X + 8), 32 + 8 * i, "", 20);
                }
            }
        }
    }

    /* Hint row at y=64. */
    menu_puts_pad(TX(X + 8), 64,
                  (menu_tab == TAB_MAPS) ? "A=GO  START=CLOSE" : "START TO CLOSE", 20);
}
