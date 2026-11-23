# Zaxxon and Super Zaxxon, without audio samples

A small standalone emulator of Sega's **Super Zaxxon** and **Zaxxon** (1982), built from MAME's `zaxxon` driver, with one
difference: **there are no recorded sound samples**. The sound boards are entirely discrete analog (a noise chip, op-amp
filters and oscillators, one-shots, 555 timers, analog switches and the MB4391 voltage-controlled attenuators), and this
emulator simulates those circuits part by part, at 192 kHz, using the component values on the schematics:
*IC Board A* (Super Zaxxon drawing 800-3300, sheets 3 and 4) and Super Zaxxon's extra sound board (834-5106, sheets 2 and 3).

It is C++17 + SDL3, a handful of files, no other dependencies. **No ROMs are included.**

## ROMs

Use your own MAME `szaxxon.zip` (Super Zaxxon) and/or `zaxxon.zip` (Zaxxon, set 1). The files are matched by CRC32, so file names
do not matter, and the game is detected automatically.

* put the zips (or the unzipped files) in a folder called `roms` next to the program, **or**
* give the zip / folder on the command line, **or**
* drop the zip / folder on the window, or press **O** (pick a zip) / **F** (pick a folder).

The folder is remembered, so you only browse once. When both games are found, a screen asks which one to play (Up/Down,
Enter), and **Play Zaxxon / Play Super Zaxxon** at the top of the menu switches between them. `--game zaxxon` or
`--game szaxxon` starts one directly.

## Build

```
./build.sh
```

Linux: install your distro's SDL3 development package. Windows: MSYS2 UCRT64, one time:
`pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-pkgconf`.
GitHub Actions builds the Windows 11 x64 zip (`zaxxon-windows.zip`: the program plus the DLLs it needs) on every push and pull
request; it is on the run's page under *Artifacts*. Pushing a tag like `v0.2` also publishes it as a release.

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
scaling (fill the window or integer multiples only), rotate, volume, sound chip (MB4391 or MC3340 curve, see below), engine sound
(Board or MAME-style, see below), fullscreen, reset, load ROM zip, quit. Settings are saved.
Lives and bonus take effect on the next game.

The picture is always scaled with nearest-neighbour filtering, never smoothed.

Options: `--scale N`, `--no-rotate`, `--volume X`, `--lives 3|4|5`, `--free-play`, `--hard`, `--bonus 10000|20000|30000|40000`,
`--cheat lives,fuel,invincible` (or `all`), `--integer`, `--mc3340`, `--engine board|mame`.
`zaxxon --help` lists them all, including the headless ones (`--headless --frames N --wav out.wav --shot out.bmp --coin-at N --start-at N --autoplay`).

## Listening to the sounds on their own

These need no ROMs:

* `zaxxon --sound-test` – a window where keys trigger every sound: Q cannon, W M-Exp, E S-Exp, R shot, T base missile,
  Y/U alarms, 1 battleship, 2 laser, 3 homing missile, 4 player ship on/off, 5 ship A/B bits, 6 ship C bit,
  **B** switches between the Zaxxon board and Super Zaxxon's (with the extra sound board), **M** switches the MB4391 / MC3340 curve,
  **N** switches the engine sound between Board and MAME-style.
* `zaxxon --sound-wavs folder` – one WAV per sound, named like the MAME sample files (`00.wav` battleship, `01` laser, `02` base missile,
  `03` homing missile, `04`/`05` player ship, `08` cannon, `10` M-Exp, `11` S-Exp, `20` alarm 3, `21` alarm 2, `23` shot), every
  player-ship setting the games use (`ship_f0` … `ship_f7`), Super Zaxxon's engine (`sz_ship_f4` … `sz_ship_f7`) and its boss sound
  (`sz_00`), the MAME-style engine (`mame_ship_f0`, `f3`, `f4`, `f7`), plus `sounds.txt`.
* `zaxxon --sound-demo demo.wav` – every sound one after another in one WAV file.

Add `--mc3340` to any of them to hear the repair part's curve instead, or `--engine mame` (`--sound-demo`) for the MAME-style engine.

## What is emulated

* **CPU board**: Z80 at 3.04125 MHz with the Sega 315-5013 opcode decryption (separate opcode and data views), memory map,
  the two 74LS259 latches, the 8255, coin / start / joystick inputs, DIP switches, vblank interrupt, service NMI.
