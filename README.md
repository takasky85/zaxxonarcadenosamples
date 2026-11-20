# Zaxxon and Super Zaxxon, without audio samples

A small standalone emulator of Sega's **Super Zaxxon** and **Zaxxon** (1982), built from MAME's `zaxxon` driver, with one
difference: **there are no recorded sound samples**. The original sound board is entirely discrete analog
(noise source, op-amp filters, one-shots, 555 timers, voltage-controlled attenuators). Each sound is calculated
from a model of its circuit, using the component values from the *IC Board A* schematics (Super Zaxxon drawing 800-3300 and Zaxxon drawing 834-0214; the sound board is the same in both, so one synth serves both games).

It is C++17 + SDL3, a handful of files, no other dependencies. **No ROMs are included.**

## ROMs

Use your own MAME `szaxxon.zip` (Super Zaxxon) or `zaxxon.zip` (Zaxxon, set 1). The files are matched by CRC32, so file names do not matter, and the game is detected automatically.

* put the zip (or the unzipped files) in a folder called `roms` next to the program, **or**
* give the zip / folder on the command line, **or**
* drop the zip / folder on the window, or press **O** (pick a zip) / **F** (pick a folder).

## Build

```
./build.sh
```

Linux: install your distro's SDL3 development package. Windows: MSYS2 UCRT64, one time:
`pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-pkgconf`.
Pushing a tag like `v0.1` builds a Windows zip through GitHub Actions.

## Controls

| | |
|---|---|
| arrows | move the ship (up/down = altitude) |
| Z / Ctrl / Space | fire |
| 5 (or 6) | coin |
| 1 / 2 | start |
| F2 | service / test mode (NMI) |
| P · R · F3 | pause · rotate · reset |
| F11 · F12 | fullscreen · screenshot |
| + / - | volume |
| O · F | browse for the ROM zip / folder |
| Esc / Tab | menu (cheats, DIP switches, scaling, volume, ...) |

A game controller works too (D-pad/stick, A/B/X fire, Start = 1P, Right bumper = 2P, Back = coin).

## Menu and cheats

Esc or Tab opens a menu (on a controller: Y or Guide; D-pad moves, A selects, B closes): **infinite lives**, **infinite fuel**,
**invincible** (the three cheats from the MAME cheat files, applied every frame), lives, bonus ship, free play, Super Zaxxon difficulty,
scaling (fill the window or integer multiples only), rotate, volume, fullscreen, reset, load ROM zip, quit. Settings are saved.
Lives and bonus take effect on the next game.

The picture is always scaled with nearest-neighbour filtering, never smoothed.

Options: `--scale N`, `--no-rotate`, `--volume X`, `--lives 3|4|5`, `--free-play`, `--hard`, `--bonus 10000|20000|30000|40000`,
`--cheat lives,fuel,invincible` (or `all`), `--integer`.
`zaxxon --help` lists them all, including the headless ones (`--headless --frames N --wav out.wav --shot out.bmp --coin-at N --start-at N --autoplay`).

## Comparing the sounds with the real thing

Two tools need no ROMs:

* `zaxxon --sound-test` – a window where keys trigger every sound (Q cannon, W M-Exp, E S-Exp, R shot,
  T base missile, Y/U alarms, 1 battleship, 2 laser, 3 homing missile, 4 player ship on/off, 5 ship A/B bits).
* `zaxxon --sound-demo demo.wav` – every sound one after another in one WAV file, for A/B listening against the MAME samples.

## What is emulated

* **CPU board**: Z80 at 3.04125 MHz with the Sega 315-5013 opcode decryption (separate opcode and data views), memory map,
  the two 74LS259 latches, the 8255, coin / start / joystick inputs, DIP switches, vblank interrupt, service NMI.
* **Video**: skewed 256x4096 background with scrolling, 32 sprites, 32x32 character layer, PROM palette (MAME's resistor-network
  weighting), picture rotated 90 degrees by default.
* **Sound board**: see below. The 8255 commands the game writes are applied at their exact cycle position inside each frame.

## Sound: what is modelled and how sure it is

Common parts: the **MM5837** noise source is a 17-bit LFSR clocked at about 103 kHz (as in MAME's `mm5837`);
**74123** one-shot widths use `0.28·R·C·(1+0.7/R[kΩ])`; the **MB4391** voltage-controlled attenuators are modelled as a Motorola **MC3340** at a 12 V supply (assumption: a forum report says two MC3340s
replace one MB4391, and the 680 pF on the MB4391's RO pin matches the MC3340's roll-off capacitor): +13 dB at full volume, 0 dB attenuation below
about 3.0 V of control voltage, then roughly 45 dB per volt (curve read from the MC3340 datasheet, Figure 3). MAME's own MB4391 model is a
self-described guess; the Sallen-Key filters are real biquads from the
schematic R and C values. It is a per-sample behavioural model, not a SPICE solver.

| Sound (8255 bit) | Model | Confidence |
|---|---|---|
| S-EXP (B4), M-EXP (B5) | noise → Sallen-Key low-pass (≈320 Hz / ≈225 Hz, Q 2) → MB4391 driven by a 74123 + diode/RC envelope | good – component values clear |
| Base missile (A5) | noise → low-pass (≈480 Hz) → MB4391 with a slow envelope | good |
| Cannon (B7) | noise through an op-amp whose feedback capacitor is shunted by a transistor driven from a peak-detector envelope | medium – the exact VCA path is partly guessed |
| Shot (C0) | relaxation VCO, control sweeps down after the trigger, gated by a one-shot/RC envelope | medium – sweep range assumed |
| Alarm 2 / 3 (C2, C3) | 556 oscillator (≈20 kHz) divided by the 74393, wired-NAND mixed and clipped | medium – which divider tap feeds which alarm is assumed |
| Battleship (A7) | two coupled op-amp relaxation oscillators (slow one sweeps the fast one) | medium – the junction coupling the two is assumed |
| Laser (A6) | 555 LFO sweeping an op-amp relaxation VCO | medium |
| Homing missile (A4) | 555 whose control voltage is driven by a Schmitt trigger / noise network | medium-low |
| Player ship (A–D) | noise → resonant band-pass whose resonance is set by a photocell (PC1) lit by an LED driven from the A/B bits; the C/D bits open two MB4391 envelopes | medium-low – the A/B to pitch mapping and the C/D envelopes are approximate |
| Not modelled | the 834-5106 daughterboard (schematic pages 14-15): the game never drives port B bits 0-3 | – |

The relative loudness of the channels comes from the summing resistors on the schematic. Because my reading of the drawing is not
perfect, `ZaxSynth::trim[]` in `zaxxon_synth.h` holds one ear-calibrated multiplier per sound (1.0 = as derived), and
`MASTER` sets the overall level. Every place where the drawing was ambiguous is marked `[ASSUMPTION]` in the source.
Tell me which sounds differ from the MAME samples and I will go back to the corresponding circuit.

## Credits and licences

Hardware behaviour follows **MAME** (BSD-3-Clause) – see `THIRD-PARTY-NOTICES.txt`, which also covers the inflate code (Mark Adler's puff)
and SDL3 (zlib licence). Super Zaxxon is © Sega; this project contains no game data.
