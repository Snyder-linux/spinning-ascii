/*
 * hat3d.c - a real-time, fully 3D rotating ASCII wizard hat.
 *
 * The hat in reference.jpg is rebuilt from parametric surfaces:
 *
 *   crown   a sagging cone whose spine is integrated from a bending angle, so
 *           the tip curls over and sways; the tube follows the spine with a
 *           parallel-transport frame and carries animated crumple folds
 *   band    a pleated velvet sleeve that gathers into the knot
 *   knot    a lumpy velvet ellipsoid, plus a cloth tail draped onto the brim
 *   brim    an oval, floppy, fluttering sheet: felt on top, satin underneath
 *   piping  a satin tube running round the brim's edge
 *
 * Every frame it:
 *   1. evaluates the surfaces on grids and takes normals from neighbours,
 *   2. renders a shadow map from the key light (self-shadowing + floor shadow),
 *   3. shades each sample (3 lights, Blinn-Phong specular, velvet sheen,
 *      fabric noise), projects it with perspective and z-buffers it,
 *   4. ray-casts a checkered floor that receives the hat's shadow,
 *   5. adds depth-tested sparkles orbiting the hat,
 *   6. emits the frame with 24-bit ANSI colour.
 *
 * Build:  cc -O2 -o hat3d hat3d.c -lm
 * Run:    ./hat3d            (interactive; see the key list below)
 *         ./hat3d --help
 *
 * Keys:  left/right spin speed   up/down camera pitch   + - zoom
 *        space pause   c colour   s sparkles   f floor   w wobble
 *        r reset   q / Esc quit
 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ math */

typedef struct { double x, y, z; } V3;
typedef struct { double m[3][3]; } M3;

static inline V3 v3(double x, double y, double z) { V3 r = {x, y, z}; return r; }
static inline V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 vmul(V3 a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline double vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 vcross(V3 a, V3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline double vlen(V3 a) { return sqrt(vdot(a, a)); }
static inline V3 vnorm(V3 a) { double l = vlen(a); return l > 1e-12 ? vmul(a, 1 / l) : a; }
static inline V3 vlerp(V3 a, V3 b, double t) { return vadd(a, vmul(vsub(b, a), t)); }

static V3 mxv(M3 A, V3 v) {
    return v3(A.m[0][0] * v.x + A.m[0][1] * v.y + A.m[0][2] * v.z,
              A.m[1][0] * v.x + A.m[1][1] * v.y + A.m[1][2] * v.z,
              A.m[2][0] * v.x + A.m[2][1] * v.y + A.m[2][2] * v.z);
}
static M3 mxm(M3 A, M3 B) {
    M3 R;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            R.m[i][j] = A.m[i][0] * B.m[0][j] + A.m[i][1] * B.m[1][j] + A.m[i][2] * B.m[2][j];
    return R;
}
static M3 mtrans(M3 A) {
    M3 R;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) R.m[i][j] = A.m[j][i];
    return R;
}
static M3 rot_x(double a) { double c = cos(a), s = sin(a); M3 r = {{{1, 0, 0}, {0, c, -s}, {0, s, c}}}; return r; }
static M3 rot_y(double a) { double c = cos(a), s = sin(a); M3 r = {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}}; return r; }
static M3 rot_z(double a) { double c = cos(a), s = sin(a); M3 r = {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}}; return r; }

