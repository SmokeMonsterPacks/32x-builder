; =====================================================================
; SMS MINI-GAME: TEST PATTERN (in-game blob flavor)
;
; This is the Master System game the 32X boots INSIDE the running
; Backrooms build. It is not a 32KB cartridge image: it executes from
; the Genesis Z80's 8KB RAM ($0000-$1FFF), where the 68K uploads it
; through the $A00000 window. The 68K then PATCHES the live level in:
; the SH-2 packs world_map to 1bpp + spawn + exit and streams it over
; COMM, and the 68K writes those 148 bytes into MAP_BITS/MAP_META
; below before releasing the Z80 from reset. Procgen or curated makes
; no difference — by then the map is just bytes in SDRAM.
;
; Division of labor (the proven mode-5 duet, see md_main.c):
;   Z80 (this file): ALL game state and logic. Owns an overhead-view
;     tile frame in TILEBUF (24 rows x 32 cols of boot-font tile ids).
;   68K: per frame, under a brief bus request, pokes the pad byte,
;     bumps FRAME_MBX, and if DIRTY_MBX is set copies TILEBUF out and
;     blits it to the mode-5 text layer. The Z80 never touches the VDP
;     (Ares' mode-4 stub and the Z80-control-write dup live in git
;     history as the reasons why).
;
; Mailbox / patch contract (fixed addresses, mirrored by
; tools/gen_z80_game.py into md_src/z80_sms_game.h):
;   TILEBUF   $1900  768B frame, row-major 24x32
;   MAP_BITS  $1C00  128B, 32 rows x 4 bytes, bit $80>>(x&7) = wall
;   MAP_META  $1C80  spawn_x, spawn_y, exit_x, exit_y
;   MAP_NAME  $1C84  16 tile-id bytes, the level's name (title + debrief)
;   PAD_MBX   $1FF4  pad byte (SEGA low byte: U1 D2 L4 R8 B10 C20 A40 ST80)
;   DIRTY_MBX $1FF5  Z80 sets 1 = new frame in TILEBUF; 68K clears
;   STATE_MBX $1FF6  0 = playing, 1 = escaped
;   FRAME_MBX $1FF7  68K increments once per 60Hz frame (Z80 paces on it)
;
; The exit cell may BE a wall cell (the exit door is stamped into the
; wall), so the win check runs before the wall check: stepping INTO
; the door is the escape.
;
; Build: make game.bin (wla-z80 + wlalink), then tools/gen_z80_game.py.
; =====================================================================

.MEMORYMAP
DEFAULTSLOT 0
SLOTSIZE $2000
SLOT 0 $0000
.ENDME

.ROMBANKMAP
BANKSTOTAL 1
BANKSIZE $2000
BANKS 1
.ENDRO

.EMPTYFILL $00

; ---------------- the contract ----------------
.DEFINE TILEBUF   $1900
.DEFINE MAP_BITS  $1C00
.DEFINE MAP_META  $1C80
.DEFINE MAP_NAME  $1C84   ; 16 tile-id bytes: the level's name, patched in
                          ; by the 68K with the map (TEST PATTERN <name>)
.DEFINE VAR_PX    $1CA0   ; player cell x
.DEFINE VAR_PY    $1CA1   ; player cell y
.DEFINE VAR_VPY   $1CA2   ; viewport top map row (0..8)
.DEFINE VAR_PPREV $1CA3   ; previous pad byte (edge detect)
.DEFINE VAR_FLAST $1CA4   ; last FRAME_MBX seen
.DEFINE VAR_RPT   $1CA5   ; held-direction auto-repeat counter
.DEFINE VAR_LFSR  $1CA6   ; music: 16-bit Galois LFSR state
.DEFINE VAR_BI    $1CA8   ; music: bass walker index
.DEFINE VAR_BATT  $1CA9   ; music: bass attenuation
.DEFINE VAR_BFADE $1CAA   ; music: frames to next bass fade step
.DEFINE VAR_BT    $1CAB   ; music: frames to next bass note (16-bit)
.DEFINE VAR_MPTR  $1CAD   ; music: current motif read pointer (16-bit)
.DEFINE VAR_MLEFT $1CAF   ; music: notes left in the motif
.DEFINE VAR_MATT  $1CB0   ; music: melody attenuation
.DEFINE VAR_MFADE $1CB1   ; music: frames to next melody fade step
.DEFINE VAR_NOTET $1CB2   ; music: frames to next note in the motif
.DEFINE VAR_GAP   $1CB3   ; music: frames of silence between phrases (16-bit)
.DEFINE VAR_EATT  $1CB5   ; music: echo attenuation
.DEFINE VAR_EFADE $1CB6   ; music: frames to next echo fade step
.DEFINE ECHO_Q    $1CB8   ; music: 4 slots x (countdown, div_lo, div_hi)
.DEFINE VAR_GSTATE $1CC4  ; 0 = SPECS TERMINAL, 1 = maze, 2 = title card
.DEFINE VAR_REVEAL $1CC5  ; card: banner wipe column (0..31, 32 = text, 33 =
                          ; done). Terminal: 0 = needs paint, 1 = painted.
