#include "z80.h"
#include <string.h>

#define A  z->af.b.h
#define F  z->af.b.l
#define B  z->bc.b.h
#define C  z->bc.b.l
#define D  z->de.b.h
#define E  z->de.b.l
#define H  z->hl.b.h
#define L  z->hl.b.l

enum { FS = 0x80, FZ = 0x40, FY = 0x20, FH = 0x10, FX = 0x08, FP = 0x04, FN = 0x02, FC = 0x01 };

static uint8_t PAR[256];
static int par_init;

void z80_reset(Z80 *z)
{
    if (!par_init) {
        for (int i = 0; i < 256; i++) {
            int p = 1, v = i;
            while (v) { p ^= v & 1; v >>= 1; }
            PAR[i] = p ? FP : 0;
        }
        par_init = 1;
    }
    z->af.w = z->bc.w = z->de.w = z->hl.w = 0xffff;
    z->af2.w = z->bc2.w = z->de2.w = z->hl2.w = 0xffff;
    z->ix.w = z->iy.w = 0xffff; z->sp.w = 0xffff; z->pc.w = 0;
    z->i = z->r = 0; z->iff1 = z->iff2 = 0; z->im = 0;
    z->halted = 0; z->ei_delay = 0; z->irq = 0; z->cycles = 0;
}

static inline uint8_t rd8(Z80 *z, uint16_t a) { return z->rd(z->ud, a); }
static inline void wr8(Z80 *z, uint16_t a, uint8_t v) { z->wr(z->ud, a, v); }
static inline uint16_t rd16(Z80 *z, uint16_t a) { return rd8(z, a) | (rd8(z, a + 1) << 8); }
static inline void wr16(Z80 *z, uint16_t a, uint16_t v) { wr8(z, a, v & 255); wr8(z, a + 1, v >> 8); }
static inline uint8_t fetch8(Z80 *z) { return rd8(z, z->pc.w++); }
static inline uint8_t fetchop(Z80 *z) { uint16_t a = z->pc.w++; return z->rdop ? z->rdop(z->ud, a) : z->rd(z->ud, a); }
static inline uint16_t fetch16(Z80 *z) { uint16_t v = rd16(z, z->pc.w); z->pc.w += 2; return v; }
static inline void push(Z80 *z, uint16_t v) { z->sp.w -= 2; wr16(z, z->sp.w, v); }
static inline uint16_t pop(Z80 *z) { uint16_t v = rd16(z, z->sp.w); z->sp.w += 2; return v; }

