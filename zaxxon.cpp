/*
 * Super Zaxxon (Sega, 1982) - a small standalone emulator with synthesized sound.
 *
 * The game board is emulated after MAME's zaxxon driver; the discrete analog sound board is not
 * played back from samples but calculated from a model of its circuit (zaxxon_synth.h).
 * You need to supply the ROMs yourself (MAME "szaxxon.zip").
 */
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

using namespace zx;

static Machine M;

static void usage()
{
	fputs(
"Super Zaxxon emulator with synthesized (sample-free) sound\n"
"usage: zaxxon [options] [szaxxon.zip | rom-folder] ...\n"
"  With no path, a folder called 'roms' next to the program (or the current folder) is searched.\n"
"  You can also drop a zip or a folder on the window, or press O (zip) / F (folder) to browse.\n\n"
"  --scale N        window scale (default 3)\n"
"  --no-rotate      do not rotate the picture 90 degrees\n"
"  --volume X       master volume, 0..2 (default 1)\n"
"  --lives N        3, 4 or 5 (default 3);  --free-play\n"
"  --bonus N        10000, 20000, 30000 or 40000 (default 10000)\n"
"  --sound-test     the sound board on its own (no ROMs needed): press keys to trigger every sound\n"
"  --sound-demo F   write every sound, one after the other, to the WAV file F and exit (no ROMs needed)\n"
"  --headless       no window or audio device; run --frames N and exit\n"
"  --frames N       frames to run in headless mode (default 600)\n"
"  --wav FILE       headless: write the audio to a WAV file\n"
"  --shot FILE      headless: save the last frame as a BMP\n"
"  --shot-every N   headless: also save every Nth frame as FILE-<n>.bmp\n"
"  --coin-at N      headless: insert a coin at frame N;  --start-at N presses 1P start\n"
"  -h, --help       this text\n\n"
"keys: arrows move, Z / Ctrl / Space fire, 5 coin, 1 / 2 start, F2 service (test mode),\n"
"      P pause, R rotate, F3 reset, F11 fullscreen, F12 screenshot, + / - volume, Esc quit\n", stderr);
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
	ZaxSynth syn;
	uint8_t port[3] = { 0xff, 0xff, 0xff };
	int pulse[3][8] = {};
	int ship_on = 0, ship_ab = 0;                 /* player ship: on/off and the A/B bits (0..3) */
	void set(int p, uint8_t v) { if (port[p] != v) { port[p] = v; syn.ppi_write(p, v); } }
	void press(const SoundItem &it)
	{
		const int b = it.bit == 0x80 ? 7 : it.bit == 0x40 ? 6 : it.bit == 0x20 ? 5 : it.bit == 0x10 ? 4 : it.bit == 8 ? 3 : it.bit == 4 ? 2 : it.bit == 2 ? 1 : 0;
		if (it.hold) set(it.port, port[it.port] ^ it.bit);
		else { set(it.port, port[it.port] & ~it.bit); pulse[it.port][b] = 6; }
	}
	void ship()
	{
		/* D low = engine running; A/B (bits 0,1) set the pitch / volume; C stays high */
		set(0, (uint8_t)((port[0] & 0xf0) | 0x04 | (ship_ab & 3) | (ship_on ? 0 : 0x08)));
	}
	void frame(int16_t *out)
	{
		for (int p = 0; p < 3; p++)
			for (int b = 0; b < 8; b++)
				if (pulse[p][b] > 0 && --pulse[p][b] == 0) set(p, port[p] | (1 << b));
		syn.render(out, FRAME_SMP);
	}
};

static void sound_demo(const char *path)
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
		ZaxSynth syn; uint8_t p[3] = { 0xff, 0xff, 0xff };
		const int n = (int)(s.total * RATE), non = (int)(s.on * RATE);
		std::vector<int16_t> b(n);
		syn.render(b.data(), 2400);
		p[s.port] &= ~s.bit; syn.ppi_write(s.port, p[s.port]);
		syn.render(b.data() + 2400, non);
		p[s.port] |= s.bit; syn.ppi_write(s.port, p[s.port]);
		syn.render(b.data() + 2400 + non, n - 2400 - non);
		all.insert(all.end(), b.begin(), b.end());
		all.insert(all.end(), RATE / 2, 0);
	}
	/* player ship: A/B = 0..3 with D low (the values the game uses are f4..f7) */
	for (int ab = 0; ab < 4; ab++) {
		ZaxSynth syn; std::vector<int16_t> b(3 * RATE);
		syn.render(b.data(), 2400);
		syn.ppi_write(0, (uint8_t)(0xf4 | ab)); syn.render(b.data() + 2400, 2 * RATE);
		syn.ppi_write(0, 0xff); syn.render(b.data() + 2400 + 2 * RATE, RATE - 2400);
		all.insert(all.end(), b.begin(), b.end());
		all.insert(all.end(), RATE / 2, 0);
	}
	write_wav(path, all);
	fprintf(stderr, "wrote %s (%.1f s)\n", path, all.size() / (double)RATE);
}

