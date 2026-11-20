/*
 * Super Zaxxon discrete sound circuits ("IC Board A - Super ZAXXON", drawing 800-3300, sheets 3 and 4).
 *
 * No audio samples are used.  Every sound is computed from a model of its circuit, with the component
 * values read from the schematic:
 *
 *   - MM5837 digital noise source (17-bit LFSR, taps 14/17 as in MAME's mm5837.h, ~103 kHz at 12 V)
 *   - 74123 one-shots (t = 0.28 * R * C * (1 + 0.7/R[k]))          -> trigger / gate pulses
 *   - RC envelope networks with diodes                              -> the explosion / missile / shot envelopes
 *   - MB4391 voltage controlled attenuators -> modelled as an MC3340 (Figure 3 of its datasheet, +13 dB maximum gain);
 *     used for the player-ship engine.  The explosion / missile / cannon / shot loudness envelopes are CALIBRATED to the
 *     decay curves of the MAME sample files instead (see DbEnv), because the circuit-derived ones were too short.
 *   - Sallen-Key low-pass filters (U3), bilinear-transformed biquads
 *   - Op-amp / comparator relaxation oscillators with transistor switches (battleship, laser, shot)
 *   - 555 timers (laser LFO, homing missile), 556 + 74393 divider (alarms)
 *   - CdS photocell (PC1) tuned resonant band-pass for the engine (player ship A/B)
 *   - the output summing network (R209/R210 ...) for the relative loudness of each channel
 *
 * Where the drawing was ambiguous the choice made is marked  [ASSUMPTION]  so it can be revisited
 * (see README.md, "Sound: what is modelled and how sure it is").
 *
 * 8255 outputs are active low: a 0 activates / triggers.  One-shots trigger on the 1 -> 0 edge.
 */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

struct ZaxSynth {
	static constexpr double FS = 48000.0;
	static constexpr int    OS = 8;                 /* oversampling of the oscillator circuits */

	/* ------------------------------------------------------------- helpers */
	struct Biquad {
		double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
		/* analog prototype (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0), bilinear transform */
		void set_analog(double n2, double n1, double n0, double d2, double d1, double d0, double fs)
		{
			const double c = 2 * fs, c2 = c * c;
			const double a0 = d2 * c2 + d1 * c + d0;
			b0 = (n2 * c2 + n1 * c + n0) / a0;
			b1 = (2 * n0 - 2 * n2 * c2) / a0;
			b2 = (n2 * c2 - n1 * c + n0) / a0;
			a1 = (2 * d0 - 2 * d2 * c2) / a0;
			a2 = (d2 * c2 - d1 * c + d0) / a0;
		}
		/* 2nd-order Sallen-Key low pass: gain K, corner f0, quality Q */
		void set_sk_lp(double f0, double Q, double K, double fs)
		{
			const double w = 2 * M_PI * f0;
			set_analog(0, 0, K * w * w, 1, w / Q, w * w, fs);
		}
		double run(double x)
		{
			double y = b0 * x + z1;
			z1 = b1 * x - a1 * y + z2;
			z2 = b2 * x - a2 * y;
			return y;
		}
		void clear() { z1 = z2 = 0; }
	};
	struct HP1 {                                   /* coupling capacitor: first-order high pass */
		double a = 0, x1 = 0, y1 = 0;
		void set(double tau) { a = tau / (tau + 1.0 / FS); }
		double run(double x) { double y = a * (y1 + x - x1); x1 = x; y1 = y; return y; }
	};
	struct LP1 {
		double a = 0, y = 0;
		void set(double tau) { a = 1.0 - std::exp(-1.0 / (FS * tau)); }
		double run(double x) { y += a * (x - y); return y; }
	};
	static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
	static double rc_step(double v, double target, double dt, double tau) { return v + (target - v) * (1.0 - std::exp(-dt / tau)); }
	/* MB4391 voltage-controlled attenuator.  [ASSUMPTION] The MB4391 behaves like a Motorola MC3340 (8-pin electronic attenuator;
	   a forum report says two MC3340s replace one MB4391, and the 680 pF on the RO pin matches the MC3340's 620 pF roll-off cap).
	   Curve read off the MC3340 datasheet, Figure 3, for VCC = 12 V (solid line): 0 dB below about 3.0 V, a soft knee to 20 dB at
	   3.59 V, then about 45 dB per volt (80 dB at 4.9 V).  The maximum gain is +13 dB (datasheet typical). */
	static double vca_atten_db(double vcon)
	{
		double a;
		if (vcon <= 3.0) a = 0.0;
		else if (vcon < 3.59) a = 20.0 * std::pow((vcon - 3.0) / 0.59, 1.3);
		else a = 20.0 + 45.0 * (vcon - 3.59);
		return a > 100.0 ? 100.0 : a;
	}
	static double vca(double vcon)
	{
		return 4.467 * std::pow(10.0, -vca_atten_db(vcon) / 20.0);      /* +13 dB at full volume */
	}
	/* 74123: pulse width in seconds for R in ohms, C in farads */
	static double oneshot_tw(double R, double C) { return 0.28 * R * C * (1.0 + 0.7 / (R / 1000.0)); }