.DEFINE VAR_MUSON  $1CC6  ; 1 = chime finished, SPACE-A engine owns the PSG
.DEFINE VAR_CHF    $1CC7  ; chime: frame counter
.DEFINE VAR_CHATT  $1CC8  ; chime: fade attenuation
.DEFINE VAR_CHSUB  $1CC9  ; chime: frames to next fade step
.DEFINE VAR_TMPR   $1CCA  ; banner scratch: band row0
.DEFINE VAR_TMPC   $1CCB  ; banner scratch: screen column
.DEFINE VAR_MSEL   $1CCC  ; terminal: 0 = TEST PATTERN, 1 = FIELD MAP
.DEFINE PSG       $7F11   ; SN76489, memory-mapped in Z80 space (no OUT)
.DEFINE HEART     $1F00   ; liveness ripple for save-state forensics
.DEFINE PAD_MBX   $1FF4
.DEFINE DIRTY_MBX $1FF5
.DEFINE STATE_MBX $1FF6
.DEFINE FRAME_MBX $1FF7

; boot-font tile ids (mars.c NextChr mapping)
.DEFINE T_FLOOR   1       ; small centered dot
.DEFINE T_WALL    44      ; '%'
.DEFINE T_PLAYER  27      ; 'P'
.DEFINE T_EXIT    16      ; 'E'

.DEFINE RPT_DELAY 7       ; frames a held direction waits between steps

.BANK 0 SLOT 0
.ORG $0000
   di
   ld sp, $1F80
   xor a
   ld (VAR_PPREV), a
   ld (VAR_RPT), a
   ld (STATE_MBX), a
   ld (DIRTY_MBX), a
   ld a, (FRAME_MBX)
   ld (VAR_FLAST), a
   ld a, (MAP_META + 0)
   ld (VAR_PX), a
   ld a, (MAP_META + 1)
   ld (VAR_PY), a
   ; TILEBUF sits ABOVE the uploaded image, so a re-upload never touches it
   ; and Z80 RAM hands us the LAST session's frame. build_frame and
   ; build_escape rewrite all 768 cells, but the title card only paints its
   ; banner and text — everything around them was last run's leftovers, and
   ; they piled up boot after boot (Mike: clean, then extra chars, then a
   ; screen of garbage). Start from a dark screen, every time.
   call clear_screen
   call music_init
   xor a                   ; boot into the SPECS TERMINAL (clinical operating
   ld (VAR_GSTATE), a      ; card, Mike 2026-08-13): TEST PATTERN and FIELD
   ld (VAR_REVEAL), a      ; MAP are its two entries. The boot chime plays
   ld (VAR_MUSON), a       ; over it, same as it did over the old card.
   ld (VAR_CHF), a
   ld (VAR_MSEL), a

; ---------------- main loop: one logic step per 68K frame tick ------
loop:
   ld hl, HEART
   inc (hl)
   ld a, (FRAME_MBX)
   ld hl, VAR_FLAST
   cp (hl)
   jr z, loop
   ld (hl), a
   ld a, (VAR_GSTATE)
   or a
   jr z, do_terminal
   cp 3
   jr z, do_diag
   cp 2
   jr nz, game_frame
   call menu_tick          ; state 2: the TEST PATTERN card (dormant)
   jp loop
do_terminal:
   call terminal_tick      ; state 0: the specs terminal
   jp loop
do_diag:
   call diag_tick          ; state 3: diagnostics page
   jp loop
