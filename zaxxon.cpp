/*
 * Zaxxon / Super Zaxxon (Sega, 1982) - a small standalone emulator with synthesized sound.
 *
 * The game board is emulated after MAME's zaxxon driver; the discrete analog sound board (and Super Zaxxon's
 * extra sound board) is not played back from samples but simulated at the circuit level (zaxxon_sound.h).
 * You need to supply the ROMs yourself (MAME "szaxxon.zip" or "zaxxon.zip"; the set is detected automatically).
 */
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "z80.c"
#undef A
#undef F
#undef B
#undef C
#undef D
#undef E
#undef H
#undef L
#include "zaxxon_machine.h"
#include "font8.h"

using namespace zx;

static Machine M;

static void usage()
{
	fputs(
"Zaxxon / Super Zaxxon emulator with synthesized (sample-free) sound\n"
"usage: zaxxon [options] [szaxxon.zip | zaxxon.zip | rom-folder] ...\n"
"  With no path, a folder called 'roms' next to the program, the current folder and the folder the ROMs were last\n"
"  loaded from are searched.  When both games are there, a screen asks which one to play.\n"
"  You can also drop a zip or a folder on the window, or press O (zip) / F (folder) to browse; that folder is remembered.\n\n"
"  --game NAME      zaxxon or szaxxon: start that game without asking\n"
"  --scale N        window scale (default 3)\n"
"  --no-rotate      do not rotate the picture 90 degrees\n"
"  --volume X       master volume, 0..2 (default 1)\n"
"  --lives N        3, 4 or 5 (default 3);  --free-play;  --hard (Super Zaxxon difficulty)\n"
"  --bonus N        10000, 20000, 30000 or 40000 (default 10000)\n"
"  --cheat LIST     any of: lives,fuel,invincible  (or all);  they can also be switched in the menu\n"
"  --integer        integer-multiple scaling only (default: fill the window, nearest-neighbour)\n"
"  --reset-settings ignore and overwrite the saved settings\n"
"  --mc3340         simulate the MB4391 volume chips with the Motorola MC3340's curve (the repair part)\n"
"                   instead of the curve fitted to a real board (can also be switched in the menu)\n"
"  --engine STYLE   board (default) or mame: MAME-style engine noise, looped, layered and clipped the way MAME\n"
"                   plays its engine samples (rougher, pulsing; no samples are used). Also in the menu\n"
"  --sound-test     the sound board on its own (no ROMs needed): press keys to trigger every sound\n"
"  --sound-demo F   write every sound, one after the other, to the WAV file F and exit (no ROMs needed)\n"
"  --sound-wavs DIR write one WAV per sound into DIR, named like MAME's sample files (00.wav ... 23.wav), and exit\n"
"                   (also sz_*.wav: the Super Zaxxon engine and boss sounds from its extra sound board)\n"
"  --headless       no window or audio device; run --frames N and exit\n"
"  --frames N       frames to run in headless mode (default 600)\n"
"  --wav FILE       headless: write the audio to a WAV file\n"
"  --shot FILE      headless: save the last frame as a BMP\n"
"  --shot-every N   headless: also save every Nth frame as FILE-<n>.bmp\n"
"  --coin-at N      headless: insert a coin at frame N;  --start-at N presses 1P start\n"
"  --autoplay       headless: after the start press, hold fire and wiggle the stick (for testing)\n"
"  -h, --help       this text\n\n"
"keys: arrows move, Z / Ctrl / Space fire, 5 coin, 1 / 2 start, F2 service (test mode),\n"
"      Esc or Tab menu (cheats, DIP switches, scaling ...), P pause, R rotate, F3 reset,\n"
"      F11 fullscreen, F12 screenshot, + / - volume\n", stderr);
}

/* ------------------------------------------------------------------ sound test */
struct SoundItem { SDL_Keycode key; const char *name; int port; uint8_t bit; bool hold; };
static const SoundItem SOUND_ITEMS[] = {
	{ SDLK_Q, "Q  Cannon",               1, 0x80, false },
	{ SDLK_W, "W  M-Exp (medium explosion)", 1, 0x20, false },
	{ SDLK_E, "E  S-Exp (small explosion)",  1, 0x10, false },
	{ SDLK_R, "R  Shot",                 2, 0x01, false },
	{ SDLK_T, "T  Base missile",         0, 0x20, false },
	{ SDLK_Y, "Y  Alarm 2",              2, 0x04, false },
	{ SDLK_U, "U  Alarm 3",              2, 0x08, false },
	{ SDLK_1, "1  Battleship (on/off)",  0, 0x80, true },
	{ SDLK_2, "2  Laser (on/off)",       0, 0x40, true },
	{ SDLK_3, "3  Homing missile (on/off)", 0, 0x10, true },
};

struct SoundTest {
	zsnd::ZaxSound syn;
	uint8_t port[3] = { 0xff, 0xff, 0xff };
	int pulse[3][8] = {};
	int ship_on = 0, ship_ab = 0, ship_c = 1;     /* player ship: on/off, the A/B bits (0..3) and the C bit */
	void set(int p, uint8_t v) { if (port[p] != v) { port[p] = v; syn.ppi_write(p, v); } }
	void press(const SoundItem &it)
	{
		const int b = it.bit == 0x80 ? 7 : it.bit == 0x40 ? 6 : it.bit == 0x20 ? 5 : it.bit == 0x10 ? 4 : it.bit == 8 ? 3 : it.bit == 4 ? 2 : it.bit == 2 ? 1 : 0;
		if (it.hold) set(it.port, port[it.port] ^ it.bit);
		else { set(it.port, port[it.port] & ~it.bit); pulse[it.port][b] = 6; }
	}
	void ship()
	{
		/* D low = engine running; A/B (bits 0,1) set the pitch / volume; C picks the channel (Zaxxon writes it low in the
		   fortress and high in space) */
		set(0, (uint8_t)((port[0] & 0xf0) | (ship_c ? 0x04 : 0) | (ship_ab & 3) | (ship_on ? 0 : 0x08)));
	}
	void frame(int16_t *out)
	{
		for (int p = 0; p < 3; p++)
			for (int b = 0; b < 8; b++)
				if (pulse[p][b] > 0 && --pulse[p][b] == 0) set(p, port[p] | (1 << b));
		syn.render(out, FRAME_SMP);
	}
};