	struct OneShot {
		double tw = 0, left = 0;
		void trigger() { left = tw; }              /* retriggerable */
		bool active() const { return left > 0; }
		void tick(double dt) { if (left > 0) left -= dt; }
	};

	/* loudness envelope read off the MAME sample files: (seconds, dB) points, linear in dB between them, silent after the last */
	struct DbPt { double t, db; };
	struct DbEnv {
		const DbPt *p = nullptr; int n = 0; double t = 1e9;
		void set(const DbPt *pts, int cnt) { p = pts; n = cnt; t = 1e9; }
		void trigger() { t = 0; }
		void tick(double dt) { if (t < 1e8) t += dt; }
		bool active() const { return p && t < p[n - 1].t; }
		double gain() const
		{
			if (!active()) return 0.0;
			int i = 1; while (i < n - 1 && t > p[i].t) i++;
			const double f = (t - p[i - 1].t) / (p[i].t - p[i - 1].t);
			return std::pow(10.0, (p[i - 1].db + f * (p[i].db - p[i - 1].db)) / 20.0);
		}
	};
	/* Op-amp / MB4391 saturation: the real board's noise stages run into the op-amp rails and the mixer, which fattens the sound
	   (the MAME explosion samples have only 4 dB between RMS and peak; plain filtered noise has 12-15 dB).  x is scaled by its
	   typical RMS `sigma`, driven into tanh, and scaled back. */
	static double sat(double x, double sigma, double drive) { return sigma * std::tanh(drive * x / sigma); }

	/* band-pass state-variable filter (topology-preserving), unity gain at the centre frequency */
	struct SVF {
		double ic1 = 0, ic2 = 0, a1 = 0, a2 = 0, a3 = 0, k = 1;
		void set(double f, double Q)
		{
			const double g = std::tan(M_PI * f / FS); k = 1.0 / Q;
			a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2;
		}
		double bp(double x)
		{
			const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
			ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
			return k * v1;
		}
		void clear() { ic1 = ic2 = 0; }
	};

	/* ------------------------------------------------------------- state */
	uint8_t port[3] = { 0xff, 0xff, 0xff };

	/* MM5837 noise */
	uint32_t lfsr = 0x1ffff; double noise_phase = 0;
	static constexpr double NOISE_CLK = 103200.0, NOISE_VPP = 7.5;

	/* explosion / missile / cannon / shot loudness envelopes (dB tables measured from the MAME samples) */
	DbEnv e_sexp, e_mexp, e_base, e_cannon, e_shot;
	Biquad lp_sexp, lp_mexp;
	HP1 hp_sexp, hp_mexp;
	SVF bp_base, bp_cannon;
	double cannon_t = 10.0;

	/* shot */
	double shot_t = 10.0, shot_phase = 0;

