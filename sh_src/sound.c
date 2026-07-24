#include "mars.h"
#include "shared.h"
#include "sound.h"
#include "amb_buzz.h"
#include "amb_neon.h"
#include "amb_hello.h"
#include "amb_step.h"

/* Neanderthal sprite position (matches the entry in raycast.c::standups).
 * Keeping a duplicate here on the audio side means the secondary doesn't
 * have to read the standups table — cleaner separation, and we only
 * have one Voyager-broadcasting sprite. */
#define NEANDER_X_CELL 16
#define NEANDER_Y_CELL 23

/* Distance-based hello attenuation: linear fade from full at the
 * neanderthal's cell to silence at HELLO_FADE_RADIUS cells away,
 * applied to the squared distance. */
#define HELLO_FADE_RADIUS_SQ 64   /* 8 cells */

/* PWM duty-cycle compare range. Widened from the old [2..1032] (half-
 * span 515) to [8..1430] (half-span 711) so the per-sample mix sum has
 * ~38% more headroom before clipping. At our 16kHz output rate
 * PWM_CYCLE = 1438; we use 1430 of those ticks leaving 8 ticks
 * (~0.6%) of safety margin. Keeps buzz/neon levels intact (they're
 * the constant ambient bed) while giving room for hello+step to mix
 * in without summing past the rails. */
#define SAMPLE_MIN 8
#define SAMPLE_MAX 1430
#define SAMPLE_CENTER ((SAMPLE_MAX + SAMPLE_MIN) / 2)   /* 719 */

/* Soft-clip thresholds — about 80% of the symmetric budget around
 * SAMPLE_CENTER. Above SOFT_HIGH or below SOFT_LOW the mix sum is
 * compressed 4:1 toward the rails (smooth harmonic warmth) instead
 * of hard-clipped (harsh digital crunch). The hard clip at
 * SAMPLE_MIN/MAX remains as a final backstop. */
#define SOFT_HIGH 1290   /* SAMPLE_CENTER + 571 (~80% of 711 budget) */
#define SOFT_LOW   148   /* SAMPLE_CENTER - 571 */

/* Ping-pong SDRAM buffers the secondary mixes into while DMA1 drains
 * the other one. DMA reads from cached SDRAM here; SH-2 cache is
 * write-through, so the secondary's stores reach memory before the DMA
 * accesses them.
 *
 * SIZED FOR RENDER STARVATION. amb_pump() historically ran only in the
 * secondary's idle COMM4 loop — during a CMD_HALF/CMD_TAIL render
 * chunk nothing pumped for tens of ms. The original 2×256 @16kHz gave
 * 16 ms per buffer / 32 ms total runway, LESS than one render chunk in
 * a dense scene, so DMA looped back into a stale buffer mid-chunk and
 * replayed 16 ms fragments — the audible PWM chop. The fix is two
 * halves: 1024 samples = 64 ms per buffer here, and pump checkpoints
 * BETWEEN the secondary's render passes (s_main.c) so the longest
 * pump-starved stretch is one pass (~10-30 ms), not a whole chunk.
 * Cost: 4 KB SDRAM, fill ~0.6 ms per 64 ms.
 *
 * 2048 (128 ms) would be nicer margin but does NOT fit: .bss ends
 * ~7 KB below the primary stack top (0x0603F000, mars_start.s), and
 * 2×2048×2B pushed _end past it — silently, since mars.ld's ram
 * LENGTH runs to 0x0603FC00. Check `nm -n | grep _end` against
 * 0x0603F000 before growing ANYTHING in .bss. */
#define AMB_SAMPLES_PER_BUF 1024   /* 64 ms per buffer @ 16 kHz */
#define AMB_SAMPLES_OLD      256   /* pre-fix size, kept as the A/B arm */

static uint16_t amb_pwm_buf[2][AMB_SAMPLES_PER_BUF]
                            __attribute__((aligned(16)));