static void sound_demo(const char *path, int vca, int engine)
{
	/* each sound for a few seconds, one after the other, with a short gap */
	struct Step { const char *name; int port; uint8_t bit; double on, total; };
	static const Step steps[] = {
		{ "s-exp", 1, 0x10, 0.1, 2.0 }, { "m-exp", 1, 0x20, 0.1, 2.5 }, { "cannon", 1, 0x80, 0.1, 1.5 }, { "shot", 2, 0x01, 0.1, 1.5 },
		{ "base missile", 0, 0x20, 0.1, 3.0 }, { "alarm 2", 2, 0x04, 0.1, 1.5 }, { "alarm 3", 2, 0x08, 0.1, 1.5 },
		{ "battleship", 0, 0x80, 4.0, 5.0 }, { "laser", 0, 0x40, 3.0, 4.0 }, { "homing missile", 0, 0x10, 4.0, 5.0 },
	};
	std::vector<int16_t> all;
	for (const Step &s : steps) {
		zsnd::ZaxSound syn; syn.set_vca_model(vca); syn.set_engine_style(engine); uint8_t p[3] = { 0xff, 0xff, 0xff };
		const int n = (int)(s.total * RATE), non = (int)(s.on * RATE);
		std::vector<int16_t> b(n);
		syn.render(b.data(), 2400);
		p[s.port] &= ~s.bit; syn.write(s.port, p[s.port]);
		syn.render(b.data() + 2400, non);
		p[s.port] |= s.bit; syn.write(s.port, p[s.port]);
		syn.render(b.data() + 2400 + non, n - 2400 - non);
		all.insert(all.end(), b.begin(), b.end());
		all.insert(all.end(), RATE / 2, 0);
	}
	/* player ship (Zaxxon board, then Super Zaxxon's engine board): the values the games write, f0..f7 */
	for (int board = 0; board < 2; board++)
		for (int ab = 0; ab < 8; ab++) {
			if (board == 1 && ab < 4) continue;               /* Super Zaxxon only uses f4..f7 */
			zsnd::ZaxSound syn; syn.set_vca_model(vca); syn.set_board(board == 1); syn.set_engine_style(engine);
			std::vector<int16_t> b(3 * RATE);
			syn.render(b.data(), 2400);
			syn.write(0, (uint8_t)(0xf0 | ab)); syn.render(b.data() + 2400, 2 * RATE);
			syn.write(0, 0xff); syn.render(b.data() + 2400 + 2 * RATE, RATE - 2400);
			all.insert(all.end(), b.begin(), b.end());
			all.insert(all.end(), RATE / 2, 0);
		}
	write_wav(path, all);
	fprintf(stderr, "wrote %s (%.1f s)\n", path, all.size() / (double)RATE);
}


/* one WAV per sound, named like the MAME sample files so they can be compared side by side */
static void sound_wavs(const std::string &dir, int vca)
{
	struct Item { const char *file; const char *what; int port; uint8_t on; uint8_t off; double on_s, total_s; bool sz; bool mame = false; };
	/* on/off = the value written to the port to start / stop; one-shots only need the start (their own one-shot ends them) */
	static const Item items[] = {
		{ "00", "Battleship (end-of-level boss), held on",       0, 0x7f, 0xff, 6.0, 7.0, false },
		{ "01", "Laser (force field), held on",                  0, 0xbf, 0xff, 5.0, 6.0, false },
		{ "02", "Base missile, one-shot",                        0, 0xdf, 0xff, 0.1, 6.0, false },
		{ "03", "Homing missile, held on",                       0, 0xef, 0xff, 6.0, 7.0, false },
		{ "04", "Player ship, channel 1 (port A = f3)",          0, 0xf3, 0xff, 5.0, 6.5, false },
		{ "05", "Player ship, channel 1 (port A = f0)",          0, 0xf0, 0xff, 5.0, 6.5, false },
		{ "08", "Cannon (your ship's fire), one-shot",           1, 0x7f, 0xff, 0.1, 3.0, false },
		{ "10", "M-Exp (ship explosion), one-shot",              1, 0xdf, 0xff, 0.1, 5.0, false },
		{ "11", "S-Exp (enemy explosion), one-shot",             1, 0xef, 0xff, 0.1, 4.0, false },
		{ "20", "Alarm 3 (low fuel), one-shot",                  2, 0xf7, 0xff, 0.1, 2.0, false },
		{ "21", "Alarm 2 (target lock), one-shot",               2, 0xfb, 0xff, 0.1, 2.0, false },
		{ "23", "Shot (enemy fire), one-shot",                   2, 0xfe, 0xff, 0.1, 3.0, false },
		/* extras: every player-ship value the games write */
		{ "ship_f0", "Zaxxon player ship, port A = f0",          0, 0xf0, 0xff, 5.0, 6.5, false },
		{ "ship_f1", "Zaxxon player ship, port A = f1",          0, 0xf1, 0xff, 5.0, 6.5, false },
		{ "ship_f2", "Zaxxon player ship, port A = f2",          0, 0xf2, 0xff, 5.0, 6.5, false },
		{ "ship_f3", "Zaxxon player ship, port A = f3",          0, 0xf3, 0xff, 5.0, 6.5, false },
		{ "ship_f4", "Zaxxon player ship, port A = f4",          0, 0xf4, 0xff, 5.0, 6.5, false },
		{ "ship_f5", "Zaxxon player ship, port A = f5",          0, 0xf5, 0xff, 5.0, 6.5, false },
		{ "ship_f6", "Zaxxon player ship, port A = f6",          0, 0xf6, 0xff, 5.0, 6.5, false },
		{ "ship_f7", "Zaxxon player ship, port A = f7",          0, 0xf7, 0xff, 5.0, 6.5, false },
		{ "sz_ship_f4", "Super Zaxxon player ship (extra board), port A = f4", 0, 0xf4, 0xff, 5.0, 6.5, true },
		{ "sz_ship_f5", "Super Zaxxon player ship (extra board), port A = f5", 0, 0xf5, 0xff, 5.0, 6.5, true },
		{ "sz_ship_f6", "Super Zaxxon player ship (extra board), port A = f6", 0, 0xf6, 0xff, 5.0, 6.5, true },
		{ "sz_ship_f7", "Super Zaxxon player ship (extra board), port A = f7", 0, 0xf7, 0xff, 5.0, 6.5, true },
		{ "sz_00", "Super Zaxxon boss: battleship tone + the extra board's SOUND OUT B", 0, 0x7f, 0xff, 6.0, 7.0, true },
		/* the MAME-style engine option (--engine mame): fortress values (both loops) and space values (one loop) */
		{ "mame_ship_f0", "MAME-style engine, port A = f0",       0, 0xf0, 0xff, 5.0, 6.5, false, true },
		{ "mame_ship_f3", "MAME-style engine, port A = f3",       0, 0xf3, 0xff, 5.0, 6.5, false, true },
		{ "mame_ship_f4", "MAME-style engine, port A = f4",       0, 0xf4, 0xff, 5.0, 6.5, false, true },
		{ "mame_ship_f7", "MAME-style engine, port A = f7",       0, 0xf7, 0xff, 5.0, 6.5, false, true },
	};
	SDL_CreateDirectory(dir.c_str());                 /* fine if it already exists */
	std::string index = "File              Sound (MAME sample names where there is one)\n";
	int written = 0;
	for (const Item &it : items) {
		zsnd::ZaxSound syn; syn.set_vca_model(vca); syn.set_board(it.sz); syn.set_engine_style(it.mame ? 1 : 0);
		const int lead = RATE / 10, non = (int)(it.on_s * RATE), n = (int)(it.total_s * RATE);
		std::vector<int16_t> b(lead + n);
		syn.render(b.data(), lead);
		syn.write(it.port, it.on);
		syn.render(b.data() + lead, non);
		syn.write(it.port, it.off);
		syn.render(b.data() + lead + non, n - non);
		const std::string path = dir + "/" + it.file + ".wav";
		if (!write_wav(path.c_str(), b)) continue;
		written++;
		index += std::string(it.file) + ".wav";
		index.append(std::max<size_t>(1, 18 - strlen(it.file) - 4), ' ');
		index += std::string(it.what) + "\n";
	}
	FILE *f = fopen((dir + "/sounds.txt").c_str(), "w");
	if (f) { fputs(index.c_str(), f); fclose(f); }
	fprintf(stderr, "wrote %d WAV files (48 kHz, 16-bit, mono) into %s\n", written, dir.c_str());
}