	/* alarms */
	OneShot os_a2, os_a3; double alarm_phase2 = 0, alarm_phase3 = 0;
	double bs_phase = 0;

	/* battleship */
	/* laser */
	double lz_v = 4.0; bool lz_charge = true;      /* 555 timing capacitor */
	double lz_vf = 5.0; bool lz_fhi = true;        /* laser VCO */
	/* homing missile */
	double hm_cv = 0.2; bool hm_cmp = true;        /* C43 node and its Schmitt trigger */
	double hm_v = 1.0; bool hm_charging = true;    /* 555 capacitor */
	HP1 hp_hm_noise, hp_hm_c46; double hm_lastdc = 0;
	double hm_dc = 6.85;

	/* engine (player ship A..D) */
	double n2_v = 0.76;                            /* U8 buffer / C25 voltage (drives the LED) */
	LP1 lp_n2; double rcell = 1e7; double last_rcell = 0; int eng_upd = 0;
	Biquad eng_t, eng_lp1, eng_lp2; HP1 hp_eng;
	double eng_c1 = 6.0, eng_c2 = 6.0;             /* C34 / C35 node voltages */

	/* output */
	HP1 hp_out;
	static constexpr double DRIVE_EXP = 4.0;

	/* Loudness envelopes: (seconds after the trigger, dB) taken from the 50 ms RMS curves of the MAME sample files */
	static constexpr DbPt ENV_SEXP[]   = { {0,-1},{0.1,0},{0.25,-1},{0.5,-5},{0.75,-8.5},{1.0,-13},{1.25,-21},{1.5,-31},{1.72,-60} };
	static constexpr DbPt ENV_MEXP[]   = { {0,-1},{0.1,-1},{0.25,0},{0.5,0},{1.0,0},{1.5,-1},{2.0,-2},{2.5,-10},{3.0,-23},{3.4,-40},{3.73,-70} };
	static constexpr DbPt ENV_BASE[]   = { {0,-1},{0.1,-1},{0.25,-2},{0.5,-2},{0.68,-3},{0.72,-40} };
	static constexpr DbPt ENV_CANNON[] = { {0,0},{0.1,-2},{0.25,-6},{0.5,-9},{0.75,-17},{1.0,-32},{1.15,-60} };
	static constexpr DbPt ENV_SHOT[]   = { {0,-1},{0.1,0},{0.25,0},{0.5,-8},{0.7,-20},{0.97,-50} };

	/* Per-channel trim on top of the levels derived from the summing resistors.  The derived levels are
	   only as good as my reading of the drawing; these are an ear-calibrated nudge (1.0 = as derived) so
	   that no sound is buried.  Order: s-exp, m-exp, cannon, shot, base missile, alarms, battleship,
	   laser, homing missile, player ship. */
	double trim[10] = { 57.69, 25.6, 25.3, 4.457, 302.8, 7.624, 2.209, 9.038, 3.452, 19.1 };

	/* ------------------------------------------------------------- init */
	ZaxSynth() { reset(); }