static volatile uint8_t amb_current_buf_idx;
static volatile uint8_t amb_buf_needs_fill;   /* bit i = buf i needs fill */

/* Title-screen gate. The ambient pump stays fully idle (skipping its
 * fill work) until the game world loads — so the title burns no
 * secondary cycles on audio, and the PWM is free for dedicated title SFX.
 * Primary flips it on via amb_set_active(); the secondary reads it through
 * the cache-through alias for coherency. */
static uint8_t amb_active_storage = 0;
#define AMB_ACTIVE (*(volatile uint8_t *)((uintptr_t)&amb_active_storage | 0x20000000))

void amb_set_active(int on) { AMB_ACTIVE = (uint8_t)(on ? 1 : 0); }

/* Runtime buffer length — the underrun fix's same-binary A/B knob
 * (AUDIO menu tab, BUFFER row: 64MS vs 16MS). The primary toggles it;
 * the secondary's IRQ handler and pump read it. Cache-through alias
 * for coherency, same pattern as AMB_ACTIVE below. Flipping mid-stream
 * causes one transient glitch (a partially-stale buffer plays once) —
 * fine for a diagnostic knob. */
static uint16_t amb_buf_len_storage = AMB_SAMPLES_PER_BUF;
#define AMB_BUF_LEN \
    (*(volatile uint16_t *)((uintptr_t)&amb_buf_len_storage | 0x20000000))

/* Underrun counter — incremented by amb_dma_handler when it swaps into
 * a buffer the pump never got to refill (i.e. DMA is about to replay
 * stale samples: the audible chop, made countable). Primary reads it
 * for the metrics HUD (AU:). Only counted while AMB_ACTIVE — at the
 * title the pump idles by design and every swap would false-positive. */
static uint16_t amb_underruns_storage = 0;
#define AMB_UNDERRUNS \
    (*(volatile uint16_t *)((uintptr_t)&amb_underruns_storage | 0x20000000))

void     amb_toggle_buf_len(void) {
    AMB_BUF_LEN = (AMB_BUF_LEN == AMB_SAMPLES_PER_BUF)
                      ? AMB_SAMPLES_OLD : AMB_SAMPLES_PER_BUF;
}
int      amb_buf_len_is_big(void) { return AMB_BUF_LEN == AMB_SAMPLES_PER_BUF; }
uint16_t amb_get_underruns(void)  { return AMB_UNDERRUNS; }

/* Mars_InitPWM — lifted verbatim from d32xr's marshw.c. Three writes
 * to the MONO register flush the FIFO; PWM_CYCLE sets the period
 * (= SH-2 clock / sample_rate); CTRL=0x0185 enables mono output with
 * DREQ on FIFO-empty + PWM interrupt. The trailing ramp from
 * min_sample up to center slowly biases the speaker's DC voltage —
 * skipping it produces a loud pop on power-on.
 *
 * NTSC clock (23.0114 MHz); PAL would need the 22.8015 MHz variant,
 * but MiSTer ships NTSC and we haven't validated PAL yet. */
static void Mars_InitPWM(int sample_rate, int min_sample, int max_sample) {
    int centre_sample = (max_sample - min_sample) / 2;

    MARS_PWM_MONO = 1;
    MARS_PWM_MONO = 1;
    MARS_PWM_MONO = 1;

    MARS_PWM_CYCLE = (uint16_t)((((23011361 << 1) / sample_rate + 1) >> 1) + 1);

    /* CTRL = 0x0185:
     *   [1:0] TM  = 01 -> mono output
     *   [3:2] RMD = 01 -> right = pulse
     *   [5:4] LMD = 00 -> left  = same as right (mono mode)
     *   [7]   AL  = 1  -> DREQ enable on FIFO-empty
     *   [8]   --  = 1  -> PWM interrupt enable on FIFO-empty
     *   [11:8] TM-count = 1
     */
    MARS_PWM_CTRL = 0x0185;

    /* DC-bias ramp to suppress power-on pop. */
    int sample = min_sample;
    while (sample < centre_sample) {
        int reps = (sample_rate * 2) / (centre_sample - min_sample);
        for (int ix = 0; ix < reps; ix++) {
            while (MARS_PWM_MONO & 0x8000) {}   /* wait for FIFO slot */
            MARS_PWM_MONO = (uint16_t)sample;
        }
        sample++;
    }
}