/* ------------------------------------------------------------------ ALU */
static uint8_t alu_add(Z80 *z, uint8_t a, uint8_t v, int c)
{
    int r = a + v + c;
    uint8_t res = r;
    F = (res & (FS | FY | FX)) | (res ? 0 : FZ) | ((a ^ v ^ res) & FH)
        | (((a ^ v ^ 0x80) & (a ^ res) & 0x80) ? FP : 0) | ((r >> 8) & 1);
    return res;
}
static uint8_t alu_sub(Z80 *z, uint8_t a, uint8_t v, int c)
{
    int r = a - v - c;
    uint8_t res = r;
    F = (res & (FS | FY | FX)) | (res ? 0 : FZ) | ((a ^ v ^ res) & FH)
        | (((a ^ v) & (a ^ res) & 0x80) ? FP : 0) | FN | ((r >> 8) & 1);
    return res;
}
static void alu(Z80 *z, int op, uint8_t v)
{
    switch (op) {
    case 0: A = alu_add(z, A, v, 0); break;
    case 1: A = alu_add(z, A, v, F & FC); break;
    case 2: A = alu_sub(z, A, v, 0); break;
    case 3: A = alu_sub(z, A, v, F & FC); break;
    case 4: A &= v; F = (A & (FS | FY | FX)) | (A ? 0 : FZ) | FH | PAR[A]; break;
    case 5: A ^= v; F = (A & (FS | FY | FX)) | (A ? 0 : FZ) | PAR[A]; break;
    case 6: A |= v; F = (A & (FS | FY | FX)) | (A ? 0 : FZ) | PAR[A]; break;
    case 7: { alu_sub(z, A, v, 0); F = (F & ~(FY | FX)) | (v & (FY | FX)); } break;
    }
}
static uint8_t inc8(Z80 *z, uint8_t v)
{
    uint8_t r = v + 1;
    F = (F & FC) | (r & (FS | FY | FX)) | (r ? 0 : FZ) | ((r & 15) == 0 ? FH : 0) | (r == 0x80 ? FP : 0);
    return r;
}
static uint8_t dec8(Z80 *z, uint8_t v)
{
    uint8_t r = v - 1;
    F = (F & FC) | (r & (FS | FY | FX)) | (r ? 0 : FZ) | ((r & 15) == 15 ? FH : 0) | (r == 0x7f ? FP : 0) | FN;
    return r;
}
static uint16_t add16(Z80 *z, uint16_t a, uint16_t b)
{
    uint32_t r = a + b;
    F = (F & (FS | FZ | FP)) | ((r >> 8) & (FY | FX)) | (((a ^ b ^ r) >> 8) & FH) | ((r >> 16) & 1);
    return r;
}
static uint16_t adc16(Z80 *z, uint16_t a, uint16_t b)
{
    uint32_t r = a + b + (F & FC);
    uint16_t res = r;
    F = ((res >> 8) & (FS | FY | FX)) | (res ? 0 : FZ) | (((a ^ b ^ res) >> 8) & FH)
        | ((((a ^ b ^ 0x8000) & (a ^ res) & 0x8000) ? FP : 0)) | ((r >> 16) & 1);
    return res;
}
static uint16_t sbc16(Z80 *z, uint16_t a, uint16_t b)
{
    uint32_t r = a - b - (F & FC);
    uint16_t res = r;
    F = ((res >> 8) & (FS | FY | FX)) | (res ? 0 : FZ) | (((a ^ b ^ res) >> 8) & FH)
        | ((((a ^ b) & (a ^ res) & 0x8000) ? FP : 0)) | FN | ((r >> 16) & 1);
    return res;
}
static void daa(Z80 *z)
{
    uint8_t a = A, adj = 0, c = F & FC;
    if ((F & FH) || (a & 15) > 9) adj |= 6;
    if (c || a > 0x99) { adj |= 0x60; c = 1; }
    uint8_t r = (F & FN) ? a - adj : a + adj;
    uint8_t h = (F & FN) ? (((F & FH) && (a & 15) < 6) ? FH : 0) : ((a & 15) > 9 ? FH : 0);
    F = (r & (FS | FY | FX)) | (r ? 0 : FZ) | h | PAR[r] | (F & FN) | c;
    A = r;
}

static uint8_t rot(Z80 *z, int y, uint8_t v)
{
    uint8_t c, r;
    switch (y) {
    case 0: c = v >> 7; r = (v << 1) | c; break;                  /* RLC */
    case 1: c = v & 1;  r = (v >> 1) | (c << 7); break;           /* RRC */
    case 2: c = v >> 7; r = (v << 1) | (F & FC); break;           /* RL  */
    case 3: c = v & 1;  r = (v >> 1) | ((F & FC) << 7); break;    /* RR  */
    case 4: c = v >> 7; r = v << 1; break;                        /* SLA */
    case 5: c = v & 1;  r = (v >> 1) | (v & 0x80); break;         /* SRA */
    case 6: c = v >> 7; r = (v << 1) | 1; break;                  /* SLL */
    default: c = v & 1; r = v >> 1; break;                        /* SRL */
    }
    F = (r & (FS | FY | FX)) | (r ? 0 : FZ) | PAR[r] | c;
    return r;
}

