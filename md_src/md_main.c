#include "common.h"
#include "z80_sms_hello.h"

// 32X COMM
static volatile uint16_t* const mars_comm0  = (uint16_t*) MARS_COMM0;
static volatile uint16_t* const mars_comm2  = (uint16_t*) MARS_COMM2;
static volatile uint16_t* const mars_comm8  = (uint16_t*) MARS_COMM8;
static volatile uint16_t* const mars_comm10 = (uint16_t*) MARS_COMM10;
static volatile uint32_t* const mars_comm12 = (uint32_t*) MARS_COMM12;

// VDP
static volatile uint16_t* const vdp_data_port = (uint16_t*) VDP_DATA_PORT;
static volatile uint16_t* const vdp_ctrl_port = (uint16_t*) VDP_CTRL_PORT;
static volatile uint32_t* const vdp_ctrl_wide = (uint32_t*) VDP_CTRL_PORT;

// External functions
extern uint16_t read_joypad(uint8_t player);

uint32_t timer = 0;
uint16_t vramOffset = 0;
// SMS mode active: the VDP is in mode 4, where the mode-5 vblank status
// bit the main loop paces on NEVER sets — polling it livelocks the loop
// BEFORE the pad-publish lines and the player can't exit (COMM8 freezes).
// While set, the loop paces on a crude delay instead and keeps serving.
static uint8_t sms_active = 0;

// It is recommended to put functions that run 1+ times every frame into RAM
// by specifying this attribute before the signature. This keeps the M68K off
// the ROM so the SH-2s can access it without slowdown.
// It should be safe to add or remove it from any function and experiment with
// the speed vs space differences

__attribute__((section(".data")))
void vdp_color(uint16_t index, uint16_t color) {
	index <<= 1;
	*vdp_ctrl_wide = ((0xC000 + (((uint32_t)index) & 0x3FFF)) << 16) + (((uint32_t)index) >> 14);
	*vdp_data_port = color;
}

__attribute__((section(".data")))
void do_commands(void) {
	uint16_t cmd = *mars_comm0;
	switch(cmd >> 8) {
	default: break; // Unknown command
	case 0: return; // No command
	case 3:
		*mars_comm8 = read_joypad(cmd);
		break;
	case 4:
		break;
	case 5: // Set VRAM or Plane offset
		vramOffset = *mars_comm2;
		break;
	case 6: // Write tile to Plane B
		*vdp_ctrl_wide = (((uint32_t)0x6000 + ((vramOffset) & 0x3FFF)) << 16) + (((vramOffset) >> 14) | 0x03);
		*vdp_data_port = *mars_comm2;
		vramOffset += 2;
		break;
	case 7: // Write word to VRAM address
		*vdp_ctrl_wide = (((uint32_t)0x4000 + ((vramOffset) & 0x3FFF)) << 16) + (((vramOffset) >> 14) | 0x00);
		*vdp_data_port = *mars_comm2;
		vramOffset += 2;
		break;
	case 8: // Set MD CRAM color: index in cmd low byte, BGR word in COMM2.
		// The SH-2 fade only reaches 32X CRAM; this is its handle on the
		// MD layer's colors (HUD text, backdrop) so fades take them too.
		vdp_color(cmd & 0xFF, *mars_comm2);
		break;
	case 9: { // SMS BOOT: Z80 heartbeat + the diag screen on the text layer.
		// Bus + reset dance: request the Z80 bus, hold reset, copy the
		// program into Z80 RAM through the $A00000 window, then release
		// reset and the bus — the Z80 wakes at $0000 inside SMS mode.
		volatile uint16_t *busreq = (uint16_t *)Z80_BUS_REQ;
		volatile uint16_t *reset  = (uint16_t *)Z80_RESET;
		volatile uint8_t  *zram   = (uint8_t *)0xA00000;
		// ORDER IS LAW (found by save-state forensics, 2026-08-09: nothing
		// ever landed in Z80 RAM): the MD never GRANTS the bus while the
		// Z80 is held in reset, so reset-then-request deadlocks the 68K in
		// the grant wait and the whole machine hangs politely on black.
		// Release reset FIRST, request, copy, then the stop-start dance.
		*reset = 0x100;                    // reset RELEASED or no grant ever
		*busreq = 0x100;                   // request the bus
		{ uint32_t g = 200000; while ((*busreq & 0x100) && --g) ; }
		for (uint16_t i = 0; i < Z80_SMS_HELLO_LEN; i++)
			zram[i] = z80_sms_hello[i];
		zram[Z80_SMS_MAILBOX_CMD]  = 0;    // the mailbox is OUTSIDE the payload
		zram[Z80_SMS_MAILBOX_DONE] = 0;    // and uninit Z80 RAM boots 0xFF —
		                                   // a phantom command fired a blind
		                                   // VDP burst that erased the font
		*reset = 0x000;                    // assert reset while we still own the bus
		for (volatile uint16_t d = 0; d < 64; d++) ;   // let it latch
		// v5: LOAD IN MODE 5, DISPLAY IN MODE 4. Slot-2 forensics closed
		// the book: in this emulator's mode 4, NO data-port write connects
		// — not the 68K's (magenta canary CRAM), not the Z80's (all 16
		// mailbox sections done, VRAM untouched). Registers work; data
		// does not. But VRAM is VRAM: fill it through the mode-5 pipes
		// the game proves every frame (font uploads, text tiles), THEN
		// flip M5 off. The Z80 keeps its heartbeat as the booted CPU.
		// v6: MODE 5 ALL THE WAY. Ares' MD VDP never implemented mode 4
		// (its own source: debug unimplemented "M5=0") — seven builds of
		// black screens were an emulator stub, not our hardware model.
		// The visible path is the one this cart proves every frame: the
		// mode-5 text layer. Draw the diag screen into Name Table B with
		// the boot font; the Z80 runs its heartbeat as the booted SMS
		// CPU. Mode-4 authenticity stays in git for a real-hardware day.
		{
			const unsigned char *t = z80_sms_text;
			while (*t != 0xFF) {
				uint16_t row = *t++;
				uint16_t col = *t++;
				uint16_t n   = *t++;
				uint32_t ofs = ((row * 64u + col) * 2u) + 0xE000;
				*vdp_ctrl_wide = (uint32_t)(0x4000 | (ofs & 0x3FFF)) << 16
				               | ((ofs >> 14) | 0x03);
				for (uint16_t i = 0; i < n; i++)
					*vdp_data_port = *t++;     // tile id, palette line 0
			}
		}
		break;
	}
	case 10: { // SMS STOP: sweep the splash rows, park the Z80. NOTHING MORE.
		// The old "full restore" tail (InitVDPRegs replay + CRAM repaint +
		// $3800 sweep) was the grey-menu FONT-ERASER: two bisect rounds
		// proved every exit that skipped it kept the font, and the wipe's
		// forensic footprint was exactly the boot font loop's 1440 bytes.
		// Mechanism never named — but in the mode-5-only design the tail
		// restored state the boot never touches, so it is deleted, not
		// gated. If a future mode-4 (real hardware) arc resurrects a
		// restore path, THIS is the first suspect when menus go grey.
		{
			const unsigned char *t = z80_sms_text;
			while (*t != 0xFF) {
				uint16_t row = *t++;
				uint16_t col = *t++;
				uint16_t n   = *t++;
				t += n;
				uint32_t ofs = ((row * 64u + col) * 2u) + 0xE000;
				*vdp_ctrl_wide = (uint32_t)(0x4000 | (ofs & 0x3FFF)) << 16
				               | ((ofs >> 14) | 0x03);
				for (uint16_t i = 0; i < n; i++)
					*vdp_data_port = 0;
			}
		}
		volatile uint16_t *busreq = (uint16_t *)Z80_BUS_REQ;
		volatile uint16_t *reset  = (uint16_t *)Z80_RESET;
		// BOUNDED grant wait — the last unbounded spin in the whole SMS
		// chain. If the grant never comes, proceed: we only want the Z80
		// parked, and asserting reset needs no bus.
		*busreq = 0x100;
		{ uint32_t g = 200000; while ((*busreq & 0x100) && --g) ; }
		*reset = 0x000;                    // park the Z80 in reset again
		*busreq = 0x000;                   // and RELEASE the bus — leaving it
		                                   // requested left the machine odd
		break;
	}
	}
	*mars_comm0 = 0;
}