	void reset()
	{
		port[0] = port[1] = port[2] = 0xff;
		lfsr = 0x1ffff; noise_phase = 0;
		e_sexp.set(ENV_SEXP, sizeof ENV_SEXP / sizeof ENV_SEXP[0]);
		e_mexp.set(ENV_MEXP, sizeof ENV_MEXP / sizeof ENV_MEXP[0]);
		e_base.set(ENV_BASE, sizeof ENV_BASE / sizeof ENV_BASE[0]);
		e_cannon.set(ENV_CANNON, sizeof ENV_CANNON / sizeof ENV_CANNON[0]);
		e_shot.set(ENV_SHOT, sizeof ENV_SHOT / sizeof ENV_SHOT[0]);
		os_a2 = OneShot();     os_a2.tw = oneshot_tw(47e3, 10e-6);
		os_a3 = OneShot();     os_a3.tw = oneshot_tw(47e3, 10e-6);
		bp_base.clear(); bp_cannon.clear(); cannon_t = 10.0; bs_phase = 0;
		/* Sallen-Key low pass filters (R, C from the schematic): S-EXP U3b 15k/15k .033 uF (321 Hz, K 2.5, Q 2), M-EXP U3d .047 uF
		   (226 Hz).  The MAME samples peak at 273 Hz and 184 Hz, i.e. about 0.85 / 0.82 of those corners (component tolerance /
		   the real op-amp's finite gain), so the corners are scaled by that. */
		lp_sexp.set_sk_lp(0.82 / (2 * M_PI * 15e3 * 0.033e-6), 2.0, 2.5, FS);
		lp_mexp.set_sk_lp(0.80 / (2 * M_PI * 15e3 * 0.047e-6), 2.0, 2.5, FS);
		lp_sexp.clear(); lp_mexp.clear();
		hp_sexp.set(2.2e-6 * 20e3); hp_mexp.set(2.2e-6 * 20e3);
		/* base missile: narrow band around 335 Hz (the sample is 74 % in 200-400 Hz, 22 % in 400-800 Hz) */
		bp_base.set(325.0, 4.0);
		lz_v = 4.0; lz_charge = true; lz_vf = 5.0; lz_fhi = true;
		hm_cv = 0.2; hm_cmp = true; hm_v = 1.0; hm_charging = true;
		hp_hm_noise.set(1e-6 * 200e3); hp_hm_c46.set(33e-6 * 3.33e3);
		n2_v = 0.76; lp_n2.set(0.4); lp_n2.y = n2_v; rcell = 1e7; last_rcell = 0; eng_upd = 0;
		eng_t.clear(); eng_lp1.clear(); eng_lp2.clear();
		eng_lp1.set_sk_lp(1.0 / (2 * M_PI * 100e3 * 2200e-12), 1.0, 2.0, FS);     /* U5b: 100k/100k, 2200 pF, K = 1 + 2.2k/2.2k */
		eng_lp2.set_sk_lp(1.0 / (2 * M_PI * 100e3 * 3300e-12), 1.0, 2.0, FS);     /* U5c: 100k/100k, 3300 pF */
		hp_eng.set(2.2e-6 * 10e3);
		eng_c1 = eng_c2 = 6.0;
		hp_out.set(1.0 / (2 * M_PI * 15.0));
		shot_t = 10.0; shot_phase = 0;
		alarm_phase2 = alarm_phase3 = 0;
	}

	/* ------------------------------------------------------------- 8255 */
	void ppi_write(int p, uint8_t v)
	{
		const uint8_t old = port[p];
		port[p] = v;
		const uint8_t fall = old & ~v;                 /* bits that went 1 -> 0 */
		if (p == 0) {
			if (fall & 0x20) e_base.trigger();         /* base missile */
		} else if (p == 1) {
			if (fall & 0x10) e_sexp.trigger();         /* S-EXP */
			if (fall & 0x20) e_mexp.trigger();         /* M-EXP */
			if (fall & 0x80) { e_cannon.trigger(); cannon_t = 0; }   /* cannon */
		} else {
			if (fall & 0x01) { e_shot.trigger(); shot_t = 0; }   /* shot */
			if (fall & 0x04) os_a2.trigger();          /* alarm 2 */
			if (fall & 0x08) os_a3.trigger();          /* alarm 3 */
		}
	}

	/* ------------------------------------------------------------- noise */
	double next_noise()      /* MM5837 output in volts around its mean, averaged over this sample's LFSR clocks */
	{
		noise_phase += NOISE_CLK / FS;
		int n = (int)noise_phase; noise_phase -= n;
		if (n < 1) n = 1;
		double s = 0;
		for (int i = 0; i < n; i++) {
			int b = (((lfsr >> 13) ^ (lfsr >> 16)) & 1) ^ (lfsr == 0);
			lfsr = ((lfsr << 1) | b) & 0x1ffff;
			s += ((lfsr >> 16) & 1) ? 0.5 : -0.5;
		}
		return s / n * NOISE_VPP;
	}

