/*
 * Sega Zaxxon hardware (Super Zaxxon, 315-5013 CPU) - machine, video and input.
 *
 * Memory map, video rendering, palette and input behaviour follow MAME's zaxxon driver
 * (src/mame/sega/zaxxon.cpp, zaxxon_v.cpp; Nicola Salmoria, Stephane Humbert et al., BSD-3-Clause)
 * and the Sega 315-5013 decryption table from MAME's segacrpt_device.cpp.
 * This is a re-implementation, not a copy.  See THIRD-PARTY-NOTICES.txt.
 *
 * No SDL in here, so the core can also be built and tested headless.
 */
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "z80.h"
#include "romzip.h"
#include "zaxxon_sound.h"

namespace zx {

static constexpr double MAIN_HZ   = 48660000.0 / 16;      /* 3.04125 MHz */
static constexpr int    FRAME_CYC = 384 * 264 / 2;        /* 50688 CPU cycles per frame (60.0 Hz) */
static constexpr int    VBLANK_AT = 240 * 192;            /* vblank begins on scanline 240 (192 cycles per line) */
static constexpr int    RATE      = 48000;
static constexpr int    FRAME_SMP = RATE / 60;            /* 800 audio samples per frame */
static constexpr int    SCR_W = 256, SCR_H = 224;         /* visible area: scanlines 16..239 */

/* ===================================================================== */
/*  ROM set description (MAME "szaxxon")                                 */
/* ===================================================================== */
struct RomSpec { const char *name; uint32_t crc; uint32_t size; int region; uint32_t offset; };
enum { RG_MAIN, RG_TX, RG_BG, RG_SPR, RG_MAP, RG_PROM, RG_COUNT };
static const uint32_t REGION_SIZE[RG_COUNT] = { 0x6000, 0x1000, 0x6000, 0x6000, 0x8000, 0x0200 };

static const RomSpec SZAXXON_ROMS[] = {
	{ "1804e.u27",   0xaf7221da, 0x2000, RG_MAIN, 0x0000 },
	{ "1803e.u28",   0x1b90fb2a, 0x2000, RG_MAIN, 0x2000 },
	{ "1802e.u29",   0x07258b4a, 0x1000, RG_MAIN, 0x4000 },
	{ "1815b.u68",   0xbccf560c, 0x0800, RG_TX,   0x0000 },
	{ "1816b.u69",   0xd28c628b, 0x0800, RG_TX,   0x0800 },
	{ "1807b.u113",  0xf51af375, 0x2000, RG_BG,   0x0000 },
	{ "1806b.u112",  0xa7de021d, 0x2000, RG_BG,   0x2000 },
	{ "1805b.u111",  0x5bfb3b04, 0x2000, RG_BG,   0x4000 },
	{ "1812e.u77",   0x1503ae41, 0x2000, RG_SPR,  0x0000 },
	{ "1813e.u78",   0x3b53d83f, 0x2000, RG_SPR,  0x2000 },
	{ "1814e.u79",   0x581e8793, 0x2000, RG_SPR,  0x4000 },
	{ "1809b.u91",   0xdd1b52df, 0x2000, RG_MAP,  0x0000 },
	{ "1808b.u90",   0xb5bc07f0, 0x2000, RG_MAP,  0x2000 },
	{ "1811b.u93",   0x68e84174, 0x2000, RG_MAP,  0x4000 },
	{ "1810b.u92",   0xa509994b, 0x2000, RG_MAP,  0x6000 },
	{ "pr-5168.u98", 0x15727a9f, 0x0100, RG_PROM, 0x0000 },   /* palette */
	{ "pr-5167.u72", 0xdeaa21f7, 0x0100, RG_PROM, 0x0100 },   /* character colour codes */
};
static constexpr int N_SZAXXON_ROMS = sizeof(SZAXXON_ROMS) / sizeof(SZAXXON_ROMS[0]);

/* MAME "zaxxon" (set 1): the same board without the 315-5013 CPU, so the program ROMs are plain */
static const RomSpec ZAXXON_ROMS[] = {
	{ "zaxxon_rom3d.u27", 0x6e2b4a30, 0x2000, RG_MAIN, 0x0000 },
	{ "zaxxon_rom2d.u28", 0x1c9ea398, 0x2000, RG_MAIN, 0x2000 },
	{ "zaxxon_rom1d.u29", 0x1c123ef9, 0x1000, RG_MAIN, 0x4000 },
	{ "zaxxon_rom14.u68", 0x07bf8c52, 0x0800, RG_TX,   0x0000 },
	{ "zaxxon_rom15.u69", 0xc215edcb, 0x0800, RG_TX,   0x0800 },
	{ "zaxxon_rom6.u113", 0x6e07bb68, 0x2000, RG_BG,   0x0000 },
	{ "zaxxon_rom5.u112", 0x0a5bce6a, 0x2000, RG_BG,   0x2000 },
	{ "zaxxon_rom4.u111", 0xa5bf1465, 0x2000, RG_BG,   0x4000 },
	{ "zaxxon_rom11.u77", 0xeaf0dd4b, 0x2000, RG_SPR,  0x0000 },
	{ "zaxxon_rom12.u78", 0x1c5369c7, 0x2000, RG_SPR,  0x2000 },
	{ "zaxxon_rom13.u79", 0xab4e8a9a, 0x2000, RG_SPR,  0x4000 },
	{ "zaxxon_rom8.u91",  0x28d65063, 0x2000, RG_MAP,  0x0000 },
	{ "zaxxon_rom7.u90",  0x6284c200, 0x2000, RG_MAP,  0x2000 },
	{ "zaxxon_rom10.u93", 0xa95e61fd, 0x2000, RG_MAP,  0x4000 },
	{ "zaxxon_rom9.u92",  0x7e42691f, 0x2000, RG_MAP,  0x6000 },
	{ "mro16.u76",        0x6cc6695b, 0x0100, RG_PROM, 0x0000 },
	{ "zaxxon.u72",       0xdeaa21f7, 0x0100, RG_PROM, 0x0100 },
};
static constexpr int N_ZAXXON_ROMS = sizeof(ZAXXON_ROMS) / sizeof(ZAXXON_ROMS[0]);

enum { SET_SZAXXON = 0, SET_ZAXXON = 1 };
static const char *const SET_NAMES[] = { "Super Zaxxon", "Zaxxon" };

struct RomSet { std::vector<uint8_t> r[RG_COUNT]; int set = SET_SZAXXON; };

/* match every ROM by CRC32 (file names do not matter) against both sets; returns false and lists what is
   missing from the set that came closest */
/* want: SET_SZAXXON or SET_ZAXXON to load only that game, -1 for the first complete set (Super Zaxxon first) */
static bool load_roms(const std::vector<rz::RomFile> &files, RomSet &rs, std::string &missing, int want = -1)
{
	struct Cand { const RomSpec *specs; int n; int set; };
	const Cand cands[2] = { { SZAXXON_ROMS, N_SZAXXON_ROMS, SET_SZAXXON }, { ZAXXON_ROMS, N_ZAXXON_ROMS, SET_ZAXXON } };
	int best_hits = -1; std::string best_missing;
	for (const Cand &c : cands) {
		if (want >= 0 && c.set != want) continue;
		RomSet t; for (int g = 0; g < RG_COUNT; g++) t.r[g].assign(REGION_SIZE[g], 0);
		t.set = c.set;
		int hits = 0; std::string miss;
		for (int i = 0; i < c.n; i++) {
			const RomSpec &sp = c.specs[i];
			const rz::RomFile *hit = nullptr;
			for (auto &f : files) if (f.crc == sp.crc && f.data.size() == sp.size) { hit = &f; break; }
			if (!hit) { miss += std::string("  ") + sp.name + "\n"; continue; }
			memcpy(t.r[sp.region].data() + sp.offset, hit->data.data(), sp.size);
			hits++;
		}
		if (hits == c.n) { rs = t; return true; }
		if (hits > best_hits) { best_hits = hits; best_missing = std::string("(closest match: ") + SET_NAMES[c.set] + ")\n" + miss; }
	}
	missing += best_missing;
	return false;
}

/* ===================================================================== */
/*  Sega 315-5013 decryption (conversion table from MAME)                */
/* ===================================================================== */
static void decrypt_315_5013(const uint8_t *src, uint8_t *data, uint8_t *opcodes, int size)
{
	static const uint8_t convtable[32][4] = {
		/*       opcode                   data                     address      */
		{ 0x88,0xa8,0x80,0xa0 }, { 0x28,0x20,0xa8,0xa0 },
		{ 0x08,0x28,0x88,0xa8 }, { 0x88,0x80,0x08,0x00 },
		{ 0xa8,0x28,0xa0,0x20 }, { 0x20,0xa0,0x00,0x80 },
		{ 0x88,0xa8,0x80,0xa0 }, { 0x28,0x20,0xa8,0xa0 },
		{ 0x08,0x28,0x88,0xa8 }, { 0x88,0x80,0x08,0x00 },
		{ 0x88,0xa8,0x80,0xa0 }, { 0x28,0x20,0xa8,0xa0 },
		{ 0xa8,0x28,0xa0,0x20 }, { 0x20,0xa0,0x00,0x80 },
		{ 0x08,0x28,0x88,0xa8 }, { 0x88,0x80,0x08,0x00 },
		{ 0x08,0x28,0x88,0xa8 }, { 0x88,0x80,0x08,0x00 },
		{ 0x88,0xa8,0x80,0xa0 }, { 0x28,0x20,0xa8,0xa0 },
		{ 0x88,0xa8,0x80,0xa0 }, { 0x28,0x20,0xa8,0xa0 },
		{ 0xa8,0x28,0xa0,0x20 }, { 0x20,0xa0,0x00,0x80 },
		{ 0xa8,0x28,0xa0,0x20 }, { 0x20,0xa0,0x00,0x80 },
		{ 0xa8,0x28,0xa0,0x20 }, { 0x20,0xa0,0x00,0x80 },
		{ 0x08,0x28,0x88,0xa8 }, { 0x88,0x80,0x08,0x00 },
		{ 0x88,0xa8,0x80,0xa0 }, { 0x28,0x20,0xa8,0xa0 }
	};
	for (int a = 0; a < size; a++) {
		int xorval = 0;
		uint8_t s = src[a];
		int row = (a & 1) + (((a >> 4) & 1) << 1) + (((a >> 8) & 1) << 2) + (((a >> 12) & 1) << 3);
		int col = ((s >> 3) & 1) + (((s >> 5) & 1) << 1);
		if (s & 0x80) { col = 3 - col; xorval = 0xa8; }
		opcodes[a] = (s & ~0xa8) | (convtable[2 * row][col] ^ xorval);
		data[a]    = (s & ~0xa8) | (convtable[2 * row + 1][col] ^ xorval);
	}
}

/* ===================================================================== */
/*  The machine                                                          */
/* ===================================================================== */
struct Machine {
	Z80 cpu;
	uint8_t rom[0x6000], opc[0x6000];     /* data view / opcode (M1) view of the decrypted program */
	uint8_t ram[0x1000];                  /* 6000-6fff */
	uint8_t vram[0x400];                  /* 8000-83ff (mirrored to 9fff) */
	uint8_t spr[0x100];                   /* a000-a0ff (mirrored to bfff) */
	uint8_t latch1 = 0, latch2 = 0;       /* the two 74LS259 addressable latches (U55, U56) */
	bool    int_en = false, irq_line = false, flip = false;
	uint8_t coin_status[3] = {0, 0, 0};
	bool    bg_enable = false;
	int     bg_color = 0, fg_color = 0;   /* 0 or 0x80 */
	int     bg_pos = 0;                   /* 11 bits */
	uint8_t sw00 = 0, sw01 = 0, sw100 = 0;/* live input state (active high) */
	uint8_t dsw02 = 0x77, dsw03 = 0x33;   /* DIP switches (MAME defaults: 3 lives, bonus 10000, 1C/1C) */
	uint8_t coin_counter[2] = {0, 0};
	int     set_id = SET_SZAXXON;
	/* cheats (the MAME cheat-file addresses): RAM bytes forced once per frame */
	bool    cheat_lives = false, cheat_fuel = false, cheat_invincible = false;
	void apply_cheats()
	{
		/* Infinite lives 03, infinite fuel 7F, invincibility 00 (addresses differ by one between the sets) */
		if (cheat_lives)      ram[(set_id == SET_SZAXXON ? 0x6066 : 0x6067) - 0x6000] = 0x03;
		if (cheat_fuel)       ram[(set_id == SET_SZAXXON ? 0x62b2 : 0x62b1) - 0x6000] = 0x7f;
		if (cheat_invincible) ram[0x6407 - 0x6000] = 0x00;
	}