// Sticky six-button latch. read_joypad returns bit 0x1000 set when the pad
// validated the six-button signature THIS frame, with M X Y Z in 0x0F00.
// Wireless receivers/adapters validate intermittently (async latching vs our
// TH probe), and a miss used to drop MODE mid-hold — which also defeats the
// SH-2's MODE-held-2-frames debounce. Once a pad has EVER validated, a miss
// holds the last good extended bits instead. The failed frame's own extended
// data is never trusted (that way lie the phantoms).
__attribute__((section(".data")))
uint16_t pad_sticky(uint8_t n, uint16_t p) {
	static uint16_t last_ext[2];
	static uint8_t  is_six[2];
	if (p & 0x1000) {
		is_six[n] = 1;
		last_ext[n] = p & 0x0F00;
	} else if (is_six[n]) {
		p = (uint16_t)((p & ~0x0F00u) | last_ext[n] | 0x1000);
	}
	return p;
}

__attribute__((section(".data")))
void main(void) {
	// Backdrop/border stays BLACK. The marsdev demo's grey color-cycle
	// lived here for the project's whole life: Ares crops the overscan so
	// nobody saw it, but a CRT shows the border pulsing grey at full
	// brightness through every dark scene and fade. Boot already set CRAM
	// 0 black; nothing may write it again.
	while(1) {
		// TODO: Remove this after fixing _vblank
		if (sms_active) {
			// mode 4: the mode-5 vblank bit is dead — pace by delay,
			// keep serving commands, and FALL THROUGH to the pad publish.
			for (volatile uint16_t dl = 0; dl < 2500; dl++) do_commands();
		} else {
			while(*vdp_ctrl_port & 8) do_commands();
			while(!(*vdp_ctrl_port & 8)) do_commands();
		}
		// Publish both pads UNSOLICITED every frame, exactly like the COMM12
		// frame tick below. The SH-2 then reads COMM8/COMM10 directly with no
		// request/response round-trip — the old on-demand handshake (COMM0
		// command 3, still serviced above for compatibility) is what starved
		// the bridge under render contention. P2 is published for free; no
		// gameplay reads COMM10 yet.
		*mars_comm8  = pad_sticky(0, read_joypad(0));
		*mars_comm10 = pad_sticky(1, read_joypad(1));
		*mars_comm12 = ++timer;
	}
}