/* DMA-complete handler: swaps to the OTHER ping-pong buffer and marks
 * the just-drained one as needing refill. The pump reads that flag in
 * the secondary's idle loop and refills. */
void amb_dma_handler(void) {
    /* The buffer we WERE draining is now empty — flag for refill. */
    amb_buf_needs_fill |= (uint8_t)(1 << amb_current_buf_idx);

    /* Swap to the other buffer. */
    amb_current_buf_idx ^= 1;

    /* UNDERRUN: the buffer we're about to drain still has its
     * needs-fill bit set — the pump never got scheduled between swaps,
     * so DMA will replay stale samples. This is the chop, counted.
     * Gated on AMB_ACTIVE: at the title the pump idles by design and
     * both buffers hold silence, so a swap there proves nothing. */
    if (AMB_ACTIVE && (amb_buf_needs_fill & (uint8_t)(1 << amb_current_buf_idx)))
        AMB_UNDERRUNS++;

    /* Point DMA at the new current buffer. */
    SH2_DMA_SAR1  = (uint32_t)(uintptr_t)amb_pwm_buf[amb_current_buf_idx];
    SH2_DMA_TCR1  = AMB_BUF_LEN;
    SH2_DMA_CHCR1 = 0x14E5;
}

/* CHCR1 = 0x14E5 (mono word transfer):
 *   [1:0]   = 01 -> DE=1 (enable), TE=0
 *   [2]     = 1  -> IE (interrupt enable on transfer end)
 *   [6]     = 1  -> DS (DREQ edge-triggered)
 *   [7]     = 1  -> AL (auto-request level)
 *   [11:10] = 01 -> TS = word (16-bit transfer)
 *   [13:12] = 01 -> SM = source-increment
 *   [15:14] = 00 -> DM = destination-fixed (PWM register)
 *   All other bits 0.
 */

/* Step C: pump. Called from the secondary's idle polling loop. Checks if
 * either ping-pong buffer needs a refill (set by amb_dma_handler when
 * a buffer completes draining), and if so reads AMB_SAMPLES_PER_BUF
 * samples from the ROM source into it. The read position advances
 * across the source and wraps at the end so the loop seamlessly
 * repeats. Step D adds runtime gain; step E exposes the gain knob to
 * the primary via shared memory. */
/* xorshift32 PRNG — cheap, decent quality. State seeded by something
 * non-zero so the sequence isn't degenerate. */
static uint32_t prng_state = 0xCAFEBABE;
static inline uint32_t prng_next(void) {
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 17;
    prng_state ^= prng_state << 5;
    return prng_state;
}

/* Neon-sting playback state. When neon_active is true, the pump mixes
 * amb_neon_samples on top of the buzz; when neon_pos reaches the end,
 * it deactivates. */
static uint32_t neon_pos = 0;
static int      neon_active = 0;

/* Voyager Golden Record "hellos in many languages" — loops continuously
 * but its mix amplitude is scaled by distance from the neanderthal
 * sprite. Out beyond HELLO_FADE_RADIUS_SQ cells, contributes zero.
 *
 * Stored as int8_t at AMB_HELLO_SAMPLE_RATE (6000 Hz) — far smaller
 * than 16-bit at the output rate, with a "lo-fi radio" character that
 * fits the Backrooms aesthetic. Played back at the 16 kHz output rate,
 * so hello_pos is a 16.16 fixed-point index that advances by
 * HELLO_STEP_FX per output sample. */
static uint32_t hello_pos_fx = 0;
#define HELLO_STEP_FX \
    ((uint32_t)(((uint64_t)AMB_HELLO_SAMPLE_RATE << 16) / AMB_BUZZ_SAMPLE_RATE))