	/* 8255: mode 0, all outputs */
	uint8_t ppi_out[3] = {0, 0, 0};

	/* decoded graphics and palette */
	uint8_t  txpix[256 * 64];             /* 256 chars, 8x8, 2bpp */
	uint8_t  bgpix[1024 * 64];            /* 1024 tiles, 8x8, 3bpp */
	uint8_t  sprpix[64 * 32 * 32];        /* 64 sprites, 32x32, 3bpp */
	const uint8_t *map_dat = nullptr;     /* tilemap_dat (0x8000) */
	uint8_t  map_store[0x8000];
	uint8_t  prom_pal[256], prom_col[256];
	uint32_t pal[256];
	uint32_t fb[SCR_W * SCR_H];           /* ARGB, scanlines 16..239 */
	std::vector<uint8_t> bgmap;           /* 256 x 4096 background pixmap (pen indices 0..127) */

	/* audio */
	zsnd::ZaxSound synth;                 /* the sound board, simulated at the circuit level (zaxxon_sound.h) */
	struct SndEv { int cyc; int port; uint8_t v; };
	std::vector<SndEv> sndev;
	std::vector<int16_t> audio;           /* FRAME_SMP samples per frame */
	float volume = 1.0f;
	long frames = 0;
	int cyc_in_frame = 0;