game_frame:
   call music_or_chime     ; music runs through the escape screen too
   ld a, (STATE_MBX)
   or a
   jp nz, debrief_tick     ; escaped: debrief until START returns to the
                           ; terminal (the second START, seen there, is the
                           ; 32X's session exit — layered by design)

   ; ---- input: edge detect + held auto-repeat on the d-pad ----
   ld a, (PAD_MBX)
   ld b, a                 ; b = current pad
   ld hl, VAR_PPREV
   ld c, (hl)
   ld (hl), b
   ld a, c
   cpl
   and b
   ld d, a                 ; d = newly pressed bits
   ld a, b
   and $0F
   jr z, rpt_clear
   ld hl, VAR_RPT
   inc (hl)
   ld a, (hl)
   cp RPT_DELAY
   jr c, dispatch
   ld (hl), 0
   ld a, b
   and $0F
   or d
   ld d, a                 ; repeat fires: treat held dirs as pressed
   jr dispatch
rpt_clear:
   xor a
   ld (VAR_RPT), a

   ; ---- movement: first direction by priority U, D, L, R ----
dispatch:
   ld a, d
   and $0F
   jp z, loop
   ld e, 0                 ; e = dx
   ld c, 0                 ; c = dy
   rra                     ; bit 0: UP
   jr nc, try_down
   ld c, $FF
   jr move
try_down:
   rra                     ; bit 1: DOWN
   jr nc, try_left
   ld c, 1
   jr move
try_left:
   rra                     ; bit 2: LEFT
   jr nc, try_right
   ld e, $FF
   jr move
try_right:
   rra                     ; bit 3: RIGHT
   jp nc, loop
   ld e, 1
move:
   ld a, (VAR_PX)
   add a, e
   cp 32
   jp nc, loop             ; unsigned: also catches -1 -> $FF
   ld e, a                 ; e = target x
   ld a, (VAR_PY)
   add a, c
   cp 32
   jp nc, loop
   ld c, a                 ; c = target y
   ld a, (MAP_META + 2)    ; win check BEFORE wall check: the door IS a wall
   cp e
   jr nz, not_exit
   ld a, (MAP_META + 3)
   cp c
   jr z, escaped
not_exit:
   call map_bit            ; NZ = wall at (e,c)
   jp nz, loop
   ld a, e
   ld (VAR_PX), a
   ld a, c
   ld (VAR_PY), a
   call build_frame
   jp loop

escaped:
   ld a, 1
   ld (STATE_MBX), a
   call build_escape
   jp loop

; ---------------- map query ----------------
; in: e = x (0..31), c = y (0..31); out: NZ = wall. Preserves bc, de.
map_bit:
   push bc
   push de
   ld a, c
   add a, a
   add a, a                ; y*4 (max 124)
   ld l, a
   ld a, e
   srl a
   srl a
   srl a                   ; x/8 (0..3)
   add a, l
   ld l, a
   ld h, $1C               ; hl = MAP_BITS + y*4 + x/8 (MAP_BITS is $1C00)
   ld b, (hl)              ; b = the map byte
   ld a, e
   and 7
   ld e, a
   ld d, 0
   ld hl, bitmask
   add hl, de
   ld a, b
   and (hl)                ; Z = open, NZ = wall
   pop de
   pop bc
   ret

bitmask: .DB $80, $40, $20, $10, $08, $04, $02, $01

; ---------------- frame build ----------------
; TILEBUF = 24 map rows starting at vp_y, then exit + player overlaid.
build_frame:
   ld a, (VAR_PY)
   sub 12
   jr nc, vp_lo_ok
   xor a
vp_lo_ok:
   cp 9
   jr c, vp_hi_ok
   ld a, 8
vp_hi_ok:
   ld (VAR_VPY), a
   ld d, a                 ; d = current map row
   ld hl, TILEBUF
   ld b, 24
frow:
   push bc
   call build_row
   inc d
   pop bc
   djnz frow
   ; ---- exit overlay (only if inside the viewport) ----
   ld a, (MAP_META + 3)
   ld hl, VAR_VPY
   sub (hl)
   jr c, exit_done
   cp 24
   jr nc, exit_done
   call cell_addr
   ld a, (MAP_META + 2)
   ld e, a
   ld d, 0
   add hl, de
   ld (hl), T_EXIT
exit_done:
   ; ---- player overlay (always inside by the vp clamp) ----
   ld a, (VAR_PY)
   ld hl, VAR_VPY
   sub (hl)
   call cell_addr
   ld a, (VAR_PX)
   ld e, a
   ld d, 0
   add hl, de
   ld (hl), T_PLAYER
   ld a, 1
   ld (DIRTY_MBX), a
   ret

; a = viewport row -> hl = TILEBUF + a*32 (clobbers de)
cell_addr:
   ld l, a
   ld h, 0
   add hl, hl
   add hl, hl
   add hl, hl
   add hl, hl
   add hl, hl
   ld de, TILEBUF
   add hl, de
   ret

; one map row d into 32 tilebuf bytes at hl (hl advances, d preserved)
build_row:
   push de
   ld a, d
   add a, a
   add a, a
   ld e, a
   ld d, $1C               ; de = MAP_BITS + row*4
   ld b, 4
brow_byte:
   ld a, (de)
   inc de
   ld c, a
   push bc
   ld b, 8
brow_bit:
   ld a, T_FLOOR
   sla c
   jr nc, put_tile
   ld a, T_WALL
put_tile:
   ld (hl), a
   inc hl
   djnz brow_bit
   pop bc
   djnz brow_byte
   pop de
   ret

; ---------------- escape screen ----------------
build_escape:
   ld hl, TILEBUF
   ld bc, 768
clr_esc:
   ld (hl), 0
   inc hl
   dec bc
   ld a, b
   or c
   jr nz, clr_esc
   ld hl, TILEBUF + 8*32 + 7
   ld de, s_complete
   ld b, 17
   call copy_str
   ld hl, TILEBUF + 11*32 + 8
   ld de, MAP_NAME          ; the debrief names the specimen
   ld b, 16
   call copy_str
   ld hl, TILEBUF + 14*32 + 6
   ld de, s_pexit
   ld b, 19
   call copy_str
   ld a, 1
   ld (DIRTY_MBX), a
   ret

copy_str:
   ld a, (de)
   ld (hl), a
   inc de
   inc hl
   djnz copy_str
   ret

; ---------------- menu screen (DESIGN.md 4b) -----------------------------
; Derived from BIOS 1.3's boot: their SEGA logo hides behind two masking
; sprites shrunk a pixel column per update while HScroll slides it in;
; ours is a tile-granular wipe, one banner column per frame, because the
; harness has no sprites and no scroll. Their chime is three voices in
; tone-ramp mode landing on a chord with envelopes; ours is a 24-frame
; table sweep landing on G minor (G3/D4/G4), hold, stepped fade, then
; the SPACE-A engine takes the channels. Any of A/B/C starts the maze.

; ---------------- state 3: DIAGNOSTICS ------------------------------
; Stats + live controller test. FRAME and HEART tick as hex (the machine
; visibly alive), the pad row lights each button as held. START returns
; to the terminal and hands START back to the 32X (STATE_MBX -> 0).
diag_tick:
   call music_or_chime
   ld a, (VAR_REVEAL)
   or a
   jr nz, diag_live
   ld hl, TILEBUF + 2*32 + 10
   ld de, s_d1
   ld b, 11
   call copy_str
   ld hl, TILEBUF + 6*32 + 6
   ld de, s_d2
   ld b, 6
   call copy_str
   ld hl, TILEBUF + 8*32 + 6
   ld de, s_d3
   ld b, 6
   call copy_str
   ld hl, TILEBUF + 10*32 + 6
   ld de, s_d4
   ld b, 4
   call copy_str
   ld hl, TILEBUF + 21*32 + 9
   ld de, s_d5
   ld b, 13
   call copy_str
   ld a, 1
   ld (VAR_REVEAL), a
diag_live:
   ld a, (FRAME_MBX)
   ld hl, TILEBUF + 6*32 + 13
   call hex_cells
   ld a, (HEART)
   ld hl, TILEBUF + 8*32 + 13
   call hex_cells
   ld a, (PAD_MBX)         ; lamps: U D L R B C A S, bit 0 first
   ld c, a
   ld hl, TILEBUF + 10*32 + 11
   ld de, lamp_letters
   ld b, 8
lamp_loop:
   ld a, (de)
   rrc c
   jr c, lamp_on
   ld a, 40                ; '-' unpressed
lamp_on:
   ld (hl), a
   inc hl
   inc hl                  ; gap cell
   inc de
   djnz lamp_loop
   ld a, 1
   ld (DIRTY_MBX), a
   ld a, (PAD_MBX)         ; START edge -> terminal
   ld b, a
   ld hl, VAR_PPREV
   ld c, (hl)
   ld (hl), b
   ld a, c
   cpl
   and b
   and $80
   ret z
   call clear_screen
   xor a
   ld (STATE_MBX), a       ; START ownership back to the 32X
   ld (VAR_GSTATE), a
   ld (VAR_REVEAL), a
   ld (VAR_MSEL), a
   ld a, 1
   ld (DIRTY_MBX), a
   ret

hex_cells:                 ; a = byte -> two hex tile ids at (hl)
   push af
   rra
   rra
   rra
   rra
   call hex_glyph
   ld (hl), a
   inc hl
   pop af
   call hex_glyph
   ld (hl), a
   ret
hex_glyph:                 ; a low nibble -> tile id. The boot font is
   and $0F                 ; contiguous 0-9 then A-Z, so hex is ONE add:
   add a, 2                ; nibble 0->'0'(2) ... 10->'A'(12) ... 15->'F'.
   ret

; Debrief: EXERCISE COMPLETE stays up until a fresh START, which resets the
; whole machine state back to the SPECS TERMINAL. Every other button idles.
debrief_tick:
   ld a, (PAD_MBX)
   ld b, a
   ld hl, VAR_PPREV
   ld c, (hl)
   ld (hl), b
   ld a, c
   cpl
   and b
   and $80                 ; START edge only
   jp z, loop
   call clear_screen
   xor a
   ld (STATE_MBX), a       ; drops the 68K's debrief flag too
   ld (VAR_GSTATE), a      ; -> terminal state
   ld (VAR_REVEAL), a      ; terminal repaints fresh
   ld (VAR_MSEL), a
   ld a, 1
   ld (DIRTY_MBX), a
   jp loop

; Shared full-screen wipe (boot, and every state hand-over — the card wipe
; and the terminal both paint on dark, and TILEBUF holds last state's text).
clear_screen:
   ld hl, TILEBUF
   ld bc, 768
clr_loop:
   ld (hl), 0
   inc hl
   dec bc
   ld a, b
   or c
   jr nz, clr_loop
   ret

; ---------------- state 0: the SPECS TERMINAL -----------------------
; Clinical operating card: Master System specs, the level's specimen id,
; two runnable entries (TEST PATTERN diag card / FIELD MAP maze), and the
; exit contract in the footer (ENTER = the 32X's session exit; this side
; only documents it). UP/DOWN move the cursor, A runs the selection.
terminal_tick:
   ld a, (VAR_REVEAL)
   or a
   jr nz, term_input
   ; one-shot paint
   ld hl, TILEBUF + 2*32 + 9
   ld de, s_t1
   ld b, 13
   call copy_str
   ld hl, TILEBUF + 5*32 + 6
   ld de, s_t2
   ld b, 18
   call copy_str
   ld hl, TILEBUF + 7*32 + 6
   ld de, s_t3
   ld b, 15
   call copy_str
   ld hl, TILEBUF + 9*32 + 6
   ld de, s_t4
   ld b, 19
   call copy_str
   ld hl, TILEBUF + 11*32 + 6
   ld de, s_t5
   ld b, 9
   call copy_str
   ld hl, TILEBUF + 11*32 + 16
   ld de, MAP_NAME
   ld b, 16
   call copy_str
   ld hl, TILEBUF + 15*32 + 8
   ld de, s_o1
   ld b, 10
   call copy_str
   ld hl, TILEBUF + 17*32 + 8
   ld de, s_o2
   ld b, 9
   call copy_str
   ld hl, TILEBUF + 21*32 + 10
   ld de, s_t6
   ld b, 11
   call copy_str
   call term_cursor
   ld a, 1
   ld (VAR_REVEAL), a
   ld a, 1
   ld (DIRTY_MBX), a
term_input:
   call music_or_chime
   ld a, (PAD_MBX)
   ld b, a
   ld hl, VAR_PPREV
   ld c, (hl)
   ld (hl), b
   ld a, c
   cpl
   and b                   ; a = newly pressed
   ld d, a
   and $03                 ; UP or DOWN: flip the 2-entry cursor
   jr z, term_try_a
   ld a, (VAR_MSEL)
   xor 1
   ld (VAR_MSEL), a
   call term_cursor
   ld a, 1
   ld (DIRTY_MBX), a
term_try_a:
   ld a, d
   and $40                 ; A runs the selection
   ret z
   ld a, (VAR_MSEL)
   or a
   jr nz, term_run_map
   ; DIAGNOSTICS (state 3): stats + live controller test, IN THIS BLOB —
   ; one ROM, every road leads back to this terminal (Mike). STATE_MBX=1
   ; while the page is up so the 32X defers START to us, exactly like the
   ; debrief does; our START handler drops it and repaints the terminal.
   call clear_screen
   xor a
   ld (VAR_REVEAL), a
   ld a, 3
   ld (VAR_GSTATE), a
   ld a, 1
   ld (STATE_MBX), a
   ld (DIRTY_MBX), a
   ret
term_run_map:
   ld a, 1
   ld (VAR_GSTATE), a
   jp build_frame          ; full rebuild overwrites the terminal

; cursor glyphs for both option rows: '>' (41) on the pick, dark on the other
term_cursor:
   ld a, (VAR_MSEL)
   or a
   jr nz, term_cur_map
   ld a, 41
   ld (TILEBUF + 15*32 + 6), a
   xor a
   ld (TILEBUF + 17*32 + 6), a
   ret
term_cur_map:
   xor a
   ld (TILEBUF + 15*32 + 6), a
   ld a, 41
   ld (TILEBUF + 17*32 + 6), a
   ret

; ---------------- state 2: the TEST PATTERN card --------------------
menu_tick:
   ld a, (VAR_REVEAL)
   cp 33
   jr nc, menu_music       ; wipe + text done
   cp 32
   jr z, menu_text
   ld (VAR_TMPC), a        ; reveal one more banner column
   ld hl, band_test
   call band_col
   ld hl, band_pattern
   call band_col
   ld a, (VAR_REVEAL)
   inc a
   ld (VAR_REVEAL), a
   ld a, 1
   ld (DIRTY_MBX), a
   jr menu_music
menu_text:
   ld hl, TILEBUF + 17*32 + 8
   ld de, MAP_NAME          ; the level's name, patched in with the map
   ld b, 16
   call copy_str
   ld hl, TILEBUF + 20*32 + 6
   ld de, s_press
   ld b, 19
   call copy_str
   ld a, 33
   ld (VAR_REVEAL), a
   ld a, 1
   ld (DIRTY_MBX), a
menu_music:
   call music_or_chime
   ld a, (PAD_MBX)         ; A/B/C edge starts the game
   ld b, a
   ld hl, VAR_PPREV
   ld c, (hl)
   ld (hl), b
   ld a, c
   cpl
   and b
   and $40                 ; A begins the exercise
   ret z
   ld a, 1
   ld (VAR_GSTATE), a
   jp build_frame          ; full rebuild overwrites the whole menu

; one banner column for one band. VAR_TMPC = screen column.
; band data: row0, col0, nletters, glyph ids...
band_col:
   ld a, (hl)
   ld (VAR_TMPR), a
   inc hl
   ld a, (VAR_TMPC)
   sub (hl)
   ret c                   ; left of the band
   inc hl
   ld e, a                 ; e = local column
   srl a
   srl a                   ; letter cell = local/4 (3 wide + 1 gap)
   cp (hl)
   ret nc                  ; past the last letter
   inc hl
   ld c, a
   ld b, 0
   add hl, bc              ; hl -> this letter's glyph id
   ld a, e
   and 3
   cp 3
   ret z                   ; the gap column
   ld b, 4                 ; mask = 4 >> (local & 3)
mask_sh:
   or a
   jr z, mask_done
   srl b
   dec a
   jr mask_sh
mask_done:
   ld a, (hl)              ; glyph data = banner_glyphs + id*5
   ld l, a
   ld h, 0
   ld e, a
   ld d, 0
   add hl, hl
   add hl, hl
   add hl, de
   ld de, banner_glyphs
   add hl, de
   push hl
   ld a, (VAR_TMPR)        ; dest = TILEBUF + row0*32 + column
   ld l, a
   ld h, 0
   add hl, hl
   add hl, hl
   add hl, hl
   add hl, hl
   add hl, hl
   ld a, (VAR_TMPC)
   ld e, a
   ld d, 0
   add hl, de
   ld de, TILEBUF
   add hl, de
   ex de, hl
   pop hl                  ; hl = glyph rows, de = dest, b = mask
   ld c, 5
brow:
   ld a, (hl)
   and b
   jr z, brow_skip
   ld a, 44                ; '%' block
   ld (de), a
brow_skip:
   inc hl
   ld a, e
   add a, 32
   ld e, a
   jr nc, brow_nc
   inc d
brow_nc:
   dec c
   jr nz, brow
   ret

; ---------------- boot chime: the SE-GA gesture REVERSED -----------------
; Mike's sms_putrats concept — the SMS startup sound played backwards.
; Forward BIOS chime = sweep up, land on a chord, decay. Ours runs the
; tape the other way: the G-minor chord (G3/D4/G4) swells IN from
; silence (frames 0-36, att 15 -> 2 every 3 frames), holds (37-81),
; then sweeps DOWNWARD through the table in reverse (82-105) to the low
; cluster and CUTS (106) — then the SPACE-A engine owns the channels.
music_or_chime:
   ld a, (VAR_MUSON)
   or a
   jp nz, music_tick
chime_tick:
   ld a, (VAR_CHF)
   cp 37
   jr c, ch_fadein
   cp 82
   jp c, ch_adv            ; the hold
   cp 106
   jr c, ch_sweepdown
   ld a, $9F               ; the cut: reversed tapes don't fade out
   ld (PSG), a
   ld a, $BF
   ld (PSG), a
   ld a, $DF
   ld (PSG), a
   ld a, 1
   ld (VAR_MUSON), a       ; chime over — engine ticks from the next frame
   jp ch_adv
ch_fadein:
   or a
   jr nz, fadein_att
   ld de, 571              ; G3 — the chord is present from frame 0,
   ld a, e                 ; hidden behind att 15, and emerges
   and $0F
   or $80
   ld (PSG), a
   call emit_data_de
   ld de, 381              ; D4
   ld a, e
   and $0F
   or $A0
   ld (PSG), a
   call emit_data_de
   ld de, 285              ; G4
   ld a, e
   and $0F
   or $C0
   ld (PSG), a
   call emit_data_de
   ld a, 15
   ld (VAR_CHATT), a
   ld a, 1
   ld (VAR_CHSUB), a       ; first swell step fires THIS frame
fadein_att:
   ld hl, VAR_CHSUB
   dec (hl)
   jp nz, ch_adv
   ld (hl), 3
   ld a, (VAR_CHATT)
   dec a
   ld (VAR_CHATT), a
   ld b, a
   or $90
   ld (PSG), a
   ld a, b
   or $B0
   ld (PSG), a
   ld a, b
   or $D0
   ld (PSG), a
   jp ch_adv
ch_sweepdown:
   ld b, a                 ; index = 105 - frame: 23 down to 0
   ld a, 105
   sub b
   ld l, a                 ; hl = sweep_tab + index*6
   ld h, 0
   add hl, hl
   ld e, l
   ld d, h
   add hl, hl
   add hl, de
   ld de, sweep_tab
   add hl, de
   ld e, (hl)
   inc hl
   ld d, (hl)
   inc hl
   push hl
   ld a, e
   and $0F
   or $80
   ld (PSG), a
   call emit_data_de
   pop hl
   ld e, (hl)
   inc hl
   ld d, (hl)
   inc hl
   push hl
   ld a, e
   and $0F
   or $A0
   ld (PSG), a
   call emit_data_de
   pop hl
   ld e, (hl)
   inc hl
   ld d, (hl)
   ld a, e
   and $0F
   or $C0
   ld (PSG), a
   call emit_data_de
ch_adv:
   ld hl, VAR_CHF
   inc (hl)
   ret

; ---------------- generative liminal music: SPACE-A (DESIGN.md 4a) -------
; Mike's pick from the WAV auditions. The phrase+echo engine, mirrored
; line for line by run_engine_space(vibrato=False) in
; tools/sms_liminal_gen.py; the simulator asserts byte parity of the PSG
; stream. ch2 plays short G-minor motifs (from the At the Inn
; transcription) with music-box decay (att 2, one step per 6 frames);
; ch1 is a pseudo-delay replaying every melody note 21 frames later at
; att+5; ch0 walks a slow D3/C3/Bb2 bass, one soft note per 7-10 s.
; Seed = $ACE1 ^ (spawn_y:spawn_x) ^ (exit_y:exit_x). Per frame the
; sections run in fixed order (bass, phrase select, note fire, melody
; decay, echo fire, echo decay) — the parity contract depends on it.

music_init:
   ld a, $9F               ; silence all four channels first
   ld (PSG), a
   ld a, $BF
   ld (PSG), a
   ld a, $DF
   ld (PSG), a
   ld a, $FF
   ld (PSG), a
   ld hl, (MAP_META + 0)   ; hl = spawn_y:spawn_x
   ld a, (MAP_META + 2)
   ld e, a
   ld a, (MAP_META + 3)
   ld d, a                 ; de = exit_y:exit_x
   ld a, h
   xor d
   ld h, a
   ld a, l
   xor e
   ld l, a
   ld a, h
   xor $AC
   ld h, a
   ld a, l
   xor $E1
   ld l, a
   ld a, h                 ; an all-zero LFSR never leaves zero
   or l
   jr nz, seed_ok
   ld hl, $ACE1
seed_ok:
   ld (VAR_LFSR), hl
   ld a, 15                ; all envelopes silent, all timers at zero:
   ld (VAR_BATT), a        ; frame 0 fires the first bass note AND the
   ld (VAR_MATT), a        ; first phrase, exactly like the Python engine
   ld (VAR_EATT), a
   xor a
   ld (VAR_BI), a
   ld (VAR_MLEFT), a
   ld (VAR_NOTET), a
   ld hl, 0
   ld (VAR_BT), hl
   ld (VAR_GAP), hl
   ld hl, ECHO_Q           ; free every echo slot (countdown 0)
   ld b, 4
eq_clear:
   ld (hl), 0
   inc hl
   inc hl
   inc hl
   djnz eq_clear
   ret

; advance the LFSR one Galois step: s >>= 1; if carry out, s ^= $B400
lfsr_step:
   ld hl, (VAR_LFSR)
   srl h
   rr l
   jr nc, lfsr_store
   ld a, h
   xor $B4
   ld h, a
lfsr_store:
   ld (VAR_LFSR), hl
   ret

; a = ((de >> 4) & $3F) -> PSG data byte for the latched channel
emit_data_de:
   ld a, e
   rrca
   rrca
   rrca
   rrca
   and $0F
   ld b, a
   ld a, d
   rlca
   rlca
   rlca
   rlca
   and $F0
   or b
   and $3F
   ld (PSG), a
   ret

music_tick:
   ; ---- bass: trigger when the timer hits zero ----
   ld hl, (VAR_BT)
   ld a, h
   or l
   jr nz, bass_count
   ld a, (VAR_BI)          ; div = bass_divs[b_i & 3]
   and 3
   add a, a
   ld l, a
   ld h, 0
   ld de, bass_divs
   add hl, de
   ld e, (hl)
   inc hl
   ld d, (hl)
   push de
   call lfsr_step          ; b_i += 1 (usually) or 2 (1 in 4)
   ld a, l
   and 3
   ld a, (VAR_BI)          ; ld a,(nn) leaves flags alone
   jr nz, bi_step
   inc a
bi_step:
   inc a
   ld (VAR_BI), a
   pop de
   ld a, e
   and $0F
   or $80                  ; ch0 tone latch
   ld (PSG), a
   call emit_data_de
   ld a, $96               ; ch0 att 6
   ld (PSG), a
   ld a, 6
   ld (VAR_BATT), a
   ld a, 30
   ld (VAR_BFADE), a
   call lfsr_step          ; next bass note 7-10.5 s out
   ld a, l
   and 7
   add a, a
   ld l, a
   ld h, 0
   ld de, bt_tab
   add hl, de
   ld e, (hl)
   inc hl
   ld d, (hl)
   ex de, hl
   ld (VAR_BT), hl
bass_count:
   ld hl, (VAR_BT)         ; b_t decrements every frame, trigger frame too
   dec hl
   ld (VAR_BT), hl
   ld a, (VAR_BATT)
   cp 15
   jr z, mel_select
   ld hl, VAR_BFADE
   dec (hl)
   jr nz, mel_select
   ld (hl), 30
   ld a, (VAR_BATT)
   inc a
   ld (VAR_BATT), a
   or $90
   ld (PSG), a

   ; ---- melody: pick a phrase when the last one is done and the gap is over
mel_select:
   ld a, (VAR_MLEFT)
   or a
   jr nz, mel_fire
   ld hl, (VAR_GAP)
   ld a, h
   or l
   jr nz, gap_count
   call lfsr_step          ; motif = motif_ptrs[step & 3]
   ld a, l
   and 3
   add a, a
   ld l, a
   ld h, 0
   ld de, motif_ptrs
   add hl, de
   ld e, (hl)
   inc hl
   ld d, (hl)
   ld a, (de)              ; first byte = note count
   ld (VAR_MLEFT), a
   inc de
   ex de, hl
   ld (VAR_MPTR), hl
   xor a
   ld (VAR_NOTET), a       ; first note fires THIS frame
   call lfsr_step          ; gap after this phrase: 2-7.25 s
   ld a, l
   and 7
   add a, a
   ld l, a
   ld h, 0
   ld de, mgap_tab
   add hl, de
   ld e, (hl)
   inc hl
   ld d, (hl)
   ex de, hl
   ld (VAR_GAP), hl
   jr mel_fire
gap_count:
   dec hl
   ld (VAR_GAP), hl
   jr mel_decay

   ; ---- melody: fire the next motif note when its timer expires ----
mel_fire:
   ld a, (VAR_MLEFT)
   or a
   jr z, mel_decay
   ld a, (VAR_NOTET)
   or a
   jr nz, notet_count
   ld hl, (VAR_MPTR)
   ld e, (hl)
   inc hl
   ld d, (hl)
   inc hl
   ld (VAR_MPTR), hl
   ld hl, VAR_MLEFT
   dec (hl)
   ld a, e
   and $0F
   or $C0                  ; ch2 tone latch
   ld (PSG), a
   call emit_data_de
   ld a, $D2               ; ch2 att 2 — the music-box attack
   ld (PSG), a
   ld a, 2
   ld (VAR_MATT), a
   ld a, 6
   ld (VAR_MFADE), a
   ld hl, ECHO_Q           ; enqueue the echo: 21 frames from now (22
   ld b, 4                 ; pre-decrement — this frame's sweep takes one)
eq_find:
   ld a, (hl)
   or a
   jr z, eq_take
   inc hl
   inc hl
   inc hl
   djnz eq_find
   jr eq_done              ; full: drop (cannot happen at 18+ frame spacing)
eq_take:
   ld (hl), 19             ; 18-frame echo (A2 tempo), +1 pre-decrement
   inc hl
   ld (hl), e
   inc hl
   ld (hl), d
eq_done:
   call lfsr_step          ; lilting inter-note timing: 13-20 frames
   ld a, l
   and 7
   add a, 13
   ld (VAR_NOTET), a
   jr mel_decay
notet_count:
   dec a
   ld (VAR_NOTET), a

   ; ---- melody decay ----
mel_decay:
   ld a, (VAR_MATT)
   cp 15
   jr z, echo_fire
   ld hl, VAR_MFADE
   dec (hl)
   jr nz, echo_fire
   ld (hl), 6
   ld a, (VAR_MATT)
   inc a
   ld (VAR_MATT), a
   or $D0
   ld (PSG), a

   ; ---- echo: sweep the queue, fire any slot reaching zero ----
echo_fire:
   ld hl, ECHO_Q
   ld b, 4
eq_sweep:
   ld a, (hl)
   or a
   jr z, eq_next
   dec a
   ld (hl), a
   jr nz, eq_next
   push hl
   push bc
   inc hl
   ld e, (hl)
   inc hl
   ld d, (hl)
   ld a, e
   and $0F
   or $A0                  ; ch1 tone latch
   ld (PSG), a
   call emit_data_de
   ld a, $B7               ; ch1 att 7 — the echo, ~10 dB down
   ld (PSG), a
   ld a, 7
   ld (VAR_EATT), a
   ld a, 6
   ld (VAR_EFADE), a
   pop bc
   pop hl
eq_next:
   inc hl
   inc hl
   inc hl
   djnz eq_sweep
   ld a, (VAR_EATT)        ; ---- echo decay ----
   cp 15
   ret z
   ld hl, VAR_EFADE
   dec (hl)
   ret nz
   ld (hl), 6
   ld a, (VAR_EATT)
   inc a
   ld (VAR_EATT), a
   or $B0
   ld (PSG), a
   ret

bass_divs:  .DW 762, 855, 960, 762                       ; D3 C3 Bb2 D3
bt_tab:     .DW 330, 360, 390, 420, 450, 480, 510, 540   ; bass 5.5-9 s
mgap_tab:   .DW 75, 105, 135, 165, 195, 225, 255, 285    ; gaps 1.25-4.75 s
motif_ptrs: .DW motif0, motif1, motif2, motif3
motif0: .DB 4
        .DW 285, 240, 254, 285                           ; G4 Bb4 A4 G4
motif1: .DB 4
        .DW 190, 214, 240, 285                           ; D5 C5 Bb4 G4
motif2: .DB 3
        .DW 143, 190, 240                                ; G5 D5 Bb4
motif3: .DB 4
        .DW 254, 240, 285, 190                           ; A4 Bb4 G4 D5

; chime sweep: 24 frames x (ch0, ch1, ch2) dividers.
; ch0: 1023-19f -> lands G3 571; ch1: 762-16f -> D4 381; ch2: 570-12f -> G4 285.
sweep_tab:
   .DW 1023, 762, 570
   .DW 1004, 746, 558
   .DW 985, 730, 546
   .DW 966, 714, 534
   .DW 947, 698, 522
   .DW 928, 682, 510
   .DW 909, 666, 498
   .DW 890, 650, 486
   .DW 871, 634, 474
   .DW 852, 618, 462
   .DW 833, 602, 450
   .DW 814, 586, 438
   .DW 795, 570, 426
   .DW 776, 554, 414
   .DW 757, 538, 402
   .DW 738, 522, 390
   .DW 719, 506, 378
   .DW 700, 490, 366
   .DW 681, 474, 354
   .DW 662, 458, 342
   .DW 643, 442, 330
   .DW 624, 426, 318
   .DW 605, 410, 306
   .DW 586, 394, 294

; 3x5 banner glyphs, one byte per row, bits %100/%010/%001 = columns
banner_glyphs:
   .DB 7,2,2,2,2     ; 0 T
   .DB 7,4,6,4,7     ; 1 E
   .DB 3,4,2,1,6     ; 2 S
   .DB 6,5,6,4,4     ; 3 P
   .DB 2,5,7,5,5     ; 4 A
   .DB 6,5,6,6,5     ; 5 R
   .DB 5,7,7,5,5     ; 6 N

; bands: row0, col0, nletters, glyph ids
band_test:    .DB 4, 8, 4, 0, 1, 2, 0             ; TEST, rows 4-8
band_pattern: .DB 10, 2, 7, 3, 4, 0, 0, 1, 5, 6   ; PATTERN, rows 10-14

; boot-font tile ids: A=12..Z=37, 0=space ('#'-less world)
; terminal strings (tile ids: 0=sp, 2-11='0'-'9', 12-37='A'-'Z', 38=':' 39='.')
s_t1: .DB 24,12,30,31,16,29,0,30,36,30,31,16,24                   ; MASTER SYSTEM
s_t2: .DB 14,27,32,38,0,37,10,2,12,0,5,39,7,10,0,24,19,37         ; CPU: Z80A 3.58 MHZ
s_t3: .DB 29,12,24,38,0,10,3,11,4,0,13,36,31,16,30                ; RAM: 8192 BYTES
s_t4: .DB 33,15,27,38,0,30,19,12,15,26,34,0,23,20,25,22,0,26,22   ; VDP: SHADOW LINK OK
s_t5: .DB 30,27,16,14,20,24,16,25,38                              ; SPECIMEN:
s_o1: .DB 15,20,12,18,25,26,30,31,20,14                           ; DIAGNOSTIC
s_o2: .DB 17,20,16,23,15,0,24,12,27                               ; FIELD MAP
s_t6: .DB 16,25,31,16,29,38,0,16,35,20,31                         ; ENTER: EXIT
s_d1: .DB 15,20,12,18,25,26,30,31,20,14,30                        ; DIAGNOSTICS
s_d2: .DB 17,29,12,24,16,38                                       ; FRAME:
s_d3: .DB 19,16,12,29,31,38                                       ; HEART:
s_d4: .DB 27,12,15,38                                             ; PAD:
s_d5: .DB 30,31,12,29,31,38,0,29,16,31,32,29,25                   ; START: RETURN
lamp_letters: .DB 32,15,23,29,13,14,12,30                         ; U D L R B C A S
s_press:    .DB 12,0,31,26,0,13,16,18,20,25,0,16,35,16,29,14,20,30,16 ; A TO BEGIN EXERCISE
s_complete: .DB 16,35,16,29,14,20,30,16,0,14,26,24,27,23,16,31,16    ; EXERCISE COMPLETE
s_pexit:    .DB 27,29,16,30,30,0,30,31,12,29,31,0,31,26,0,16,35,20,31 ; PRESS START TO EXIT