static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
static double smooth(double e0, double e1, double x) {
    double t = clampd((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}
static double angdiff(double a, double b) {          /* a-b wrapped to (-pi, pi] */
    double d = fmod(a - b + M_PI, 2 * M_PI);
    if (d < 0) d += 2 * M_PI;
    return d - M_PI;
}
static double hash3(unsigned a, unsigned b, unsigned c) {   /* -> [0,1) */
    unsigned h = a * 0x8da6b343u ^ b * 0xd8163841u ^ c * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (h & 0xffffff) / 16777216.0;
}

/* ------------------------------------------------------------ hat model */

#define NSPINE    512
#define CROWN_LEN 3.7
#define BRIM_RIN  0.78
#define BAND_U1   0.105         /* band covers crown u in [0, BAND_U1] */
#define KNOT_U    0.052

typedef struct {
    double t;
    V3 C[NSPINE + 1], T[NSPINE + 1], N[NSPINE + 1], B[NSPINE + 1];
    double knot_th;
} Hat;

/* Integrate the crown's spine from a bending angle: straight at the base,
   curling over to the right and down at the tip, swaying with time. */
static void build_spine(Hat *h, double t) {
    h->t = t;
    double curl = 1.0 + 0.10 * sin(1.1 * t);
    double sway = 0.35 * sin(0.8 * t + 0.6);
    double ds = CROWN_LEN / NSPINE;
    V3 p = v3(0, 0, 0);
    for (int i = 0; i <= NSPINE; i++) {
        double u = (double)i / NSPINE;
        double k = pow(smooth(0.42, 1.0, u), 2.0);
        double phi = 0.05 + 2.75 * curl * k;      /* angle from vertical */
        double psi = sway * k;                    /* out of the bend plane */
        V3 d = v3(sin(phi) * cos(psi), cos(phi) * cos(psi), sin(psi));
        h->C[i] = p;
        h->T[i] = d;
        p = vadd(p, vmul(d, ds));
    }
    /* parallel transport: a twist-free frame along the curve */
    V3 n = vnorm(vsub(v3(1, 0, 0), vmul(h->T[0], h->T[0].x)));
    for (int i = 0; i <= NSPINE; i++) {
        n = vnorm(vsub(n, vmul(h->T[i], vdot(n, h->T[i]))));
        h->N[i] = n;
        h->B[i] = vcross(h->T[i], n);
    }
    h->knot_th = -M_PI / 2 + 0.3;   /* +z faces the camera at yaw 0 */
}

static void spine_at(const Hat *h, double u, V3 *C, V3 *T, V3 *N, V3 *B) {
    double f = clampd(u, 0, 1) * NSPINE;
    int i = (int)f;
    if (i >= NSPINE) i = NSPINE - 1;
    double a = f - i;
    *C = vlerp(h->C[i], h->C[i + 1], a);
    if (T) *T = vlerp(h->T[i], h->T[i + 1], a);
    *N = vlerp(h->N[i], h->N[i + 1], a);
    *B = vlerp(h->B[i], h->B[i + 1], a);
}

static double crown_r0(double u) {   /* smooth radius profile: sack then taper */
    return 0.80 * pow(1.0 - u, 0.80) * (1.0 + 0.22 * sin(M_PI * fmin(u * 1.5, 1.0)));
}

static V3 tube_point(const Hat *h, double u, double th, double r) {
    V3 C, N, B;
    spine_at(h, u, &C, NULL, &N, &B);
    return vadd(C, vadd(vmul(N, r * cos(th)), vmul(B, r * sin(th))));
}

static V3 surf_crown(const Hat *h, double u, double v) {
    double th = v * 2 * M_PI, w = sin(M_PI * u);
    double cr = 0.070 * sin(5 * th + 7 * u + 1.3 + 0.4 * sin(0.9 * h->t)) * w
              + 0.040 * sin(9 * th - 13 * u) * sin(M_PI * fmin(u * 1.5, 1.0))
              + 0.025 * sin(3 * th + 25 * u) * w;
    return tube_point(h, u, th, crown_r0(u) * (1 + cr));
}

static V3 surf_band(const Hat *h, double u, double v) {
    double th = v * 2 * M_PI;
    double dk = angdiff(th, h->knot_th);
    double gather = exp(-dk * dk / 0.35);              /* 1 at the knot */
    double uu = BAND_U1 * (0.5 + (u - 0.5) * (1 - 0.45 * gather));
    double pleat = 0.022 * sin(u * 2 * M_PI * 4.5 + 0.8 * sin(3 * th)) * (1 + 1.2 * gather);
    return tube_point(h, uu, th, crown_r0(uu) + 0.075 + 0.05 * gather + pleat);
}

static void knot_frame(const Hat *h, V3 *c, V3 *e1, V3 *e2, V3 *e3) {
    V3 C, T, N, B;
    spine_at(h, KNOT_U, &C, &T, &N, &B);
    *e1 = vnorm(vadd(vmul(N, cos(h->knot_th)), vmul(B, sin(h->knot_th)))); /* out */
    *e2 = vnorm(T);                                                       /* up  */
    *e3 = vcross(*e2, *e1);                                               /* side */
    *c = vadd(C, vmul(*e1, crown_r0(KNOT_U) + 0.075 + 0.05 + 0.05));
}

static V3 surf_knot(const Hat *h, double u, double v) {
    V3 c, e1, e2, e3;
    knot_frame(h, &c, &e1, &e2, &e3);
    double a = (u - 0.5) * M_PI, b = v * 2 * M_PI;
    double lump = 1 + 0.18 * sin(3 * b + 2 * a) + 0.10 * sin(5 * a + 1);
    return vadd(c, vadd(vmul(e1, 0.11 * cos(a) * cos(b) * lump),
                   vadd(vmul(e2, 0.17 * sin(a) * lump),
                        vmul(e3, 0.15 * cos(a) * sin(b) * lump))));
}

static double brim_rout(double th) {
    return 2.05 * (1 + 0.06 * cos(2 * th + 0.4) + 0.025 * sin(5 * th + 1));
}

static double brim_y(const Hat *h, double r, double th) {
    double rho = clampd((r - BRIM_RIN) / (brim_rout(th) - BRIM_RIN), 0, 1);
    double flutter = 0.06 * sin(1.7 * h->t + 2 * th);
    return rho * rho * (0.40 * cos(th - 3.4) + 0.12 * cos(2 * th + 0.7) - 0.06 + flutter)
         + 0.03 * sin(7 * th + h->t) * rho * rho * rho;
}

static V3 surf_brim(const Hat *h, double u, double v) {
    double th = v * 2 * M_PI;
    double r = BRIM_RIN + u * (brim_rout(th) - BRIM_RIN);
    return v3(r * cos(th), brim_y(h, r, th), r * sin(th));
}

static V3 surf_piping(const Hat *h, double u, double v) {
    double th = v * 2 * M_PI, a = u * 2 * M_PI, R = brim_rout(th);
    V3 e = v3(R * cos(th), brim_y(h, R, th), R * sin(th));
    return vadd(e, vadd(vmul(v3(cos(th), 0, sin(th)), 0.035 * cos(a)), v3(0, 0.035 * sin(a), 0)));
}

/* The knot's tail: a ribbon that leaves the knot and settles onto the brim. */
static V3 surf_flap(const Hat *h, double u, double v) {
    V3 c, e1, e2, e3;
    knot_frame(h, &c, &e1, &e2, &e3);
    double w = (v - 0.5) * 2;
    V3 p = vadd(c, vmul(e1, 0.04 + 0.42 * u));
    p = vadd(p, vmul(e3, w * (0.09 + 0.07 * u) + 0.06 * sin(u * 3)));
    double r = sqrt(p.x * p.x + p.z * p.z), th = atan2(p.z, p.x);
    double s = smooth(0, 0.75, u);
    p.y = c.y * (1 - s) + (brim_y(h, r, th) + 0.05) * s
        + 0.025 * sin(w * 3.5 + u * 6) + 0.05 * sin(M_PI * u) * (1 - s);
    return p;
}

/* ------------------------------------------------------------- materials */

enum { M_FELT, M_VELVET, M_BRIM, M_LINING, M_PIPING, M_COUNT };
typedef struct { double base[3], kd, ks, shin, sheen, tex; } Material;
static const Material MATS[M_COUNT] = {
    [M_FELT]   = {{52, 72, 170}, 0.85, 0.06, 10, 0.22, 0.10},
    [M_VELVET] = {{62, 84, 195}, 0.80, 0.22, 24, 0.55, 0.05},
    [M_BRIM]   = {{50, 70, 165}, 0.85, 0.06, 10, 0.22, 0.10},
    [M_LINING] = {{36, 46, 125}, 0.55, 0.85, 48, 0.08, 0.02},
    [M_PIPING] = {{46, 60, 155}, 0.65, 0.90, 60, 0.10, 0.00},
};

/* ------------------------------------------------------------- sampling */

typedef struct { V3 p, n; float tex; unsigned char mat; } Sample;
typedef V3 (*SurfFn)(const Hat *, double, double);
enum { WRAP_U = 1, WRAP_V = 2, ORIENT_UP = 4 };

static Sample *g_s;  static size_t g_ns, g_scap;
static V3 *g_grid;   static size_t g_gcap;

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { fputs("out of memory\n", stderr); exit(1); }
    return q;
}

/* Evaluate a surface on an nu x nv grid, take normals from grid neighbours,
   and append camera-space samples. */
static void add_surface(const Hat *h, SurfFn f, int nu, int nv, int flags,
                        int mat, unsigned seed, M3 M, V3 off) {
    int cu = (flags & WRAP_U) ? nu : nu + 1;
    int cv = (flags & WRAP_V) ? nv : nv + 1;
    size_t need = (size_t)cu * cv;
    if (need > g_gcap) { g_gcap = need; g_grid = xrealloc(g_grid, need * sizeof *g_grid); }
    if (g_ns + need > g_scap) {
        g_scap = (g_ns + need) * 2;
        g_s = xrealloc(g_s, g_scap * sizeof *g_s);
    }
    for (int i = 0; i < cu; i++)
        for (int j = 0; j < cv; j++)
            g_grid[(size_t)i * cv + j] = f(h, (double)i / nu, (double)j / nv);

    for (int i = 0; i < cu; i++) {
        int ip = (flags & WRAP_U) ? (i + 1) % cu : (i + 1 < cu ? i + 1 : i);
        int im = (flags & WRAP_U) ? (i - 1 + cu) % cu : (i > 0 ? i - 1 : i);
        for (int j = 0; j < cv; j++) {
            int jp = (flags & WRAP_V) ? (j + 1) % cv : (j + 1 < cv ? j + 1 : j);
            int jm = (flags & WRAP_V) ? (j - 1 + cv) % cv : (j > 0 ? j - 1 : j);
            V3 du = vsub(g_grid[(size_t)ip * cv + j], g_grid[(size_t)im * cv + j]);
            V3 dv = vsub(g_grid[(size_t)i * cv + jp], g_grid[(size_t)i * cv + jm]);
            V3 n = vcross(du, dv);
            double l = vlen(n);
            if (l < 1e-14) continue;               /* degenerate (the tip) */
            n = vmul(n, 1 / l);
            if ((flags & ORIENT_UP) && n.y < 0) n = vmul(n, -1);
            Sample *s = &g_s[g_ns++];
            s->p = vadd(mxv(M, g_grid[(size_t)i * cv + j]), off);
            s->n = mxv(M, n);
            s->tex = (float)(hash3(i, j, seed) * 2 - 1);
            s->mat = (unsigned char)mat;
        }
    }
}

/* ----------------------------------------------------------- shadow map */

#define SM     320
#define SM_EXT 4.5
static float g_sm[SM * SM];
static V3 g_lx, g_ly;

static int sm_coord(V3 p, V3 lk, int *x, int *y, double *d) {
    *x = (int)((vdot(p, g_lx) / SM_EXT * 0.5 + 0.5) * SM);
    *y = (int)((vdot(p, g_ly) / SM_EXT * 0.5 + 0.5) * SM);
    *d = vdot(p, lk);
    return *x >= 0 && *y >= 0 && *x < SM && *y < SM;
}

static void build_shadow_map(V3 lk) {
    g_lx = vnorm(vcross(lk, v3(0, 1, 0)));
    g_ly = vcross(g_lx, lk);
    for (int i = 0; i < SM * SM; i++) g_sm[i] = -1e30f;
    for (size_t k = 0; k < g_ns; k++) {
        int x, y; double d;
        if (!sm_coord(g_s[k].p, lk, &x, &y, &d)) continue;
        for (int dy = 0; dy < 2; dy++)          /* 2x2 splat closes gaps */
            for (int dx = 0; dx < 2; dx++) {
                int xx = x + dx, yy = y + dy;
                if (xx < SM && yy < SM && g_sm[yy * SM + xx] < d) g_sm[yy * SM + xx] = (float)d;
            }
    }
}

/* 1 = fully lit, lower = in shadow; 3x3 PCF for soft edges. */
static double shadow_at(V3 p, V3 lk) {
    int x, y; double d;
    if (!sm_coord(p, lk, &x, &y, &d)) return 1;
    int occ = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= SM || yy >= SM) continue;
            if (g_sm[yy * SM + xx] > d + 0.08) occ++;
        }
    return 1 - 0.85 * occ / 9.0;
}

