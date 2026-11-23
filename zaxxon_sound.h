/*
 * Zaxxon / Super Zaxxon sound board, simulated at the circuit level.  No recorded samples are used.
 *
 * Sources: "IC Board A - Super ZAXXON" (Sega/Gremlin drawing 800-3300, sheets 3 and 4) and the Super Zaxxon
 * sound daughterboard (834-5106, sheets 2 and 3), with every part value taken from those drawings; the
 * Motorola MC3340 data sheet; MAME's mm5837.h for the noise chip's clock and levels.
 *
 * How it works
 *   Everything runs at 192 kHz (4x the 48 kHz output).  Each circuit block is a small model of the real parts:
 *     - MM5837 noise: 17-bit shift register, taps 14 and 17, clocked at 103.2 kHz (12 V), output 3.0 / 10.5 V.
 *     - LM324 op-amps: output range 0.03..10.5 V on 12 V, slew rate 0.5 V/us.
 *     - op-amp VCOs (integrator + Schmitt trigger + switching transistor), solved event by event, so the
 *       Schmitt output's slewing, the transistor switching point and the integrator ramps are exact.  The Schmitt
 *       op-amp also needs about 40 us to come out of saturation before it switches (see SCHMITT_TD).
 *     - 555 / 556 timers: exponential charge / discharge, thresholds at CV and CV/2, discharge transistor
 *       saturation, comparator-to-output delay.  Also solved event by event.
 *     - 74123 one-shots: t = 0.28 R C (1 + 0.7 / R[kOhm]), retriggerable.
 *     - diode / RC envelope networks: exact exponentials.
 *     - Sallen-Key, multiple-feedback and bridged-T filters: their exact transfer functions from the part values,
 *       run as zero-delay-feedback state-variable filters (stable while the part values move).
 *     - 4016 analog switches with the coupling capacitors around them (the clicks when a sound starts are real).
 *     - MB4391 voltage-controlled attenuators (see below).
 *     - the output summing network (R174..R209) and the U11 summing amplifier, including its clipping.
 *   The result is low-pass filtered (96-tap FIR) and decimated to 48 kHz.
 *
 * The MB4391
 *   The six "4391/2" boxes on the drawings are halves of the Fujitsu MB4391, a dual attenuator.  Each half has the
 *   same IN / OUT / CON / RO pins as one Motorola MC3340, which is why a dead MB4391 is replaced by two MC3340s.
 *   The model is the MC3340's own circuit: an input stage that clips about 0.44 V below its bias point, a
 *   differential pair steering the signal (attenuation in dB = 20 log10(1 + exp((Vcon - V0) / Vs))) and +13 dB of
 *   maximum gain.  For the MC3340 at 12 V the data sheet's curve gives V0 = 3.28 V, Vs = 0.157 V.  The MB4391's
 *   curve is not published.  Fitting the fade-outs of four recordings of a real board (small explosion, medium
 *   explosion, shot and cannon: each one's control voltage comes from a plain RC network, so the fade-out shape is
 *   the attenuator's curve) gives a gentler curve, V0 = 3.435 V, Vs = 0.230 V, about 2 dB rms from the recordings;
 *   the MC3340's curve is 6.5 to 9.4 dB off on the same fade-outs (its sounds end too early).  The fitted curve is
 *   the default; the MC3340 curve can be selected instead (set_vca_model).
 *
 * Engine styles
 *   By default the player ship's engine is the circuit.  set_engine_style(ENGINE_MAME) instead plays loops cut from
 *   that circuit the way MAME plays its engine samples (looped, layered, clipped); see set_engine_style.
 *
 * Assumptions (where the drawings or data sheets do not say) are marked [ASSUMPTION] in the code; see README.md.
 *
 * 8255 outputs are active low: 0 starts a sound.  One-shots trigger on the 1 -> 0 edge.
 */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace zsnd {

static constexpr double FS = 192000.0;          /* internal rate */
static constexpr double DT = 1.0 / FS;
static constexpr int    OS = 4;                 /* FS / 48 kHz */
static constexpr double PI = 3.14159265358979323846;

/* LM324 on a single 12 V supply */
static constexpr double OA_LO = 0.03, OA_HI = 10.5, OA_SR = 0.5e6;
static constexpr double VREF = 6.0;             /* the board's 6 V reference (R34/R35 divider, sheet 4) */
static constexpr double VD = 0.6;               /* silicon diode drop */
static constexpr double TTL_LO = 0.2;           /* LS-TTL / 7406 / 7417 output low */
static constexpr double RON_4016 = 400.0;       /* CD4016 on-resistance at 12 V */

static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline double par(double a, double b) { return a * b / (a + b); }
static inline double oneshot_t(double r, double c) { return 0.28 * r * c * (1.0 + 700.0 / r); }   /* 74123 */
/* Lowest output of an LM324 that has to sink i amps.  Up to its 50 uA internal current sink it reaches ground; above
   that the output PNP has to conduct, which needs about one Vbe.  [CALIBRATED: VFLOOR at 1 mA, which puts the
   battleship tone, whose pitch is set by this floor, at the 132 Hz of a real board] */
static constexpr double VFLOOR = 0.648;
/* An LM324 wired as a Schmitt trigger sits saturated at a rail; when its input crosses the threshold it takes a while
   to come out of saturation before its output starts to slew.  [CALIBRATED: 40 us.  Fitted to the laser recording,
   whose sweep (273..504 Hz) it then matches with the drawn part values; it also brings the shot's chirps down to the
   recording's 2.1..2.8 kHz peaks, which nothing was fitted to] */
static constexpr double SCHMITT_TD = 40e-6;
static constexpr double HM_C45 = 9.1e-9;          /* [CALIBRATED] see init_parts() */
/* [CALIBRATED] the shot's C89 (drawn 0.68u, an electrolytic: those were made -20 % / +80 %) at 0.78u.  It sets how fast
   the shot fades and how long its chirps stay high; 0.78u matches the recording's fade within 0.6 dB down to -20 dB. */
static constexpr double SHOT_C89 = 0.78e-6;
static inline double oa_floor(double i)
{
	return i <= 50e-6 ? OA_LO : std::max(OA_LO, VFLOOR + 0.026 * std::log((i - 50e-6) / 1e-3));
}

/* ---------------------------------------------------------------------------------------------- MM5837 */
struct MM5837 {
	uint32_t sr = 0x1ffff;
	double ph = 0, f = 103200.0;
	bool out = true;
	double lo = 3.0, hi = 10.5;
	void clock()
	{
		const uint32_t b = ((sr >> 13) ^ (sr >> 16) ^ (sr == 0 ? 1u : 0u)) & 1u;
		sr = ((sr << 1) | b) & 0x1ffff;
		out = (sr >> 16) & 1;
	}
	/* average output voltage over one step (box filter, so the 103 kHz clock does not alias) */
	double step()
	{
		const double v0 = out ? hi : lo;
		ph += f * DT;
		if (ph < 1.0) return v0;
		ph -= 1.0;
		const double after = ph / (f * DT);      /* part of the step after the clock edge */
		clock();
		return v0 * (1.0 - after) + (out ? hi : lo) * after;
	}
};

/* ---------------------------------------------------------------------------------------------- first-order helpers */
struct RC {                                     /* v follows x with time constant tau (exact for x constant over the step) */
	double v = 0, k = 0;
	void set(double tau) { k = 1.0 - std::exp(-DT / tau); }
	double run(double x) { v += (x - v) * k; return v; }
};
struct HP {                                     /* coupling capacitor into a resistor: x - (slow average of x) */
	RC lp;
	void set(double tau, double init = 0) { lp.set(tau); lp.v = init; }
	double run(double x) { return x - lp.run(x); }
};

/* ---------------------------------------------------------------------------------------------- state-variable filter */
/* Any 2nd-order H(s) = (b2 s^2 + b1 s + b0) / (s^2 + (w0/Q) s + w0^2), as a zero-delay-feedback SVF.
   y = b2 HP + (b1/w0) BP + (b0/w0^2) LP, with HP = s^2/D, BP = s w0/D, LP = w0^2/D. */
struct SVF {
	double ic1 = 0, ic2 = 0, g = 0, k = 1, a1 = 1, a2 = 0, a3 = 0;
	double c_hp = 0, c_bp = 0, c_lp = 0;
	void set(double f0, double Q, double b2, double b1, double b0)
	{
		const double w0 = 2 * PI * f0;
		g = std::tan(PI * std::min(f0, FS * 0.45) / FS); k = 1.0 / Q;
		a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2;
		c_hp = b2; c_bp = b1 / w0; c_lp = b0 / (w0 * w0);
	}
	double run(double x)
	{
		const double v3 = x - ic2;
		const double v1 = a1 * ic1 + a2 * v3;
		const double v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
		const double hp = x - k * v1 - v2;
		return c_hp * hp + c_bp * v1 + c_lp * v2;
	}
	void clear() { ic1 = ic2 = 0; }
};
/* Sallen-Key low pass: R1 (input), R2 (to + input), C1 (from R1/R2 junction to the output), C2 (+ input to ground), gain K */
static inline void sk_lowpass(SVF &f, double R1, double R2, double C1, double C2, double K)
{
	const double w0 = 1.0 / std::sqrt(R1 * R2 * C1 * C2);
	const double Q = std::sqrt(R1 * R2 * C1 * C2) / (C2 * (R1 + R2) + R1 * C1 * (1.0 - K));
	f.set(w0 / (2 * PI), Q, 0, 0, K * w0 * w0);
}
/* Sallen-Key high pass: C1 (input), C2 (to + input), R1 (from C1/C2 junction to the output), R2 (+ input to ref), gain K */
static inline void sk_highpass(SVF &f, double C1, double C2, double R1, double R2, double K)
{
	const double w0 = 1.0 / std::sqrt(R1 * R2 * C1 * C2);
	const double wq = (1.0 - K) / (R1 * C1) + (C1 + C2) / (R2 * C1 * C2);
	f.set(w0 / (2 * PI), w0 / wq, K, 0, 0);
}

/* ---------------------------------------------------------------------------------------------- MB4391 half */
struct VCA {
	static constexpr double GMAX = 4.467;       /* +13 dB (MC3340 data sheet) */
	static constexpr double CLIP = -0.44;       /* input stage cuts off 0.44 V below its bias (1.09 V base, 1.3k emitter) */
	double v0 = 3.435, vs = 0.230;
	mutable double last_in = 0, last_con = 0;   /* for the test tools */
	mutable double g_con = -1e9, g_val = 0;
	double run(double in, double con) const
	{
		last_in = in; last_con = con;
		/* soft cut-off of the input transistor (exponential knee, about one thermal voltage wide) */
		const double x = (in - CLIP) / 0.026;
		const double v = x > 14 ? in : CLIP + 0.026 * std::log1p(std::exp(x));
		if (con != g_con) {                     /* the control voltage moves slowly: only recompute when it changes */
			g_con = con;
			const double e = (con - v0) / vs;
			g_val = e > 40 ? 0.0 : GMAX / (1.0 + std::exp(e));
		}
		return clampd(g_val * v, -4.0, 4.0);
	}
};

/* exp(-DT * a), remembered for the last a (the RC networks switch between a few states and stay in each for long) */
struct ExpK {
	double a = -1, k = 1;
	double operator()(double na) { if (na != a) { a = na; k = std::exp(-DT * na); } return k; }
};

/* ---------------------------------------------------------------------------------------------- 74123 */
struct OneShot {
	double T = 0, left = 0;
	void trigger() { left = T; }
	bool step() { const bool a = left > 0; if (a) left -= DT; return a; }
	bool active() const { return left > 0; }
};

/* ---------------------------------------------------------------------------------------------- op-amp VCO */
/*
 * Integrator (C between - input and output; Rin from Vc; + input at kp*Vc; transistor pulls the - input to ground
 * through Rq while it conducts) followed by an inverting Schmitt trigger (- input = integrator output; + input from
 * VREF through Ra and from its own output through Rb).  The transistor is driven from the Schmitt output through a
 * diode and a resistor divider, and conducts once that output is above qth.
 */
struct OpVCO {
	double C = 1e-8, Rin = 1e5, kp = 0.5, Rq = 5e4, vsat = 0.0075, qth = 3.3;  /* vsat: switch saturated hard at ~10 uA: VT ln(1 + 1/bR), reverse beta 3 */
	double a = 0, b = 0;                        /* Schmitt + input = a + b * so */
	double vi = 5.0, so = OA_HI; int dir = 1;   /* integrator output, Schmitt output, direction the Schmitt output moves */
	double hold = 0;                            /* Schmitt output still saturated after its input crossed (SCHMITT_TD) */
	double avg_vi = 5.0, avg_so = OA_HI;
	void init(double c, double rin, double kplus, double rq, double ra, double rb, double rbase, double rshunt)
	{
		C = c; Rin = rin; kp = kplus; Rq = rq;
		const double g = 1.0 / ra + 1.0 / rb;
		a = (VREF / ra) / g; b = (1.0 / rb) / g;
		qth = 1.0 + rbase * 0.5 / rshunt;      /* diode + Vbe (about 0.5 V each at these currents) + drop across the base resistor */
		vi = a + b * OA_LO + 0.1; so = OA_HI; dir = 1; hold = 0;
	}
	double vplus(double s) const { return a + b * s; }
	void step(double vc)
	{
		const double vm = kp * vc;
		const double ioff = (vc - vm) / Rin;
		const double ion = ioff - (vm > vsat ? (vm - vsat) / Rq : 0.0);
		const double s_off = -ioff / C, s_on = -ion / C;
		double t = 0, svi = 0, sso = 0;
		for (int it = 0; it < 24; it++) {
			const double rem = DT - t;
			if (rem <= 0) break;
			double s = so > qth ? s_on : s_off;
			if ((vi >= OA_HI && s > 0) || (vi <= OA_LO && s < 0)) s = 0;
			double sr = 0;
			if (hold > 0) sr = 0;
			else if (dir > 0 && so < OA_HI) sr = OA_SR; else if (dir < 0 && so > OA_LO) sr = -OA_SR;
			const double m = vplus(so) - vi, mp = b * sr - s;
			double tev = rem; int ev = 0;
			auto cand = [&](double tc, int e) { if (tc >= 0 && tc < tev) { tev = tc; ev = e; } };
			if (hold > 0) cand(hold, 5);
			else if (dir > 0 && mp < 0) cand(m / -mp, 1);
			else if (dir < 0 && mp > 0) cand(-m / mp, 1);
			if (sr > 0) { cand((OA_HI - so) / sr, 2); if (so < qth) cand((qth - so) / sr, 3); }
			else if (sr < 0) { cand((OA_LO - so) / sr, 2); if (so > qth) cand((qth - so) / sr, 3); }
			if (s > 0) cand((OA_HI - vi) / s, 4); else if (s < 0) cand((OA_LO - vi) / s, 4);
			svi += (vi + 0.5 * s * tev) * tev; sso += (so + 0.5 * sr * tev) * tev;
			vi += s * tev; so += sr * tev; t += tev;
			if (hold > 0) hold -= tev;
			if (ev == 1) { dir = -dir; hold = SCHMITT_TD; }
			else if (ev == 5) hold = 0;
			else if (ev == 2) so = sr > 0 ? OA_HI : OA_LO;
			else if (ev == 3) so = qth + (sr > 0 ? 1e-9 : -1e-9);
			else if (ev == 4) vi = s > 0 ? OA_HI : OA_LO;
			else break;
		}
		if (t < DT) { svi += vi * (DT - t); sso += so * (DT - t); }
		avg_vi = svi / DT; avg_so = sso / DT;
	}
};

/* ---------------------------------------------------------------------------------------------- 555 */
struct T555 {
	double vch = 12, rch = 1e4, vdis = 0.1, rdis = 1e4, C = 1e-6;   /* charge toward vch through rch, discharge toward vdis through rdis */
	double td = 0.3e-6;                         /* comparator to output delay [ASSUMPTION: 0.3 us; the alarm 556 is CALIBRATED below] */
	double vhi = 10.6, vlo = 0.1;               /* output levels */
	double v = 0; bool out = true; double pend = -1;
	double avg = 0;
	template <class F> void step(double cv, F &&edge)
	{
		const double hi = cv, lo = cv * 0.5;
		double t = 0, acc = 0;
		for (int it = 0; it < 32; it++) {
			const double rem = DT - t;
			if (rem <= 1e-15) break;
			const double tau = (out ? rch : rdis) * C, vt = out ? vch : vdis;
			double tev = rem; int ev = 0;
			if (pend >= 0) { if (pend <= rem) { tev = pend; ev = 1; } }
			else if (out ? v >= hi : v <= lo) { tev = 0; ev = 2; }
			else if (out ? vt > hi : vt < lo) {
				const double L = out ? hi : lo;
				const double tc = tau * std::log((v - vt) / (L - vt));
				if (tc <= rem) { tev = std::max(0.0, tc); ev = 2; }
			}
			v = vt + (v - vt) * std::exp(-tev / tau);
			acc += (out ? vhi : vlo) * tev; t += tev;
			if (pend >= 0) pend -= tev;
			if (ev == 1) { out = !out; pend = -1; edge(t, out); }
			else if (ev == 2) { v = out ? std::max(v, hi) : std::min(v, lo); pend = td; if (td <= 0) { out = !out; pend = -1; edge(t, out); } }
			else break;
		}
		if (t < DT) acc += (out ? vhi : vlo) * (DT - t);
		avg = acc / DT;
	}
	void step(double cv) { step(cv, [](double, bool) {}); }
};

/* ---------------------------------------------------------------------------------------------- 4016 switch + coupling capacitor */
/* source (Thevenin vs, rs) -> capacitor C -> switch -> node B (Thevenin vb, rb).  Returns node B minus its idle level. */
struct SwCap {
	double C = 1e-6, rs = 0, rb = 1e4, vb = 6, vc = 0;
	void init(double c, double r_s, double r_b, double v_b, double v_src_idle) { C = c; rs = r_s; rb = r_b; vb = v_b; vc = v_src_idle - v_b; }
	double step(double vsrc, bool on)
	{
		if (!on) return 0.0;
		const double i = (vsrc - vc - vb) / (rs + RON_4016 + rb);
		vc += i * DT / C;
		return i * rb;
	}
};

/* ---------------------------------------------------------------------------------------------- the board */
class ZaxSound {
public:
	enum { VCA_MB4391 = 0, VCA_MC3340 = 1 };

	ZaxSound() { build_fir(); reset(); }

	void set_board(bool super_zaxxon) { if (sz != super_zaxxon) { sz = super_zaxxon; reset(); } }
	bool board_super() const { return sz; }
	void set_vca_model(int m)
	{
		vca_model = m;
		const double v0 = m == VCA_MC3340 ? 3.279 : 3.435, vs = m == VCA_MC3340 ? 0.157 : 0.230;
		for (VCA *x : { &vca_ship1, &vca_ship2, &vca_bm, &vca_sexp, &vca_mexp, &vca_can, &vca_shot, &vca_rumble, &vca_whine, &vca_sob }) { x->v0 = v0; x->vs = vs; }
	}
	int get_vca_model() const { return vca_model; }

	/* The player ship's engine noise.  ENGINE_BOARD is the circuit.  ENGINE_MAME imitates the way MAME plays the engine from its
	   sample set: two short recordings looped (0.64 s and 0.75 s), layered, and clipped, which gives MAME's rougher, pulsing
	   engine.  No recording is used here: the two loops are cut from this emulator's own ship circuit at the two settings the
	   real-board recordings were made at (port A = f0 and f3), then saturated and clipped by the amounts measured in those
	   recordings, and played with MAME's rules (see make_loops and the player ship in tick).  Both games use it, as in MAME;
	   on Super Zaxxon it replaces the extra board's SOUND OUT A. */
	enum { ENGINE_BOARD = 0, ENGINE_MAME = 1 };
	void set_engine_style(int s) { eng_style = s == ENGINE_MAME ? ENGINE_MAME : ENGINE_BOARD; if (eng_style == ENGINE_MAME) make_loops(); }
	int get_engine_style() const { return eng_style; }

	void reset()
	{
		pa = pb = pc = 0xff; pos_a = pos_b = 0;
		noise = MM5837();
		fifo.clear(); fir_pos = 0; sub_phase = 0; std::fill(std::begin(firbuf), std::end(firbuf), 0.0); dc_lp = 0;
		init_parts();
		set_vca_model(vca_model);
		/* settle the coupling capacitors and the slow RC nodes with every port idle */
		for (int i = 0; i < (int)(FS * 0.6); i++) tick();
		fifo.clear(); dc_lp = 0; std::fill(std::begin(firbuf), std::end(firbuf), 0.0);
	}

	/* an 8255 port changed (0 = A, 1 = B, 2 = C); takes effect at once */
	void write(int port, uint8_t v)
	{
		if (port == 0) {
			const uint8_t fall = pa & ~v; pa = v;
			if (fall & 0x20) os_bm.trigger();
			if (fall & 0x04) pos_a = 0;                      /* MAME restarts a looped sample when it is switched on */
			if (fall & 0x08) pos_b = 0;
		} else if (port == 1) {
			const uint8_t fall = pb & ~v; pb = v;
			if (fall & 0x10) os_sexp.trigger();
			if (fall & 0x20) os_mexp.trigger();
			if (fall & 0x80) os_can.trigger();
		} else if (port == 2) {
			const uint8_t fall = pc & ~v; pc = v;
			if (fall & 0x01) os_shot.trigger();
			if (fall & 0x04) os_al2.trigger();
			if (fall & 0x08) os_al3.trigger();
		}
	}
	/* old name kept for the front end */
	void ppi_write(int port, uint8_t v) { write(port, v); }

	/* advance n steps of 1/192000 s; every 4th step adds an output sample to the queue */
	void run(int n) { for (int i = 0; i < n; i++) tick(); }
	int available() const { return (int)fifo.size(); }
	int take(int16_t *dst, int n)
	{
		const int k = std::min(n, (int)fifo.size());
		std::copy(fifo.begin(), fifo.begin() + k, dst);
		fifo.erase(fifo.begin(), fifo.begin() + k);
		for (int i = k; i < n; i++) dst[i] = 0;
		return k;
	}
	/* n output samples (48 kHz) */
	void render(int16_t *dst, int n)
	{
		while ((int)fifo.size() < n) tick();
		take(dst, n);
	}

	/* output level: 16-bit units per volt at the U11 summing amplifier output [CALIBRATED: loudest sounds near -3 dBFS] */
	double master = 40000.0;

	/* for tests: the last U11 output (volts), and the input / control voltage of an attenuator
	   (0 S-EXP, 1 M-EXP, 2 shot, 3 cannon, 4 base missile, 5 ship 1, 6 ship 2) */
	double last_u11 = VREF;
	void probe_vca(int which, double &in, double &con) const
	{
		const VCA *v[] = { &vca_sexp, &vca_mexp, &vca_shot, &vca_can, &vca_bm, &vca_ship1, &vca_ship2 };
		in = v[which]->last_in; con = v[which]->last_con;
	}

private:
	bool sz = false;
	int vca_model = VCA_MB4391;
	int eng_style = ENGINE_BOARD;
	std::vector<float> loop_a, loop_b;          /* ENGINE_MAME loops, scaled to +-1 like a full-scale sample */
	size_t pos_a = 0, pos_b = 0;
	double loop_r = 0;                          /* rms of the board's ship output at port A = f0 (the ship branch's volts) */
	double last_ship = 0;                       /* the ship branch's output, for make_loops */

	/* Cut a loop from a second, board-mode copy of the sound board with port A held at port_a.  The board's noise is roughly
	   Gaussian; tanh(k x) then a clip at c times its rms reproduce how much of each MAME recording is flattened at full scale
	   and its rms-to-peak ratio (05.wav: 18 % of samples clipped, rms 0.71 of peak; 04.wav: 8.8 %, 0.62). */
	static double cut_loop(uint8_t port_a, double settle, double secs, double k, double c, std::vector<float> &out)
	{
		auto t = std::make_unique<ZaxSound>();
		t->write(0, port_a);
		for (int i = 0; i < 4000; i++) t->ship_dac(port_a, 1e-3);    /* C25 takes seconds to settle: jump it there */
		t->ship_g = t->cds_target(); t->set_mfb();
		t->run((int)(FS * settle));
		const int n = (int)std::lrint(secs * FS);
		std::vector<double> x(n);
		double ss = 0;
		for (int i = 0; i < n; i++) { t->tick(); x[i] = t->last_ship; ss += x[i] * x[i]; }
		const double r = std::sqrt(ss / n), zc = std::tanh(k * c);
		out.resize(n);
		for (int i = 0; i < n; i++) out[i] = (float)clampd(std::tanh(k * x[i] / r) / zc, -1.0, 1.0);
		return r;
	}
	void make_loops()
	{
		if (!loop_a.empty()) return;
		/* different settle times so the two loops hold different stretches of the noise generator's sequence */
		loop_r = cut_loop(0xf0, 0.2, 0.6354, 1.087, 1.338, loop_a);            /* like 05.wav, 627 Hz */
		cut_loop(0xf3, 0.9, 0.7516, 0.737, 1.706, loop_b);                      /* like 04.wav, 331 Hz */
	}
	uint8_t pa = 0xff, pb = 0xff, pc = 0xff;

	MM5837 noise;
	HP n1_ac;                                   /* MM5837 output with its average removed (C66 / C67 / C44 / C28 couplings) */

	/* -- player ship (Zaxxon) */
	double ship_y = 2.18, ship_g = 1.06e-4;     /* C25 node, CdS conductance (settled at PA &3 = 3) */
	double c34 = 6, c35 = 6;
	SVF mfb, sk_s1, sk_s2;
	VCA vca_ship1, vca_ship2;
	int ship_cnt = 0;

	/* -- homing missile */
	double hm_c43 = 5, hm_so = OA_HI;
	HP hm_cv;
	T555 u6;
	SwCap sw_hm;

	/* -- base missile */
	OneShot os_bm; double c49 = 5; SVF sk_bm; VCA vca_bm;

	/* -- laser */
	T555 u7; OpVCO vco_las; SwCap sw_las;

	/* -- battleship */
	OpVCO lfo_bs, vco_bs;
	double bs_v59 = 0, bs_v28 = 0;              /* C59 and C28 (Super Zaxxon SOUND OUT B) voltages */

	/* -- S-EXP / M-EXP */
	OneShot os_sexp, os_mexp; double c61 = 5, c63 = 5;
	SVF sk_sexp, sk_mexp; VCA vca_sexp, vca_mexp;

	/* -- cannon */
	OneShot os_can; double c79 = 0; SVF bt_can; VCA vca_can; int can_cnt = 0; double can_rm = 1600;

	/* -- shot */
	OneShot os_shot; double sh_y = 8.4, sh_z = 5.6, sh_p = 0; T555 u18; OpVCO vco_shot; VCA vca_shot; HP sh_out;

	/* -- alarms */
	OneShot os_al2, os_al3; T555 u50; unsigned cnt393 = 0; double al_int = OA_LO; HP al_out;

	/* -- Super Zaxxon daughterboard */
	double sz_t = 3.95, sz_q = 3.95;
	T555 u12; OpVCO vco_eng; SVF sk_rumble, sk_whine; VCA vca_rumble, vca_whine; SwCap sw_whine, sw_out; HP hp_eng;
	T555 u7b; OpVCO vco1, vco2, vco3; HP c25, c30, c32; VCA vca_sob;

	/* -- mixer */
	struct Branch { double k, g; };
	enum { B_SOA, B_SHIP, B_HOM, B_BM, B_LAS, B_BS, B_SEXP, B_MEXP, B_CAN, B_SHOT, B_ALM, B_N };
	Branch br[B_N];
	double gtot = 1;

	/* -- output */
	static constexpr int FIR_N = 96;
	double fir[FIR_N], firbuf[FIR_N * 2];
	int fir_pos = 0, sub_phase = 0;
	double dc_lp = 0, dc_k = 1.0 - std::exp(-2 * PI * 20.0 / 48000.0);
	std::vector<int16_t> fifo;

	void build_fir()
	{
		/* Kaiser-windowed sinc, cutoff 20.5 kHz at 192 kHz, beta 8 */
		auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
		const double fc = 20500.0 / FS, beta = 8.0;
		double sum = 0;
		for (int i = 0; i < FIR_N; i++) {
			const double n = i - (FIR_N - 1) / 2.0;
			const double sinc = n == 0 ? 2 * fc : std::sin(2 * PI * fc * n) / (PI * n);
			const double r = 2.0 * i / (FIR_N - 1) - 1.0;
			fir[i] = sinc * i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta);
			sum += fir[i];
		}
		for (double &c : fir) c /= sum;
	}

	void init_parts()
	{
		/* noise coupling: C66 10u / R111 100k (1 s) */
		n1_ac.set(1.0, 6.75);

		/* ---- player ship */
		ship_y = 2.18; ship_g = 1.06e-4; c34 = c35 = VREF; ship_cnt = 0;
		mfb.clear(); sk_s1.clear(); sk_s2.clear();
		sk_lowpass(sk_s1, 100e3, 100e3, 2200e-12, 2200e-12, 2.0);      /* U5: R24/R25 100k, C30/C31 2200p, K = 1 + R23/R22 */
		sk_lowpass(sk_s2, 100e3, 100e3, 3300e-12, 3300e-12, 2.0);      /* U5: R38/R39 100k, C39/C40 3300p */
		set_mfb();

		/* ---- homing missile: 555 U6 at 5 V, R48 47k, R49 68k, C45 10n; output pulled up by R50 3.3k, loaded by R51+R52 */
		hm_c43 = 5.0; hm_so = OA_HI;
		hm_cv.set(3333.0 * 33e-6, 0);                                 /* C46 33u into the CV pin's 3.33k */
		/* [CALIBRATED] C45 at 9.1n (its 10n value -9 %, inside a ceramic disc's tolerance) puts the pitch at the
		   real board's 950..1120 Hz; 10n gives 840..1020 Hz */
		u6 = T555(); u6.vch = 5.0; u6.rch = 47e3 + 68e3; u6.vdis = 0.1; u6.rdis = 68e3 + 15; u6.C = HM_C45;
		u6.vhi = 5.0 * 15.3 / 18.6; u6.vlo = 0.1; u6.v = 2.0;
		sw_hm.init(10e-6, par(12e3, 3.3e3), par(25.5e3, 49e3), VREF * 49 / (25.5 + 49), 0.5);

		/* ---- base missile: 74123 R56 36k C48 15u; Sallen-Key U4 R61/R62 15k, C137/C50 22n, K = 1 + R63/R64 */
		os_bm = OneShot(); os_bm.T = oneshot_t(36e3, 15e-6); c49 = 5.0;
		sk_bm.clear(); sk_lowpass(sk_bm, 15e3, 15e3, 22e-9, 22e-9, 1.0 + 50.0 / 100.0);

		/* ---- laser: 555 U7 at 12 V, R65 5.1k with D3 across R66 22k, C53 10u; VCO U8: C138 10n, R67 120k, R68/R69 51k, R70 47k;
		   Schmitt R72 51k / R73 100k; Q3 base via D4, R74 10k / R71 2.2k */
		u7 = T555(); u7.vch = 12.0 - VD; u7.rch = 5.1e3; u7.vdis = 0.1; u7.rdis = 22e3 + 15; u7.C = 10e-6; u7.v = 4.0;
		vco_las = OpVCO(); vco_las.init(10e-9, 120e3, 0.5, 47e3, 51e3, 100e3, 10e3, 2.2e3);
		sw_las.init(2.2e-6, par(10e3, 2.2e3), par(25.5e3, 55.2e3), VREF * 55.2 / (25.5 + 55.2), 5.0 * 0.1803);

		/* ---- battleship: LFO U9 (C56+C57 in series = 1.65u, R82 30k, R83/R84 51k, R85 2.2k) Schmitt R86 51k / R88 100k;
		   VCO U10 (C58 10n, R93 30k, R94/R95 51k, R96 15k) Schmitt R98 51k / R99 100k */
		lfo_bs = OpVCO(); lfo_bs.init(1.65e-6, 30e3, 0.5, 2.2e3, 51e3, 100e3, 10e3, 2.2e3);
		vco_bs = OpVCO(); vco_bs.init(10e-9, 30e3, 0.5, 15e3, 51e3, 100e3, 10e3, 2.2e3);
		bs_v59 = vco_bs.vplus(OA_HI) - 0.0; bs_v28 = 0;

		/* ---- explosions: 74123 U22 R102 36k C60 1u, U21 R107 47k C62 3.3u */
		os_sexp = OneShot(); os_sexp.T = oneshot_t(36e3, 1e-6);
		os_mexp = OneShot(); os_mexp.T = oneshot_t(47e3, 3.3e-6);
		c61 = c63 = 5.0;
		sk_sexp.clear(); sk_lowpass(sk_sexp, 15e3, 15e3, 0.033e-6, 0.033e-6, 1.0 + 150.0 / 100.0);   /* U3: R113/R116, C70/C71, R114/R115 */
		sk_mexp.clear(); sk_lowpass(sk_mexp, 15e3, 15e3, 0.047e-6, 0.047e-6, 1.0 + 150.0 / 100.0);   /* U3: R119/R122, C72/C73, R120/R121 */

		/* ---- cannon: 74123 U45 R125 47k C78 1u; C79 6.8u / R127 100k; bridged-T U12 */
		os_can = OneShot(); os_can.T = oneshot_t(47e3, 1e-6); c79 = 0; can_cnt = 0; can_rm = 1600;
		bt_can.clear(); set_cannon(1600);

		/* ---- shot: 74123 U21 R142 18k C87 2.2u; 555 U18 at 12 V R151/R152 10k C90 3.3u; VCO U19 C92 1n, R156 33k, R157/R158 33k,
		   R159 15k, Schmitt R161 33k / R162 100k, Q7 via D11, R163 10k / R160 2.2k */
		os_shot = OneShot(); os_shot.T = oneshot_t(18e3, 2.2e-6);
		sh_y = 8.42; sh_z = 5.6; sh_p = 0; sh_prev = false; sh_w = 5.0;
		u18 = T555(); u18.vch = 12.0; u18.rch = 20e3; u18.vdis = 0.1; u18.rdis = 10e3 + 15; u18.C = 3.3e-6; u18.out = false; u18.v = 0.1;
		vco_shot = OpVCO(); vco_shot.init(1e-9, 33e3, 0.5, 15e3, 33e3, 100e3, 10e3, 2.2e3);
		sh_out.set(2.2e-6 * 17e3, OA_HI);

		/* ---- alarms: 74123 U46 R166/R167 47k C95/C96 10u; 556 U50 at 5 V R168 470, R169 120, C97 0.1u; 74393; 7426; U12 R172 1.5k,
		   R173 330k || C99 10n.  The part values alone give 19.8 kHz; [CALIBRATED] a 1.15 us comparator-to-output delay brings the 556
		   to the 18.1 kHz of a real board's alarm tones (1132 Hz = /16 and 2265 Hz = /8 in MAME's recordings) */
		os_al2 = OneShot(); os_al2.T = oneshot_t(47e3, 10e-6);
		os_al3 = OneShot(); os_al3.T = oneshot_t(47e3, 10e-6);
		u50 = T555(); u50.vch = 5.0; u50.rch = 470 + 120; u50.vdis = 0.05; u50.rdis = 120 + 15; u50.C = 0.1e-6; u50.td = 1.15e-6;
		u50.vhi = 5.0; u50.vlo = 0.1; u50.v = 2.0;
		cnt393 = 0; al_int = OA_LO; al_out.set(2.2e-6 * 52e3, OA_LO);
		idle_hm = idle_las = idle_bs = idle_al = 0;

		/* ---- Super Zaxxon board */
		sz_t = sz_q = 3.95;
		u12 = T555(); u12.vch = 12.0; u12.rch = 12e3 + 33e3; u12.vdis = 0.1; u12.rdis = 33e3 + 15; u12.C = 1e-6; u12.v = 5;  /* R1, R2, C5 */
		vco_eng = OpVCO(); vco_eng.init(0.1e-6, 30e3, 0.5, 3.9e3, 47e3, 100e3, 10e3, 27e3);       /* C6, R3, R4/R5, R6; R7/R8; Q1 base R10 10k / R9 27k */
		sk_rumble.clear(); sk_lowpass(sk_rumble, 15e3, 68e3, 0.1e-6, 0.1e-6, 1.0 + 100.0 / 51.0);  /* U11: R22, R23, C13, C12, R21/R20 */
		sk_whine.clear(); sk_highpass(sk_whine, 4700e-12, 4700e-12, 47e3, 33e3, 1.0 + 100.0 / 51.0); /* U6: C7, C8, R14, R13, R12/R11 */
		sw_whine.init(1e-6, par(82e3, 10e3), 17e3, 1.09, VREF * 10.0 / 92.0);
		sw_out.init(1e-6, 0, par(23.5e3, 37e3), VREF, VREF);
		hp_eng.set(0.1, VREF);
		u7b = T555(); u7b.vch = 12.0; u7b.rch = 680e3 + 20e3; u7b.vdis = 0.1; u7b.rdis = 20e3 + 15; u7b.C = 2.2e-6; u7b.v = 5;  /* R42, R43, C23 */
		vco1 = OpVCO(); vco1.init(6800e-12, 150e3, 0.5, 68e3, 51e3, 100e3, 10e3, 2.2e3);   /* C24, R44, R45/R46, R47; R48/R49; R51/R50 */
		vco2 = OpVCO(); vco2.init(2200e-12, 150e3, 0.5, 47e3, 51e3, 100e3, 12e3, 2.2e3);   /* C29, R55, R56/R57, R58; R59/R60; R62/R61 */
		vco3 = OpVCO(); vco3.init(0.022e-6, 150e3, 0.5, 47e3, 36e3, 100e3, 12e3, 2.2e3);   /* C31, R63, R64/R65, R66; R67/R68; R70/R69 */
		c25.set(2.2e-6 * 220e3, VREF); c30.set(2.2e-6 * 220e3, VREF); c32.set(2.2e-6 * 100e3, VREF);

		/* ---- output summing network, sheet 4: tap Rser/Rsh -> 1u -> 51k -> SJ;  SJ -> R209 10k -> U11 (R210 82k) */
		auto mk = [&](int i, double rser, double rsh) { br[i].k = rsh / (rser + rsh); br[i].g = 1.0 / (51e3 + par(rser, rsh)); };
		mk(B_SOA, 15e3, 22e3);   mk(B_SHIP, 15e3, 22e3); mk(B_HOM, 22e3, 27e3); mk(B_BM, 39e3, 8.2e3);
		mk(B_LAS, 47e3, 8.2e3);  mk(B_BS, 47e3, 4.7e3);  mk(B_SEXP, 15e3, 8.2e3); mk(B_MEXP, 8.2e3, 47e3);
		mk(B_CAN, 47e3, 3.9e3);  mk(B_SHOT, 39e3, 8.2e3); mk(B_ALM, 68e3, 1e3);
		gtot = 1.0 / 10e3;
		for (auto &b : br) gtot += b.g;
	}

	/* MFB band-pass U5a: R20 10k in, CdS cell (PC1) from the middle node to 6 V, C26/C27 10n, R21 470k */
	void set_mfb()
	{
		const double C = 10e-9, R1 = 10e3, R3 = 470e3;
		const double w0 = std::sqrt((1.0 / R1 + ship_g) / R3) / C;
		const double Q = w0 * R3 * C / 2.0;
		mfb.set(w0 / (2 * PI), Q, 0, -1.0 / (R1 * C), 0);
	}
	/* bridged-T U12b: R128 10k in, C81/C82 10n, R129 47k, R_M = R130 100 + (R133 1.5k || Q6) */
	void set_cannon(double rm)
	{
		const double C = 10e-9, R128 = 10e3, R129 = 47e3, gm = 1.0 / rm;
		const double w0 = std::sqrt(gm / R129) / C;
		const double Q = std::sqrt(gm * R129) / 2.0;
		bt_can.set(w0 / (2 * PI), Q, 0, -2.0 / (R128 * C), -gm / (R128 * C * C));
	}

	/* one 192 kHz step of the whole board */
	void tick()
	{
		/* main-board view of port A: on Super Zaxxon PA0-PA3 go to the daughterboard and the main board's inputs float high */
		const uint8_t mpa = sz ? (uint8_t)(pa | 0x0f) : pa;
		const double n1 = noise.step();
		const double n1ac = n1_ac.run(n1);
		const double n2ac = -0.1 * n1ac;                     /* U3a: R112 10k / R111 100k */
		const double naac = -(100.0 / 330.0) * n1ac;         /* U3b: R118 100k / R117 330k */

		double sj = 0;                                       /* sum of k * g * v over the branches */

		/* ================= player ship, MAME-style (see set_engine_style) ================= */
		if (eng_style == ENGINE_MAME && !loop_a.empty()) {
			/* MAME: PA2 = 0 loops 05.wav, PA3 = 0 loops 04.wav, both at volume 0.5 + 0.157 x (PA & 3); the raw port is used, so
			   Super Zaxxon gets the same loops, as in MAME.  [CALIBRATED: MAME_K sets MAME's own balance: at port A = f0 its
			   engine is 21 dB above this emulator's board engine while its other samples are about 12 dB above this emulator's
			   other sounds, so the loops play about 9 dB above the board engine there] */
			static constexpr double MAME_K = 5.9;
			double v = 0;
			if (!(pa & 0x04)) { v += loop_a[pos_a]; if (++pos_a >= loop_a.size()) pos_a = 0; }
			if (!(pa & 0x08)) { v += loop_b[pos_b]; if (++pos_b >= loop_b.size()) pos_b = 0; }
			sj += br[B_SHIP].k * br[B_SHIP].g * MAME_K * loop_r * (0.5 + 0.157 * (pa & 3)) * v;
		}
		/* ================= player ship (Zaxxon) ================= */
		else {
			ship_dac(mpa, DT);
			if (++ship_cnt >= 8) {
				ship_cnt = 0;
				const double gt = cds_target();
				const double tau = gt > ship_g ? 8e-3 : 40e-3;
				ship_g = std::exp(std::log(ship_g) + (std::log(gt) - std::log(ship_g)) * (1.0 - std::exp(-8 * DT / tau)));
				set_mfb();
			}
			/* 74LS139 (A = PA2, B = PA3): Y0 -> channel 1, Y1 -> channel 2; 7417 -> R28/R31 1k -> C34/C35 0.68u; R29+R30 / R32+R33 440k to 6 V */
			const bool y0 = !(mpa & 0x08) && !(mpa & 0x04), y1 = !(mpa & 0x08) && (mpa & 0x04);
			auto ctl = [&](double &c, bool act, ExpK &ek) {
				const double g = 1.0 / 440e3 + (act ? 1.0 / 1e3 : 0.0);
				const double vinf = (VREF / 440e3 + (act ? TTL_LO / 1e3 : 0.0)) / g;
				c = vinf + (c - vinf) * ek(g / 0.68e-6);
			};
			ctl(c34, y0, ek_c34); ctl(c35, y1, ek_c35);
			const double u4a = -0.05 * n1ac;                                         /* U4a: R19 10k / R18 200k */
			const double bp = clampd(mfb.run(u4a), -4.97, 5.5);                     /* U5a, + input at 5 V */
			const double s1 = clampd(sk_s1.run(bp), -5.97, 4.5), s2 = clampd(sk_s2.run(bp), -5.97, 4.5);
			const double kin = par(3.3e3, 17e3) / (12e3 + par(3.3e3, 17e3));        /* R26 12k / R27 3.3k || MB4391 input */
			const double o1 = vca_ship1.run(s1 * kin, (c34 + VREF) / 2);            /* U14 half 1, CON from U11 (R29/R30) */
			const double o2 = vca_ship2.run(s2 * kin, (c35 + VREF) / 2);            /* U15 half 1 */
			/* [ASSUMPTION] U14 and U15 OUT1 are both wired to R177 (the trace is lost in the scan); tied outputs average */
			last_ship = 0.5 * (o1 + o2);
			sj += br[B_SHIP].k * br[B_SHIP].g * last_ship;
		}

		/* ================= homing missile (PA4) ================= */
		{
			/* U5c Schmitt LFO: R42 51k from 6 V, R43 100k feedback, R44 6.8k / C43 6.8u (about 15 Hz) */
			const bool on = !(mpa & 0x10);
			if (awake(idle_hm, on)) {
				const double vp = (VREF / 51e3 + hm_so / 100e3) / (1.0 / 51e3 + 1.0 / 100e3);
				hm_c43 += (hm_so - hm_c43) * k_c43;
				if (hm_so > 6 && hm_c43 > vp) hm_so = OA_LO; else if (hm_so < 6 && hm_c43 < vp) hm_so = OA_HI;
				/* U4c follows C43; U4b: R45 68k, R47 10k, noise via C44 / R46 200k; -> C46 33u -> 555 U6 CV */
				const double u4b = VREF - (10.0 / 68.0) * (hm_c43 - VREF) - 0.05 * n1ac;
				const double cv = 3.333 + hm_cv.run(u4b);
				u6.step(cv);
			}
			const double v = sw_hm.step(u6.avg * 3.3 / 15.3, on);                   /* R51 12k / R52 3.3k -> C47 10u -> U17 (11-10) */
			sj += br[B_HOM].k * br[B_HOM].g * v;
		}

		/* ================= base missile (PA5) ================= */
		{
			const bool p = os_bm.step();
			/* Q-bar low -> D2 + R58 470 discharge C49 15u; R59 + R60 220k charge it from 5 V; U20b follows (5 + V49) / 2 */
			env(c49, p, 440e3, 470.0, 15e-6, ek_c49);
			const double in = clampd(sk_bm.run(n2ac), -6.47, 4.0);
			sj += br[B_BM].k * br[B_BM].g * vca_bm.run(in, (5.0 + c49) / 2);
		}

		/* ================= laser (PA6) ================= */
		{
			const bool on = !(mpa & 0x40);
			if (awake(idle_las, on)) {
				u7.step(8.0);
				vco_las.step(u7.v);                                                /* U8a follows the 555's timing capacitor */
			}
			const double v = sw_las.step(vco_las.avg_so * 2.2 / 12.2, on);          /* R75 10k / R76 2.2k -> C55 -> U17 (8-9) */
			sj += br[B_LAS].k * br[B_LAS].g * v;
		}

		/* ================= battleship (PA7) and Super Zaxxon SOUND OUT B ================= */
		{
			const bool on = !(mpa & 0x80);
			const bool run = awake(idle_bs, on);
			if (run) lfo_bs.step(1.09);                                            /* U9a: R80 2.2M / R81 220k from 12 V */
			/* U10a should follow 0.495 V (R90 120k / R91 100k), but R92 30k feeds the LFO ramp into its output and an LM324
			   cannot sink that current below about 0.6 V, so the VCO's control sits at the op-amp's floor (oa_floor) and moves
			   only a little with the ramp. */
			const double vc = std::max(0.495, oa_floor((lfo_bs.avg_vi - 0.6) / 30e3));
			if (run) vco_bs.step(vc);
			const double vbs = vco_bs.vplus(vco_bs.avg_so);                        /* U10d follows the Schmitt's + input */
			double sob = 0;
			if (sz && run) sob = sound_out_b();
			/* U17 pin 1 node: U10d -> C59 2.2u, and on Super Zaxxon SOUND OUT B -> C28 1u (MB4391 output, [ASSUMPTION] 200 ohm) */
			const double rb = par(25.5e3, 51.7e3), vb = VREF * 51.7 / 77.2;         /* R189/R190 51k, R191 + R192 */
			const double pin1 = vbs - bs_v59;
			const double i28 = sz ? (sob + VREF - bs_v28 - pin1) / 200.0 : 0.0;
			const double iload = on ? (pin1 - vb) / (RON_4016 + rb) : 0.0;
			bs_v59 += (iload - i28) * DT / 2.2e-6;
			bs_v28 += i28 * DT / 1e-6;
			const double v = on ? iload * rb : 0.0;
			sj += br[B_BS].k * br[B_BS].g * v;
		}

		/* ================= S-EXP (PB4) and M-EXP (PB5) ================= */
		{
			env(c61, os_sexp.step(), 940e3, 1e3, 2.2e-6, ek_c61);                           /* D7, R106 1k; R104 + R105 470k */
			env(c63, os_mexp.step(), 2e6, 470.0, 1e-6, ek_c63);                             /* D8, R212 470; R109 + R110 1M */
			const double s = clampd(sk_sexp.run(n2ac), -5.97, 4.5);
			const double m = clampd(sk_mexp.run(naac), -5.97, 4.5);
			sj += br[B_SEXP].k * br[B_SEXP].g * vca_sexp.run(s, (5.0 + c61) / 2);  /* C74 -> U15 half 2 */
			const double km = par(33e3, 17e3) / (33e3 + par(33e3, 17e3));           /* R123 / R124 33k || MB4391 input */
			sj += br[B_MEXP].k * br[B_MEXP].g * vca_mexp.run(m * km, (5.0 + c63) / 2);  /* C76 -> U13 half 2 */
		}

		/* ================= cannon (PB7) ================= */
		{
			const bool q = os_can.step();
			/* Q high -> R126 330 pull-up -> D9 -> C79 6.8u; R127 100k */
			{
				const double g = 1.0 / 100e3 + (q && c79 < 5.0 - VD ? 1.0 / 330.0 : 0.0);
				const double vinf = (q && c79 < 5.0 - VD ? (5.0 - VD) / 330.0 : 0.0) / g;
				c79 = vinf + (c79 - vinf) * ek_c79(g / 6.8e-6);
			}
			if (++can_cnt >= 8) {
				can_cnt = 0;
				/* Q6 (C1684): base from U12a through R131 15k with R132 3.3k to ground.  Saturated with ~0 V across it, its
				   collector-emitter conductance is Ib (1 + 1/bR) / ((1/bF + 1/bR) VT) (Ebers-Moll): 3.9 Ib / VT for a reverse beta
				   of 3, the same transistor model as the VCO switches.  That opens the sweep at the 3.7 kHz average of the real
				   board's recording. */
				const double vth = c79 * 3.3 / 18.3, rth = par(15e3, 3.3e3);
				double vbe = 0.55;
				for (int k = 0; k < 6; k++) {                                       /* Newton: vth = rth Ib + vbe, Ib = Is e^(vbe/VT) */
					const double ib = 8.2e-16 * std::exp(vbe / 0.02585);
					const double f = rth * ib + vbe - vth, df = rth * ib / 0.02585 + 1.0;
					vbe -= f / df; vbe = std::min(vbe, 0.8);
				}
				const double ib = 8.2e-16 * std::exp(vbe / 0.02585);
				const double gce = 3.9 * ib / 0.02585;
				can_rm = 100.0 + 1.0 / (1.0 / 1.5e3 + gce);
				set_cannon(can_rm);
			}
			const double f = clampd(bt_can.run(n2ac), -5.97, 4.5);                 /* N2 -> C80 2.2u -> R128 */
			const double kin = par(100e3, 17e3) / (100e3 + par(100e3, 17e3));       /* C83 -> R134 100k -> R135 100k || input */
			const double con = std::max(OA_LO, 2 * 2.4 - c79);                      /* U12c: R136/R137 51k, + at 2.4 V (R139/R141) */
			sj += br[B_CAN].k * br[B_CAN].g * vca_can.run(f * kin, con);           /* C84 -> U13 half 1 */
		}

		/* ================= shot (PC0) ================= */
		{
			const bool p = os_shot.step();
			/* W: Q-bar through R144 560 against R143 3.3k to 5 V; C88 47n couples W into Y; D10 discharges Z (C89 0.68u) into W */
			const double w = p ? (5.0 / 3.3e3 + TTL_LO / 560.0) / (1.0 / 3.3e3 + 1.0 / 560.0) : 5.0;
			const double rw = p ? par(3.3e3, 560.0) : 3.3e3;
			if (p != sh_prev) { sh_y += w - sh_w; sh_prev = p; }
			sh_w = w;
			/* Y: R145 + R146 (1.27M) from 12 V, R147 1M to Z; Z: R148 2.2M to ground, diode into W */
			sh_y += ((12.0 - sh_y) / 1.27e6 + (sh_z - sh_y) / 1e6) * DT / 47e-9;
			{
				const bool d = sh_z > w + VD;
				const double g = 1.0 / 1e6 + 1.0 / 2.2e6 + (d ? 1.0 / rw : 0.0);
				const double vinf = (sh_y / 1e6 + (d ? (w + VD) / rw : 0.0)) / g;
				sh_z = vinf + (sh_z - vinf) * ek_z(g / SHOT_C89);
			}
			/* U19a follows Y; U19b: R149 5.6k, R150 33k around 6 V.  U19b also drives U18's CV pin (8 V through 3.33k inside the
			   555), so near ground it must sink over 2 mA and stops at the LM324's floor: U18 keeps running at idle. */
			double s = clampd(VREF - (33.0 / 5.6) * (sh_y - VREF), OA_LO, OA_HI);
			if (s < 1.0) {
				const double fl = oa_floor((8.0 - 0.66) / 3333.0 + (VREF - 0.66) / 33e3 + (sh_p - 0.66) / 8.2e3);
				s = std::max(s, fl);
			}
			u18.step(s);                                                           /* CV pin driven by U19b */
			/* P: R153 2.7k from U18, R154 8.2k from U19b, R155 820 and C91 15u to ground, the VCO's input (about 33k) */
			{
				const double g = 1.0 / 2.7e3 + 1.0 / 8.2e3 + 1.0 / 820.0 + 1.0 / 33e3;
				const double vinf = (u18.avg / 2.7e3 + s / 8.2e3) / g;
				sh_p = vinf + (sh_p - vinf) * ek_p(g / 15e-6);
			}
			vco_shot.step(sh_p);
			/* U19d -> R164 1M / R165 220k (|| the MB4391 input) -> C93 -> U16 half 1; CON from U20a (follows Z) */
			const double kin = par(220e3, 17e3) / (1e6 + par(220e3, 17e3));
			const double in = sh_out.run(vco_shot.avg_so) * kin;
			sj += br[B_SHOT].k * br[B_SHOT].g * vca_shot.run(in, sh_z);
		}

		/* ================= alarms (PC2, PC3) ================= */
		{
			const bool a2 = os_al2.step(), a3 = os_al3.step();
			/* 556 U50 -> 74393 (counts on the falling edge); 7426 gates (alarm 2 Q & QD), (alarm 3 Q & QC), wired with R171 1k to 12 V */
			double t_last = 0, acc = 0;
			auto lvl = [&]() { const bool low = (a2 && (cnt393 & 8)) || (a3 && (cnt393 & 4)); return low ? TTL_LO : 12.0; };
			double cur = lvl();
			if (awake(idle_al, a2 || a3))
				u50.step(3.333, [&](double t, bool out) {
					acc += cur * (t - t_last); t_last = t;
					if (!out) cnt393 = (cnt393 + 1) & 15;
					cur = lvl();
				});
			acc += cur * (DT - t_last);
			const double vin = acc / DT;
			/* U12d: R172 1.5k into an integrator, R173 330k || C99 10n, + input at 6 V */
			const double dv = -((vin - VREF) / 1.5e3 + (al_int - VREF) / 330e3) / 10e-9 * DT;
			al_int = clampd(al_int + clampd(dv, -OA_SR * DT, OA_SR * DT), OA_LO, OA_HI);
			sj += br[B_ALM].k * br[B_ALM].g * al_out.run(al_int);
		}

		/* ================= Super Zaxxon SOUND OUT A (engine) ================= */
		if (sz && eng_style == ENGINE_BOARD) sj += br[B_SOA].k * br[B_SOA].g * sound_out_a();

		/* ================= summing amplifier U11 ================= */
		const double sjv = sj / gtot;
		const double u11 = clampd(VREF - 8.2 * sjv, OA_LO, OA_HI);
		last_u11 = u11;

		/* decimate */
		firbuf[fir_pos] = firbuf[fir_pos + FIR_N] = u11 - VREF;
		fir_pos = (fir_pos + 1) % FIR_N;
		if (++sub_phase >= OS) {
			sub_phase = 0;
			double y = 0;
			const double *x = &firbuf[fir_pos];
			for (int i = 0; i < FIR_N; i++) y += fir[i] * x[i];
			dc_lp += (y - dc_lp) * dc_k;
			double s = (y - dc_lp) * master;
			if (std::fabs(s) > 24000.0) s = (s > 0 ? 1 : -1) * (24000.0 + 8767.0 * std::tanh((std::fabs(s) - 24000.0) / 8767.0));
			fifo.push_back((int16_t)std::lrint(clampd(s, -32767, 32767)));
		}
	}

	bool sh_prev = false; double sh_w = 5.0;

	/* An oscillator whose sound is switched off cannot be heard (its 4016 switch or gate is open), so it is paused a quarter
	   of a second after its sound stops, to save time.  Its phase when the sound starts again is arbitrary on the real board too. */
	int idle_hm = 0, idle_las = 0, idle_bs = 0, idle_al = 0;
	double k_c43 = 1.0 - std::exp(-DT / (6.8e3 * 6.8e-6));
	static bool awake(int &idle, bool on)
	{
		if (on) { idle = 0; return true; }
		if (idle < (int)(FS * 0.25)) { idle++; return true; }
		return false;
	}

	/* diode-discharged envelope capacitor: charged to 5 V through rup; while the one-shot's Q-bar is low it is pulled to
	   TTL low + one diode drop through rdn */
	ExpK ek_c34, ek_c35, ek_c49, ek_c61, ek_c63, ek_c79, ek_z, ek_p;
	static void env(double &v, bool pulse, double rup, double rdn, double c, ExpK &ek)
	{
		const double vl = TTL_LO + VD;
		const bool d = pulse && v > vl;
		const double g = 1.0 / rup + (d ? 1.0 / rdn : 0.0);
		const double vinf = (5.0 / rup + (d ? vl / rdn : 0.0)) / g;
		v = vinf + (v - vinf) * ek(g / c);
	}

	/* -------- Zaxxon player ship: the two-bit DAC that sets the engine's pitch */
	void ship_dac(uint8_t m, double dt)
	{
		/* 7406 open-collector outputs pulled up by R10 / R11 2.2k: PA = 0 -> high */
		const bool a_hi = !(m & 1), b_hi = !(m & 2);
		const double va = a_hi ? 12.0 : TTL_LO, ra = a_hi ? 2.2e3 + 68e3 : 68e3;      /* through R12 68k */
		const double vb = b_hi ? 12.0 : TTL_LO, rb = b_hi ? 2.2e3 + 100e3 : 100e3;    /* through R16 100k */
		const double t = (12.0 / 82e3 + va / ra + ship_y / 36e3) / (1.0 / 82e3 + 1.0 / 56e3 + 1.0 / ra + 1.0 / 36e3);  /* R13, R15, R14 */
		ship_y += ((t - ship_y) / 36e3 + (vb - ship_y) / rb) * dt / 15e-6;              /* C25 15u */
	}
	/* U8 follower -> MCD-725H LED through R17 390 -> CdS cell.  The DAC gives C25 about 8.9 / 6.5 / 4.6 / 2.2 V for PA&3 = 0..3,
	   so the LED runs at 19.3 / 13.0 / 8.2 / 2.0 mA.  [CALIBRATED: LED 1.4 V; cell 1.55k at 19.3 mA, R ~ I^-0.8, 2 M dark, 8 ms to
	   brighten, 40 ms to darken.  This puts the noise band at 630 Hz for PA&3 = 0 and 331 Hz for PA&3 = 3, the peaks of the real
	   board's two recordings (627 and 331 Hz)]  Returns the conductance the cell is heading for. */
	double cds_target() const
	{
		const double i = std::max(0.0, (std::min(ship_y, OA_HI) - 1.4) / 390.0);
		return 1.0 / 2e6 + (i > 0 ? (1.0 / 1.55e3) * std::pow(i / 19.3e-3, 0.8) : 0.0);
	}

	/* -------- Super Zaxxon daughterboard, SOUND OUT A: the engine */
	double sound_out_a()
	{
		/* PA0 -> 7416 -> R37 330k, PA1 -> 7416 -> R36 220k pull node Q down; T: R33 18k from 5 V, R34 68k, C19 33u; R35 15k; C20 2.2u */
		const bool d0 = pa & 1, d1 = pa & 2;
		sz_t += ((5.0 - sz_t) / 18e3 - sz_t / 68e3 + (sz_q - sz_t) / 15e3) * DT / 33e-6;
		sz_q += ((sz_t - sz_q) / 15e3 - (d0 ? (sz_q - TTL_LO) / 330e3 : 0.0) - (d1 ? (sz_q - TTL_LO) / 220e3 : 0.0)) * DT / 2.2e-6;
		/* 555 U12 (R1 12k, R2 33k, C5 1u, about 18 Hz) -> VCO U6 (about 43 Hz per volt) */
		u12.step(8.0);
		vco_eng.step(u12.v);
		const double saw = vco_eng.avg_vi;                                      /* U11 follows the integrator output */
		const double r = clampd(sk_rumble.run(hp_eng.run(saw)), -5.97, 4.5);  /* 50 Hz low pass, Q 0.6 */
		const double w = clampd(sk_whine.run(saw - VREF), -5.97, 4.5);         /* 860 Hz high pass, Q 1.34 */
		/* rumble: C14 -> U10 half 2, CON = U9 pin 14 (follows Q);  whine: R15 82k / R16 10k -> U5a (on when PA2 = 1) -> C9 -> U10 half 1,
		   CON = U9 pin 1 = 2 x 3.238 - Q (R38/R39 12k, R40 37k / R41 68k) */
		const double orum = vca_rumble.run(r, sz_q);
		const double win = sw_whine.step((VREF + w) * 10.0 / 92.0, (pa & 0x04) != 0);
		const double owh = vca_whine.run(win, 2 * 3.238 - sz_q);
		/* taps R24 5.6k / R25 47k and R17 36k / R18 13k -> C11 / C16 -> R19 / R26 47k -> node -> R27 12k -> U9b (R28 47k) */
		const double g1 = 1.0 / (47e3 + par(5.6e3, 47e3)), g2 = 1.0 / (47e3 + par(36e3, 13e3)), g3 = 1.0 / 12e3;
		const double x = (g1 * orum * 47.0 / 52.6 + g2 * owh * 13.0 / 49.0) / (g1 + g2 + g3);
		const double u9b = clampd(VREF - (47.0 / 12.0) * x, OA_LO, OA_HI);
		/* C17 -> U5b (on when PA3 = 0) -> R29 / R30 47k -> C18 -> SOUND OUT A */
		return sw_out.step(u9b, !(pa & 0x08));
	}

	/* -------- Super Zaxxon daughterboard, SOUND OUT B: three VCOs and an attenuator, swept by a 0.9 Hz ramp */
	double sound_out_b()
	{
		u7b.step(8.0);                                                          /* R42 680k, R43 20k, C23 2.2u */
		const double vc = u7b.v;                                               /* U6 follows the timing capacitor */
		vco1.step(vc); vco2.step(vc); vco3.step(vc);
		/* U2a: VCO1 and VCO2 squares through C25 / C30 and R52 / R53 220k, R54 8.2k */
		const double mix = -(8.2 / 220.0) * (c25.run(vco1.avg_so) + c30.run(vco2.avg_so));
		/* U2d: VCO3 triangle through C32 / R71 100k, R72 56k, + input at 2.9 V (R73 47k / R74 15k) */
		const double con = 2.903 * (1.0 + 56.0 / 100.0) - (56.0 / 100.0) * c32.run(vco3.avg_vi);
		return vca_sob.run(mix, con);                                          /* U1, out through C28 */
	}
};

} // namespace zsnd
