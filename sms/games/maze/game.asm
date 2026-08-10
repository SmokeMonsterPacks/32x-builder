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
.DEFINE VAR_LFSR  $1C96   ; music: 16-bit Galois LFSR state
.DEFINE VAR_BI    $1C98   ; music: bass walker index
.DEFINE VAR_BATT  $1C99   ; music: bass attenuation
.DEFINE VAR_BFADE $1C9A   ; music: frames to next bass fade step
.DEFINE VAR_BT    $1C9B   ; music: frames to next bass note (16-bit)
.DEFINE VAR_MPTR  $1C9D   ; music: current motif read pointer (16-bit)
.DEFINE VAR_MLEFT $1C9F   ; music: notes left in the motif
.DEFINE VAR_MATT  $1CA0   ; music: melody attenuation
.DEFINE VAR_MFADE $1CA1   ; music: frames to next melody fade step
.DEFINE VAR_NOTET $1CA2   ; music: frames to next note in the motif
.DEFINE VAR_GAP   $1CA3   ; music: frames of silence between phrases (16-bit)
.DEFINE VAR_EATT  $1CA5   ; music: echo attenuation
.DEFINE VAR_EFADE $1CA6   ; music: frames to next echo fade step
.DEFINE ECHO_Q    $1CA8   ; music: 4 slots x (countdown, div_lo, div_hi)
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
   call music_init
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
   call music_tick         ; music runs through the escape screen too
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

; boot-font tile ids: A=12..Z=37, 0=space ('#'-less world)
s_escaped: .DB 36,26,32,0,16,30,14,12,27,16,15                       ; YOU ESCAPED
s_back:    .DB 31,19,16,0,13,12,14,22,29,26,26,24,30                 ; THE BACKROOMS
s_press:   .DB 27,29,16,30,30,0,30,31,12,29,31,0,31,26,0,16,35,20,31 ; PRESS START TO EXIT