/* -------------------------------------------------------- register access */
static uint8_t getr(Z80 *z, int i, int pfx)
{
    switch (i) {
    case 0: return B; case 1: return C; case 2: return D; case 3: return E;
    case 4: return pfx == 1 ? z->ix.b.h : pfx == 2 ? z->iy.b.h : H;
    case 5: return pfx == 1 ? z->ix.b.l : pfx == 2 ? z->iy.b.l : L;
    default: return A;
    }
}
static void setr(Z80 *z, int i, int pfx, uint8_t v)
{
    switch (i) {
    case 0: B = v; break; case 1: C = v; break; case 2: D = v; break; case 3: E = v; break;
    case 4: if (pfx == 1) z->ix.b.h = v; else if (pfx == 2) z->iy.b.h = v; else H = v; break;
    case 5: if (pfx == 1) z->ix.b.l = v; else if (pfx == 2) z->iy.b.l = v; else L = v; break;
    default: A = v; break;
    }
}
static uint16_t *rp(Z80 *z, int p, int pfx)
{
    switch (p) {
    case 0: return &z->bc.w; case 1: return &z->de.w;
    case 2: return pfx == 1 ? &z->ix.w : pfx == 2 ? &z->iy.w : &z->hl.w;
    default: return &z->sp.w;
    }
}
static uint16_t *rp2(Z80 *z, int p, int pfx)
{
    if (p == 3) return &z->af.w;
    return rp(z, p, pfx);
}
static int cc(Z80 *z, int y)
{
    switch (y) {
    case 0: return !(F & FZ); case 1: return F & FZ;
    case 2: return !(F & FC); case 3: return F & FC;
    case 4: return !(F & FP); case 5: return F & FP;
    case 6: return !(F & FS); default: return F & FS;
    }
}

/* -------------------------------------------------------------- CB group */
static int do_cb(Z80 *z, int pfx)
{
    uint16_t addr = 0; int8_t d = 0; int cyc;
    if (pfx) d = (int8_t)fetch8(z);
    uint8_t op = pfx ? fetch8(z) : fetchop(z);   /* DD CB d op: the op byte is not an M1 fetch */
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7;
    uint8_t v;
    int mem = (zz == 6) || pfx;
    if (pfx) addr = (pfx == 1 ? z->ix.w : z->iy.w) + d; else addr = z->hl.w;
    v = mem ? rd8(z, addr) : getr(z, zz, 0);
    if (x == 1) {
        uint8_t t = v & (1 << y);
        F = (F & FC) | FH | (t ? 0 : (FZ | FP)) | (t & FS) | (mem ? ((addr >> 8) & (FY | FX)) : (v & (FY | FX)));
        return pfx ? 20 : (mem ? 12 : 8);
    }
    uint8_t r;
    if (x == 0) r = rot(z, y, v);
    else if (x == 2) r = v & ~(1 << y);
    else r = v | (1 << y);
    if (mem) { wr8(z, addr, r); if (pfx && zz != 6) setr(z, zz, 0, r); cyc = pfx ? 23 : 15; }
    else { setr(z, zz, 0, r); cyc = 8; }
    return cyc;
}