/* Voyager-hello playback-speed trim. On real hardware the hellos drag
 * slightly slower than in Ares while the buzz bed sounds right, so this
 * scales ONLY the hello's fractional read step: the voice speeds up and
 * rises in pitch, buzz/neon/footsteps are untouched. Baseline
 * HELLO_STEP_FX = 100%. Primary adjusts it from the AUDIO menu (VOICE
 * row); the secondary snapshots it once per buffer fill. Once the value
 * that matches Ares on hardware is found, bake it as the default step
 * and drop the knob. Cache-through alias so the two CPUs stay coherent. */
static uint32_t hello_step_storage = HELLO_STEP_FX;
#define HELLO_STEP (*(volatile uint32_t *)((uintptr_t)&hello_step_storage | 0x20000000))
#define HELLO_STEP_PCT_STEP ((HELLO_STEP_FX + 50) / 100)   /* ~1% per menu press */
#define HELLO_STEP_MIN      ((HELLO_STEP_FX * 90)  / 100)  /* 90%  floor */
#define HELLO_STEP_MAX      ((HELLO_STEP_FX * 150) / 100)  /* 150% ceiling */

/* Menu hooks (called on the PRIMARY). dir = -1/+1 steps the voice speed
 * by ~1%; the pct query drives the "VOICE 1xx%" readout. */
void amb_voice_speed_adjust(int dir) {
    int32_t s = (int32_t)HELLO_STEP + dir * (int32_t)HELLO_STEP_PCT_STEP;
    if (s < (int32_t)HELLO_STEP_MIN) s = HELLO_STEP_MIN;
    if (s > (int32_t)HELLO_STEP_MAX) s = HELLO_STEP_MAX;
    HELLO_STEP = (uint32_t)s;
}
int amb_voice_speed_pct(void) {
    return (int)((HELLO_STEP * 100 + (HELLO_STEP_FX / 2)) / HELLO_STEP_FX);
}

/* Carpet footstep — 3 s of continuous walking sounds, plays/loops only
 * while SHARED_UC->is_walking is set. Same 8-bit s8 / fractional-rate
 * scheme as the hello. When walking stops, step_pos_fx freezes so the
 * loop resumes mid-stride on the next walking interval instead of
 * restarting from the beginning (sounds more natural). */
static uint32_t step_pos_fx = 0;
#define STEP_STEP_FX \
    ((uint32_t)(((uint64_t)AMB_STEP_SAMPLE_RATE << 16) / AMB_BUZZ_SAMPLE_RATE))

/* Buzz envelope state for the "fluorescent flicker" effect. Most of
 * the time amp = 256 (full); occasionally the envelope dips to ~12%,
 * holds, then rises back, simulating the dim-and-restore behavior of
 * a failing ballast. Phase state machine:
 *   0 = steady   (full amp, random chance to trigger phase 1)
 *   1 = fading down
 *   2 = hold low
 *   3 = fading up
 */
static int buzz_env_amp   = 256;
static int buzz_env_phase = 0;
static int buzz_env_timer = 0;