/* ------------------------------------------------------------------ file dialogs */
static std::mutex g_pm; static std::string g_pending;
static void SDLCALL dialog_cb(void *, const char *const *list, int)
{
	if (list && list[0]) { std::lock_guard<std::mutex> l(g_pm); g_pending = list[0]; }
}


/* ------------------------------------------------------------------ settings */
struct Opts {
	int lives = 3, bonus = 10000;
	bool free_play = false, hard = false;
	bool inf_lives = false, inf_fuel = false, invincible = false;
	bool integer = false, rotate = true, fullscreen = false;
	int volume = 100;                              /* percent */
	int vca = 0;                                   /* 0 = MB4391 (curve fitted to a real board), 1 = MC3340 */
	int engine = 0;                                /* 0 = the board's engine noise, 1 = MAME-style (looped, layered, clipped) */
	int game = SET_SZAXXON;                        /* the game played last */
	std::string rom_dir;                           /* the folder the ROMs were last loaded from */
};
static Opts O;

static uint8_t make_dsw02()
{
	const int b = O.bonus == 20000 ? 0x01 : O.bonus == 30000 ? 0x02 : O.bonus == 40000 ? 0x00 : 0x03;
	const int l = O.free_play ? 0x00 : O.lives == 4 ? 0x10 : O.lives == 5 ? 0x20 : 0x30;
	/* bits 2/3: Super Zaxxon has a difficulty switch on bit 2 (set = normal); Zaxxon leaves both unused (set) */
	const int d = M.set_id == SET_SZAXXON ? (O.hard ? 0x00 : 0x04) : 0x0c;
	return (uint8_t)(b | d | l | 0x40);
}
static void apply_opts()
{
	M.dsw02 = make_dsw02();
	M.volume = O.volume / 100.0f;
	M.cheat_lives = O.inf_lives; M.cheat_fuel = O.inf_fuel; M.cheat_invincible = O.invincible;
	M.synth.set_vca_model(O.vca);
	M.synth.set_engine_style(O.engine);
}

static std::string settings_path()
{
	char *p = SDL_GetPrefPath("zaxxonnosamples", "zaxxon");
	std::string r = p ? std::string(p) + "settings.ini" : "";
	SDL_free(p);
	return r;
}
static void settings_load()
{
	const std::string path = settings_path(); if (path.empty()) return;
	FILE *f = fopen(path.c_str(), "r"); if (!f) return;
	char line[1200];
	while (fgets(line, sizeof line, f)) {
		std::string ln = line;
		while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r')) ln.pop_back();
		const size_t eq = ln.find('='); if (eq == std::string::npos) continue;
		const std::string key = ln.substr(0, eq), val = ln.substr(eq + 1);
		const int v = atoi(val.c_str());
		if (key == "rom_dir") O.rom_dir = val;
		else if (key == "game") O.game = v == SET_ZAXXON ? SET_ZAXXON : SET_SZAXXON;
		else if (key == "lives") O.lives = v; else if (key == "bonus") O.bonus = v; else if (key == "free_play") O.free_play = v;
		else if (key == "hard") O.hard = v; else if (key == "inf_lives") O.inf_lives = v; else if (key == "inf_fuel") O.inf_fuel = v;
		else if (key == "invincible") O.invincible = v; else if (key == "integer") O.integer = v; else if (key == "rotate") O.rotate = v;
		else if (key == "fullscreen") O.fullscreen = v; else if (key == "volume") O.volume = v;
		else if (key == "vca") O.vca = v ? 1 : 0;
		else if (key == "engine") O.engine = v ? 1 : 0;
	}
	fclose(f);
}
static void settings_save()
{
	const std::string path = settings_path(); if (path.empty()) return;
	FILE *f = fopen(path.c_str(), "w"); if (!f) return;
	fprintf(f, "lives=%d\nbonus=%d\nfree_play=%d\nhard=%d\ninf_lives=%d\ninf_fuel=%d\ninvincible=%d\ninteger=%d\nrotate=%d\nfullscreen=%d\nvolume=%d\nvca=%d\nengine=%d\ngame=%d\nrom_dir=%s\n",
		O.lives, O.bonus, O.free_play, O.hard, O.inf_lives, O.inf_fuel, O.invincible, O.integer, O.rotate, O.fullscreen, O.volume, O.vca,
		O.engine, O.game, O.rom_dir.c_str());
	fclose(f);
}

/* ------------------------------------------------------------------ ROM library */
/* Every ROM file found in the usual places (or the paths given), and which of the two games they complete, so both
   games can be offered without browsing for the ROMs each time. */
static std::vector<rz::RomFile> g_lib;
static bool g_have[2];                             /* indexed by SET_SZAXXON / SET_ZAXXON */