/* -------------------------------------------------------------- ED group */
static int do_ed(Z80 *z)
{
    uint8_t op = fetchop(z);
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;
    if (x == 1) {
        switch (zz) {
        case 0: { uint8_t v = z->in(z->ud, z->bc.w);
                  F = (F & FC) | (v & (FS | FY | FX)) | (v ? 0 : FZ) | PAR[v];
                  if (y != 6) setr(z, y, 0, v); return 12; }
        case 1: z->out(z->ud, z->bc.w, y == 6 ? 0 : getr(z, y, 0)); return 12;
        case 2: { uint16_t *r = rp(z, p, 0);
                  z->hl.w = q ? adc16(z, z->hl.w, *r) : sbc16(z, z->hl.w, *r); return 15; }
        case 3: { uint16_t a = fetch16(z); uint16_t *r = rp(z, p, 0);
                  if (q) *r = rd16(z, a); else wr16(z, a, *r); return 20; }
        case 4: { uint8_t a = A; A = 0; A = alu_sub(z, 0, a, 0); return 8; }
        case 5: z->pc.w = pop(z); z->iff1 = z->iff2; return 14;
        case 6: { static const int m[8] = {0, 0, 1, 2, 0, 0, 1, 2}; z->im = m[y]; return 8; }
        default:
            switch (y) {
            case 0: z->i = A; return 9;
            case 1: z->r = A; return 9;
            case 2: A = z->i; F = (F & FC) | (A & (FS | FY | FX)) | (A ? 0 : FZ) | (z->iff2 ? FP : 0); return 9;
            case 3: A = z->r; F = (F & FC) | (A & (FS | FY | FX)) | (A ? 0 : FZ) | (z->iff2 ? FP : 0); return 9;
            case 4: { uint8_t m = rd8(z, z->hl.w); uint8_t a = A;
                      /* RRD: A.low <- (HL).low ; (HL) <- (A.low<<4)|(HL>>4) */
                      wr8(z, z->hl.w, (a << 4) | (m >> 4));
                      A = (a & 0xf0) | (m & 0x0f);
                      F = (F & FC) | (A & (FS | FY | FX)) | (A ? 0 : FZ) | PAR[A]; return 18; }
            case 5: { uint8_t m = rd8(z, z->hl.w); uint8_t a = A;
                      wr8(z, z->hl.w, (m << 4) | (a & 0x0f));
                      A = (a & 0xf0) | (m >> 4);
                      F = (F & FC) | (A & (FS | FY | FX)) | (A ? 0 : FZ) | PAR[A]; return 18; }
            default: return 8;
            }
        }
    }
    if (x == 2 && zz < 4 && y >= 4) {
        int inc = !(y & 1), rep = y >= 6; int cyc = 16;
        switch (zz) {
        case 0: { uint8_t v = rd8(z, z->hl.w); wr8(z, z->de.w, v);
                  z->hl.w += inc ? 1 : -1; z->de.w += inc ? 1 : -1; z->bc.w--;
                  uint8_t n = v + A;
                  F = (F & (FS | FZ | FC)) | (n & FX) | ((n << 4) & FY) | (z->bc.w ? FP : 0);
                  if (rep && z->bc.w) { z->pc.w -= 2; cyc = 21; } } break;
        case 1: { uint8_t v = rd8(z, z->hl.w); uint8_t cf = F & FC;
                  uint8_t r = A - v; z->hl.w += inc ? 1 : -1; z->bc.w--;
                  uint8_t h = ((A ^ v ^ r) & FH);
                  uint8_t n = r - (h ? 1 : 0);
                  F = cf | FN | (r & FS) | (r ? 0 : FZ) | h | (z->bc.w ? FP : 0) | (n & FX) | ((n << 4) & FY);
                  if (rep && z->bc.w && r) { z->pc.w -= 2; cyc = 21; } } break;
        case 2: { uint8_t v = z->in(z->ud, z->bc.w); wr8(z, z->hl.w, v); z->hl.w += inc ? 1 : -1; B--;
                  F = (B & (FS | FY | FX)) | (B ? 0 : FZ) | FN;
                  if (rep && B) { z->pc.w -= 2; cyc = 21; } } break;
        default: { uint8_t v = rd8(z, z->hl.w); B--; z->out(z->ud, z->bc.w, v); z->hl.w += inc ? 1 : -1;
                  F = (B & (FS | FY | FX)) | (B ? 0 : FZ) | FN;
                  if (rep && B) { z->pc.w -= 2; cyc = 21; } } break;
        }
        return cyc;
    }
    return 8; /* undefined ED: NOP */
}