	/* ------------------------------------------------------------- setup */
	static uint8_t rd_cb(void *ud, uint16_t a) { return ((Machine *)ud)->mread(a); }
	static uint8_t rdop_cb(void *ud, uint16_t a) { Machine *m = (Machine *)ud; return a < 0x6000 ? m->opc[a] : m->mread(a); }
	static void    wr_cb(void *ud, uint16_t a, uint8_t v) { ((Machine *)ud)->mwrite(a, v); }
	static uint8_t in_cb(void *, uint16_t) { return 0xff; }
	static void    out_cb(void *, uint16_t, uint8_t) {}

	void load(const RomSet &rs)
	{
		set_id = rs.set;
		if (set_id == SET_SZAXXON) decrypt_315_5013(rs.r[RG_MAIN].data(), rom, opc, 0x6000);
		else { memcpy(rom, rs.r[RG_MAIN].data(), 0x6000); memcpy(opc, rom, 0x6000); }
		memcpy(map_store, rs.r[RG_MAP].data(), 0x8000); map_dat = map_store;
		memcpy(prom_pal, rs.r[RG_PROM].data(), 256);
		memcpy(prom_col, rs.r[RG_PROM].data() + 256, 256);
		build_palette();
		/* MAME gfx_8x8x2_planar / gfx_8x8x3_planar / zaxxon_spritelayout: the FIRST plane listed is the
		   most significant bit and sits in the LAST part of the region; bit 7 of a byte is the leftmost pixel */
		const uint8_t *tx = rs.r[RG_TX].data();
		for (int c = 0; c < 256; c++) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
			int b = 7 - x;
			txpix[c * 64 + y * 8 + x] = (((tx[0x800 + c * 8 + y] >> b) & 1) << 1) | ((tx[c * 8 + y] >> b) & 1);
		}
		const uint8_t *bg = rs.r[RG_BG].data();
		for (int c = 0; c < 1024; c++) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
			int b = 7 - x;
			bgpix[c * 64 + y * 8 + x] = (((bg[0x4000 + c * 8 + y] >> b) & 1) << 2) | (((bg[0x2000 + c * 8 + y] >> b) & 1) << 1) | ((bg[c * 8 + y] >> b) & 1);
		}
		const uint8_t *sp = rs.r[RG_SPR].data();
		for (int c = 0; c < 64; c++) for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
			int off = c * 128 + (y >> 3) * 32 + (x >> 3) * 8 + (y & 7), b = 7 - (x & 7);
			sprpix[c * 1024 + y * 32 + x] = (((sp[0x4000 + off] >> b) & 1) << 2) | (((sp[0x2000 + off] >> b) & 1) << 1) | ((sp[off] >> b) & 1);
		}
		/* background pixmap: 32 x 512 tiles of 8x8 -> 256 x 4096 pen indices (colour*8 + pixel) */
		bgmap.assign(256 * 4096, 0);
		const int size = 0x4000;
		for (int t = 0; t < 16384; t++) {
			int code = map_dat[t] + 256 * (map_dat[t + size] & 3), color = map_dat[t + size] >> 4;
			int tx0 = (t % 32) * 8, ty0 = (t / 32) * 8;
			for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
				bgmap[(size_t)(ty0 + y) * 256 + tx0 + x] = (uint8_t)(color * 8 + bgpix[code * 64 + y * 8 + x]);
		}
	}

	/* MAME compute_resistor_weights() with resistances 1000/470/220 (R,G: 3 bits; B: 2 bits), 470 ohm pulldown */
	void build_palette()
	{
		auto weights = [](int n, const int *res, double *w, double *sum_out) {
			double sum = 0;
			for (int i = 0; i < n; i++) {
				double R0 = 1.0 / 470, R1 = 1.0 / 1e12;
				for (int j = 0; j < n; j++) { if (j == i) R1 += 1.0 / res[j]; else R0 += 1.0 / res[j]; }
				R0 = 1.0 / R0; R1 = 1.0 / R1;
				w[i] = 255.0 * R0 / (R1 + R0);
				sum += w[i];
			}
			*sum_out = sum;
		};
		static const int res3[3] = { 1000, 470, 220 };
		double rw[3], gw[3], bw[2], s3, s2;
		weights(3, res3, rw, &s3); weights(3, res3, gw, &s3); weights(2, res3 + 1, bw, &s2);
		double scale = 255.0 / (s3 > s2 ? s3 : s2);
		for (int i = 0; i < 3; i++) { rw[i] *= scale; gw[i] *= scale; }
		for (int i = 0; i < 2; i++) bw[i] *= scale;
		for (int i = 0; i < 256; i++) {
			int p = prom_pal[i];
			int r = (int)(rw[0] * (p & 1) + rw[1] * ((p >> 1) & 1) + rw[2] * ((p >> 2) & 1) + 0.5);
			int g = (int)(gw[0] * ((p >> 3) & 1) + gw[1] * ((p >> 4) & 1) + gw[2] * ((p >> 5) & 1) + 0.5);
			int b = (int)(bw[0] * ((p >> 6) & 1) + bw[1] * ((p >> 7) & 1) + 0.5);
			pal[i] = 0xff000000u | (r << 16) | (g << 8) | b;
		}
	}

	void reset()
	{
		memset(&cpu, 0, sizeof cpu);
		cpu.ud = this; cpu.rd = rd_cb; cpu.rdop = rdop_cb; cpu.wr = wr_cb; cpu.in = in_cb; cpu.out = out_cb;
		z80_reset(&cpu);
		memset(ram, 0, sizeof ram); memset(vram, 0, sizeof vram); memset(spr, 0, sizeof spr);
		latch1 = latch2 = 0; int_en = irq_line = flip = false; bg_enable = false;
		bg_color = fg_color = 0; bg_pos = 0; coin_status[0] = coin_status[1] = coin_status[2] = 0;
		ppi_out[0] = ppi_out[1] = ppi_out[2] = 0;
		synth.set_board(set_id == SET_SZAXXON);   /* Super Zaxxon has the extra sound board in the 8255 socket */
		synth.reset();
		sndev.clear(); audio.assign(FRAME_SMP, 0); frames = 0; cyc_in_frame = 0;
		memset(fb, 0, sizeof fb);
	}

	/* ------------------------------------------------------------- bus */
	uint8_t sw100_read() const
	{
		return (uint8_t)((sw100 & 0x0c) | (coin_status[0] ? 0x20 : 0) | (coin_status[1] ? 0x40 : 0) | (coin_status[2] ? 0x80 : 0));
	}

	uint8_t mread(uint16_t a)
	{
		if (a < 0x6000) return rom[a];
		if (a < 0x7000) return ram[a - 0x6000];
		switch (a & 0xe000) {
		case 0x8000: return vram[a & 0x3ff];
		case 0xa000: return spr[a & 0xff];
		case 0xc000:
			if ((a & 0xe700) == 0xc100) return sw100_read();
			if ((a & 0xe700) == 0xc000) {
				switch (a & 3) { case 0: return sw00; case 1: return sw01; case 2: return dsw02; default: return dsw03; }
			}
			break;
		case 0xe000:
			if ((a & 0xe0fc) == 0xe03c) return ppi_read(a & 3);
			break;
		}
		return 0;
	}

	void mwrite(uint16_t a, uint8_t v)
	{
		if (a < 0x6000) return;                                   /* ROM */
		if (a < 0x7000) { ram[a - 0x6000] = v; return; }
		switch (a & 0xe000) {
		case 0x8000: vram[a & 0x3ff] = v; return;
		case 0xa000: spr[a & 0xff] = v; return;
		case 0xc000:
			if ((a & 0xe700) == 0xc000) latch1_write(a & 7, v & 1);
			return;
		case 0xe000:
			if ((a & 0xe0fc) == 0xe03c) { ppi_write(a & 3, v); return; }
			if ((a & 0xe0f4) == 0xe0f0) {                          /* video / interrupt control */
				int off = (a & 3) | (a & 8);
				bool a3 = off & 8;
				latch2_write((a3 ? 4 : 0) | (off & 3), v & 1);
				if (a3 && !(off & 2)) {                              /* background scroll position, 11 bits */
					if ((off & 1) == 0) bg_pos = (bg_pos & 0x700) | v;
					else bg_pos = (bg_pos & 0x0ff) | ((v << 8) & 0x700);
				}
			}
			return;
		}
	}

	/* U55: coin enables, coin counters, flip */
	void latch1_write(int bit, int s)
	{
		uint8_t old = latch1;
		latch1 = (uint8_t)((latch1 & ~(1 << bit)) | (s << bit));
		if (latch1 == old) return;
		if (bit <= 2) for (int n = 0; n < 3; n++) if (!((latch1 >> n) & 1)) coin_status[n] = 0;
		if (bit == 3) coin_counter[0] += s;
		if (bit == 4) coin_counter[1] += s;
		if (bit == 6) flip = !s;
	}
	/* U56: interrupt enable, colour banks, background enable */
	void latch2_write(int bit, int s)
	{
		uint8_t old = latch2;
		latch2 = (uint8_t)((latch2 & ~(1 << bit)) | (s << bit));
		if (latch2 == old) return;
		if (bit == 0) { int_en = s; if (!int_en) irq_line = false; }
		if (bit == 1) fg_color = s * 0x80;
		if (bit == 6) bg_color = s * 0x80;
		if (bit == 7) bg_enable = s;
	}

	/* ------------------------------------------------------------- 8255 (mode 0, outputs) -> discrete sound */
	uint8_t ppi_read(int r) const { return r < 3 ? ppi_out[r] : 0; }
	void ppi_port(int p, uint8_t v) { ppi_out[p] = v; sndev.push_back({ cyc_in_frame, p, v }); }
	void ppi_write(int r, uint8_t v)
	{
		if (r < 3) { ppi_port(r, v); return; }
		if (v & 0x80) { ppi_port(0, 0); ppi_port(1, 0); ppi_port(2, 0); }   /* mode set: all output latches clear */
		else {                                                                /* bit set/reset on port C */
			int bit = (v >> 1) & 7;
			ppi_port(2, (uint8_t)((ppi_out[2] & ~(1 << bit)) | ((v & 1) << bit)));
		}
	}

	/* ------------------------------------------------------------- input */
	void coin_pressed(int n)    /* a coin switch closing; only latched if the game enabled that input */
	{
		if (n >= 0 && n < 3 && ((latch1 >> n) & 1)) coin_status[n] = 1;
	}
	void service_nmi() { cpu.nmi = 1; }

	/* ------------------------------------------------------------- video (MAME zaxxon_v.cpp) */
	static int find_min_y(int value, int flip)
	{
		int flipmask = flip ? 0xff : 0, flipconst = flip ? 0xef : 0xf1, y;
		for (y = 0; y < 256; y += 16) {
			int sum = (value + flipconst + 1) + (y ^ flipmask);
			if ((sum & 0xe0) == 0xe0) break;
		}
		while (1) {
			int sum = (value + flipconst + 1) + ((y - 1) ^ flipmask);
			if ((sum & 0xe0) != 0xe0) break;
			y--;
		}
		return (y + 1) & 0xff;
	}
	static int find_min_x(int value, int flip)
	{
		int flipmask = flip ? 0xff : 0;
		int x = (value + 0xef + 1) ^ flipmask;
		if (flipmask) x -= 31;
		return x & 0xff;
	}

	void render()
	{
		/* ---- background ---- */
		if (bg_enable) {
			int flipmask = flip ? 0xff : 0, flipoffs = flip ? 0x38 : 0x40;
			if (!flip) flipoffs -= 1; else flipoffs += 7;
			for (int y = 16; y < 240; y++) {
				uint32_t *dst = &fb[(y - 16) * SCR_W];
				int vf = y ^ flipmask;
				int srcy = vf + ((bg_pos << 1) ^ 0xfff) + 1;
				const uint8_t *src = &bgmap[(size_t)(srcy & 4095) * 256];
				for (int x = 0; x < 256; x++) {
					int srcx = x ^ flipmask;
					srcx += ((vf >> 1) ^ 0xff) + 1;
					srcx += flipoffs;
					dst[x] = pal[(src[srcx & 255] + bg_color) & 255];
				}
			}
		} else {
			for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xff000000u;
		}
		/* ---- sprites (lower half of sprite RAM only, drawn from the highest slot down) ---- */
		int fm = flip ? 0xff : 0;
		for (int offs = 0x7c; offs >= 0; offs -= 4) {
			int sy = find_min_y(spr[offs], flip);
			int b1 = spr[offs + 1];
			bool flipy = ((b1 ^ fm) & 0x80) != 0, flipx = ((b1 ^ fm) & 0x40) != 0;
			int code = b1 & 0x3f, color = spr[offs + 2] & 0x1f;
			int sx = find_min_x(spr[offs + 3], flip);
			for (int k = 0; k < 4; k++) {            /* four copies 256 pixels apart so sprites wrap */
				int ox = sx - ((k & 2) ? 0x100 : 0), oy = sy - ((k & 1) ? 0x100 : 0);
				for (int y = 0; y < 32; y++) {
					int py = oy + y;
					if (py < 16 || py > 239) continue;
					int sr = flipy ? 31 - y : y;
					uint32_t *dst = &fb[(py - 16) * SCR_W];
					for (int x = 0; x < 32; x++) {
						int px = ox + x;
						if (px < 0 || px > 255) continue;
						int sc = flipx ? 31 - x : x;
						int p = sprpix[code * 1024 + sr * 32 + sc];
						if (p) dst[px] = pal[(color * 8 + p) & 255];
					}
				}
			}
		}
		/* ---- foreground characters (pen 0 transparent) ---- */
		for (int t = 0; t < 1024; t++) {
			int sx = t % 32, sy = t / 32, code = vram[t];
			int color = prom_col[sx + 32 * (sy / 4)] & 0x0f;
			int base = fg_color + color * 8;
			for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
				int p = txpix[code * 64 + y * 8 + x];
				if (!p) continue;
				int px = sx * 8 + x, py = sy * 8 + y;
				if (flip) { px = 255 - px; py = 255 - py; }
				if (py < 16 || py > 239) continue;
				fb[(py - 16) * SCR_W + px] = pal[(base + p) & 255];
			}
		}
	}

	/* ------------------------------------------------------------- one video frame */
	void run_cycles(int n)
	{
		int target = cyc_in_frame + n;
		while (cyc_in_frame < target) {
			cpu.irq = irq_line ? 1 : 0;           /* level-triggered: stays asserted until the game clears INTON */
			cyc_in_frame += z80_step(&cpu);
		}
	}

	void run_frame()
	{
		cyc_in_frame = 0; sndev.clear();
		run_cycles(VBLANK_AT);
		if (int_en) irq_line = true;              /* vblank interrupt */
		render();
		run_cycles(FRAME_CYC - cyc_in_frame);
		/* audio for this frame: the sound board runs at 4 x 48 kHz; each 8255 write lands on its own 5.2 us step */
		const int nsub = FRAME_SMP * zsnd::OS;
		int pos = 0;
		for (auto &e : sndev) {
			int at = (int)((long long)e.cyc * nsub / FRAME_CYC);
			if (at > nsub) at = nsub;
			if (at > pos) { synth.run(at - pos); pos = at; }
			synth.write(e.port, e.v);
		}
		if (pos < nsub) synth.run(nsub - pos);
		audio.assign(FRAME_SMP, 0);
		synth.take(audio.data(), FRAME_SMP);
		if (volume != 1.0f) for (auto &s : audio) { int v = (int)(s * volume); s = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
		apply_cheats();
		frames++;
	}
};

