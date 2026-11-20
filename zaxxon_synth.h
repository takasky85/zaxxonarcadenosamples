/*
 * Super Zaxxon discrete sound circuits ("IC Board A - Super ZAXXON", drawing 800-3300, sheets 3 and 4).
 *
 * No audio samples are used.  Every sound is computed from a model of its circuit, with the component
 * values read from the schematic:
 *
 *   - MM5837 digital noise source (17-bit LFSR, taps 14/17 as in MAME's mm5837.h, ~103 kHz at 12 V)
 *   - 74123 one-shots (t = 0.28 * R * C * (1 + 0.7/R[k]))          -> trigger / gate pulses
 *   - RC envelope networks with diodes                              -> the explosion / missile / shot envelopes
 *   - MB4391 voltage controlled attenuators -> modelled as an MC3340: +13 dB max, 0 dB below 3.0 V, about 45 dB per volt above 3.6 V
 *     (treated as an MC3340: Figure 3 of its datasheet, +13 dB maximum gain)
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

	/* ------------------------------------------------------------- state */
	uint8_t port[3] = { 0xff, 0xff, 0xff };

	/* MM5837 noise */
	uint32_t lfsr = 0x1ffff; double noise_phase = 0;
	static constexpr double NOISE_CLK = 103200.0, NOISE_VPP = 7.5;

	/* explosion style envelopes (74123 + diode + RC + follower -> MB4391 CON) */
	struct DiodeEnv {
		double B = 5.0;                            /* voltage on the envelope capacitor */
		double tau_dis = 0, tau_chg = 0, vlow = 0.85, vtop = 5.0;
		OneShot os;
		void tick(double dt)
		{
			os.tick(dt);
			if (os.active()) B = rc_step(B, vlow, dt, tau_dis); else B = rc_step(B, vtop, dt, tau_chg);
		}
		double con() const { return (5.0 + B) * 0.5; }        /* R_a = R_b divider into the follower */
	};
	DiodeEnv env_sexp, env_mexp, env_base;
	Biquad lp_sexp, lp_mexp, lp_base;
	HP1 hp_sexp, hp_mexp, hp_base;

	/* cannon */
	OneShot os_cannon; double v_cannon = 0; Biquad lp_cannon_dummy; LP1 lp_cannon; HP1 hp_cannon;

	/* shot */
	OneShot os_shot; double v_shot = 7.61, shot_t = 10.0, shot_phase = 0; bool shot_state = false; double shot_v = 5.0;

	/* alarms */
	OneShot os_a2, os_a3; double alarm_phase2 = 0, alarm_phase3 = 0;

	/* battleship (two relaxation oscillators) */
	double bs_vs = 5.0; bool bs_hi = true;         /* slow oscillator: integrator voltage and comparator state */
	double bs_vf = 5.0; bool bs_fhi = true;        /* fast oscillator */
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
	/* Per-channel trim on top of the levels derived from the summing resistors.  The derived levels are
	   only as good as my reading of the drawing; these are an ear-calibrated nudge (1.0 = as derived) so
	   that no sound is buried.  Order: s-exp, m-exp, cannon, shot, base missile, alarms, battleship,
	   laser, homing missile, player ship. */
	double trim[10] = { 1.5, 0.6, 0.8, 0.5, 1.5, 3.0, 0.5, 1.5, 0.6, 1.5 };

	/* ------------------------------------------------------------- init */
	ZaxSynth() { reset(); }

	void reset()
	{
		port[0] = port[1] = port[2] = 0xff;
		lfsr = 0x1ffff; noise_phase = 0;
		env_sexp = DiodeEnv(); env_sexp.os.tw = oneshot_tw(36e3, 1e-6);   env_sexp.tau_dis = 1e3 * 2.2e-6;   env_sexp.tau_chg = 940e3 * 2.2e-6;
		env_mexp = DiodeEnv(); env_mexp.os.tw = oneshot_tw(47e3, 3.3e-6); env_mexp.tau_dis = 470.0 * 1e-6;  env_mexp.tau_chg = 2e6 * 1e-6;
		env_base = DiodeEnv(); env_base.os.tw = oneshot_tw(36e3, 15e-6);  env_base.tau_dis = 470.0 * 15e-6;  env_base.tau_chg = 440e3 * 15e-6;
		os_cannon = OneShot(); os_cannon.tw = oneshot_tw(47e3, 1e-6);
		os_shot = OneShot();   os_shot.tw = oneshot_tw(18e3, 2.2e-6);
		os_a2 = OneShot();     os_a2.tw = oneshot_tw(47e3, 10e-6);
		os_a3 = OneShot();     os_a3.tw = oneshot_tw(47e3, 10e-6);
		v_cannon = 0; v_shot = 7.61;
		/* Sallen-Key low pass filters (R, C from the schematic):
		   S-EXP  U3b: 15k/15k, .033uF  -> 321 Hz, K = 1 + 150k/100k = 2.5, Q = 1/(3-K) = 2
		   M-EXP  U3d: 15k/15k, .047uF  -> 226 Hz, K = 2.5, Q = 2
		   BASE MISSILE U4b: 15k/15k, .022uF -> 482 Hz, K = 1 + 50k/100k = 1.5, Q = 1/(3-K) = 0.667 */
		lp_sexp.set_sk_lp(1.0 / (2 * M_PI * 15e3 * 0.033e-6), 2.0, 2.5, FS);
		lp_mexp.set_sk_lp(1.0 / (2 * M_PI * 15e3 * 0.047e-6), 2.0, 2.5, FS);
		lp_base.set_sk_lp(1.0 / (2 * M_PI * 15e3 * 0.022e-6), 1.0 / (3.0 - 1.5), 1.5, FS);
		lp_sexp.clear(); lp_mexp.clear(); lp_base.clear();
		hp_sexp.set(2.2e-6 * 20e3); hp_mexp.set(2.2e-6 * 20e3); hp_base.set(22e-6 * 20e3);
		hp_cannon.set(10e-6 * 100e3);
		/* cannon: U12a feedback R129 47k || (C81 + C82 in series = 5 nF) -> 677 Hz when Q6 is off */
		lp_cannon.set(1.0 / (2 * M_PI * 47e3 * 5e-9));
		bs_vs = 5.0; bs_hi = true; bs_vf = 5.0; bs_fhi = true;
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
		shot_t = 10.0; shot_phase = 0; shot_state = false;
		alarm_phase2 = alarm_phase3 = 0;
	}

	/* ------------------------------------------------------------- 8255 */
	void ppi_write(int p, uint8_t v)
	{
		const uint8_t old = port[p];
		port[p] = v;
		const uint8_t fall = old & ~v;                 /* bits that went 1 -> 0 */
		if (p == 0) {
			if (fall & 0x20) env_base.os.trigger();    /* base missile */
		} else if (p == 1) {
			if (fall & 0x10) env_sexp.os.trigger();    /* S-EXP */
			if (fall & 0x20) env_mexp.os.trigger();    /* M-EXP */
			if (fall & 0x80) os_cannon.trigger();      /* cannon */
		} else {
			if (fall & 0x01) { os_shot.trigger(); shot_t = 0; }   /* shot */
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
		env_sexp.tick(dt); env_mexp.tick(dt); env_base.tick(dt);
		os_cannon.tick(dt); os_shot.tick(dt); os_a2.tick(dt); os_a3.tick(dt);
		/* cannon: peak detector (diode + C79 6.8 uF, R127 100k) */
		if (os_cannon.active()) v_cannon = rc_step(v_cannon, 3.8, dt, 2e-3); else v_cannon = rc_step(v_cannon, 0.0, dt, 100e3 * 6.8e-6);
		/* shot: C89 0.68 uF recovers to 7.61 V (12 V through 270k+1M, 2.2M to ground) after being pulled down by the one-shot */
		if (os_shot.active()) v_shot = rc_step(v_shot, 0.9, dt, 560.0 * 0.68e-6); else v_shot = rc_step(v_shot, 7.61, dt, 0.68e-6 * 0.805e6);
		shot_t += dt;

		double out = 0;

		/* ---------------- S-EXP ---------------- */
		{
			double s = hp_sexp.run(lp_sexp.run(n2));
			out += s * vca(env_sexp.con()) * 0.154 * trim[0];
		}
		/* ---------------- M-EXP ---------------- */
		{
			double s = hp_mexp.run(lp_mexp.run(-0.30303 * n1 * 0.5));    /* U3c gain -100k/330k, then R123/R124 halve it */
			out += s * vca(env_mexp.con()) * 0.365 * trim[1];
		}
		/* ---------------- BASE MISSILE ---------------- */
		{
			double s = hp_base.run(lp_base.run(n2));
			out += s * vca(env_base.con()) * 0.21 * trim[4];
		}
		/* ---------------- CANNON ---------------- */
		{
			/* U12a: inverting amp, noise 2 in through 10k, R129 47k feedback; Q6 (base from the envelope through 15k/3.3k)
			   shunts the C81/C82 junction and so un-bypasses the high frequencies [ASSUMPTION: amplitude follows the
			   envelope squared because the VCA control for this channel could not be traced on the drawing] */
			const double vb = 0.18 * v_cannon;
			const double on = clampd((vb - 0.55) / 0.12, 0.0, 1.0);
			double low = lp_cannon.run(n2);
			double s = hp_cannon.run(4.7 * ((1.0 - on) * low + on * n2));
			const double e = v_cannon / 3.8;
			out += s * e * e * 0.0342 * 10.0 * trim[2];     /* x10: see the note on the U13 attenuation in README */
		}
		/* ---------------- ALARMS ---------------- */
		if (os_a2.active() || os_a3.active()) {
			/* 556 U50: 1.44 / ((470 + 2*120) * 0.1 uF) = 20.28 kHz, 74393 taps: QA = /2, QD = /16 [ASSUMPTION: which tap each alarm uses] */
			const double f556 = 1.44 / ((470.0 + 2 * 120.0) * 0.1e-6);
			double v = 0;
			if (os_a2.active()) { alarm_phase2 += f556 / 2 / FS; alarm_phase2 -= std::floor(alarm_phase2); v += (alarm_phase2 < 0.5 ? 1 : -1); }
			if (os_a3.active()) { alarm_phase3 += f556 / 16 / FS; alarm_phase3 -= std::floor(alarm_phase3); v += (alarm_phase3 < 0.5 ? 1 : -1); }
			/* NANDs (open collector, wired together) then U12c; clipped by the op-amp rails: ~10.4 Vpp square */
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

		double sum_bs = 0, sum_lz = 0, sum_hm = 0;
		for (int k = 0; k < OS; k++) {
			/* --- battleship --- */
			{
				/* slow oscillator (U9b/U9c, Q4): integrator C56+C57 = 1.65 uF; V1 = 12 * 220k/(2.2M+220k) = 1.091 V */
				const double thr_hi = 3.974 + 0.3377 * 10.5, thr_lo = 3.974 + 0.3377 * 0.1;
				const double slope = bs_hi ? 84.2 : -11.0;
				bs_vs += slope * hdt;
				if (bs_hi && bs_vs > thr_hi) bs_hi = false; else if (!bs_hi && bs_vs < thr_lo) bs_hi = true;
				/* fast oscillator (U10b/U10c, Q5), C58 = 0.01 uF.
				   [ASSUMPTION] its control voltage X follows the slow sawtooth: X = 0.4955 V (U9d) + (Vs - 4.0 V) */
				const double X = 0.4955 + (bs_vs - thr_lo);
				double sl = bs_fhi ? (1667.0 * X - 1333.0) : -1667.0 * X;
				bs_vf += sl * hdt;
				if (bs_fhi && bs_vf > thr_hi) bs_fhi = false; else if (!bs_fhi && bs_vf < thr_lo) bs_fhi = true;
				else if (bs_fhi && sl <= 0) { /* Q5 cannot overcome the input current: oscillator stalls with Q5 on */ }
				sum_bs += bs_fhi ? 5.2 : -5.2;
			}
			/* --- laser --- */
			{
				/* 555 U7: charge through R65 + R66 = 27.1k to 12 V between 4 V and 8 V, discharge through D3 */
				if (lz_charge) { lz_v += (12.0 - lz_v) / (27.1e3 * 10e-6) * hdt; if (lz_v >= 8.0) lz_charge = false; }
				else           { lz_v -= lz_v / (100.0 * 10e-6) * hdt;           if (lz_v <= 4.0) lz_charge = true; }
				const double X = lz_v;                      /* U8a follower */
				const double thr_hi = 3.974 + 0.3377 * 10.5, thr_lo = 3.974 + 0.3377 * 0.1;
				double sl = lz_fhi ? (647.0 * X - 426.0) : -417.0 * X;
				lz_vf += sl * hdt;
				if (lz_fhi && lz_vf > thr_hi) lz_fhi = false; else if (!lz_fhi && lz_vf < thr_lo) lz_fhi = true;
				sum_lz += lz_fhi ? 5.2 : -5.2;
			}
			/* --- homing missile 555 (U6): R48 47k, R49 68k, C45 0.01 uF, Vcc 5 V --- */
			{
				const double vc = hm_vc;
				if (hm_charging) { hm_v += (5.0 - hm_v) / ((47e3 + 68e3) * 0.01e-6) * hdt; if (hm_v >= vc) hm_charging = false; }
				else             { hm_v -= hm_v / (68e3 * 0.01e-6) * hdt;                  if (hm_v <= vc * 0.5) hm_charging = true; }
				sum_hm += hm_charging ? 0.54 : -0.54;
			}
		}
		if (bs_on) osc += sum_bs / OS * 0.0417 * trim[6];
		if (lz_on) osc += sum_lz / OS * 0.0115 * trim[7];
		if (hm_on) osc += sum_hm / OS * 0.198 * trim[8];
		out += osc;

		/* ---------------- shot: relaxation VCO [ASSUMPTION: control sweeps 9 kHz -> 1.5 kHz] gated by its envelope ---------------- */
		{
			const double g = vca(v_shot);
			if (g > 1e-4) {
				const double f = 1500.0 + 7500.0 * std::exp(-shot_t / 0.15);
				shot_phase += f / FS; shot_phase -= std::floor(shot_phase);
				const double sq = shot_phase < 0.5 ? 1.0 : -1.0;
				out += sq * 2.6 * g * 0.0753 * 0.5 * trim[3];
			}
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