/* ------------------------------------------------------------- main step */
int z80_step(Z80 *z)
{
    int cyc = 0;

    if (z->nmi) {
        z->nmi = 0; z->halted = 0; z->iff2 = z->iff1; z->iff1 = 0;
        push(z, z->pc.w); z->pc.w = 0x66;
        z->cycles += 11; z->r++;
        return 11;
    }
    if (z->irq && z->iff1 && !z->ei_delay) {
        z->irq = 0; z->halted = 0; z->iff1 = z->iff2 = 0;
        push(z, z->pc.w);
        if (z->im == 2) z->pc.w = rd16(z, (z->i << 8) | 0xff);
        else z->pc.w = 0x38;
        z->cycles += 13; z->r++;
        return 13;
    }
    z->ei_delay = 0;
    if (z->halted) { z->cycles += 4; return 4; }

    int pfx = 0;
    uint8_t op;
    for (;;) {
        op = fetchop(z); z->r++; cyc += 4;
        if (op == 0xdd) { pfx = 1; continue; }
        if (op == 0xfd) { pfx = 2; continue; }
        break;
    }
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;
    uint16_t *HLp = pfx == 1 ? &z->ix.w : pfx == 2 ? &z->iy.w : &z->hl.w;

    /* helper for (HL)/(IX+d) operand address */
    #define MEMADDR() (pfx ? (uint16_t)(*HLp + (int8_t)fetch8(z)) : z->hl.w)

    switch (x) {
    case 0:
        switch (zz) {
        case 0:
            if (y == 0) { cyc += 0; }
            else if (y == 1) { uint16_t t = z->af.w; z->af.w = z->af2.w; z->af2.w = t; }
            else if (y == 2) { int8_t d = fetch8(z); if (--B) { z->pc.w += d; cyc += 9; } else cyc += 4; }
            else if (y == 3) { int8_t d = fetch8(z); z->pc.w += d; cyc += 8; }
            else { int8_t d = fetch8(z); if (cc(z, y - 4)) { z->pc.w += d; cyc += 8; } else cyc += 3; }
            break;
        case 1:
            if (!q) { *rp(z, p, pfx) = fetch16(z); cyc += 6; }
            else { *HLp = add16(z, *HLp, *rp(z, p, pfx)); cyc += 7; }
            break;
        case 2:
            if (!q) {
                switch (p) {
                case 0: wr8(z, z->bc.w, A); cyc += 3; break;
                case 1: wr8(z, z->de.w, A); cyc += 3; break;
                case 2: { uint16_t a = fetch16(z); wr16(z, a, *HLp); cyc += 12; } break;
                default: { uint16_t a = fetch16(z); wr8(z, a, A); cyc += 9; } break;
                }
            } else {
                switch (p) {
                case 0: A = rd8(z, z->bc.w); cyc += 3; break;
                case 1: A = rd8(z, z->de.w); cyc += 3; break;
                case 2: { uint16_t a = fetch16(z); *HLp = rd16(z, a); cyc += 12; } break;
                default: { uint16_t a = fetch16(z); A = rd8(z, a); cyc += 9; } break;
                }
            }
            break;
        case 3: *rp(z, p, pfx) += q ? -1 : 1; cyc += 2; break;
        case 4:
            if (y == 6) { uint16_t a = MEMADDR(); wr8(z, a, inc8(z, rd8(z, a))); cyc += pfx ? 15 : 7; }
            else setr(z, y, pfx, inc8(z, getr(z, y, pfx)));
            break;
        case 5:
            if (y == 6) { uint16_t a = MEMADDR(); wr8(z, a, dec8(z, rd8(z, a))); cyc += pfx ? 15 : 7; }
            else setr(z, y, pfx, dec8(z, getr(z, y, pfx)));
            break;
        case 6:
            if (y == 6) { uint16_t a = MEMADDR(); uint8_t n = fetch8(z); wr8(z, a, n); cyc += pfx ? 11 : 6; }
            else { setr(z, y, pfx, fetch8(z)); cyc += 3; }
            break;
        default:
            switch (y) {
            case 0: { uint8_t c = A >> 7; A = (A << 1) | c; F = (F & (FS | FZ | FP)) | (A & (FY | FX)) | c; } break;
            case 1: { uint8_t c = A & 1; A = (A >> 1) | (c << 7); F = (F & (FS | FZ | FP)) | (A & (FY | FX)) | c; } break;
            case 2: { uint8_t c = A >> 7; A = (A << 1) | (F & FC); F = (F & (FS | FZ | FP)) | (A & (FY | FX)) | c; } break;
            case 3: { uint8_t c = A & 1; A = (A >> 1) | ((F & FC) << 7); F = (F & (FS | FZ | FP)) | (A & (FY | FX)) | c; } break;
            case 4: daa(z); break;
            case 5: A = ~A; F = (F & (FS | FZ | FP | FC)) | FH | FN | (A & (FY | FX)); break;
            case 6: F = (F & (FS | FZ | FP)) | FC | (A & (FY | FX)); break;
            default: { uint8_t c = F & FC; F = (F & (FS | FZ | FP)) | (c ? FH : FC) | (A & (FY | FX)); } break;
            }
        }
        break;
    case 1:
        if (op == 0x76) { z->halted = 1; }
        else if (y == 6) { uint16_t a = MEMADDR(); wr8(z, a, getr(z, zz, 0)); cyc += pfx ? 11 : 3; }
        else if (zz == 6) { uint16_t a = MEMADDR(); setr(z, y, 0, rd8(z, a)); cyc += pfx ? 11 : 3; }
        else setr(z, y, pfx, getr(z, zz, pfx));
        break;
    case 2:
        if (zz == 6) { uint16_t a = MEMADDR(); alu(z, y, rd8(z, a)); cyc += pfx ? 11 : 3; }
        else alu(z, y, getr(z, zz, pfx));
        break;
    default:
        switch (zz) {
        case 0: if (cc(z, y)) { z->pc.w = pop(z); cyc += 7; } else cyc += 1; break;
        case 1:
            if (!q) { *rp2(z, p, pfx) = pop(z); cyc += 6; }
            else switch (p) {
                case 0: z->pc.w = pop(z); cyc += 6; break;
                case 1: { uint16_t t; t = z->bc.w; z->bc.w = z->bc2.w; z->bc2.w = t;
                          t = z->de.w; z->de.w = z->de2.w; z->de2.w = t;
                          t = z->hl.w; z->hl.w = z->hl2.w; z->hl2.w = t; } break;
                case 2: z->pc.w = *HLp; break;
                default: z->sp.w = *HLp; cyc += 2; break;
            }
            break;
        case 2: { uint16_t a = fetch16(z); if (cc(z, y)) z->pc.w = a; cyc += 6; } break;
        case 3:
            switch (y) {
            case 0: z->pc.w = fetch16(z); cyc += 6; break;
            case 1: cyc = do_cb(z, pfx); break;
            case 2: { uint8_t n = fetch8(z); z->out(z->ud, (A << 8) | n, A); cyc += 7; } break;
            case 3: { uint8_t n = fetch8(z); A = z->in(z->ud, (A << 8) | n); cyc += 7; } break;
            case 4: { uint16_t t = rd16(z, z->sp.w); wr16(z, z->sp.w, *HLp); *HLp = t; cyc += 15; } break;
            case 5: { uint16_t t = z->de.w; z->de.w = z->hl.w; z->hl.w = t; } break;
            case 6: z->iff1 = z->iff2 = 0; break;
            default: z->iff1 = z->iff2 = 1; z->ei_delay = 1; break;
            }
            break;
        case 4: { uint16_t a = fetch16(z); if (cc(z, y)) { push(z, z->pc.w); z->pc.w = a; cyc += 13; } else cyc += 6; } break;
        case 5:
            if (!q) { push(z, *rp2(z, p, pfx)); cyc += 7; }
            else if (p == 0) { uint16_t a = fetch16(z); push(z, z->pc.w); z->pc.w = a; cyc += 13; }
            else if (p == 2) { cyc += do_ed(z) - 4; }
            break;
        case 6: alu(z, y, fetch8(z)); cyc += 3; break;
        default: push(z, z->pc.w); z->pc.w = y * 8; cyc += 7; break;
        }
    }
    z->cycles += cyc;
    return cyc;
}
