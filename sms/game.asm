; =====================================================================
; SMS MINI-GAME: ESCAPE THE BACKROOMS (in-game blob flavor)
;
; This is the Master System game the 32X boots INSIDE the running
; Backrooms build. It is not a 32KB cartridge image: it executes from
; the Genesis Z80's 8KB RAM ($0000-$1FFF), where the 68K uploads it
; through the $A00000 window. The 68K then PATCHES the live level in:
; the SH-2 packs world_map to 1bpp + spawn + exit and streams it over
; COMM, and the 68K writes those 132 bytes into MAP_BITS/MAP_META
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
.DEFINE VAR_PX    $1C90   ; player cell x
.DEFINE VAR_PY    $1C91   ; player cell y
.DEFINE VAR_VPY   $1C92   ; viewport top map row (0..8)
.DEFINE VAR_PPREV $1C93   ; previous pad byte (edge detect)
.DEFINE VAR_FLAST $1C94   ; last FRAME_MBX seen
.DEFINE VAR_RPT   $1C95   ; held-direction auto-repeat counter
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
   call build_frame

; ---------------- main loop: one logic step per 68K frame tick ------
loop:
   ld hl, HEART
   inc (hl)
   ld a, (FRAME_MBX)
   ld hl, VAR_FLAST
   cp (hl)
   jr z, loop
   ld (hl), a
   ld a, (STATE_MBX)
   or a
   jr nz, loop             ; escaped: idle until the 68K tears us down

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
   ld hl, TILEBUF + 8*32 + 10
   ld de, s_escaped
   ld b, 11
   call copy_str
   ld hl, TILEBUF + 10*32 + 9
   ld de, s_back
   ld b, 13
   call copy_str
   ld hl, TILEBUF + 14*32 + 6
   ld de, s_press
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

; boot-font tile ids: A=12..Z=37, 0=space ('#'-less world)
s_escaped: .DB 36,26,32,0,16,30,14,12,27,16,15                       ; YOU ESCAPED
s_back:    .DB 31,19,16,0,13,12,14,22,29,26,26,24,30                 ; THE BACKROOMS
s_press:   .DB 27,29,16,30,30,0,30,31,12,29,31,0,31,26,0,16,35,20,31 ; PRESS START TO EXIT
