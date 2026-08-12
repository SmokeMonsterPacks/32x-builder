#!/usr/bin/env python3
"""Smoke-test the SMS mini-game blob with a subset Z80 interpreter.

Runs the REAL bytes out of sms/game.bin against a fake 68K: loads a
known maze into MAP_BITS/MAP_META, ticks FRAME_MBX like the bridge
does, pokes pad bytes, and reads TILEBUF back. Asserts:

  1. first frame renders (DIRTY set, walls/floor/player/exit tiles)
  2. the player moves on a d-pad press and the frame redraws
  3. walls block
  4. held direction auto-repeats
  5. viewport scrolls when the player walks deep
  6. stepping into the exit cell flips STATE and draws the escape text

The interpreter covers only the opcodes the game uses and raises on
anything else — an unknown opcode IS a test failure.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GAME = sys.argv[1] if len(sys.argv) > 1 else "maze"
BIN = ROOT / "sms" / "games" / GAME / "game.bin"

TILEBUF, MAP_BITS, MAP_META = 0x1900, 0x1C00, 0x1C80
PAD, DIRTY, STATE, FRAME = 0x1FF4, 0x1FF5, 0x1FF6, 0x1FF7
T_FLOOR, T_WALL, T_PLAYER, T_EXIT = 1, 44, 27, 16


PSG_PORT = 0x7F11


class Z80:
    def __init__(self, mem):
        self.m = mem
        self.pc = 0
        self.sp = 0
        self.a = self.b = self.c = self.d = self.e = self.h = self.l = 0
        self.fz = self.fc = False   # only Z and C matter to this program
        self.psg = []               # bytes written to $7F11, in order

    # -- register helpers -------------------------------------------------
    def hl(self): return (self.h << 8) | self.l
    def de(self): return (self.d << 8) | self.e
    def bc(self): return (self.b << 8) | self.c
    def set_hl(self, v): self.h, self.l = (v >> 8) & 0xFF, v & 0xFF
    def set_de(self, v): self.d, self.e = (v >> 8) & 0xFF, v & 0xFF
    def set_bc(self, v): self.b, self.c = (v >> 8) & 0xFF, v & 0xFF

    R = ["b", "c", "d", "e", "h", "l", None, "a"]   # r-field encoding

    def get_r(self, i):
        if i == 6:
            return self.m[self.hl()]
        return getattr(self, self.R[i])

    def set_r(self, i, v):
        if i == 6:
            self.m[self.hl()] = v
        else:
            setattr(self, self.R[i], v)

    def fetch(self):
        v = self.m[self.pc]
        self.pc = (self.pc + 1) & 0xFFFF
        return v

    def fetch16(self):
        lo = self.fetch()
        return lo | (self.fetch() << 8)

    def push(self, v):
        self.sp = (self.sp - 2) & 0xFFFF
        self.m[self.sp] = v & 0xFF
        self.m[self.sp + 1] = (v >> 8) & 0xFF

    def pop(self):
        v = self.m[self.sp] | (self.m[self.sp + 1] << 8)
        self.sp = (self.sp + 2) & 0xFFFF
        return v

    def alu(self, op, v):
        if op == 0:                                   # ADD
            r = self.a + v
            self.fc = r > 0xFF
            self.a = r & 0xFF
            self.fz = self.a == 0
        elif op == 2:                                 # SUB
            r = self.a - v
            self.fc = r < 0
            self.a = r & 0xFF
            self.fz = self.a == 0
        elif op == 4:                                 # AND
            self.a &= v
            self.fc = False
            self.fz = self.a == 0
        elif op == 5:                                 # XOR
            self.a ^= v
            self.fc = False
            self.fz = self.a == 0
        elif op == 6:                                 # OR
            self.a |= v
            self.fc = False
            self.fz = self.a == 0
        elif op == 7:                                 # CP
            r = self.a - v
            self.fc = r < 0
            self.fz = (r & 0xFF) == 0
        else:
            raise NotImplementedError(f"alu op {op}")

    def step(self):
        op = self.fetch()
        if op == 0x00:  return                        # nop
        if op == 0xF3:  return                        # di
        if op == 0x31:  self.sp = self.fetch16(); return
        if op == 0x21:  self.set_hl(self.fetch16()); return
        if op == 0x11:  self.set_de(self.fetch16()); return
        if op == 0x01:  self.set_bc(self.fetch16()); return
        if op == 0x3A:  self.a = self.m[self.fetch16()]; return
        if op == 0x32:
            addr = self.fetch16()
            self.m[addr] = self.a
            if addr == PSG_PORT:
                self.psg.append(self.a)
            return
        if op == 0x2A:                                # ld hl,(nn)
            addr = self.fetch16()
            self.l, self.h = self.m[addr], self.m[addr + 1]
            return
        if op == 0x22:                                # ld (nn),hl
            addr = self.fetch16()
            self.m[addr], self.m[addr + 1] = self.l, self.h
            return
        if op == 0x36:  self.m[self.hl()] = self.fetch(); return
        if op == 0x34:                                # inc (hl)
            v = (self.m[self.hl()] + 1) & 0xFF
            self.m[self.hl()] = v
            self.fz = v == 0
            return
        if op == 0x35:                                # dec (hl)
            v = (self.m[self.hl()] - 1) & 0xFF
            self.m[self.hl()] = v
            self.fz = v == 0
            return
        if op == 0x23:  self.set_hl((self.hl() + 1) & 0xFFFF); return
        if op == 0x13:  self.set_de((self.de() + 1) & 0xFFFF); return
        if op == 0x0B:  self.set_bc((self.bc() - 1) & 0xFFFF); return
        if op == 0x2B:  self.set_hl((self.hl() - 1) & 0xFFFF); return
        if op == 0x19:                                # add hl,de
            r = self.hl() + self.de()
            self.fc = r > 0xFFFF
            self.set_hl(r & 0xFFFF)
            return
        if op == 0x09:                                # add hl,bc
            r = self.hl() + self.bc()
            self.fc = r > 0xFFFF
            self.set_hl(r & 0xFFFF)
            return
        if op == 0x29:                                # add hl,hl
            r = self.hl() * 2
            self.fc = r > 0xFFFF
            self.set_hl(r & 0xFFFF)
            return
        if op == 0x1A:  self.a = self.m[self.de()]; return
        if op == 0x12:                                # ld (de),a
            addr = self.de()
            self.m[addr] = self.a
            if addr == PSG_PORT:
                self.psg.append(self.a)
            return
        if op == 0x2F:  self.a ^= 0xFF; return        # cpl
        if op == 0x0F:                                # rrca
            c = self.a & 1
            self.a = (self.a >> 1) | (c << 7)
            self.fc = bool(c)
            return
        if op == 0x07:                                # rlca
            c = (self.a >> 7) & 1
            self.a = ((self.a << 1) | c) & 0xFF
            self.fc = bool(c)
            return
        if op == 0x1F:                                # rra
            c = self.a & 1
            self.a = (self.a >> 1) | (0x80 if self.fc else 0)
            self.fc = bool(c)
            return
        if op == 0xEB:                                # ex de,hl
            d, e = self.d, self.e
            self.d, self.e, self.h, self.l = self.h, self.l, d, e
            return
        if 0x40 <= op <= 0x7F and op != 0x76:         # ld r,r'
            self.set_r((op >> 3) & 7, self.get_r(op & 7))
            return
        if 0x80 <= op <= 0xBF:                        # alu a,r
            self.alu((op >> 3) & 7, self.get_r(op & 7))
            return
        if op in (0xC6, 0xD6, 0xE6, 0xEE, 0xF6, 0xFE):  # alu a,n
            self.alu({0xC6: 0, 0xD6: 2, 0xE6: 4, 0xEE: 5,
                      0xF6: 6, 0xFE: 7}[op], self.fetch())
            return
        if op in (0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x3E):  # ld r,n
            self.set_r((op >> 3) & 7, self.fetch())
            return
        if op in (0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x3C):  # inc r
            i = (op >> 3) & 7
            v = (self.get_r(i) + 1) & 0xFF
            self.set_r(i, v)
            self.fz = v == 0
            return
        if op in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x3D):  # dec r
            i = (op >> 3) & 7
            v = (self.get_r(i) - 1) & 0xFF
            self.set_r(i, v)
            self.fz = v == 0
            return
        if op == 0x18:                                # jr d
            d = self.fetch()
            self.pc = (self.pc + (d - 256 if d > 127 else d)) & 0xFFFF
            return
        if op in (0x20, 0x28, 0x30, 0x38):            # jr cc,d
            d = self.fetch()
            take = {0x20: not self.fz, 0x28: self.fz,
                    0x30: not self.fc, 0x38: self.fc}[op]
            if take:
                self.pc = (self.pc + (d - 256 if d > 127 else d)) & 0xFFFF
            return
        if op == 0x10:                                # djnz
            d = self.fetch()
            self.b = (self.b - 1) & 0xFF
            if self.b:
                self.pc = (self.pc + (d - 256 if d > 127 else d)) & 0xFFFF
            return
        if op == 0xC3:  self.pc = self.fetch16(); return
        if op in (0xC2, 0xCA, 0xD2, 0xDA):            # jp cc,nn
            t = self.fetch16()
            take = {0xC2: not self.fz, 0xCA: self.fz,
                    0xD2: not self.fc, 0xDA: self.fc}[op]
            if take:
                self.pc = t
            return
        if op == 0xCD:                                # call
            t = self.fetch16()
            self.push(self.pc)
            self.pc = t
            return
        if op == 0xC9:  self.pc = self.pop(); return  # ret
        if op in (0xC0, 0xC8, 0xD0, 0xD8):            # ret cc
            take = {0xC0: not self.fz, 0xC8: self.fz,
                    0xD0: not self.fc, 0xD8: self.fc}[op]
            if take:
                self.pc = self.pop()
            return
        if op == 0xC5:  self.push(self.bc()); return
        if op == 0xD5:  self.push(self.de()); return
        if op == 0xE5:  self.push(self.hl()); return
        if op == 0xC1:  self.set_bc(self.pop()); return
        if op == 0xD1:  self.set_de(self.pop()); return
        if op == 0xE1:  self.set_hl(self.pop()); return
        if op == 0xCB:                                # CB prefix: sla/srl
            sub = self.fetch()
            i = sub & 7
            v = self.get_r(i)
            if 0x20 <= sub <= 0x27:                   # sla
                self.fc = bool(v & 0x80)
                v = (v << 1) & 0xFF
            elif 0x38 <= sub <= 0x3F:                 # srl
                self.fc = bool(v & 1)
                v >>= 1
            elif 0x18 <= sub <= 0x1F:                 # rr (through carry)
                c = v & 1
                v = (v >> 1) | (0x80 if self.fc else 0)
                self.fc = bool(c)
            else:
                raise NotImplementedError(f"CB {sub:02X}")
            self.set_r(i, v)
            self.fz = v == 0
            return
        raise NotImplementedError(f"opcode {op:02X} at {self.pc - 1:04X}")


# -- harness ---------------------------------------------------------------
mem = bytearray(0x10000)
blob = BIN.read_bytes()
mem[0:len(blob)] = blob

# maze: border walls + a vertical bar at x=5 with a gap at y=16.
# spawn (2,2), exit stamped INTO the right border wall at (31,16).
maze = [[0] * 32 for _ in range(32)]
for i in range(32):
    maze[0][i] = maze[31][i] = maze[i][0] = maze[i][31] = 1
for y in range(1, 31):
    if y != 16:
        maze[y][5] = 1
for y in range(32):
    for x in range(32):
        if maze[y][x]:
            mem[MAP_BITS + y * 4 + (x >> 3)] |= 0x80 >> (x & 7)
mem[MAP_META:MAP_META + 4] = bytes([2, 2, 31, 16])

cpu = Z80(mem)


FRAME_COUNT = [0]


def run_frame(pad):
    """One 68K frame: poke pad, tick FRAME, run the Z80 a while."""
    FRAME_COUNT[0] += 1
    mem[PAD] = pad
    mem[FRAME] = (mem[FRAME] + 1) & 0xFF
    for _ in range(200000):
        cpu.step()
        # loop is "settled" when it's back polling FRAME with no change
        if cpu.pc == 0 and False:
            break
    # 200k steps >> one frame of work; the loop just spins on FRAME_MBX


def settle(n=1, pad=0):
    for _ in range(n):
        run_frame(pad)


def tile(row, col):
    return mem[TILEBUF + row * 32 + col]


def frame_rows():
    return ["".join({0: " ", T_FLOOR: ".", T_WALL: "#",
                     T_PLAYER: "P", T_EXIT: "E"}.get(tile(r, c), "?")
                    for c in range(32)) for r in range(24)]


def find_player():
    for r in range(24):
        for c in range(32):
            if tile(r, c) == T_PLAYER:
                return r, c
    return None


fails = 0


def check(name, cond):
    global fails
    print(("PASS  " if cond else "FAIL  ") + name)
    if not cond:
        fails += 1


# boot: lands on the MENU (banner wipe + chime), not the game
for _ in range(400000):
    cpu.step()
check("boot: menu not game (no frame, no player)",
      mem[DIRTY] == 0 and find_player() is None)
settle(34)                           # 32 wipe columns + the text step
check("menu: BACK banner drawn", tile(4, 8) == 44 and tile(4, 9) == 44)
check("menu: ROOMS banner drawn", tile(10, 6) == 44)
check("menu: PRESS BUTTON prompt", tile(20, 10) == 27)
mem[DIRTY] = 0
settle(80)                           # chime completes at frame 106
check("menu: no redraw while idle", mem[DIRTY] == 0)
run_frame(0x10)                      # button 1 (B) starts the maze
check("button starts game: player at spawn (2,2)",
      mem[DIRTY] == 1 and tile(2, 2) == T_PLAYER)
run_frame(0x00)
check("game: border wall drawn", tile(0, 0) == T_WALL and tile(0, 31) == T_WALL)
check("game: floor drawn", tile(1, 1) == T_FLOOR)
check("game: exit visible at (16,31)", tile(16, 31) == T_EXIT)

mem[DIRTY] = 0                       # 68K consumed the frame
settle(2, 0)                         # idle frames: nothing redraws
check("idle: no spurious redraw", mem[DIRTY] == 0)

run_frame(0x08)                      # RIGHT pressed
check("move right: redraw", mem[DIRTY] == 1)
check("move right: player at (2,3)", tile(2, 3) == T_PLAYER
      and tile(2, 2) != T_PLAYER)
mem[DIRTY] = 0

run_frame(0x00)                      # release
run_frame(0x01)                      # UP into the wall at y=1... (2,3): up = (1,3) floor!
check("move up: (1,3) is open, player moved", tile(1, 3) == T_PLAYER)
mem[DIRTY] = 0
run_frame(0x00)
run_frame(0x01)                      # UP again into border wall y=0
check("wall blocks: no redraw", mem[DIRTY] == 0)
check("wall blocks: player still at (1,3)", tile(1, 3) == T_PLAYER)

# auto-repeat: hold DOWN for 20 frames, expect >= 2 steps
p0 = find_player()
run_frame(0x02)
for _ in range(19):
    run_frame(0x02)
    mem[DIRTY] = 0
p1 = find_player()
check(f"auto-repeat: held DOWN moved {p0} -> {p1}", p1[0] - p0[0] >= 2)

# walk to the gap and deep down: viewport must scroll (player row pinned ~12)
for _ in range(60):
    run_frame(0x02)
    mem[DIRTY] = 0
    run_frame(0)
p = find_player()
check(f"viewport scrolled: player row {p[0]} = 22 (map y=30, vp=8)",
      p == (22, find_player()[1]))

# route to the exit door at (31,16): align y=16 (the bar's gap row), then
# walk right through the gap to x=30, then step INTO the door. Steer off
# the game's own player vars so the route can't drift.
VAR_PX, VAR_PY = 0x1C90, 0x1C91


def tap(pad):
    run_frame(pad)
    mem[DIRTY] = 0
    run_frame(0)


for _ in range(40):
    if mem[VAR_PY] == 16:
        break
    tap(0x01 if mem[VAR_PY] > 16 else 0x02)
for _ in range(40):
    if mem[VAR_PX] == 30 or mem[STATE]:
        break
    tap(0x08)
tap(0x08)                            # the step into the door itself
check("escape: STATE flipped", mem[STATE] == 1)
settle(150, 0)                       # music keeps playing on the escape screen


# ---- music parity: PSG stream must match the reference engine -----------
# Independent reimplementation of games/maze/game.asm's SPACE-A music (and
# of run_engine_space in sms_liminal_gen.py): the whole point is three
# copies of the algorithm agreeing byte for byte. Echoes use the Z80's
# countdown model; section order per frame is the parity contract:
# bass, phrase select, note fire, melody decay, echo fire, echo decay.
def ref_psg(seed, frames):
    s = (seed & 0xFFFF) or 1

    def step():
        nonlocal s
        lsb = s & 1
        s >>= 1
        if lsb:
            s ^= 0xB400
        return s

    MOTIFS = [[285, 240, 254, 285], [190, 214, 240, 285],
              [143, 190, 240], [254, 240, 285, 190]]
    BASS = [762, 855, 960, 762]
    BT = [330 + i * 30 for i in range(8)]
    MGAP = [75 + i * 30 for i in range(8)]

    def tone(base, d):
        return [base | (d & 0xF), (d >> 4) & 0x3F]

    out = [0x9F, 0xBF, 0xDF, 0xFF]               # boot silence
    # chime (frames 0-106): REVERSED SE-GA (sms_putrats) — the chord
    # swells in from silence (0-36), holds (37-81), sweeps DOWN the
    # table (82-105), and cuts (106). Engine ticks from frame 107.
    CH = [(0x80, 1023, 19, 571), (0xA0, 762, 16, 381), (0xC0, 570, 12, 285)]
    b_i, b_att, b_fade, b_t = 0, 15, 0, 0
    motif, mi = [], 0
    m_att, m_fade, note_t, gap = 15, 0, 0, 0
    e_att, e_fade = 15, 0
    echo = []                                    # [countdown, div]
    ch_att = 15
    for f in range(frames):
        if f < 107:
            if f == 0:
                for base, _, _, tgt in CH:
                    out += tone(base, tgt)
            if f <= 36 and f % 3 == 0:
                ch_att -= 1
                out += [0x90 | ch_att, 0xB0 | ch_att, 0xD0 | ch_att]
            elif 82 <= f <= 105:
                i = 105 - f
                for base, start, stp, _ in CH:
                    out += tone(base, start - stp * i)
            elif f == 106:
                out += [0x9F, 0xBF, 0xDF]
            continue
        if b_t == 0:
            d = BASS[b_i & 3]
            b_i += 1 if (step() & 3) else 2
            out += tone(0x80, d)
            out.append(0x90 | 6)
            b_att, b_fade = 6, 30
            b_t = BT[step() & 7]
        b_t -= 1
        if b_att < 15:
            b_fade -= 1
            if b_fade == 0:
                b_fade = 30
                b_att += 1
                out.append(0x90 | b_att)
        if mi >= len(motif):
            if gap == 0:
                motif = MOTIFS[step() & 3]
                mi, note_t = 0, 0
                gap = MGAP[step() & 7]
            else:
                gap -= 1
        if mi < len(motif) and note_t == 0:
            d = motif[mi]
            mi += 1
            out += tone(0xC0, d)
            out.append(0xD0 | 2)
            m_att, m_fade = 2, 6
            echo.append([19, d])
            note_t = 13 + (step() & 7)
        elif note_t > 0:
            note_t -= 1
        if m_att < 15:
            m_fade -= 1
            if m_fade == 0:
                m_fade = 6
                m_att += 1
                out.append(0xD0 | m_att)
        for slot in echo:
            slot[0] -= 1
            if slot[0] == 0:
                out += tone(0xA0, slot[1])
                out.append(0xB0 | 7)
                e_att, e_fade = 7, 6
        echo = [x for x in echo if x[0] > 0]
        if e_att < 15:
            e_fade -= 1
            if e_fade == 0:
                e_fade = 6
                e_att += 1
                out.append(0xB0 | e_att)
    return out


m_seed = 0xACE1 ^ 0x0202 ^ 0x101F   # ^ (spawn_y:x) ^ (exit_y:x) of this maze
expected = ref_psg(m_seed, FRAME_COUNT[0])
check(f"music parity: {len(cpu.psg)} PSG writes match reference byte-for-byte",
      cpu.psg == expected)
check("music: melody onsets occurred",
      sum(1 for b in cpu.psg if b & 0xF0 == 0xC0) >= 1)
rows = frame_rows()
esc = "".join(chr(t - 12 + ord('A')) if 12 <= t <= 37 else ' '
              for t in mem[TILEBUF + 8 * 32:TILEBUF + 8 * 32 + 32])
check(f"escape screen: 'YOU ESCAPED' drawn ({esc.strip()!r})",
      "YOUESCAPED" in esc.replace(" ", ""))
p2 = find_player()
settle(3, 0x08)
check("escaped: further input ignored", mem[STATE] == 1)

if fails:
    print("\n".join(frame_rows()))
    sys.exit(f"{fails} FAILURES")
print("all good")