static bool set_complete(const std::vector<rz::RomFile> &files, int set)
{
	RomSet t; std::string miss;
	return load_roms(files, t, miss, set);
}
static void scan_library(const std::vector<std::string> &paths, bool search_default)
{
	g_lib.clear();
	if (paths.empty() && search_default) {
		rz::add_path("roms", g_lib); rz::add_path(".", g_lib);
		const char *bp = SDL_GetBasePath();
		if (bp) { rz::add_path(std::string(bp) + "roms", g_lib); rz::add_path(bp, g_lib); }
		if (!O.rom_dir.empty()) rz::add_path(O.rom_dir, g_lib);
	} else for (auto &p : paths) rz::add_path(p, g_lib);
	for (int s = 0; s < 2; s++) g_have[s] = set_complete(g_lib, s);
}
static int other_set(int s) { return s == SET_SZAXXON ? SET_ZAXXON : SET_SZAXXON; }
static bool boot_set(int set)
{
	static RomSet rs;
	std::string missing;
	if (!load_roms(g_lib, rs, missing, set)) {
		if (!g_lib.empty()) fprintf(stderr, "Missing ROMs:\n%s", missing.c_str());
		return false;
	}
	M.load(rs);
	apply_opts();
	M.reset();
	O.game = M.set_id;
	return true;
}
/* a zip or folder chosen by drop / dialog: remember its folder, and look there for the other game too.
   Returns the game it holds (-1: not recognised); both = true when the folder has both games. */
static int add_rom_path(const std::string &path, bool &both)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	std::vector<rz::RomFile> one; rz::add_path(path, one);
	int set = set_complete(one, SET_SZAXXON) ? SET_SZAXXON : set_complete(one, SET_ZAXXON) ? SET_ZAXXON : -1;
	const std::string dir = fs::is_directory(path, ec) ? path : fs::path(path).parent_path().string();
	both = set_complete(one, SET_SZAXXON) && set_complete(one, SET_ZAXXON);
	if (set < 0) return -1;
	if (!dir.empty()) O.rom_dir = dir;
	scan_library({}, true);
	for (auto &f : one) g_lib.push_back(f);       /* in case the folder could not be read back */
	g_have[set] = true;
	return set;
}

/* ------------------------------------------------------------------ main */

enum { MI_RESUME, MI_GAME, MI_LIVES_CHEAT, MI_FUEL_CHEAT, MI_INVINCIBLE, MI_LIVES, MI_BONUS, MI_FREEPLAY, MI_DIFF, MI_SCALE, MI_ROTATE,
       MI_VOLUME, MI_VCA, MI_ENGINE, MI_FULLSCREEN, MI_RESET, MI_LOAD, MI_QUIT, MI_COUNT };