* **Video**: skewed 256x4096 background with scrolling, 32 sprites, 32x32 character layer, PROM palette (MAME's resistor-network
  weighting), picture rotated 90 degrees by default.
* **Sound**: see below. The 8255 writes are applied at their exact cycle position inside each frame.

## How the sound works

`zaxxon_sound.h` is a circuit simulation, not a set of synthesizer patches. Each block on the drawings is a small model of the real
parts, all running at 192 kHz and filtered down to 48 kHz:

* **MM5837 noise chip**: its 17-bit shift register, clocked at 103 kHz (MAME's measurements at 12 V), output 3.0 / 10.5 V.
* **LM324 op-amps**: output limits on the 12 V supply, 0.5 V/µs slew rate, and the fact that an LM324 cannot pull its output
  below about one diode drop once it has to sink more than its 50 µA internal current. That detail matters: it is what keeps the
  shot sound going for its full length, and it sets the battleship's pitch.
* **Op-amp oscillators** (integrator, Schmitt trigger, switching transistor) and **555 / 556 timers**: solved event by event, so
  every threshold crossing and ramp is exact. The Schmitt trigger's LM324 sits saturated at a rail and needs about 40 µs to come
  out of saturation before it switches; that delay is what keeps the shot's chirps and the top of the laser sweep from going too high.
* **74123 one-shots**, **diode/RC envelope networks**, **Sallen-Key, multiple-feedback and bridged-T filters** (exact transfer
  functions from the part values), **4016 analog switches** with their coupling capacitors (the clicks at sound start are real),
  the **CdS photocell** that tunes the player-ship filter, the transistor that sweeps the cannon's filter, and the
  **output mixer** (R174 … R209 into the U11 summing amplifier, which can clip like the real one).

### Engine sound: Board or MAME-style

On the real board the player ship's engine is the noise chip through a band-pass filter that a photocell tunes (a two-bit DAC
on port A bits 0–1 moves it from about 630 Hz down to 334 Hz) and then one of two filters (bit 2). It is smooth noise that never repeats. The menu's
**Engine sound** switch (or `--engine mame`) offers a second sound, **MAME-style**, for players who know MAME's engine: MAME plays two
short recordings (0.64 s and 0.75 s) looped and layered while bit 2 / bit 3 are low, at a volume that bit 0–1 set, and both
recordings were boosted until about 9–18 % of their samples clipped. The loops repeating against each other are the pulsing, and the
clipping is the roughness. MAME's samples are also about 9 dB louder against the other sounds than the board's engine.

MAME-style does the same with **no recordings**: it cuts two loops of the same lengths from this emulator's own ship circuit, at the
two settings the real recordings match (port A = f0 and f3), saturates them as hard as the recordings, and plays them with MAME's
rules and balance. Up to 3 kHz its spectrum is within about 3 dB of MAME's, including the third harmonic that makes it rough. It
leaves out the static: the saturation is smooth instead of a hard clip (hard clipping crackles whenever the noise peaks), nothing
above 3 kHz is kept, the loop points are crossfaded, and starting, stopping and volume changes glide over 2 ms instead of
clicking. On Super Zaxxon it replaces the extra board's engine, as MAME does. Board is the default.

### The MB4391

The six "4391/2" boxes on the drawing are halves of the **Fujitsu MB4391**, a dual voltage-controlled attenuator that Sega never
documented. Each half has the same IN / CON / OUT / RO pins as one Motorola **MC3340**, which is why repairers replace a dead
MB4391 with two MC3340s. The model is the MC3340's own circuit: an input stage that clips about 0.44 V below its bias point,
+13 dB at full volume, and an attenuation of `20·log10(1 + exp((Vcon − V0) / Vs))` dB as the control voltage rises.