	/* ------------------------------------------------------------- engine photocell network */
	/* solves the U8 input network (R10..R16, 7406 outputs from player ship A and B) for its DC level */
	static double engine_ctl_volts(bool a_on, bool b_on)
	{
		/* conductances in siemens */
		const double g13 = 1 / 82e3, g15 = 1 / 56e3, g14 = 1 / 36e3;
		const double ga = a_on ? 1 / (2.2e3 + 6.8e3) : 1 / 6.8e3, va = a_on ? 12.0 : 0.2;
		const double gb = b_on ? 1 / (2.2e3 + 100e3) : 1 / 100e3, vb = b_on ? 12.0 : 0.2;
		/* node 1:  (g13+g15+ga+g14) V1 - g14 V2 = g13*12 + ga*va
		   node 2:  (g14+gb) V2 - g14 V1 = gb*vb */
		const double a11 = g13 + g15 + ga + g14, a12 = -g14, b1 = g13 * 12 + ga * va;
		const double a21 = -g14, a22 = g14 + gb, b2 = gb * vb;
		const double det = a11 * a22 - a12 * a21;
		return (a11 * b2 - a21 * b1) / det;
	}

	void engine_update_filter(double rc)
	{
		/* U5a: noise -> Rz (10k) -> T node (C26, C27 = 0.01 uF, photocell to +6V) -> inverting amp with R21 = 470k
		   H(s) = - R21 C26 s / (1 + Rz/Rc + Rz (C26 + C27) s + Rz R21 C26 C27 s^2) */
		const double Rz = 10e3, R21 = 470e3, C = 0.01e-6;
		eng_t.set_analog(0, -R21 * C, 0, Rz * R21 * C * C, Rz * 2 * C, 1.0 + Rz / rc, FS);
	}

