## Audio

- The fluorescent hum is now synthesized in real time on the YM2612 FM chip. The 875KB looped hum recording has been removed from the ROM.
- Hum volume follows the ambience slider.
- The neon chime and ambient bed volumes are increased.
- Footstep and neon samples requantized from 16-bit to 8-bit. No change to trim or timing.
- The Voyager broadcast is unchanged.
- Known: the synthesized hum can exhibit brief amplitude dips. Under investigation.

## Monitors

- Monitor power-on and power-off sounds replaced with dedicated recordings: degauss on power-on, relay click on power-off. Levels raised.
- Power-on now displays a raster strike: a white line, then the picture opens vertically. Duration 3 frames.
- Power-off displays the inverse: the picture collapses to a dim line that falls off the bottom of the glass. Duration 2 frames.

## Master System minigame

- Generative music (SPACE-A, phrase and echo engine) plays on the Z80 during minigame sessions.
- The 32X audio mix ducks while the minigame window is active; the PSG owns playback.
- SMS games can now be compiled from C source (devkitSMS toolchain).

## Size

- ROM: 2,342,912 bytes to 1,400,832 bytes.
