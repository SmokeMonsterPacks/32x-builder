#include "common.h"

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
		while(*vdp_ctrl_port & 8) do_commands();
		while(!(*vdp_ctrl_port & 8)) do_commands();
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