/* ------------------------------------------------------------- renderer */

static const char RAMP[] = ".,-~:;=!*#$@";       /* dim -> bright */
#define NRAMP ((int)sizeof RAMP - 1)
#define ASP      0.5            /* char cell width / height */
#define CAM_D    8.0
#define CENTER_Y 1.15
#define FLOOR_Y  (-1.25)
#define FLOOR_R  2.7
#define NSPARK   90

typedef struct {
    int W, H;                   /* render area (HUD row excluded) */
    char *chr;
    unsigned char *rgb;
    float *zb;
    /* animation / view state */
    double t, yaw, spin, pitch, zoom;
    int colour, sparkles, floor, wobble, paused, hud;
    double fps;
} State;

static void put(State *S, int x, int y, float ooz, char c, double r, double g, double b) {
    if (x < 0 || y < 0 || x >= S->W || y >= S->H) return;
    int i = y * S->W + x;
    if (ooz <= S->zb[i]) return;
    S->zb[i] = ooz;
    S->chr[i] = c;
    S->rgb[i * 3 + 0] = (unsigned char)clampd(r, 0, 255);
    S->rgb[i * 3 + 1] = (unsigned char)clampd(g, 0, 255);
    S->rgb[i * 3 + 2] = (unsigned char)clampd(b, 0, 255);
}