void amb_pump(void) {
    if (!AMB_ACTIVE) return;        /* title: silent, zero fill cost */
    uint8_t needs = amb_buf_needs_fill;
    if (needs == 0) return;

    int buf_idx = (needs & 1) ? 0 : 1;
    /* Snapshot shared state once per buffer: (a) the primary's mid-fill
     * writes can't produce a discontinuity within a buffer, and (b) the
     * per-sample loop stays free of ~12-cycle uncached SDRAM reads —
     * at 1024 samples per fill those reads were about to become the
     * fill's dominant cost. Walking latency doesn't suffer: the buffer
     * plays 64-128 ms after it's mixed regardless, so per-sample
     * re-reads of is_walking bought nothing. */
    int vol       = (int)SHARED_UC->amb_volume;
    int walking   = (int)SHARED_UC->is_walking;
    int step_vol  = (int)SHARED_UC->step_volume;
    int len       = (int)AMB_BUF_LEN;
    uint32_t hstep = HELLO_STEP;   /* voice-speed trim, snapshot per fill */
    /* Footstep cadence: 1.5x the read step when sprinting so the carpet
     * steps quicken to match the run (STEP_STEP_FX + half). Snapshot once
     * per fill — the state only needs to be fresh per buffer, not per sample. */
    uint32_t step_adv = ((int)SHARED_UC->is_running)
                        ? (STEP_STEP_FX + (STEP_STEP_FX >> 1)) : STEP_STEP_FX;

    /* Neon sting trigger — rare, 1/512 ≈ avg 12 s. */
    if (!neon_active && (prng_next() & 0x1FF) == 0) {
        neon_active = 1;
        neon_pos = 0;
    }

    /* Buzz envelope state machine — random fade-out events. */
    switch (buzz_env_phase) {
    case 0:  /* steady full */
        if ((prng_next() & 0x3FF) == 0) {       /* 1/1024 → avg ~24 s */
            buzz_env_phase = 1;
        }
        break;
    case 1:  /* fade down: 256 → 32 in ~88 buffers (~2 s) */
        buzz_env_amp -= 3;
        if (buzz_env_amp <= 32) {
            buzz_env_amp = 32;
            buzz_env_phase = 2;
            buzz_env_timer = 24;                /* ~0.5 s hold */
        }
        break;
    case 2:  /* hold low */
        if (--buzz_env_timer <= 0) buzz_env_phase = 3;
        break;
    case 3:  /* fade up */
        buzz_env_amp += 3;
        if (buzz_env_amp >= 256) {
            buzz_env_amp = 256;
            buzz_env_phase = 0;
        }
        break;
    }

    /* Distance-attenuated hello volume — emitted by the neanderthal.
     * Squared-distance linear fade: full inside the cell, zero at
     * sqrt(HELLO_FADE_RADIUS_SQ) cells, square-law dropoff between. */
    /* Player position is fx_t 16.16 — shift to integer cell coord. */
    int player_x_cell = (int)(SHARED_UC->player.x >> 16);
    int player_y_cell = (int)(SHARED_UC->player.y >> 16);
    int dx = player_x_cell - NEANDER_X_CELL;
    int dy = player_y_cell - NEANDER_Y_CELL;
    int dist_sq = dx * dx + dy * dy;
    int hello_amp;
    if (dist_sq >= HELLO_FADE_RADIUS_SQ) {
        hello_amp = 0;
    } else {
        hello_amp = ((HELLO_FADE_RADIUS_SQ - dist_sq) * 256)
                    / HELLO_FADE_RADIUS_SQ;     /* 0..256 */
    }

    /* Broken-tape death of the hello (SHARED_UC->hero_dying, 0 = alive). Three
     * phases warp the reused sample: A) overload speed-up (forward), B) the motor
     * gives out and it plays in REVERSE, C) a slow drift + fade to silence. Bit-
     * crush grows throughout for the lo-fi/mechanical grit. Snapshot per fill;
     * hero_dying ramps over ~2.5s so the 64 ms step is smooth. */
    int32_t hstep_eff = (int32_t)hstep;   /* signed: negative = reverse */
    int death_amp = 256, crush = 0;
    {
        int hd = (int)SHARED_UC->hero_dying;
        if (hd) {
            if (hd < 85) {                 /* A: surge 1x -> ~2.6x forward */
                hstep_eff = (int32_t)(((uint32_t)hstep * (256u + (uint32_t)hd * 5u)) >> 8);
                crush = hd >> 6;                                  /* 0..1 */
            } else if (hd < 190) {         /* B: reverse ~1.5x, warbling lo-fi */
                hstep_eff = -(int32_t)(((uint32_t)hstep * 3u) >> 1);
                crush = 1 + ((hd - 85) >> 6);                     /* 1..2 */
            } else {                       /* C: slow drift, fade to nothing */
                hstep_eff = (int32_t)(hstep >> 2);
                death_amp = 256 - (hd - 190) * 256 / 65;
                if (death_amp < 0) death_amp = 0;
                crush = 3;
            }
        }
    }

    static uint32_t buzz_pos = 0;
    for (int i = 0; i < len; i++) {
        /* Buzz with envelope. >>9 (was >>8) halves the contribution so
         * the source's fade-in/out events sit underneath neon rather
         * than overpowering it. Buzz is the atmospheric bed, not the
         * lead. */
        int buzz = (int)amb_buzz_samples[buzz_pos];
        int delta = ((buzz - SAMPLE_CENTER) * buzz_env_amp) >> 9;

        /* Neon sting at quarter amplitude (was >>1). Source peak is
         * ±633 of the ±711 budget — at >>1 it dominated the mix and
         * blew through the soft-clip headroom; at >>2 it sits around
         * ±158, in line with buzz/hello/step. */
        if (neon_active) {
            int neon = (int)amb_neon_samples[neon_pos];
            delta += (neon - SAMPLE_CENTER) >> 2;
            neon_pos++;
            if (neon_pos >= AMB_NEON_SAMPLE_COUNT) neon_active = 0;
        }

        /* Hello looping continuously, volume by distance to neanderthal.
         * int8_t samples expand to centered ~10-bit range via << 2.
         * Restored to >>8 (unity) since the buzz drop opened up enough
         * headroom — hello carries the voyager voice and audibility
         * matters more than peak budgeting for it. Worst-case overlap
         * (close + walking + neon) goes through the soft-clipper. */
        if (hello_amp > 0 && death_amp > 0) {
            uint32_t hello_idx = hello_pos_fx >> 16;
            int s = (int)amb_hello_samples[hello_idx];
            if (crush) s = (s >> crush) << crush;          /* lo-fi quantize (tape grit) */
            int hello = s << 2;
            delta += (((hello * hello_amp) >> 8) * death_amp) >> 8;
        }
        /* Signed advance so the death's phase-B plays backward; clamp at 0 on a
         * reverse underflow (settles at the sample start), wrap at the end forward. */
        if (hstep_eff >= 0) {
            hello_pos_fx += (uint32_t)hstep_eff;
            if ((hello_pos_fx >> 16) >= AMB_HELLO_SAMPLE_COUNT) hello_pos_fx = 0;
        } else {
            uint32_t dec = (uint32_t)(-hstep_eff);
            hello_pos_fx = (hello_pos_fx > dec) ? (hello_pos_fx - dec) : 0;
        }

        /* Carpet footstep — bypasses primary `vol` (amb_volume) below.
         * >>9 (was >>8) halves its contribution so it shares the
         * budget evenly with buzz/neon/hello. Source baked 11kHz/16-bit. */
        int step_delta = 0;
        if (walking) {
            uint32_t step_idx = step_pos_fx >> 16;
            int step = (int)amb_step_samples[step_idx] - SAMPLE_CENTER;
            step_delta = (step * step_vol) >> 9;
            step_pos_fx += step_adv;
            if ((step_pos_fx >> 16) >= AMB_STEP_SAMPLE_COUNT) step_pos_fx = 0;
        }

        /* Overall gain on ambient sources; footstep added post-gain. */
        int s = ((delta * vol) >> 7) + step_delta + SAMPLE_CENTER;

        /* Soft clip — piecewise linear 4:1 compression above/below
         * the SOFT_HIGH/SOFT_LOW thresholds (about 80% of the budget
         * each way), with the hard clip as a backstop for catastrophic
         * peaks. Sounds like tube saturation instead of harsh digital
         * clipping when the mix sum overshoots. */
        if (s > SOFT_HIGH) s = SOFT_HIGH + ((s - SOFT_HIGH) >> 2);
        if (s < SOFT_LOW)  s = SOFT_LOW  - ((SOFT_LOW  - s) >> 2);
        if (s > SAMPLE_MAX) s = SAMPLE_MAX;
        if (s < SAMPLE_MIN) s = SAMPLE_MIN;
        amb_pwm_buf[buf_idx][i] = (uint16_t)s;

        buzz_pos++;
        if (buzz_pos >= AMB_BUZZ_SAMPLE_COUNT) buzz_pos = 0;
    }

    /* Clear only OUR bit, against a FRESH read — not the `needs` value
     * snapshotted before the fill. The DMA IRQ can set the other
     * buffer's bit during the ~0.6 ms fill; storing the stale snapshot
     * would erase that request for a whole swap cycle. Re-reading here
     * shrinks the race window from the full fill to a few instructions. */
    amb_buf_needs_fill = (uint8_t)(amb_buf_needs_fill & ~(1 << buf_idx));
}