The MC3340 data sheet gives V0 = 3.28 V, Vs = 0.157 V at 12 V. To find the MB4391's own values, the fade-outs of four recordings of a
real board were fitted (small explosion, medium explosion, shot, cannon: in each one the control voltage comes from a plain RC
network, so the fade-out shape *is* the attenuator's curve). The best fit is **V0 = 3.435 V, Vs = 0.230 V**, about 2 dB rms from the
recordings; the MC3340 curve is 6.5 to 9.4 dB off on the same fade-outs, because its sounds die away too early. So the MB4391 has a
gentler curve than its replacement part, and that is the default. The MC3340 curve is one menu switch (or `--mc3340`) away, which is
roughly what a board repaired with MC3340s sounds like.

### Super Zaxxon's extra sound board

Super Zaxxon is a Zaxxon board plus the 834-5106 board, which plugs into the 8255's socket. It takes over port A bits 0–3 and has its
own player-ship engine (a 555-driven op-amp VCO through a rumble low-pass and a whine high-pass, with two MB4391 halves; port A bit 2
switches the whine on) and a three-oscillator sound that joins the battleship output. Its two outputs go back to the main board
through the 8255's port B pins. MAME and earlier attempts leave this board out; here it is emulated, and it is selected
automatically when Super Zaxxon's ROMs are loaded.

### Checked against recordings of a real board

The MAME sample set for Zaxxon is a set of recordings of a real board. They were used only to *measure* against (they are not part of
this repository and are never played):

| Sound | This emulator | Real board recording |
|---|---|---|
| Battleship (boss) | 132 Hz square, wobbling 129–135 Hz at 2.9 Hz | 132 Hz, 129–135 Hz |
| Alarm 2 / alarm 3 | 1130 Hz / 2270 Hz | 1132 Hz / 2265 Hz |
| Shot (also the coin sound) | a chirp every 30–40 ms peaking at 2.1–2.8 kHz, falling to about 300 Hz | same |
| Cannon (your ship's fire) | noise through a filter sweeping 3.7 → 1.85 kHz | same sweep |
| Small / medium explosion | low-passed noise, fade within about 2 dB | same spectrum |
| Player ship (port A = f0) | noise band peaking at 630 Hz | 627 Hz (`05.wav`) |
| Player ship (port A = f3) | noise band peaking at 334 Hz, centroid 366 Hz | 331 Hz, centroid 366 Hz (`04.wav`) |
| Laser (force field) | 275–504 Hz sweep, 0.19 s period | 273–504 Hz, 0.19 s |
| Homing missile | 900–1125 Hz, median 1012 Hz, 15 Hz wobble | 950–1125 Hz, median 1019 Hz, 14 Hz |

The recording named `02.wav` (base missile) has the same spectrum as `04.wav`, the player ship at port A = f3; the emulator follows
the drawing (filtered noise) instead. `05.wav` also has strong energy around 1.6–2 kHz that the board does not make: about 16 % of
its samples are clipped at full scale, and clipping adds those harmonics.

### What was calibrated, and what is assumed

These were set from the recordings because the drawings and data sheets do not fix them; they are marked `[CALIBRATED]` in
the source: the MB4391 curve (above), the LM324's output floor when sinking current (0.648 V at 1 mA, which sets the battleship
pitch), the LM324 Schmitt triggers' 40 µs saturation delay (fitted to the laser; the shot then matched without further fitting),
the 556 comparator delay of the alarm clock (1.15 µs), the CdS cell's resistance and light curve (1.55 kΩ at 19.3 mA, R ∝ I^-0.8; fitted to the
engine's pitch at port A = f0 and f3),
the homing missile's C45 at 9.1 nF (drawn 10 nF, so inside its tolerance; 10 nF plays about 10 % low), and the shot's electrolytic
C89 at 0.78 µF (drawn 0.68 µF; sets how fast the shot fades, which then matches the recording within 0.6 dB). The cannon transistor uses textbook Ebers-Moll values (reverse beta 3).

Marked `[ASSUMPTION]`: the outputs of U14 and U15 both reach R177 (that trace is unreadable on the scan), Super Zaxxon's SOUND OUT B
has a 200 Ω source impedance, the photocell's speed (8 ms to brighten, 40 ms to darken) and LED drop, and a 0.3 µs comparator delay
for the other 555s.

## Credits and licences

Hardware behaviour follows **MAME** (BSD-3-Clause) – see `THIRD-PARTY-NOTICES.txt`, which also covers the inflate code (Mark Adler's puff)
and SDL3 (zlib licence). The sound is simulated from the Sega schematics and the Motorola MC3340 data sheet.
Zaxxon and Super Zaxxon are © Sega; this project contains no game data and no recorded sounds.