int main(int argc, char **argv)
{
	bool headless = false, sound_test = false, reset_settings = false;
	int frames_to_run = 600, shot_every = 0, coin_at = -1, start_at = -1;
	bool autoplay = false;
	std::string wav_path, shot_path, demo_path, wavs_dir;
	std::vector<std::string> paths;
	int scale = 3;

	/* command-line values override the saved settings, so remember which were given */
	struct Cli { int lives = -1, bonus = -1, volume = -1, free_play = -1, hard = -1, integer = -1, rotate = -1, vca = -1, engine = -1, game = -1;
	             bool inf_lives = false, inf_fuel = false, invincible = false; } cli;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&](const char *n) -> const char * {
			if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", n); exit(2); }
			return argv[++i];
		};
		if (a == "-h" || a == "--help") { usage(); return 0; }
		else if (a == "--scale") scale = std::max(1, atoi(next("--scale")));
		else if (a == "--no-rotate") cli.rotate = 0;
		else if (a == "--volume") cli.volume = (int)(atof(next("--volume")) * 100);
		else if (a == "--lives") cli.lives = atoi(next("--lives"));
		else if (a == "--free-play") cli.free_play = 1;
		else if (a == "--hard") cli.hard = 1;
		else if (a == "--bonus") cli.bonus = atoi(next("--bonus"));
		else if (a == "--integer") cli.integer = 1;
		else if (a == "--reset-settings") reset_settings = true;
		else if (a == "--cheat") {
			std::string v = next("--cheat");
			if (v.find("all") != std::string::npos) cli.inf_lives = cli.inf_fuel = cli.invincible = true;
			if (v.find("lives") != std::string::npos) cli.inf_lives = true;
			if (v.find("fuel") != std::string::npos) cli.inf_fuel = true;
			if (v.find("invinc") != std::string::npos) cli.invincible = true;
		}
		else if (a == "--mc3340") cli.vca = 1;
		else if (a == "--engine") {
			std::string v = next("--engine");
			for (auto &ch : v) ch = (char)tolower((unsigned char)ch);
			if (v == "board") cli.engine = 0;
			else if (v == "mame" || v == "mame-style") cli.engine = 1;
			else { fprintf(stderr, "--engine: board or mame\n"); return 1; }
		}
		else if (a == "--game") {
			std::string v = next("--game");
			for (auto &ch : v) ch = (char)tolower((unsigned char)ch);
			if (v == "zaxxon") cli.game = SET_ZAXXON;
			else if (v == "szaxxon" || v == "super" || v == "superzaxxon" || v == "super-zaxxon") cli.game = SET_SZAXXON;
			else { fprintf(stderr, "--game: zaxxon or szaxxon\n"); return 1; }
		}
		else if (a == "--sound-test") sound_test = true;
		else if (a == "--sound-demo") demo_path = next("--sound-demo");
		else if (a == "--sound-wavs") wavs_dir = next("--sound-wavs");
		else if (a == "--headless") headless = true;
		else if (a == "--frames") frames_to_run = atoi(next("--frames"));
		else if (a == "--wav") wav_path = next("--wav");
		else if (a == "--shot") shot_path = next("--shot");
		else if (a == "--shot-every") shot_every = atoi(next("--shot-every"));
		else if (a == "--coin-at") coin_at = atoi(next("--coin-at"));
		else if (a == "--start-at") start_at = atoi(next("--start-at"));
		else if (a == "--autoplay") autoplay = true;
		else if (a.size() > 1 && a[0] == '-') { fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 1; }
		else paths.push_back(a);
	}
	if (!demo_path.empty()) { sound_demo(demo_path.c_str(), cli.vca > 0 ? 1 : 0, cli.engine > 0 ? 1 : 0); return 0; }
	if (!wavs_dir.empty()) { sound_wavs(wavs_dir, cli.vca > 0 ? 1 : 0); return 0; }

	if (!headless && !reset_settings) settings_load();
	if (cli.lives >= 0) O.lives = cli.lives;
	if (cli.bonus >= 0) O.bonus = cli.bonus;
	if (cli.volume >= 0) O.volume = cli.volume;
	if (cli.free_play >= 0) O.free_play = cli.free_play;
	if (cli.hard >= 0) O.hard = cli.hard;
	if (cli.integer >= 0) O.integer = cli.integer;
	if (cli.rotate >= 0) O.rotate = cli.rotate;
	if (cli.inf_lives) O.inf_lives = true;
	if (cli.inf_fuel) O.inf_fuel = true;
	if (cli.invincible) O.invincible = true;
	if (cli.vca >= 0) O.vca = cli.vca;
	if (cli.engine >= 0) O.engine = cli.engine;

	/* ---------------- headless ---------------- */
	if (headless) {
		scan_library(paths, true);
		const int want = cli.game >= 0 ? cli.game : g_have[SET_SZAXXON] ? SET_SZAXXON : SET_ZAXXON;
		if (!boot_set(want)) {
			fprintf(stderr, "Could not find all the ROMs (szaxxon.zip or zaxxon.zip). Pass the zip or folder on the command line.\n");
			return 1;
		}
		fprintf(stderr, "set: %s\n", SET_NAMES[M.set_id]);
		const int rot = O.rotate ? 1 : 0;
		std::vector<int16_t> au;
		for (int f = 0; f < frames_to_run; f++) {
			M.sw100 = 0; M.sw00 = 0;
			if (f == coin_at) M.coin_pressed(0);
			if (start_at >= 0 && f >= start_at && f < start_at + 8) M.sw100 |= 0x04;
			if (autoplay && start_at >= 0 && f > start_at + 120) {
				if ((f / 40) & 1) M.sw00 |= 0x01; else M.sw00 |= 0x02;
				if ((f / 7) & 1) M.sw00 |= 0x10;
				if ((f / 90) & 1) M.sw00 |= 0x08; else M.sw00 |= 0x04;
			}
			M.run_frame();
			if (!wav_path.empty()) au.insert(au.end(), M.audio.begin(), M.audio.end());
			if (shot_every > 0 && !shot_path.empty() && f % shot_every == 0) {
				std::vector<uint32_t> v; int w, h; make_view(M.fb, rot, v, w, h);
				char n[512]; snprintf(n, sizeof n, "%s-%d.bmp", shot_path.c_str(), f);
				write_bmp(n, v.data(), w, h);
			}
			if (autoplay && f % 600 == 0) fprintf(stderr, "frame %d  lives byte %02x  fuel byte %02x\n", f, M.ram[(M.set_id == SET_SZAXXON ? 0x6066 : 0x6067) - 0x6000], M.ram[(M.set_id == SET_SZAXXON ? 0x62b2 : 0x62b1) - 0x6000]);
		}
		if (!shot_path.empty()) { std::vector<uint32_t> v; int w, h; make_view(M.fb, rot, v, w, h); write_bmp(shot_path.c_str(), v.data(), w, h); }
		if (!wav_path.empty()) write_wav(wav_path.c_str(), au);
		return 0;
	}

	/* ---------------- windowed ---------------- */
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
	SDL_InitSubSystem(SDL_INIT_GAMEPAD);
	SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
	bool have_game = false, picking = false;
	int psel = O.game;                             /* the game picker's highlighted game */
	if (!sound_test) {
		scan_library(paths, true);
		if (cli.game >= 0) have_game = boot_set(cli.game);
		else if (g_have[0] && g_have[1]) picking = true;              /* both games: let the player choose */
		else if (g_have[SET_SZAXXON]) have_game = boot_set(SET_SZAXXON);
		else if (g_have[SET_ZAXXON]) have_game = boot_set(SET_ZAXXON);
		if ((have_game || picking) && !paths.empty()) {          /* remember where the ROMs given on the command line are */
			namespace fs = std::filesystem; std::error_code ec;
			O.rom_dir = fs::is_directory(paths[0], ec) ? paths[0] : fs::path(paths[0]).parent_path().string();
		}
	}
	apply_opts();

	int cur_w = O.rotate ? SCR_H : SCR_W, cur_h = O.rotate ? SCR_W : SCR_H;
	auto base_title = [&]() { return sound_test ? std::string("Zaxxon - sound test") : have_game ? std::string(SET_NAMES[M.set_id]) : std::string("Zaxxon / Super Zaxxon"); };
	SDL_Window *win = SDL_CreateWindow(base_title().c_str(), cur_w * scale, cur_h * scale, SDL_WINDOW_RESIZABLE | (O.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
	SDL_Renderer *ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
	if (!win || !ren) { fprintf(stderr, "window/renderer: %s\n", SDL_GetError()); return 1; }
	SDL_SetRenderVSync(ren, 1);
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 256);
	SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);              /* nearest-neighbour only, never filtered */
	auto set_logical = [&]() {
		SDL_SetRenderLogicalPresentation(ren, cur_w, cur_h, O.integer ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE : SDL_LOGICAL_PRESENTATION_LETTERBOX);
	};
	set_logical();

	SDL_AudioSpec spec; SDL_zero(spec);
	spec.format = SDL_AUDIO_S16; spec.channels = 1; spec.freq = RATE;
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
	SDL_AudioStream *as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (!as) fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
	/* Keep three to four frames (50 to 67 ms) of sound queued.  The loop also waits for the display's vsync, so between two
	   top-ups the queue must ride out a late frame or two plus one device period; if it runs dry the device plays a gap,
	   and gaps sound like static.  (It used to keep one to three frames, which a 1024-sample device period could empty.) */
	const int Q_HIGH = 3 * FRAME_SMP * 2, Q_LOW = 3 * FRAME_SMP * 2;
	if (as) SDL_ResumeAudioStreamDevice(as);

	std::vector<SDL_Gamepad *> pads;
	auto open_pad = [&](SDL_JoystickID id) { SDL_Gamepad *g = SDL_OpenGamepad(id); if (g) { pads.push_back(g); fprintf(stderr, "controller: %s\n", SDL_GetGamepadName(g)); } };
	{ int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n); for (int i = 0; i < n; i++) open_pad(ids[i]); SDL_free(ids); }

	SoundTest st;
	st.syn.set_vca_model(O.vca); st.syn.set_engine_style(O.engine);
	bool running = true, paused = false, prev_coin = false, prev_svc = false, menu = false;
	int msel = 0, shot_n = 0;
	std::vector<uint32_t> view;
	Uint64 next_ns = SDL_GetTicksNS();

	/* the menu font (font8.h, converted from Press Start 2P) as one white-on-clear strip, tinted per string */
	SDL_Texture *font_tex = nullptr;
	{
		std::vector<uint32_t> px(95 * 8 * 8, 0);
		for (int c = 0; c < 95; c++)
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
					if (zfont::GLYPHS[c][y] & (0x80 >> x)) px[y * 95 * 8 + c * 8 + x] = 0xffffffffu;
		SDL_Surface *sf = SDL_CreateSurfaceFrom(95 * 8, 8, SDL_PIXELFORMAT_RGBA8888, px.data(), 95 * 8 * 4);
		if (sf) {
			font_tex = SDL_CreateTextureFromSurface(ren, sf);
			SDL_DestroySurface(sf);
		}
		if (font_tex) { SDL_SetTextureScaleMode(font_tex, SDL_SCALEMODE_NEAREST); SDL_SetTextureBlendMode(font_tex, SDL_BLENDMODE_BLEND); }
	}
	auto text = [&](float x, float y, const char *s, int r, int g, int b) {
		if (!font_tex) {                                   /* fall back to SDL's built-in font */
			SDL_SetRenderDrawColor(ren, (Uint8)r, (Uint8)g, (Uint8)b, 255);
			SDL_RenderDebugText(ren, x, y, s);
			return;
		}
		SDL_SetTextureColorMod(font_tex, (Uint8)r, (Uint8)g, (Uint8)b);
		for (; *s; s++, x += 8) {
			const unsigned char c = (unsigned char)*s;
			if (c <= 32 || c > 126) continue;
			const SDL_FRect src = { (float)((c - 32) * 8), 0, 8, 8 }, dst = { x, y, 8, 8 };
			SDL_RenderTexture(ren, font_tex, &src, &dst);
		}
	};
	auto set_rotate = [&](bool r) { O.rotate = r; cur_w = r ? SCR_H : SCR_W; cur_h = r ? SCR_W : SCR_H; set_logical(); };
	auto set_fullscreen = [&](bool f) { O.fullscreen = f; SDL_SetWindowFullscreen(win, f); };
	auto open_pick_zip = [&]() { static const SDL_DialogFileFilter f[] = { { "ROM zip", "zip" } }; SDL_ShowOpenFileDialog(dialog_cb, nullptr, win, f, 1, nullptr, false); };
	auto start_game = [&](int set) {
		if (boot_set(set)) {
			have_game = true; picking = false; menu = false;
			SDL_SetWindowTitle(win, base_title().c_str());
			if (as) SDL_ClearAudioStream(as);
			settings_save();
		}
	};

	/* ---- menu ---- */
	auto item_text = [&](int i, char *b, size_t n) {
		auto onoff = [](bool v) { return v ? "ON" : "OFF"; };
		switch (i) {
		case MI_RESUME:        snprintf(b, n, "Resume"); break;
		case MI_GAME:
			if (g_have[0] && g_have[1]) snprintf(b, n, "Play %s", SET_NAMES[have_game ? other_set(M.set_id) : O.game]);
			else snprintf(b, n, "Load the other game...");
			break;
		case MI_LIVES_CHEAT:   snprintf(b, n, "Infinite lives  %s", onoff(O.inf_lives)); break;
		case MI_FUEL_CHEAT:    snprintf(b, n, "Infinite fuel   %s", onoff(O.inf_fuel)); break;
		case MI_INVINCIBLE:    snprintf(b, n, "Invincible      %s", onoff(O.invincible)); break;
		case MI_LIVES:         snprintf(b, n, "Lives           %d", O.lives); break;
		case MI_BONUS:         snprintf(b, n, "Bonus ship  %d", O.bonus); break;
		case MI_FREEPLAY:      snprintf(b, n, "Free play       %s", onoff(O.free_play)); break;
		case MI_DIFF:          snprintf(b, n, "Difficulty    %s", M.set_id == SET_SZAXXON ? (O.hard ? "Hard" : "Normal") : "n/a"); break;
		case MI_SCALE:         snprintf(b, n, "Scaling  %s", O.integer ? "Integer" : "Fill window"); break;
		case MI_ROTATE:        snprintf(b, n, "Rotate screen   %s", onoff(O.rotate)); break;
		case MI_VOLUME:        snprintf(b, n, "Volume          %d%%", O.volume); break;
		case MI_VCA:           snprintf(b, n, "Sound chip    %s", O.vca ? "MC3340" : "MB4391"); break;
		case MI_ENGINE:        snprintf(b, n, "Engine sound %s", O.engine ? "MAME-style" : "     Board"); break;
		case MI_FULLSCREEN:    snprintf(b, n, "Fullscreen      %s", onoff(O.fullscreen)); break;
		case MI_RESET:         snprintf(b, n, "Reset game"); break;
		case MI_LOAD:          snprintf(b, n, "Load ROM zip..."); break;
		case MI_QUIT:          snprintf(b, n, "Quit"); break;
		}
	};
	auto change = [&](int i, int dir) {      /* dir: -1 left, +1 right / enter */
		switch (i) {
		case MI_RESUME: menu = false; break;
		case MI_GAME:
			if (g_have[0] && g_have[1]) start_game(have_game ? other_set(M.set_id) : O.game);
			else open_pick_zip();
			break;
		case MI_LIVES_CHEAT: O.inf_lives = !O.inf_lives; break;
		case MI_FUEL_CHEAT:  O.inf_fuel = !O.inf_fuel; break;
		case MI_INVINCIBLE:  O.invincible = !O.invincible; break;
		case MI_LIVES:       O.lives = std::max(3, std::min(5, O.lives + dir)); break;
		case MI_BONUS:       O.bonus = std::max(10000, std::min(40000, O.bonus + 10000 * dir)); break;
		case MI_FREEPLAY:    O.free_play = !O.free_play; break;
		case MI_DIFF:        if (M.set_id == SET_SZAXXON) O.hard = !O.hard; break;
		case MI_SCALE:       O.integer = !O.integer; set_logical(); break;
		case MI_ROTATE:      set_rotate(!O.rotate); break;
		case MI_VOLUME:      O.volume = std::max(0, std::min(200, O.volume + 10 * dir)); break;
		case MI_VCA:         O.vca = !O.vca; st.syn.set_vca_model(O.vca); break;
		case MI_ENGINE:      O.engine = !O.engine; st.syn.set_engine_style(O.engine); break;
		case MI_FULLSCREEN:  set_fullscreen(!O.fullscreen); break;
		case MI_RESET:       if (have_game) { M.reset(); menu = false; } break;
		case MI_LOAD:        open_pick_zip(); break;
		case MI_QUIT:        running = false; break;
		}
		apply_opts();
		settings_save();
	};
	auto menu_nav = [&](int dy, int dx, bool enter) {
		if (dy) msel = (msel + dy + MI_COUNT) % MI_COUNT;
		if (dx) change(msel, dx);
		if (enter) change(msel, 1);
	};

	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_EVENT_QUIT) running = false;
			else if (ev.type == SDL_EVENT_GAMEPAD_ADDED) open_pad(ev.gdevice.which);
			else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
				for (size_t i = 0; i < pads.size(); i++)
					if (SDL_GetGamepadID(pads[i]) == ev.gdevice.which) { SDL_CloseGamepad(pads[i]); pads.erase(pads.begin() + i); break; }
			}
			else if (ev.type == SDL_EVENT_DROP_FILE && !sound_test) {
				std::lock_guard<std::mutex> l(g_pm); g_pending = ev.drop.data ? ev.drop.data : "";
			}
			else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !sound_test) {
				const int b = ev.gbutton.button;
				if (menu) {
					if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) menu_nav(-1, 0, false);
					else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) menu_nav(1, 0, false);
					else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) menu_nav(0, -1, false);
					else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) menu_nav(0, 1, false);
					else if (b == SDL_GAMEPAD_BUTTON_SOUTH) menu_nav(0, 0, true);
					else if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_NORTH || b == SDL_GAMEPAD_BUTTON_GUIDE) menu = false;
				} else if (b == SDL_GAMEPAD_BUTTON_NORTH || b == SDL_GAMEPAD_BUTTON_GUIDE) { menu = true; msel = 0; }
				else if (picking) {
					if (b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) psel = other_set(psel);
					else if (b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_START) start_game(psel);
				}
			}
			else if (ev.type == SDL_EVENT_KEY_DOWN && picking && !menu && !sound_test &&
			         ev.key.key != SDLK_ESCAPE && ev.key.key != SDLK_TAB) {
				const SDL_Keycode k = ev.key.key;
				if (k == SDLK_UP || k == SDLK_DOWN) psel = other_set(psel);
				else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_Z || k == SDLK_1) start_game(psel);
				else if (k == SDLK_O) open_pick_zip();
				else if (k == SDLK_F) SDL_ShowOpenFolderDialog(dialog_cb, nullptr, win, nullptr, false);
				else if (k == SDLK_F11) { set_fullscreen(!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)); settings_save(); }
			}
			else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && picking && !menu && !sound_test) {
				SDL_ConvertEventToRenderCoordinates(ren, &ev);
				for (int i = 0; i < 2; i++) {
					const float y0 = 60.0f + 20.0f * i;
					if (ev.button.y >= y0 - 5 && ev.button.y < y0 + 13) { psel = i == 0 ? SET_ZAXXON : SET_SZAXXON; start_game(psel); }
				}
			}
			else if (ev.type == SDL_EVENT_KEY_DOWN && menu && !sound_test) {
				const SDL_Keycode k = ev.key.key;
				if (k == SDLK_UP) menu_nav(-1, 0, false);
				else if (k == SDLK_DOWN) menu_nav(1, 0, false);
				else if (k == SDLK_LEFT) menu_nav(0, -1, false);
				else if (k == SDLK_RIGHT) menu_nav(0, 1, false);
				else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_Z) menu_nav(0, 0, true);
				else if (k == SDLK_ESCAPE || k == SDLK_TAB) menu = false;
			}
			else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
				const SDL_Keycode k = ev.key.key;
				if (k == SDLK_ESCAPE) { if (sound_test) running = false; else { menu = true; msel = 0; } }
				else if (k == SDLK_TAB && !sound_test) { menu = true; msel = 0; }
				else if (k == SDLK_P) paused = !paused;
				else if (k == SDLK_R && !sound_test) { set_rotate(!O.rotate); settings_save(); }
				else if (k == SDLK_F3 && have_game) M.reset();
				else if (k == SDLK_F11) { set_fullscreen(!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)); settings_save(); }
				else if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) { O.volume = std::min(200, O.volume + 10); apply_opts(); }
				else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) { O.volume = std::max(0, O.volume - 10); apply_opts(); }
				else if (k == SDLK_F12 && have_game) {
					int w, h; make_view(M.fb, O.rotate ? 1 : 0, view, w, h);
					char n[64]; snprintf(n, sizeof n, "zaxxon-%03d.bmp", shot_n++);
					write_bmp(n, view.data(), w, h); fprintf(stderr, "saved %s\n", n);
				}
				else if (k == SDLK_O && !sound_test) open_pick_zip();
				else if (k == SDLK_F && !sound_test) SDL_ShowOpenFolderDialog(dialog_cb, nullptr, win, nullptr, false);
				if (sound_test) {
					for (const SoundItem &it : SOUND_ITEMS) if (it.key == k) st.press(it);
					if (k == SDLK_4) { st.ship_on = !st.ship_on; st.ship(); }
					if (k == SDLK_5) { st.ship_ab = (st.ship_ab + 1) & 3; st.ship(); }
					if (k == SDLK_SPACE) { st.port[0] = st.port[1] = st.port[2] = 0xff; st.syn.reset(); st.ship_on = 0; }
					if (k == SDLK_B) { st.port[0] = st.port[1] = st.port[2] = 0xff; st.syn.set_board(!st.syn.board_super()); st.ship_on = 0; }
					if (k == SDLK_M) { O.vca = !O.vca; st.syn.set_vca_model(O.vca); settings_save(); }
					if (k == SDLK_6) { st.ship_c = !st.ship_c; st.ship(); }
					if (k == SDLK_N) { O.engine = !O.engine; st.syn.set_engine_style(O.engine); settings_save(); }
				}
			}
		}
		if (!running) break;

		/* a path chosen by drop / dialog */
		std::string pend; { std::lock_guard<std::mutex> l(g_pm); pend.swap(g_pending); }
		if (!pend.empty()) {
			bool both = false;
			const int set = add_rom_path(pend, both);
			if (set < 0) SDL_SetWindowTitle(win, "Zaxxon - those ROMs were not recognised");
			else if (both) { picking = true; have_game = false; menu = false; psel = O.game; settings_save(); SDL_SetWindowTitle(win, base_title().c_str()); }
			else start_game(set);
		}

		/* pacing: keep three to four frames of audio queued (see Q_HIGH) */
		if (as) { while (SDL_GetAudioStreamQueued(as) > Q_HIGH && running) SDL_Delay(1); }
		else {
			Uint64 now = SDL_GetTicksNS();
			if (now < next_ns) SDL_DelayNS(next_ns - now);
			next_ns += 16666667;
			if (SDL_GetTicksNS() > next_ns + 100000000ULL) next_ns = SDL_GetTicksNS();
		}

		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_RenderClear(ren);

		if (sound_test) {
			int16_t buf[FRAME_SMP];
			st.frame(buf);
			if (as) SDL_PutAudioStreamData(as, buf, sizeof buf);
			int y = 8;
			text(6, (float)y, "SOUND TEST", 255, 255, 0); y += 14;
			for (const SoundItem &it : SOUND_ITEMS) {
				const bool on = !(st.port[it.port] & it.bit);
				text(6, (float)y, it.name, on ? 255 : 200, on ? 255 : 200, on ? 0 : 200); y += 11;
			}
			char t[64]; snprintf(t, sizeof t, "4  Player ship %s", st.ship_on ? "(on)" : "(off)"); text(6, (float)y, t, st.ship_on ? 255 : 200, st.ship_on ? 255 : 200, st.ship_on ? 0 : 200); y += 11;
			snprintf(t, sizeof t, "5  Ship A/B bits: %d", st.ship_ab); text(6, (float)y, t, 200, 200, 200); y += 11;
			snprintf(t, sizeof t, "6  Ship C bit: %d", st.ship_c); text(6, (float)y, t, 200, 200, 200); y += 11;
			snprintf(t, sizeof t, "B  Board: %s", st.syn.board_super() ? "Super Zaxxon" : "Zaxxon"); text(6, (float)y, t, 200, 200, 200); y += 11;
			snprintf(t, sizeof t, "M  Sound chip: %s", O.vca ? "MC3340" : "MB4391"); text(6, (float)y, t, 200, 200, 200); y += 11;
			snprintf(t, sizeof t, "N  Engine: %s", O.engine ? "MAME-style" : "Board"); text(6, (float)y, t, 200, 200, 200); y += 11;
			text(6, (float)y, "Space  silence all", 200, 200, 200); y += 11;
			text(6, (float)y, "+ -    volume", 200, 200, 200);
		} else {
			if (have_game) {
				uint8_t s0 = 0, s100 = 0;
				const bool *k = SDL_GetKeyboardState(nullptr);
				bool coin = k[SDL_SCANCODE_5] || k[SDL_SCANCODE_6], svc = k[SDL_SCANCODE_F2];
				if (k[SDL_SCANCODE_RIGHT]) s0 |= 0x01;
				if (k[SDL_SCANCODE_LEFT])  s0 |= 0x02;
				if (k[SDL_SCANCODE_DOWN])  s0 |= 0x04;
				if (k[SDL_SCANCODE_UP])    s0 |= 0x08;
				if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL] || k[SDL_SCANCODE_SPACE]) s0 |= 0x10;
				if (k[SDL_SCANCODE_1]) s100 |= 0x04;
				if (k[SDL_SCANCODE_2]) s100 |= 0x08;
				for (SDL_Gamepad *g : pads) {
					const int ax = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
					if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || ax > 14500) s0 |= 0x01;
					if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT)  || ax < -14500) s0 |= 0x02;
					if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN)  || ay > 14500) s0 |= 0x04;
					if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP)    || ay < -14500) s0 |= 0x08;
					if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH) || SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST) || SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_WEST)) s0 |= 0x10;
					if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)) s100 |= 0x04;
					if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) s100 |= 0x08;
					if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK) || SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) coin = true;
				}
				if (menu) { s0 = s100 = 0; coin = svc = false; }
				if (coin && !prev_coin) M.coin_pressed(0);
				if (svc && !prev_svc) M.service_nmi();
				prev_coin = coin; prev_svc = svc;
				M.sw00 = s0; M.sw01 = 0; M.sw100 = s100;
				if (!paused && !menu) {
					M.run_frame();
					if (as) SDL_PutAudioStreamData(as, M.audio.data(), (int)(M.audio.size() * sizeof(int16_t)));
					for (int extra = 0; extra < 3 && as && SDL_GetAudioStreamQueued(as) < Q_LOW; extra++) {
						M.run_frame();
						SDL_PutAudioStreamData(as, M.audio.data(), (int)(M.audio.size() * sizeof(int16_t)));
					}
				} else SDL_Delay(8);
				int w, h; make_view(M.fb, O.rotate ? 1 : 0, view, w, h);
				SDL_Rect r = { 0, 0, w, h };
				SDL_UpdateTexture(tex, &r, view.data(), w * 4);
				SDL_FRect src = { 0, 0, (float)w, (float)h };
				SDL_RenderTexture(ren, tex, &src, nullptr);
			} else if (picking) {
				text(8, 24, "Choose a game", 255, 255, 0);
				text(8, 34, "-------------", 120, 120, 120);
				for (int i = 0; i < 2; i++) {
					const int set = i == 0 ? SET_ZAXXON : SET_SZAXXON;
					const bool on = psel == set;
					const float y = 60.0f + 20.0f * i;
					if (on) text(8, y, ">", 255, 255, 0);
					text(20, y, SET_NAMES[set], on ? 255 : 200, on ? 255 : 200, on ? 0 : 200);
				}
				text(8, 120, "Up/Down: choose", 160, 160, 160);
				text(8, 130, "Enter / fire: play", 160, 160, 160);
				text(8, 140, "Esc / Tab: menu", 160, 160, 160);
				text(8, 160, "Switch games later from", 130, 130, 130);
				text(8, 170, "the menu (Esc).", 130, 130, 130);
				if (as) { int16_t z[FRAME_SMP] = {}; SDL_PutAudioStreamData(as, z, sizeof z); }
			} else {
				text(8, 24, "No ROMs found.", 255, 255, 0);
				text(8, 44, "Drop szaxxon.zip or", 220, 220, 220);
				text(8, 54, "zaxxon.zip (or its", 220, 220, 220);
				text(8, 64, "folder) on this window,", 220, 220, 220);
				text(8, 74, "press O to pick the zip,", 220, 220, 220);
				text(8, 84, "or F to pick a folder.", 220, 220, 220);
				text(8, 104, "The folder is remembered:", 160, 160, 160);
				text(8, 114, "put both zips in it to", 160, 160, 160);
				text(8, 124, "choose between the games.", 160, 160, 160);
				text(8, 144, "Esc / Tab: menu", 160, 160, 160);
				if (as) { int16_t z[FRAME_SMP] = {}; SDL_PutAudioStreamData(as, z, sizeof z); }
			}
			if (menu) {
				SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
				SDL_SetRenderDrawColor(ren, 0, 0, 0, 215);
				SDL_RenderFillRect(ren, nullptr);
				SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
				float y = 10;
				text(8, y, have_game ? SET_NAMES[M.set_id] : "Zaxxon", 255, 255, 0); y += 8;
				text(8, y, "-----------------------", 120, 120, 120); y += 12;
				const float gap = cur_h >= 256 ? 13.0f : 9.0f;   /* group spacing only when the screen is tall enough */
				for (int i = 0; i < MI_COUNT; i++) {
					char b[64]; item_text(i, b, sizeof b);
					const bool on = i == msel;
					if (on) text(0, y, ">", 255, 255, 0);
					text(10, y, b, on ? 255 : 210, on ? 255 : 210, on ? 0 : 210);
					y += (i == MI_GAME || i == MI_INVINCIBLE || i == MI_DIFF || i == MI_FULLSCREEN) ? gap : 9.0f;
				}
				y += 4;
				text(8, y, "Arrows/Enter: change", 130, 130, 130); y += 9;
				text(8, y, "Esc / Tab close", 130, 130, 130); y += 9;
				text(8, y, "Lives/bonus: next game", 130, 130, 130);
			}
		}
		SDL_RenderPresent(ren);
	}

	settings_save();
	for (SDL_Gamepad *g : pads) SDL_CloseGamepad(g);
	if (as) SDL_DestroyAudioStream(as);
	SDL_DestroyTexture(tex);
	if (font_tex) SDL_DestroyTexture(font_tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