/* rotate the 256x224 frame for display: 0 = as scanned, 1 = 90 degrees clockwise (the arcade monitor) */
static void make_view(const uint32_t *src, int rotation, std::vector<uint32_t> &dst, int &w, int &h)
{
	if (rotation == 0) { w = SCR_W; h = SCR_H; dst.assign(src, src + SCR_W * SCR_H); return; }
	w = SCR_H; h = SCR_W; dst.resize((size_t)w * h);
	for (int y = 0; y < SCR_H; y++)
		for (int x = 0; x < SCR_W; x++)
			dst[(size_t)x * w + (SCR_H - 1 - y)] = src[y * SCR_W + x];
}

static void write_bmp(const char *path, const uint32_t *px, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	uint32_t rowb = (uint32_t)((w * 3 + 3) & ~3), size = 54 + rowb * h;
	uint8_t hdr[54] = {'B', 'M'};
	auto p32 = [&](int o, uint32_t v) { memcpy(hdr + o, &v, 4); };
	p32(2, size); p32(10, 54); p32(14, 40); p32(18, (uint32_t)w); p32(22, (uint32_t)h); hdr[26] = 1; hdr[28] = 24; p32(34, rowb * h);
	fwrite(hdr, 1, 54, f);
	std::vector<uint8_t> row(rowb, 0);
	for (int y = h - 1; y >= 0; y--) {
		for (int x = 0; x < w; x++) {
			uint32_t c = px[y * w + x];
			row[x * 3 + 0] = c & 0xff; row[x * 3 + 1] = (c >> 8) & 0xff; row[x * 3 + 2] = (c >> 16) & 0xff;
		}
		fwrite(row.data(), 1, rowb, f);
	}
	fclose(f);
}

static bool write_wav(const char *path, const std::vector<int16_t> &s)
{
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return false; }
	uint32_t n = (uint32_t)(s.size() * 2), rate = RATE, br = RATE * 2;
	auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
	auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
	fwrite("RIFF", 1, 4, f); w32(36 + n); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(1); w32(rate); w32(br); w16(2); w16(16);
	fwrite("data", 1, 4, f); w32(n); fwrite(s.data(), 2, s.size(), f);
	return fclose(f) == 0;
}

} // namespace zx