static void render(State *S) {
    static Hat h;
    int W = S->W, H = S->H;
    memset(S->chr, ' ', (size_t)W * H);
    memset(S->rgb, 0, (size_t)W * H * 3);
    for (int i = 0; i < W * H; i++) S->zb[i] = 0;

    double t = S->t;
    build_spine(&h, t);

    /* object: spin about its own axis, which precesses (wobble) and bobs */
    double wx = S->wobble ? 0.12 * sin(0.7 * t + 1) : 0;
    double wz = S->wobble ? 0.18 * sin(0.9 * t) : 0;
    double bob = S->wobble ? 0.08 * sin(1.3 * t) : 0;
    M3 Mobj = mxm(mxm(rot_z(wz), rot_x(wx)), rot_y(S->yaw));
    M3 Rc = rot_x(S->pitch);
    M3 M = mxm(Rc, Mobj);
    V3 off = mxv(Rc, v3(0, bob - CENTER_Y, 0));

    /* projection: fit ~5.6 units across and ~4.6 units tall */
    double K = fmin(W * 0.92 * CAM_D / 5.4, H * 0.92 * CAM_D / (5.0 * ASP)) * S->zoom;
    double cx = W / 2.0, cy = H / 2.0 + 0.5;
    double q = fmax(26, K / CAM_D * 2.4);     /* grid samples per world unit */

    /* lights, in camera space: key (casts shadows), fill, back rim */
    V3 Lk = vnorm(v3(-0.55, 0.75, 0.45));
    V3 Lf = vnorm(v3(0.80, 0.10, 0.50));
    V3 Lr = vnorm(v3(0.30, 0.40, -0.85));

    /* 1. geometry */
    g_ns = 0;
    int nth = (int)(2 * M_PI * 0.95 * q);
    add_surface(&h, surf_crown, (int)(CROWN_LEN * q), nth, WRAP_V, M_FELT, 1, M, off);
    add_surface(&h, surf_band, (int)(0.5 * q) + 10, nth, WRAP_V, M_VELVET, 2, M, off);
    add_surface(&h, surf_knot, (int)fmax(24, 0.9 * q), (int)fmax(32, 1.2 * q), WRAP_V, M_VELVET, 3, M, off);
    add_surface(&h, surf_flap, (int)fmax(16, 0.7 * q), (int)fmax(10, 0.4 * q), 0, M_VELVET, 4, M, off);
    int nbr = (int)(2 * M_PI * 2.15 * q);
    add_surface(&h, surf_brim, (int)(1.4 * q), nbr, WRAP_V | ORIENT_UP, M_BRIM, 5, M, off);
    add_surface(&h, surf_piping, 10, nbr, WRAP_U | WRAP_V, M_PIPING, 6, M, off);

    /* 2. shadow map from the key light */
    build_shadow_map(Lk);

    /* 3. shade, project, z-buffer */
    for (size_t k = 0; k < g_ns; k++) {
        const Sample *s = &g_s[k];
        double zc = CAM_D - s->p.z;
        if (zc < 0.2) continue;
        double ooz = 1 / zc;
        int sx = (int)(cx + K * s->p.x * ooz);
        int sy = (int)(cy - K * ASP * s->p.y * ooz);
        if (sx < 0 || sy < 0 || sx >= W || sy >= H) continue;
        if (ooz <= S->zb[sy * W + sx]) continue;

        V3 V = vnorm(v3(-s->p.x, -s->p.y, zc));
        V3 n = s->n;
        int mat = s->mat;
        if (vdot(n, V) < 0) {                   /* two-sided surfaces */
            n = vmul(n, -1);
            if (mat == M_BRIM) mat = M_LINING;  /* seeing the underside */
        }
        const Material *m = &MATS[mat];
        double sh = shadow_at(s->p, Lk);
        double diff = fmax(0, vdot(n, Lk)) * sh;
        double fill = fmax(0, vdot(n, Lf));
        double rim = fmax(0, vdot(n, Lr));
        double spec = pow(fmax(0, vdot(n, vnorm(vadd(Lk, V)))), m->shin) * m->ks * sh;
        double sheen = pow(1 - fmax(0, vdot(n, V)), 3) * m->sheen;
        double tex = s->tex * m->tex;

        double lum = 0.16 + m->kd * (0.85 * diff + 0.40 * fill) + 0.30 * rim + spec + sheen + tex;
        int ci = (int)(clampd(lum, 0, 0.999) * NRAMP);

        double lit = 0.45 + 1.10 * (m->kd * diff + 0.45 * fill) + tex;
        double r = m->base[0] * lit + 255 * spec + 110 * sheen + 60 * rim;
        double g = m->base[1] * lit + 255 * spec + 130 * sheen + 70 * rim;
        double b = m->base[2] * lit + 255 * spec + 230 * sheen + 120 * rim;
        put(S, sx, sy, (float)ooz, RAMP[ci], r, g, b);
    }

    /* 4. ray-cast the floor (world plane y = FLOOR_Y) */
    if (S->floor) {
        M3 RcT = mtrans(Rc);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                /* camera-space point at depth zc:  zc * a + (0,0,CAM_D) */
                V3 a = v3((x + 0.5 - cx) / K, -(y + 0.5 - cy) / (K * ASP), -1);
                double ay = mxv(RcT, a).y, by = mxv(RcT, v3(0, 0, CAM_D)).y;
                if (fabs(ay) < 1e-9) continue;
                double zc = (FLOOR_Y - CENTER_Y - by) / ay;
                if (zc < 0.5) continue;
                V3 pc = vadd(vmul(a, zc), v3(0, 0, CAM_D));
                V3 pw = vadd(mxv(RcT, pc), v3(0, CENTER_Y, 0));
                double rr = sqrt(pw.x * pw.x + pw.z * pw.z);
                if (rr > FLOOR_R) continue;
                double fade = 1 - smooth(FLOOR_R * 0.6, FLOOR_R, rr);
                int tile = ((int)floor(pw.x / 0.45) + (int)floor(pw.z / 0.45)) & 1;
                double sh = shadow_at(pc, Lk);
                double lum = (tile ? 0.30 : 0.18) * (0.15 + 0.85 * sh) * (0.35 + 0.65 * fade);
                static const char FR[] = " .,:";
                int fi = (int)clampd(lum / 0.30 * 3.99, 0, 3);
                if (FR[fi] == ' ') continue;
                double k = 0.25 + 0.75 * lum / 0.30;
                put(S, x, y, (float)(1 / zc), FR[fi], 150 * k, 138 * k, 120 * k);
            }
    }

    /* 5. sparkles: helical orbits rising around the hat, depth-tested */
    if (S->sparkles) {
        for (int i = 0; i < NSPARK; i++) {
            double r = 1.3 + 1.6 * hash3(i, 1, 7);
            double speed = (0.4 + 0.8 * hash3(i, 2, 7)) * (i & 1 ? 1 : -1);
            double ph = 2 * M_PI * hash3(i, 3, 7);
            double rise = 0.15 + 0.30 * hash3(i, 4, 7);
            double yy = fmod(hash3(i, 5, 7) * 4.2 + rise * t, 4.2) - 1.0;
            double ang = ph + speed * t;
            double life = sin(M_PI * (yy + 1.0) / 4.2);
            double tw = 0.5 + 0.5 * sin(t * (3 + 4 * hash3(i, 6, 7)) + ph * 5);
            double br = tw * life;
            if (br < 0.15) continue;
            V3 pc = mxv(Rc, v3(r * cos(ang), yy - CENTER_Y, r * sin(ang)));
            double zc = CAM_D - pc.z;
            if (zc < 0.5) continue;
            float ooz = (float)(1 / zc);
            int sx = (int)(cx + K * pc.x / zc), sy = (int)(cy - K * ASP * pc.y / zc);
            double R = 255, G = 200 + 55 * br, B = 90 + 165 * br * br;
            char c = br > 0.6 ? '*' : br > 0.35 ? '+' : '.';
            put(S, sx, sy, ooz, c, R, G, B);
            if (br > 0.94) {                     /* the brightest flare */
                put(S, sx - 1, sy, ooz, '-', R * .6, G * .55, B * .4);
                put(S, sx + 1, sy, ooz, '-', R * .6, G * .55, B * .4);
            }
        }
    }
}