void amb_sound_init(void) {
    /* Default runtime audio volumes — unity gain for ambient, the
     * same half-amp baseline for footsteps that we used before.
     * Set here on the secondary at boot for robustness against whether
     * crt0 actually copies .data from ROM to SDRAM at startup. */
    SHARED_UC->amb_volume  = 128;
    SHARED_UC->step_volume = 140;   /* 25% above the 11kHz/16-bit re-bake baseline */
    AMB_ACTIVE = 0;                 /* gated silent until the game starts */

    /* Initialize the ping-pong state and prefill both buffers FULLY
     * with silence (DC center) — the whole array, not just AMB_BUF_LEN,
     * so an A/B toggle to the long buffer never drains uninitialized
     * SDRAM. Runtime knobs re-initialized here (like the volumes above)
     * for the same crt0 .data-copy robustness. */
    amb_current_buf_idx = 0;
    amb_buf_needs_fill  = 0;
    AMB_BUF_LEN   = AMB_SAMPLES_PER_BUF;
    AMB_UNDERRUNS = 0;
    HELLO_STEP    = HELLO_STEP_FX;   /* voice speed 100% until tuned */
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < AMB_SAMPLES_PER_BUF; i++)
            amb_pwm_buf[b][i] = SAMPLE_CENTER;

    Mars_InitPWM(AMB_BUZZ_SAMPLE_RATE, SAMPLE_MIN, SAMPLE_MAX);

    /* DMA destination = PWM mono register, fixed. */
    SH2_DMA_DAR1  = (uint32_t)(uintptr_t)&MARS_PWM_MONO;
    SH2_DMA_DRCR1 = 0;                /* external DREQ source = PWM */
    SH2_DMA_DMAOR = 1;                /* DMAOR.DME — enable the DMAC */

    /* SH-2 IPRA layout (SH7095): [15:12]=DIVU, [11:8]=DMAC, [7:4]=WDT,
     * [3:0]=REF. Setting DMA priority to 4 — secondary's SR mask is 2, so
     * priority 4 is high enough to be taken. */
    SH2_INT_IPRA = (SH2_INT_IPRA & 0xF0FF) | 0x0400;

    /* DMA1 uses a USER-DEFINED interrupt vector. VCR1 holds the vector
     * number; SH-2 reads VBR[VCR1*4] to get the handler. We point it
     * at vector slot 66 = "Level 4 & 5" entry in the secondary vector
     * table (mars_start.s line 203), which already holds slav_irq.
     * slav_irq's dispatch chain has the new `cmp/eq #0x10` branch to
     * slav_dma_irq for level-4 source = DMA. */
    SH2_DMA_VCR1 = 66;

    /* Fire the first transfer. Both ping-pong buffers start at silence
     * (DC center), so the first ~128 ms is silent until the pump gets
     * going — inaudible at boot; the game gates audio on amb_set_active
     * anyway. After that, completions are handled by the IRQ →
     * amb_dma_handler swap chain. */
    SH2_DMA_SAR1  = (uint32_t)(uintptr_t)amb_pwm_buf[0];
    SH2_DMA_TCR1  = AMB_BUF_LEN;
    SH2_DMA_CHCR1 = 0x14E5;
}