/* ------------------------------------------------------------------ file dialogs */
static std::mutex g_pm; static std::string g_pending;
static void SDLCALL dialog_cb(void *, const char *const *list, int)
{
	if (list && list[0]) { std::lock_guard<std::mutex> l(g_pm); g_pending = list[0]; }
}

/* ------------------------------------------------------------------ main */
static bool boot(const std::vector<std::string> &paths, bool search_default, uint8_t dsw02, float volume)
{
	std::vector<rz::RomFile> files;
	if (paths.empty() && search_default) {
		rz::add_path("roms", files); rz::add_path(".", files);
		const char *bp = SDL_GetBasePath();
		if (bp) { rz::add_path(std::string(bp) + "roms", files); rz::add_path(bp, files); }
	} else for (auto &p : paths) rz::add_path(p, files);
	static RomSet rs;
	std::string missing;
	if (!load_roms(files, rs, missing)) {
		if (!files.empty()) fprintf(stderr, "Missing ROMs:\n%s", missing.c_str());
		return false;
	}
	M.load(rs);
	M.dsw02 = dsw02; M.volume = volume;
	M.reset();
	return true;
}

int main(int argc, char **argv)
{
	int scale = 3, rotation = 1;
	float volume = 1.0f;
	int lives = 3, bonus = 10000; bool free_play = false;
	bool headless = false, sound_test = false;
	int frames_to_run = 600, shot_every = 0, coin_at = -1, start_at = -1;
	std::string wav_path, shot_path, demo_path;
	std::vector<std::string> paths;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&](const char *n) -> const char * {
			if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", n); exit(2); }
			return argv[++i];
		};
		if (a == "-h" || a == "--help") { usage(); return 0; }
		else if (a == "--scale") scale = std::max(1, atoi(next("--scale")));
		else if (a == "--no-rotate") rotation = 0;
		else if (a == "--volume") volume = (float)atof(next("--volume"));
		else if (a == "--lives") lives = atoi(next("--lives"));
		else if (a == "--free-play") free_play = true;
		else if (a == "--bonus") bonus = atoi(next("--bonus"));
		else if (a == "--sound-test") sound_test = true;
		else if (a == "--sound-demo") demo_path = next("--sound-demo");
		else if (a == "--headless") headless = true;
		else if (a == "--frames") frames_to_run = atoi(next("--frames"));
		else if (a == "--wav") wav_path = next("--wav");
		else if (a == "--shot") shot_path = next("--shot");
		else if (a == "--shot-every") shot_every = atoi(next("--shot-every"));
		else if (a == "--coin-at") coin_at = atoi(next("--coin-at"));
		else if (a == "--start-at") start_at = atoi(next("--start-at"));
		else if (a.size() > 1 && a[0] == '-') { fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 1; }
		else paths.push_back(a);
	}
	if (!demo_path.empty()) { sound_demo(demo_path.c_str()); return 0; }

	uint8_t dsw02;
	{
		int b = bonus == 20000 ? 0x01 : bonus == 30000 ? 0x02 : bonus == 40000 ? 0x00 : 0x03;
		int l = free_play ? 0x00 : lives == 4 ? 0x10 : lives == 5 ? 0x20 : 0x30;
		dsw02 = (uint8_t)(b | 0x04 | l | 0x40);
	}

	/* ---------------- headless ---------------- */
	if (headless) {
		if (!boot(paths, true, dsw02, volume)) {
			fprintf(stderr, "Could not find all the ROMs (szaxxon.zip). Pass the zip or folder on the command line.\n");
			return 1;
		}
		std::vector<int16_t> au;
		for (int f = 0; f < frames_to_run; f++) {
			M.sw100 = 0;
			if (f == coin_at) M.coin_pressed(0);
			if (start_at >= 0 && f >= start_at && f < start_at + 8) M.sw100 |= 0x04;
			M.run_frame();
			if (!wav_path.empty()) au.insert(au.end(), M.audio.begin(), M.audio.end());
			if (shot_every > 0 && !shot_path.empty() && f % shot_every == 0) {
				std::vector<uint32_t> v; int w, h; make_view(M.fb, rotation, v, w, h);
				char n[512]; snprintf(n, sizeof n, "%s-%d.bmp", shot_path.c_str(), f);
				write_bmp(n, v.data(), w, h);
			}
		}
		if (!shot_path.empty()) { std::vector<uint32_t> v; int w, h; make_view(M.fb, rotation, v, w, h); write_bmp(shot_path.c_str(), v.data(), w, h); }
		if (!wav_path.empty()) write_wav(wav_path.c_str(), au);
		return 0;
	}

	/* ---------------- windowed ---------------- */
	bool have_game = false;
	if (!sound_test) have_game = boot(paths, true, dsw02, volume);

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
	SDL_InitSubSystem(SDL_INIT_GAMEPAD);
	SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
	const int vw = rotation ? SCR_H : SCR_W, vh = rotation ? SCR_W : SCR_H;
	SDL_Window *win = SDL_CreateWindow(sound_test ? "Super Zaxxon - sound test" : "Super Zaxxon", vw * scale, vh * scale, SDL_WINDOW_RESIZABLE);
	SDL_Renderer *ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
	if (!win || !ren) { fprintf(stderr, "window/renderer: %s\n", SDL_GetError()); return 1; }
	SDL_SetRenderVSync(ren, 1);
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 256);
	SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
	int cur_w = vw, cur_h = vh;
	auto set_logical = [&]() { SDL_SetRenderLogicalPresentation(ren, cur_w, cur_h, SDL_LOGICAL_PRESENTATION_LETTERBOX); };
	set_logical();

	SDL_AudioSpec spec; SDL_zero(spec);
	spec.format = SDL_AUDIO_S16; spec.channels = 1; spec.freq = RATE;
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
	SDL_AudioStream *as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (!as) fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
	const int Q_HIGH = 3 * FRAME_SMP * 2, Q_LOW = FRAME_SMP * 2;
	if (as) SDL_ResumeAudioStreamDevice(as);

	std::vector<SDL_Gamepad *> pads;
	auto open_pad = [&](SDL_JoystickID id) { SDL_Gamepad *g = SDL_OpenGamepad(id); if (g) { pads.push_back(g); fprintf(stderr, "controller: %s\n", SDL_GetGamepadName(g)); } };
	{ int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n); for (int i = 0; i < n; i++) open_pad(ids[i]); SDL_free(ids); }

	SoundTest st;
	bool running = true, paused = false, prev_coin = false, prev_svc = false;
	int shot_n = 0;
	std::vector<uint32_t> view;
	Uint64 next_ns = SDL_GetTicksNS();

	auto text = [&](float x, float y, const char *s, int r, int g, int b) {
		SDL_SetRenderDrawColor(ren, (Uint8)r, (Uint8)g, (Uint8)b, 255);
		SDL_RenderDebugText(ren, x, y, s);
	};
	auto apply_volume = [&]() { M.volume = volume; };

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
			else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
				const SDL_Keycode k = ev.key.key;
				if (k == SDLK_ESCAPE) running = false;
				else if (k == SDLK_P) paused = !paused;
				else if (k == SDLK_R && !sound_test) { rotation = !rotation; cur_w = rotation ? SCR_H : SCR_W; cur_h = rotation ? SCR_W : SCR_H; set_logical(); }
				else if (k == SDLK_F3 && have_game) M.reset();
				else if (k == SDLK_F11) SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
				else if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) { volume = std::min(2.0f, volume + 0.1f); apply_volume(); }
				else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) { volume = std::max(0.0f, volume - 0.1f); apply_volume(); }
				else if (k == SDLK_F12 && have_game) {
					make_view(M.fb, rotation, view, cur_w, cur_h);
					char n[64]; snprintf(n, sizeof n, "zaxxon-%03d.bmp", shot_n++);
					write_bmp(n, view.data(), cur_w, cur_h); fprintf(stderr, "saved %s\n", n);
				}
				else if (k == SDLK_O && !sound_test) { static const SDL_DialogFileFilter f[] = { { "ROM zip", "zip" } }; SDL_ShowOpenFileDialog(dialog_cb, nullptr, win, f, 1, nullptr, false); }
				else if (k == SDLK_F && !sound_test) SDL_ShowOpenFolderDialog(dialog_cb, nullptr, win, nullptr, false);
				if (sound_test) {
					for (const SoundItem &it : SOUND_ITEMS) if (it.key == k) st.press(it);
					if (k == SDLK_4) { st.ship_on = !st.ship_on; st.ship(); }
					if (k == SDLK_5) { st.ship_ab = (st.ship_ab + 1) & 3; st.ship(); }
					if (k == SDLK_SPACE) { st.port[0] = st.port[1] = st.port[2] = 0xff; st.syn.reset(); st.ship_on = 0; }
				}
			}
		}
		if (!running) break;

		/* a path chosen by drop / dialog */
		std::string pend; { std::lock_guard<std::mutex> l(g_pm); pend.swap(g_pending); }
		if (!pend.empty()) {
			if (boot({ pend }, false, dsw02, volume)) { have_game = true; SDL_SetWindowTitle(win, "Super Zaxxon"); if (as) SDL_ClearAudioStream(as); }
			else SDL_SetWindowTitle(win, "Super Zaxxon - those ROMs were not recognised");
		}

		/* pacing: keep about two frames of audio queued */
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
				const int b = it.bit == 0x80 ? 7 : it.bit == 0x40 ? 6 : it.bit == 0x20 ? 5 : it.bit == 0x10 ? 4 : it.bit == 8 ? 3 : it.bit == 4 ? 2 : it.bit == 2 ? 1 : 0;
				const bool on = !(st.port[it.port] & it.bit); (void)b;
				text(6, (float)y, it.name, on ? 255 : 200, on ? 255 : 200, on ? 0 : 200); y += 11;
			}
			char t[64]; snprintf(t, sizeof t, "4  Player ship %s", st.ship_on ? "(on)" : "(off)"); text(6, (float)y, t, st.ship_on ? 255 : 200, st.ship_on ? 255 : 200, st.ship_on ? 0 : 200); y += 11;
			snprintf(t, sizeof t, "5  Ship A/B bits: %d", st.ship_ab); text(6, (float)y, t, 200, 200, 200); y += 11;
			text(6, (float)y, "Space  silence all", 200, 200, 200); y += 11;
			text(6, (float)y, "+ -    volume", 200, 200, 200);
		} else if (!have_game) {
			text(8, 24, "No ROMs found.", 255, 255, 0);
			text(8, 44, "Drop szaxxon.zip (or its", 220, 220, 220);
			text(8, 54, "folder) on this window,", 220, 220, 220);
			text(8, 64, "press O to pick the zip,", 220, 220, 220);
			text(8, 74, "or F to pick a folder.", 220, 220, 220);
			if (as) { int16_t z[FRAME_SMP] = {}; SDL_PutAudioStreamData(as, z, sizeof z); }
		} else {
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
			if (coin && !prev_coin) M.coin_pressed(0);
			if (svc && !prev_svc) M.service_nmi();
			prev_coin = coin; prev_svc = svc;
			M.sw00 = s0; M.sw01 = 0; M.sw100 = s100;
			if (!paused) {
				M.run_frame();
				if (as) SDL_PutAudioStreamData(as, M.audio.data(), (int)(M.audio.size() * sizeof(int16_t)));
				for (int extra = 0; extra < 3 && as && SDL_GetAudioStreamQueued(as) < Q_LOW; extra++) {
					M.run_frame();
					SDL_PutAudioStreamData(as, M.audio.data(), (int)(M.audio.size() * sizeof(int16_t)));
				}
			}
			int w, h; make_view(M.fb, rotation, view, w, h);
			SDL_Rect r = { 0, 0, w, h };
			SDL_UpdateTexture(tex, &r, view.data(), w * 4);
			SDL_FRect src = { 0, 0, (float)w, (float)h };
			SDL_RenderTexture(ren, tex, &src, nullptr);
		}
		SDL_RenderPresent(ren);
	}

	for (SDL_Gamepad *g : pads) SDL_CloseGamepad(g);
	if (as) SDL_DestroyAudioStream(as);
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