/* ------------------------------------------------------------- terminal */

static struct termios g_old_tio;
static int g_raw, g_alt;
static volatile sig_atomic_t g_quit;

static void on_signal(int sig) { (void)sig; g_quit = 1; }

static void term_restore(void) {
    if (g_raw) { tcsetattr(STDIN_FILENO, TCSANOW, &g_old_tio); g_raw = 0; }
    if (g_alt) { fputs("\033[0m\033[?25h\033[?1049l", stdout); fflush(stdout); g_alt = 0; }
}

static void term_setup(void) {
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_old_tio) == 0) {
        struct termios raw = g_old_tio;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        g_raw = 1;
    }
    fputs("\033[?1049h\033[?25l\033[2J", stdout);
    fflush(stdout);
    g_alt = 1;
    atexit(term_restore);
}

static void reset_view(State *S) {
    S->spin = 0.8; S->pitch = 0.38; S->zoom = 1.0;
}

static void handle_keys(State *S) {
    unsigned char kb[64];
    ssize_t n = g_raw ? read(STDIN_FILENO, kb, sizeof kb) : 0;
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = kb[i];
        if (c == 27 && i + 2 < n && kb[i + 1] == '[') {
            switch (kb[i + 2]) {
            case 'A': S->pitch = clampd(S->pitch + 0.05, -0.3, 1.3); break;
            case 'B': S->pitch = clampd(S->pitch - 0.05, -0.3, 1.3); break;
            case 'C': S->spin += 0.2; break;
            case 'D': S->spin -= 0.2; break;
            }
            i += 2;
            continue;
        }
        switch (c) {
        case 27: case 'q': case 'Q': g_quit = 1; break;
        case '+': case '=': S->zoom = fmin(S->zoom * 1.1, 4.0); break;
        case '-': case '_': S->zoom = fmax(S->zoom / 1.1, 0.25); break;
        case ' ': S->paused ^= 1; break;
        case 'c': S->colour ^= 1; break;
        case 's': S->sparkles ^= 1; break;
        case 'f': S->floor ^= 1; break;
        case 'w': S->wobble ^= 1; break;
        case 'r': reset_view(S); break;
        }
    }
}