	/* ------------------------------------------------------------- one output sample */
	double tick()
	{
		const double dt = 1.0 / FS;
		const double n1 = next_noise();            /* NOISE 1 */
		const double n2 = -0.1 * n1;               /* NOISE 2: U3a, gain -R112/R111 = -0.1 */
		const uint8_t A = port[0];

		/* ---------------- envelopes ---------------- */
		e_sexp.tick(dt); e_mexp.tick(dt); e_base.tick(dt); e_cannon.tick(dt); e_shot.tick(dt);
		os_a2.tick(dt); os_a3.tick(dt);
		shot_t += dt; cannon_t += dt;

		double out = 0;

		/* ---------------- S-EXP ---------------- */
		if (e_sexp.active()) {
			double s = sat(hp_sexp.run(lp_sexp.run(n2)), 0.130, DRIVE_EXP);
			out += s * e_sexp.gain() * 0.154 * trim[0];
		} else { lp_sexp.clear(); }
		/* ---------------- M-EXP ---------------- */
		if (e_mexp.active()) {
			double s = sat(hp_mexp.run(lp_mexp.run(-0.30303 * n1 * 0.5)), 0.159, DRIVE_EXP);    /* U3c gain -100k/330k, then R123/R124 halve it */
			out += s * e_mexp.gain() * 0.365 * trim[1];
		} else { lp_mexp.clear(); }
		/* ---------------- BASE MISSILE ---------------- */
		if (e_base.active()) out += bp_base.bp(n2) * e_base.gain() * 0.21 * trim[4]; else bp_base.clear();
		/* ---------------- CANNON ---------------- */
		if (e_cannon.active()) {
			/* [CALIBRATED] noise through a resonant band-pass that sweeps from about 3.5 kHz down to 1.95 kHz in the first
			   ~0.15 s, then stays there (the sample's strongest frequency: 3517, 2004, 1876, 1966 Hz in successive 0.25 s) */
			bp_cannon.set(1800.0 + 1700.0 * std::exp(-cannon_t / 0.05), 3.2);
			out += bp_cannon.bp(n2) * e_cannon.gain() * 0.0342 * 10.0 * trim[2];
		} else bp_cannon.clear();
		/* ---------------- ALARMS ---------------- */
		if (os_a2.active() || os_a3.active()) {
			/* [CALIBRATED] the samples are square waves of 1141 Hz (alarm 2) and 2272 Hz (alarm 3): a 74393 divided by 16 and by 8
			   from a 556 running at about 18.2 kHz (my component-value estimate was 20.3 kHz) */
			const double f556 = 18220.0;
			double v = 0;
			if (os_a2.active()) { alarm_phase2 += f556 / 16 / FS; alarm_phase2 -= std::floor(alarm_phase2); v += (alarm_phase2 < 0.5 ? 1 : -1); }
			if (os_a3.active()) { alarm_phase3 += f556 / 8 / FS; alarm_phase3 -= std::floor(alarm_phase3); v += (alarm_phase3 < 0.5 ? 1 : -1); }
			out += v * 5.2 * 0.00714 * trim[5];
		}

		/* ---------------- engine (player ship) ---------------- */
		{
			const bool a_on = !(A & 0x01), b_on = !(A & 0x02);
			const double target = engine_ctl_volts(a_on, b_on);
			n2_v = lp_n2.run(target);
			const double iled = std::max(0.0, n2_v - 1.7) / 390.0 * 1000.0;       /* mA */
			rcell = iled < 0.05 ? 1e7 : std::min(1e7, 25e3 / std::pow(iled, 0.9));
			if (eng_upd-- <= 0 || std::fabs(rcell - last_rcell) > 0.05 * last_rcell) {
				engine_update_filter(rcell); last_rcell = rcell; eng_upd = 32;
			}
			const bool c_low = !(A & 0x04), d_low = !(A & 0x08);
			const bool y0 = c_low && d_low;                  /* 74LS139: both asserted */
			const bool y1 = !c_low && d_low;                 /* only D asserted */
			eng_c1 = y0 ? rc_step(eng_c1, 0.2, dt, 1e3 * 0.68e-6) : rc_step(eng_c1, 6.0, dt, 440e3 * 0.68e-6);
			eng_c2 = y1 ? rc_step(eng_c2, 0.2, dt, 1e3 * 0.68e-6) : rc_step(eng_c2, 6.0, dt, 440e3 * 0.68e-6);
			const double g1 = vca((6.0 + eng_c1) * 0.5), g2 = vca((6.0 + eng_c2) * 0.5);
			if (g1 > 1e-4 || g2 > 1e-4) {
				double t = eng_t.run(hp_eng.run(-0.05 * n1));
				double b1 = eng_lp1.run(t) * 0.216, b2 = eng_lp2.run(t) * 0.216;
				out += (b1 * g1 + b2 * g2) * 0.158 * trim[9];
			} else { eng_lp1.clear(); eng_lp2.clear(); }
		}

		/* ---------------- oscillator circuits, oversampled ---------------- */
		double osc = 0;
		const bool bs_on = !(A & 0x80), lz_on = !(A & 0x40), hm_on = !(A & 0x10);
		const double hdt = dt / OS;
		/* homing missile: Schmitt trigger / C43 node and the CV amplifier */
		{
			/* 7406 releases (homing active) -> R55 10k pulls the node to 12 V; U5 output -> R44 6.8k */
			const double vo = hm_cmp ? 10.5 : 0.1;
			double tgt, tau;
			if (hm_on) { tgt = (12.0 / 10e3 + vo / 6.8e3) / (1 / 10e3 + 1 / 6.8e3); tau = 6.8e-6 / (1 / 10e3 + 1 / 6.8e3); }
			else       { tgt = 0.2; tau = 6.8e-6 * 30.0; }
			hm_cv = rc_step(hm_cv, tgt, dt, tau);
			const double thr = 3.974 + 0.3377 * vo;
			if (hm_cmp && hm_cv > thr) hm_cmp = false; else if (!hm_cmp && hm_cv < 3.974 + 0.3377 * 0.1) hm_cmp = true;
		}
		const double u4b = 6.0 - (10e3 / 68e3) * (hm_cv - 6.0) - hp_hm_noise.run(0.05 * n1);
		const double hm_vc = clampd(3.333 + hp_hm_c46.run(u4b), 0.8, 4.4);        /* 555 CONTROL pin (Vcc = 5 V) */

		double sum_lz = 0, sum_hm = 0;
		for (int k = 0; k < OS; k++) {
			/* --- battleship: see the steady 131.9 Hz square below (sample-calibrated) --- */
			/* --- laser --- */
			{
				/* 555 U7: charge through R65 + R66 = 27.1k to 12 V between 4 V and 8 V, discharge through D3 */
				if (lz_charge) { lz_v += (12.0 - lz_v) / (27.1e3 * 10e-6) * hdt; if (lz_v >= 8.0) lz_charge = false; }
				else           { lz_v -= lz_v / (100.0 * 10e-6) * hdt;           if (lz_v <= 4.0) lz_charge = true; }
				const double X = lz_v;                      /* U8a follower */
				const double thr_hi = 3.974 + 0.3377 * 10.5, thr_lo = 3.974 + 0.3377 * 0.1;
				double sl = (lz_fhi ? (647.0 * X - 426.0) : -417.0 * X) * 0.87;   /* x0.87: sample peaks 299-460 Hz vs my 304-533 Hz */
				lz_vf += sl * hdt;
				if (lz_fhi && lz_vf > thr_hi) lz_fhi = false; else if (!lz_fhi && lz_vf < thr_lo) lz_fhi = true;
				sum_lz += lz_fhi ? 5.2 : -5.2;
			}
			/* --- homing missile 555 (U6): R48 47k, R49 68k, C45 0.01 uF, Vcc 5 V --- */
			{
				const double vc = hm_vc;
				if (hm_charging) { hm_v += (5.0 - hm_v) / ((47e3 + 68e3) * 0.01e-6) * 1.153 * hdt; if (hm_v >= vc) hm_charging = false; }
				else             { hm_v -= hm_v / (68e3 * 0.01e-6) * 1.153 * hdt;                  if (hm_v <= vc * 0.5) hm_charging = true; }
				sum_hm += hm_charging ? 0.54 : -0.54;
			}
		}
		if (bs_on) {
			/* [CALIBRATED] the sample is a steady square wave at 132 Hz (strongest peaks 132, 393, 654, 915 Hz at 0, -9, -14, -17 dB =
			   1, 1/3, 1/5, 1/7 of a square wave).  The two coupled relaxation oscillators I modelled from the drawing sweep instead. */
			bs_phase += 131.9 / FS; bs_phase -= std::floor(bs_phase);
			osc += (bs_phase < 0.5 ? 5.2 : -5.2) * 0.0417 * trim[6];
		}
		if (lz_on) osc += sum_lz / OS * 0.0115 * trim[7];
		if (hm_on) osc += sum_hm / OS * 0.198 * trim[8];
		out += osc;

		/* ---------------- shot: square-wave VCO with a falling control sweep [CALIBRATED to the sample: strongest frequency 1176 Hz in the
		   first 0.25 s, then 308 and 280 Hz] ---------------- */
		if (e_shot.active()) {
			const double f = 280.0 + 2800.0 * std::exp(-shot_t / 0.14);
			shot_phase += f / FS; shot_phase -= std::floor(shot_phase);
			out += (shot_phase < 0.5 ? 1.0 : -1.0) * 2.6 * e_shot.gain() * 0.0753 * 0.5 * trim[3];
		}

		return hp_out.run(out);
	}

	/* ------------------------------------------------------------- audio out */
	static constexpr double MASTER = 11000.0;    /* output volts (after the summing amplifier) -> 16 bit */
	void render(int16_t *dst, int n)
	{
		for (int i = 0; i < n; i++) {
			double v = tick() * MASTER;
			v = 30000.0 * std::tanh(v / 30000.0);                 /* gentle limiter: many sounds at once never hard-clip */
			dst[i] = (int16_t)v;
		}
	}
};
