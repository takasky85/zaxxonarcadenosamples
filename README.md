# Super Zaxxon, without audio samples

A small standalone emulator of Sega's **Super Zaxxon** (1982), built from MAME's `zaxxon` driver, with one
difference: **there are no recorded sound samples**. The original sound board is entirely discrete analog
(noise source, op-amp filters, one-shots, 555 timers, voltage-controlled attenuators). Each sound is calculated
from a model of its circuit, using the component values from the *IC Board A - Super ZAXXON* schematics.

It is C++17 + SDL3, a handful of files, no other dependencies. **No ROMs are included.**

## ROMs

Use your own MAME `szaxxon.zip` (the files are matched by CRC32, so file names do not matter).

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
| Esc | quit |

A game controller works too (D-pad/stick, A/B/X fire, Start = 1P, Right bumper = 2P, Back = coin).

Options: `--scale N`, `--no-rotate`, `--volume X`, `--lives 3|4|5`, `--free-play`, `--bonus 10000|20000|30000|40000`.
`zaxxon --help` lists them all, including the headless ones (`--headless --frames N --wav out.wav --shot out.bmp --coin-at N --start-at N`).

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
**74123** one-shot widths use `0.28·R·C·(1+0.7/R[kΩ])`; the **MB4391** voltage-controlled attenuators use the curve
`gain = clamp((4.76 − Vcon)/1.92, 0, 1)²` from MAME's `nl_brdrline` model; the Sallen-Key filters are real biquads from the
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