static char *g_out;
static size_t g_outcap;

static void present(State *S, int interactive) {
    int W = S->W, H = S->H;
    size_t need = (size_t)W * (H + 1) * 24 + 512;
    if (need > g_outcap) { g_outcap = need; g_out = xrealloc(g_out, need); }
    char *p = g_out;
    if (interactive) p += sprintf(p, "\033[H");
    long last = -1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            char c = S->chr[i];
            if (S->colour && c != ' ') {
                long col = (long)S->rgb[i * 3] << 16 | S->rgb[i * 3 + 1] << 8 | S->rgb[i * 3 + 2];
                if (col != last) {
                    p += sprintf(p, "\033[38;2;%d;%d;%dm", S->rgb[i * 3], S->rgb[i * 3 + 1], S->rgb[i * 3 + 2]);
                    last = col;
                }
            }
            *p++ = c;
        }
        if (y < H - 1 || !interactive) { *p++ = '\r'; *p++ = '\n'; }
    }
    if (S->colour) p += sprintf(p, "\033[0m");
    if (S->hud) {
        char hud[512];
        int deg = (int)fmod(fmod(S->yaw * 180 / M_PI, 360) + 360, 360);
        int len = snprintf(hud, sizeof hud,
            " wizard hat 3D | yaw %3d  pitch %+3d  spin %+.1f  zoom %.2f  %4.0f fps%s | "
            "<- -> spin  up/dn pitch  +/- zoom  space pause  c s f w r  q quit",
            deg, (int)(S->pitch * 180 / M_PI), S->spin, S->zoom, S->fps,
            S->paused ? "  PAUSED" : "");
        if (len > W) len = W;
        p += sprintf(p, "\r\n%s%.*s%*s%s", S->colour ? "\033[38;2;140;140;160m" : "",
                     len, hud, W - len, "", S->colour ? "\033[0m" : "");
    }
    size_t total = (size_t)(p - g_out), off = 0;
    while (off < total) {
        ssize_t w = write(STDOUT_FILENO, g_out + off, total - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void usage(const char *argv0) {
    printf("usage: %s [options]\n"
           "  --size WxH     render size (default: terminal size, or 100x45)\n"
           "  --frames N     render N frames, then exit\n"
           "  --at T         start at animation time T seconds\n"
           "  --yaw DEG      start rotation\n"
           "  --spin R       spin speed in rad/s (default 0.8)\n"
           "  --fps N        target frame rate (default 30)\n"
           "  --mono         no colour\n"
           "  --no-floor  --no-sparkles  --no-wobble\n"
           "keys: <- -> spin, up/down pitch, +/- zoom, space pause, c colour,\n"
           "      s sparkles, f floor, w wobble, r reset, q quit\n", argv0);
}

int main(int argc, char **argv) {
    State S = {0};
    reset_view(&S);
    S.colour = S.sparkles = S.floor = S.wobble = 1;
    int fixW = 0, fixH = 0, frames = -1;
    double fps = 30;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "--mono")) S.colour = 0;
        else if (!strcmp(a, "--no-floor")) S.floor = 0;
        else if (!strcmp(a, "--no-sparkles")) S.sparkles = 0;
        else if (!strcmp(a, "--no-wobble")) S.wobble = 0;
        else if (v && !strcmp(a, "--size")) { sscanf(v, "%dx%d", &fixW, &fixH); i++; }
        else if (v && !strcmp(a, "--frames")) { frames = atoi(v); i++; }
        else if (v && !strcmp(a, "--at")) { S.t = atof(v); i++; }
        else if (v && !strcmp(a, "--yaw")) { S.yaw = atof(v) * M_PI / 180; i++; }
        else if (v && !strcmp(a, "--spin")) { S.spin = atof(v); i++; }
        else if (v && !strcmp(a, "--fps")) { fps = fmax(1, atof(v)); i++; }
        else { fprintf(stderr, "unknown option: %s\n", a); usage(argv[0]); return 1; }
    }

    int interactive = isatty(STDOUT_FILENO);
    S.hud = interactive;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (interactive) term_setup();

    double prev = now_s(), fps_acc = 0;
    int fps_n = 0;
    for (int frame = 0; !g_quit && frame != frames; frame++) {
        int W = fixW, H = fixH;
        if (W <= 0 || H <= 0) {
            struct winsize ws;
            if (interactive && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
                W = ws.ws_col; H = ws.ws_row;
            } else { W = 100; H = 45; }
        }
        if (S.hud) H -= 1;
        if (W < 20 || H < 10) { W = W < 20 ? 20 : W; H = H < 10 ? 10 : H; }
        if (W != S.W || H != S.H) {
            S.W = W; S.H = H;
            S.chr = xrealloc(S.chr, (size_t)W * H);
            S.rgb = xrealloc(S.rgb, (size_t)W * H * 3);
            S.zb = xrealloc(S.zb, (size_t)W * H * sizeof(float));
            if (interactive) fputs("\033[2J", stdout);
        }

        handle_keys(&S);
        double tnow = now_s();
        double dt = interactive ? fmin(tnow - prev, 0.1) : 1.0 / fps;
        prev = tnow;
        if (frame > 0 && !S.paused) { S.t += dt; S.yaw += S.spin * dt; }
        if (interactive && dt > 0) {
            fps_acc += dt; fps_n++;
            if (fps_acc > 0.5) { S.fps = fps_n / fps_acc; fps_acc = 0; fps_n = 0; }
        }

        render(&S);
        present(&S, interactive);

        if (interactive) {
            double spare = 1.0 / fps - (now_s() - tnow);
            if (spare > 0) {
                struct timespec ts = {0, (long)(spare * 1e9)};
                nanosleep(&ts, NULL);
            }
        }
    }
    if (interactive) term_restore();
    free(S.chr); free(S.rgb); free(S.zb); free(g_s); free(g_grid); free(g_out);
    return 0;
}
