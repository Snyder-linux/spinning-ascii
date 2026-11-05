/*
 * brain3d.c - a large, anatomically referenced, rotating ASCII human brain.
 *
 * The brain is sculpted in millimetres in Talairach-style coordinates
 * (x = right, y = anterior, z = superior, origin near the anterior commissure)
 * from published reference anatomy (see README.md for the sources):
 *
 *   size       ~140 x 170 x 120 mm hemispheres (Talairach atlas brain is
 *              136 x 172 x 118 mm; Chudler lists 140 x 167 x 93 mm)
 *   lobes      frontal, parietal, temporal, occipital + the insula hidden
 *              inside the Sylvian fissure
 *   sulci      longitudinal fissure, central sulcus (with its "hand knob"),
 *              pre/postcentral, superior/inferior frontal, Sylvian fissure
 *              with its rami, superior/inferior temporal, intraparietal,
 *              transverse/lateral occipital, parieto-occipital, calcarine,
 *              cingulate, collateral, olfactory and orbital sulci; plus
 *              procedural secondary folds so gyri are ~1 cm wide meanders
 *   asymmetry  Yakovlevian torque (right frontal / left occipital petalia)
 *              and a longer, flatter left Sylvian fissure (Geschwind 1968)
 *   hindbrain  cerebellum with fan-shaped folia and a great horizontal
 *              fissure, vermis, midbrain, pons, medulla with olives
 *   interior   2-4 mm cortical ribbon over white matter, corpus callosum,
 *              thalami, basal ganglia, lateral/3rd/4th ventricles - shown by
 *              the cut-away modes (key x)
 *
 * Pipeline:
 *   1. at start-up, evaluate the analytic shape into a signed-distance volume
 *      (0.8 mm voxels, ~10 M samples, built on all cores in well under 1 s),
 *      together with a tissue/lobe label and the depth below the unfolded hull
 *   2. every frame, cast supersampled rays per character cell (all cores),
 *      sphere-trace the volume, and shade with wrap-diffuse key light, soft
 *      shadows, sulcal ambient occlusion, wet specular and a rim light
 *   3. pick a glyph from the luminance and emit 24-bit ANSI colour
 *
 * Build:  cc -O2 -pthread -o brain3d brain3d.c -lm    (or `make` from the repo root)
 * Run:    ./brain3d          (interactive, fills the terminal)
 *         ./brain3d --help
 *
 * Keys:  left/right spin   up/down tilt   + - zoom   space pause
 *        m material (fresh / formalin / lobes / MRI)   x cut-away
 *        h shadows   a activity   c colour   r reset   q quit
 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ math */

typedef struct { double x, y, z; } V3;

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

static inline double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline double mixd(double a, double b, double t) { return a + (b - a) * t; }
static inline double smooth(double e0, double e1, double x) {
    double t = clampd((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}
static inline double smin(double a, double b, double k) {
    double h = fmax(k - fabs(a - b), 0) / k;
    return fmin(a, b) - h * h * k * 0.25;
}
static inline double smax(double a, double b, double k) { return -smin(-a, -b, k); }
static inline double sq(double x) { return x * x; }

static double sd_ellipsoid(V3 p, V3 c, V3 r) {           /* Quilez's bound */
    V3 q = v3((p.x - c.x) / r.x, (p.y - c.y) / r.y, (p.z - c.z) / r.z);
    double k0 = vlen(q);
    double k1 = vlen(v3(q.x / r.x, q.y / r.y, q.z / r.z));
    if (k1 < 1e-12) return -fmin(r.x, fmin(r.y, r.z));
    return k0 * (k0 - 1) / k1;
}
/* superellipsoid |x/a|^n + |y/b|^n + |z/c|^n = 1, distance by first-order
   normalisation (accurate near the surface, which is all we need) */
static double sd_superellipsoid(V3 p, V3 c, V3 r, double n) {
    double qx = fabs(p.x - c.x) / r.x, qy = fabs(p.y - c.y) / r.y, qz = fabs(p.z - c.z) / r.z;
    double sx = pow(qx, n), sy = pow(qy, n), sz = pow(qz, n);
    double sum = sx + sy + sz;
    if (sum < 1e-12) return -fmin(r.x, fmin(r.y, r.z));
    double k = pow(sum, 1 / n);
    double kn = pow(k, 1 - n);
    double gx = qx > 0 ? kn * sx / qx / r.x : 0;
    double gy = qy > 0 ? kn * sy / qy / r.y : 0;
    double gz = qz > 0 ? kn * sz / qz / r.z : 0;
    double g = sqrt(gx * gx + gy * gy + gz * gz);
    return (k - 1) / fmax(g, 1e-9) * (k > 1.6 ? 0.8 : 1);
}

static double sd_capsule(V3 p, V3 a, V3 b, double ra, double rb) {
    V3 pa = vsub(p, a), ba = vsub(b, a);
    double h = clampd(vdot(pa, ba) / vdot(ba, ba), 0, 1);
    return vlen(vsub(pa, vmul(ba, h))) - (ra + (rb - ra) * h);
}

/* Ken Perlin's improved noise, roughly in [-1, 1]. */
static unsigned char PERM[512];
static void noise_init(unsigned seed) {
    unsigned char p[256];
    for (int i = 0; i < 256; i++) p[i] = (unsigned char)i;
    unsigned s = seed;
    for (int i = 255; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = (int)((s >> 8) % (unsigned)(i + 1));
        unsigned char t = p[i]; p[i] = p[j]; p[j] = t;
    }
    for (int i = 0; i < 512; i++) PERM[i] = p[i & 255];
}
static inline double pfade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static inline double pgrad(int h, double x, double y, double z) {
    h &= 15;
    double u = h < 8 ? x : y, v = h < 4 ? y : (h == 12 || h == 14) ? x : z;
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}
static double noise3(double x, double y, double z) {
    double fx = floor(x), fy = floor(y), fz = floor(z);
    int X = (int)fx & 255, Y = (int)fy & 255, Z = (int)fz & 255;
    x -= fx; y -= fy; z -= fz;
    double u = pfade(x), v = pfade(y), w = pfade(z);
    int A = PERM[X] + Y, AA = PERM[A] + Z, AB = PERM[A + 1] + Z;
    int B = PERM[X + 1] + Y, BA = PERM[B] + Z, BB = PERM[B + 1] + Z;
    return mixd(mixd(mixd(pgrad(PERM[AA], x, y, z), pgrad(PERM[BA], x - 1, y, z), u),
                     mixd(pgrad(PERM[AB], x, y - 1, z), pgrad(PERM[BB], x - 1, y - 1, z), u), v),
                mixd(mixd(pgrad(PERM[AA + 1], x, y, z - 1), pgrad(PERM[BA + 1], x - 1, y, z - 1), u),
                     mixd(pgrad(PERM[AB + 1], x, y - 1, z - 1), pgrad(PERM[BB + 1], x - 1, y - 1, z - 1), u), v),
                w);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fputs("out of memory\n", stderr); exit(1); }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { fputs("out of memory\n", stderr); exit(1); }
    return q;
}

/* ============================================================ ANATOMY ==== */
/*
 * All hemisphere work happens in "local" coordinates: lx = distance from the
 * midline towards that hemisphere's lateral side, so one description serves
 * both sides; the per-side table adds the real left/right differences.
 */

enum {
    T_NONE, T_FRONTAL, T_PARIETAL, T_TEMPORAL, T_OCCIPITAL, T_INSULA,
    T_CEREBELLUM, T_STEM, T_CALLOSUM, T_THALAMUS, T_NERVE, T_COUNT
};
static const char *TISSUE_NAME[T_COUNT] = {
    "", "frontal lobe", "parietal lobe", "temporal lobe", "occipital lobe", "insula",
    "cerebellum", "brainstem", "corpus callosum", "thalamus", "cranial nerve"
};

typedef struct {
    double syl_slope, syl_end;   /* Sylvian fissure: rise per mm backwards, posterior end (y) */
    double front_push, occ_push; /* petalia, mm */
    V3 noise_off;                /* each hemisphere folds a little differently */
} Side;

static const Side SIDES[2] = {
    /* right: steeper, shorter Sylvian; frontal lobe protrudes (Yakovlevian torque) */
    {0.55, -37, 3.0, 0.0, {0, 0, 0}},
    /* left: longer, flatter Sylvian (larger planum temporale); occipital protrudes */
    {0.45, -46, 0.0, 4.0, {37.1, 11.7, -53.3}},
};

static const V3 HEMI_C = {22, -16, 6};   /* centre for the radial sulcus map */

static inline double syl_z(int s, double y) { return -8 - (y - 5) * SIDES[s].syl_slope; }

/* the tentorium cerebelli: roof of the cerebellum, floor of the occipital lobes */
static inline double tent_z(double ax, double y) {
    return -4 - 0.42 * ax - 0.10 * fmax(0, -60 - y);
}

static V3 to_local(V3 p, int s) {
    const Side *S = &SIDES[s];
    double lx = s ? -p.x : p.x, y = p.y;
    y -= S->front_push * smooth(0, 60, y);
    y += S->occ_push * smooth(-40, -100, y);
    double wide = 1 + 0.035 * (s ? smooth(-40, -100, p.y) : smooth(0, 60, p.y));
    return v3(lx / wide, y, p.z);
}

/* The unfolded hull of one hemisphere (local coords). Also reports the dome
   and temporal-lobe parts so the lobe labeller can tell them apart. */
static double hemi_hull(V3 l, double *dome_out, double *temp_out) {
    double y = l.y;
    /* main dome: frontal + parietal + occipital; narrower at both poles */
    /* centred near the midline so each hemisphere is half of one dome and the
       superior margins stay high, as in a coronal section */
    double wsc = (1 - 0.13 * smooth(-25, 72, y)) * (1 - 0.14 * smooth(-50, -105, y));
    /* the crown slopes down towards the forehead instead of ending in a wall */
    double zq = l.z > 8 ? 8 + (l.z - 8) / (1 - 0.16 * smooth(5, 75, y)) : l.z;
    double dome = sd_superellipsoid(v3(l.x / wsc, y, zq), v3(1, -17, 8), v3(67, 86, 66), 2.35) * wsc;
    /* frontal pole: the frontal lobe bulges forward at mid height, over a low,
       nearly flat orbital surface (Gray's Anatomy fig. 728) */
    double fp = sd_ellipsoid(l, v3(22, 30, -2), v3(36, 44, 34));
    dome = smin(dome, fp, 10);
    /* floor: flat orbital surface rising to the frontal pole, then the tentorium */
    double zf = y > 20 ? -22 + (y - 20) * 0.15 : -22;
    double wt = smooth(-30, -45, y);
    zf = mixd(zf, tent_z(l.x, y) + 2.5, wt);
    dome = smax(dome, zf - l.z, 8);

    /* temporal lobe: a 15-degree tilted ellipsoid, pole forward and down */
    const double ca = cos(0.27), sa = sin(0.27);
    V3 r = v3(l.x - 46, y + 28, l.z + 15);
    double ry = r.y * ca - r.z * sa, rz = r.y * sa + r.z * ca;
    double temp = sd_ellipsoid(v3(r.x, ry, rz), v3(0, 0, 0), v3(21, 52, 22));
    temp = smax(temp, (tent_z(l.x, y) + 2.5) - l.z - 40 * (1 - smooth(-32, -42, y)), 5);

    double d = smin(dome, temp, 7);
    d = smax(d, 0.9 - l.x, 7);                    /* longitudinal fissure, rounded superior margin */
    if (dome_out) *dome_out = dome;
    if (temp_out) *temp_out = temp;
    return d;
}

/* ---- primary sulci: polylines on the surface, carved radially from HEMI_C */

#define MAXP 12
typedef struct { double w, depth; int n; double p[MAXP][3]; } SulcusDef;

/* Right-hemisphere local coordinates (lx, y, z) in mm, read off Talairach /
   MNI atlas plates. Only the direction from HEMI_C matters. */
static const SulcusDef SULCI[] = {
    /* central sulcus, with the posteriorly bulging hand knob at z ~ 55 */
    {1.2, 20, 12, {{4,-40,73},{16,-36,72},{28,-30,66},{34,-22,62},{38,-25,58},{42,-28,54},
                   {46,-23,50},{52,-16,42},{57,-11,33},{60,-7,24},{61,-5,19},{61,-4,16}}},
    /* precentral sulcus: superior and inferior segments */
    {1.0, 14, 4, {{12,-16,73},{22,-12,68},{30,-8,63},{37,-5,57}}},
    {1.0, 14, 5, {{43,0,47},{50,3,38},{55,7,29},{58,10,21},{58,13,16}}},
    /* postcentral sulcus */
    {1.0, 14, 6, {{12,-54,72},{24,-49,67},{36,-43,58},{46,-37,48},{54,-31,38},{59,-27,28}}},
    /* superior and inferior frontal sulci */
    {1.0, 13, 6, {{26,-10,66},{26,4,65},{27,20,60},{27,36,51},{25,50,38},{22,60,24}}},
    {1.0, 12, 6, {{45,0,45},{46,14,39},{45,28,32},{43,42,23},{39,54,14},{34,62,6}}},
    /* Sylvian fissure: anterior ascending and horizontal rami */
    {0.9, 12, 3, {{54,14,-9},{53,17,1},{51,19,10}}},
    {0.9, 12, 3, {{52,14,-11},{49,26,-12},{45,37,-13}}},
    /* superior temporal sulcus, ending in the angular gyrus */
    {1.1, 16, 7, {{50,20,-27},{58,6,-21},{63,-12,-13},{64,-30,-5},{61,-46,5},{56,-56,17},{49,-63,28}}},
    /* inferior temporal sulcus (often broken) */
    {0.9, 10, 4, {{51,8,-36},{58,-10,-31},{62,-28,-25},{59,-44,-18}}},
    {0.9, 10, 3, {{56,-50,-14},{51,-63,-9},{44,-75,-4}}},
    /* intraparietal sulcus, from the postcentral to the occipital lobe */
    {1.1, 18, 6, {{42,-40,56},{36,-50,57},{31,-60,52},{28,-70,43},{27,-79,33},{26,-86,24}}},
    /* transverse occipital and lateral occipital sulci */
    {0.9, 10, 3, {{14,-81,42},{25,-82,33},{36,-80,24}}},
    {0.9, 10, 3, {{48,-74,6},{40,-87,3},{29,-97,4}}},
    /* medial surface: parieto-occipital (notching the top), calcarine, cingulate */
    {1.2, 17, 6, {{2,-56,8},{2,-63,24},{2,-69,40},{3,-72,52},{7,-73,62},{12,-73,66}}},
    {1.1, 15, 5, {{2,-58,5},{2,-71,3},{2,-85,4},{4,-98,5},{10,-103,7}}},
    {1.0, 12, 9, {{2,28,2},{2,38,14},{2,32,30},{2,12,42},{2,-12,46},{2,-34,48},{2,-42,58},{4,-46,70},{9,-46,74}}},
    /* inferior surface: collateral, olfactory and the H-shaped orbital sulci */
    {1.0, 12, 4, {{28,-18,-34},{30,-40,-24},{30,-60,-15},{26,-76,-9}}},
    {1.0, 8, 3, {{9,6,-23},{10,24,-21},{12,46,-16}}},
    {0.9, 6, 3, {{19,18,-22},{21,32,-19},{23,46,-15}}},
    {0.9, 6, 3, {{31,18,-21},{33,32,-18},{33,44,-14}}},
    {0.9, 6, 2, {{19,28,-20},{33,28,-19}}},
};
#define NSULCI ((int)(sizeof SULCI / sizeof SULCI[0]))
#define NS_TOTAL (NSULCI + 1)            /* + the Sylvian posterior ramus */

typedef struct { int n; double w, depth; V3 d[MAXP]; double len[MAXP]; } Sulcus;
static Sulcus g_sulc[2][NS_TOTAL];
static Sulcus g_cs_ext[2];               /* central sulcus extended to the Sylvian, for labels */

#define MAPW 1024
#define MAPH 512
typedef struct { float s, depth, w; } MapT;
static MapT *g_map[2];

static double arc_dist(V3 d, V3 a, V3 b, double *tt) {
    V3 n = vcross(a, b);
    double ln = vlen(n), ab = atan2(ln, vdot(a, b));
    if (ln > 1e-9) {
        n = vmul(n, 1 / ln);
        double sd = vdot(d, n);
        V3 dp = vsub(d, vmul(n, sd));
        double lp = vlen(dp);
        if (lp > 1e-9) {
            dp = vmul(dp, 1 / lp);
            double ta = atan2(vdot(vcross(a, dp), n), vdot(a, dp));
            if (ta >= 0 && ta <= ab) { *tt = ta / ab; return fabs(asin(clampd(sd, -1, 1))); }
        }
    }
    double da = acos(clampd(vdot(d, a), -1, 1)), db = acos(clampd(vdot(d, b), -1, 1));
    if (da < db) { *tt = 0; return da; }
    *tt = 1; return db;
}

static void sulcus_from(Sulcus *S, const double (*pts)[3], int n, double w, double depth,
                        int side, unsigned seed) {
    S->n = n; S->w = w; S->depth = depth;
    double acc = 0;
    for (int i = 0; i < n; i++) {
        V3 p = v3(pts[i][0], pts[i][1], pts[i][2]);
        /* no two hemispheres are alike: jitter control points by a few mm */
        double j = side ? 2.5 : 1.0;
        p.y += j * noise3(seed * 0.37 + i * 0.61, 3.3 + side, 0.5);
        p.z += j * noise3(seed * 0.37 + i * 0.61, 7.7 + side, 1.5);
        S->d[i] = vnorm(vsub(p, HEMI_C));
        if (i) acc += acos(clampd(vdot(S->d[i], S->d[i - 1]), -1, 1));
        S->len[i] = acc;
    }
}

/* distance (radians) from direction d to the sulcus, and position along it */
static double sulcus_dist(const Sulcus *S, V3 d, double *along, int *seg) {
    double best = 1e9, ba = 0;
    int bs = 0;
    for (int i = 0; i + 1 < S->n; i++) {
        double tt, a = arc_dist(d, S->d[i], S->d[i + 1], &tt);
        if (a < best) { best = a; bs = i; ba = S->len[i] + tt * (S->len[i + 1] - S->len[i]); }
    }
    if (along) *along = S->len[S->n - 1] > 0 ? ba / S->len[S->n - 1] : 0;
    if (seg) *seg = bs;
    return best;
}

static void build_sulci(int s) {
    for (int k = 0; k < NSULCI; k++)
        sulcus_from(&g_sulc[s][k], (const double (*)[3])SULCI[k].p, SULCI[k].n,
                    SULCI[k].w, SULCI[k].depth, s, (unsigned)(k * 7 + 3));
    /* posterior ascending ramus of the Sylvian fissure: steeper on the right */
    double ye = SIDES[s].syl_end, ze = syl_z(s, ye);
    double ram[4][3];
    if (s == 0) {
        double r[4][3] = {{62, ye + 2, ze - 1}, {60, ye - 5, ze + 8}, {57, ye - 9, ze + 16}, {53, ye - 12, ze + 23}};
        memcpy(ram, r, sizeof r);
    } else {
        double r[4][3] = {{62, ye + 2, ze - 1}, {60, ye - 7, ze + 5}, {57, ye - 13, ze + 11}, {53, ye - 18, ze + 17}};
        memcpy(ram, r, sizeof r);
    }
    sulcus_from(&g_sulc[s][NSULCI], (const double (*)[3])ram, 4, 1.2, 16, s, 999);
    /* labelling copy of the central sulcus, run all the way down to the Sylvian */
    double cs[MAXP][3];
    memcpy(cs, SULCI[0].p, sizeof cs);
    cs[11][0] = 62; cs[11][1] = -3; cs[11][2] = syl_z(s, -3) - 2;
    sulcus_from(&g_cs_ext[s], (const double (*)[3])cs, 12, 0, 0, s, 3);
}

typedef struct { int side, j0, j1; } MapJob;
static void *map_worker(void *arg) {
    MapJob *J = arg;
    int s = J->side;
    for (int j = J->j0; j < J->j1; j++) {
        double lat = -M_PI / 2 + (j + 0.5) / MAPH * M_PI;
        for (int i = 0; i < MAPW; i++) {
            double lon = -M_PI + (i + 0.5) / MAPW * 2 * M_PI;
            V3 d = v3(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat));
            MapT best = {10, 0, 0};
            for (int k = 0; k < NS_TOTAL; k++) {
                const Sulcus *S = &g_sulc[s][k];
                double t, a = sulcus_dist(S, d, &t, NULL);
                if (a < best.s) {
                    /* sulci are shallower and narrower towards their ends */
                    double taper = smooth(0, 0.18, t) * smooth(1, 0.82, t);
                    best.s = (float)a;
                    best.depth = (float)(S->depth * (0.35 + 0.65 * taper));
                    best.w = (float)(S->w * (0.6 + 0.4 * taper));
                }
            }
            g_map[s][(size_t)j * MAPW + i] = best;
        }
    }
    return NULL;
}

static MapT map_lookup(int s, V3 d) {
    double lon = atan2(d.y, d.x), lat = asin(clampd(d.z, -1, 1));
    double fx = (lon + M_PI) / (2 * M_PI) * MAPW - 0.5;
    double fy = (lat + M_PI / 2) / M_PI * MAPH - 0.5;
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    double ax = fx - x0, ay = fy - y0;
    int x1 = (x0 + 1) % MAPW;
    x0 = (x0 % MAPW + MAPW) % MAPW;
    if (x1 < 0) x1 += MAPW;
    int y1 = y0 + 1;
    y0 = y0 < 0 ? 0 : y0 >= MAPH ? MAPH - 1 : y0;
    y1 = y1 < 0 ? 0 : y1 >= MAPH ? MAPH - 1 : y1;
    const MapT *M = g_map[s];
    MapT a = M[(size_t)y0 * MAPW + x0], b = M[(size_t)y0 * MAPW + x1];
    MapT c = M[(size_t)y1 * MAPW + x0], e = M[(size_t)y1 * MAPW + x1];
    MapT r;
    r.s = (float)mixd(mixd(a.s, b.s, ax), mixd(c.s, e.s, ax), ay);
    /* depth / width follow the nearest texel so neighbouring sulci don't blur */
    MapT n = ay < 0.5 ? (ax < 0.5 ? a : b) : (ax < 0.5 ? c : e);
    r.depth = n.depth; r.w = n.w;
    return r;
}

/* carve a groove: s = distance from its centre line, b = depth into the hull
   (negative inside). Lips round over like real gyral crowns. */
static inline double groove(double b, double s, double w, double depth, double R) {
    double open = w + R * exp(fmin(b, 0) / R);
    return smin(open - s, b + depth, 2.0);
}

/* one folded hemisphere. Returns the field, the hull depth and its label. */
static double hemisphere(V3 p, int s, double *hull_out, int *label_out) {
    V3 l = to_local(p, s);
    double dome, temp;
    double b = hemi_hull(l, &dome, &temp);
    *hull_out = b;
    if (label_out) *label_out = T_NONE;
    if (b > 6 || b < -38) return b;

    double carve = -1e9;
    V3 rel = vsub(l, HEMI_C);
    double rr = vlen(rel);
    V3 d = vmul(rel, 1 / fmax(rr, 1e-6));

    /* Sylvian (lateral) fissure: a near-horizontal cleft rising ~15-20 deg
       backwards, opening down to the insula ~25 mm in */
    double zs = syl_z(s, l.y);
    double yend = SIDES[s].syl_end;
    double along = smooth(yend - 2, yend + 8, l.y) * smooth(30, 22, l.y);
    if (along > 0) {
        double floor_x = 38 - 14 * smooth(2, 24, l.y);      /* free temporal pole */
        double zw = zs + 1.8 * noise3(l.y * 0.07, 3.1 + s, 0.4);
        double c = smin(groove(b, fabs(l.z - zw), 0.9, 40, 2.6), l.x - floor_x, 3);
        c -= (1 - along) * 30;
        carve = fmax(carve, c);
    }

    /* named sulci from the precomputed map */
    MapT m = map_lookup(s, d);
    double s_mm = m.s * rr;
    /* real sulci wander and send off side branches; keep them from looking ruled */
    double wig = 1.6 * noise3(l.x * 0.11 + 4 * s, l.y * 0.11, l.z * 0.11);
    if (m.depth > 0) carve = fmax(carve, groove(b, fabs(s_mm + wig), m.w, m.depth, 3.2));

    /* secondary / tertiary folds: zero-set of domain-warped noise on the
       sphere, stretched so gyri run front-to-back in the frontal and
       temporal lobes and top-to-bottom around the central sulcus */
    double mask = smooth(3.0, 8.0, s_mm);
    mask *= 1 - smooth(-6, -20, l.z - zs) * smooth(30, 40, l.x) * along;   /* inside fissure */
    mask *= 1 - (1 - smooth(4, 9, l.x)) * smooth(18, 10, l.z) * smooth(-48, -40, l.y) * smooth(28, 20, l.y);
    if (mask > 0.01) {
        V3 q = vadd(vadd(HEMI_C, vmul(d, 72)), SIDES[s].noise_off);
        double wf = smooth(-5, 25, l.y) * (1 - smooth(40, 62, l.y));   /* not on the pole's front face */
        double wtm = smooth(zs - 2, zs - 12, l.z) * smooth(-72, -55, l.y);
        double ycs = -8 - (l.z - 20) * 0.55;
        double wc = exp(-sq((l.y - ycs) / 16));
        double sy = 1 - 0.30 * fmax(wf, wtm), sz = 1 - 0.40 * wc;
        const double f = 1 / 17.0;
        double nx = q.x * f, ny = q.y * f * sy, nz = q.z * f * sz;
        double wx = noise3(nx * 0.4 + 5.2, ny * 0.4, nz * 0.4);
        double wy = noise3(nx * 0.4, ny * 0.4 + 9.1, nz * 0.4);
        double wz = noise3(nx * 0.4, ny * 0.4, nz * 0.4 + 2.7);
        double ux = nx + 0.9 * wx, uy = ny + 0.9 * wy, uz = nz + 0.9 * wz;
        double n1 = noise3(ux, uy, uz);
        /* distance to the zero-contour = |n| / |grad n|. Using the true gradient
           (not a constant) stops flat stretches of noise from opening into wide
           flat-bottomed terraces. rr/72 maps the sphere back onto the surface. */
        const double e = 0.05;
        double gx = (noise3(ux + e, uy, uz) - n1) / e * f;
        double gy = (noise3(ux, uy + e, uz) - n1) / e * f * sy;
        double gz = (noise3(ux, uy, uz + e) - n1) / e * f * sz;
        double g1 = fmax(sqrt(gx * gx + gy * gy + gz * gz), 0.6 * f);
        double s1 = fabs(n1) / g1 * rr / 72;
        double dep1 = (10.0 + 6.0 * noise3(nx * 0.7 + 11, ny * 0.7, nz * 0.7)) * mask;
        if (dep1 > 0.3) carve = fmax(carve, groove(b, s1, 0.6, dep1, 3.0));
        /* a sparser second family: side branches and short tertiary sulci */
        const double f2 = 1 / 12.0;
        double vx = q.x * f2 + 31, vy = q.y * f2 * (0.5 + 0.5 * sy), vz = q.z * f2;
        double n2 = noise3(vx, vy, vz);
        double hx = (noise3(vx + e, vy, vz) - n2) / e * f2;
        double hy = (noise3(vx, vy + e, vz) - n2) / e * f2 * (0.5 + 0.5 * sy);
        double hz = (noise3(vx, vy, vz + e) - n2) / e * f2;
        double s2 = fabs(n2) / fmax(sqrt(hx * hx + hy * hy + hz * hz), 0.6 * f2) * rr / 72;
        double dep2 = 6.0 * mask * smooth(0.15, 0.45, noise3(q.x * f2 * 0.45, q.y * f2 * 0.45 + 7, q.z * f2 * 0.45));
        if (dep2 > 0.5) carve = fmax(carve, groove(b, s2, 0.4, dep2, 2.6));
    }

    /* blend rather than intersect: gyral crowns round over into the sulci
       instead of meeting them at a crease */
    double F = smax(b, carve, 3.0);

    if (label_out) {
        int lab;
        double yocc = -66 - (l.z + 20) * 0.12;            /* parieto-occipital / preoccipital line */
        if (l.x < 42 && fabs(l.z - zs) < 9 && along > 0.5 && b < -11 && l.x > 20) lab = T_INSULA;
        else if (l.y < yocc) lab = T_OCCIPITAL;
        else if (l.z < (l.y > yend ? zs : syl_z(s, yend)) && (l.y < 8 || temp < dome)) lab = T_TEMPORAL;
        else {
            int seg;
            const Sulcus *C = &g_cs_ext[s];
            sulcus_dist(C, d, NULL, &seg);
            V3 n = vcross(C->d[seg], C->d[seg + 1]);
            V3 fr = vnorm(vsub(v3(40, 40, 30), HEMI_C));
            lab = (vdot(d, n) > 0) == (vdot(fr, n) > 0) ? T_FRONTAL : T_PARIETAL;
        }
        *label_out = lab;
    }
    return F;
}

/* cerebellum: two hemispheres + vermis under the tentorium, with folia that
   fan out from the peduncles like the pages of a book */
static double cerebellum(V3 p, double *hull_out) {
    double ax = fabs(p.x);
    V3 q = v3(ax, p.y, p.z);
    double dh = sd_ellipsoid(q, v3(23, -65, -37), v3(28, 27, 22));
    double dv = sd_ellipsoid(p, v3(0, -63, -33), v3(13, 27, 21));
    double d = smin(dh, dv, 9);
    d = smax(d, p.z - (tent_z(ax, p.y) - 1.4), 4);
    *hull_out = d;
    if (d > 4 || d < -26) return d;

    /* the fan axis sags laterally so folia arc down over each hemisphere */
    double yc = -42, zc = -31 + 0.010 * ax * ax, dy = p.y - yc, dz = p.z - zc;
    double r = sqrt(dy * dy + dz * dz);
    double th = atan2(dz, -dy);
    th += 0.05 * sin(ax / 9.0) + 0.06 * noise3(p.x * 0.06, p.y * 0.06, p.z * 0.06);
    const double per = 0.062;
    double k = th / per;
    int idx = (int)floor(k + 0.5);
    double s = fabs(k - idx) * per * r;
    double hsh = PERM[(idx + 1000) & 255] / 255.0;          /* irregular lobule spacing */
    /* fissures between lobules reach deep into the white core, which is what
       draws the "arbor vitae" tree in a midsagittal cut */
    int lobular = idx == -1 || ((idx % 4) + 4) % 4 == 1;   /* ~10 lobules, as in lobules I-X */
    double depth = lobular ? 15 : hsh < 0.5 ? 6.5 : 3.0 + 1.5 * hsh;
    double w = depth > 10 ? 0.6 : depth > 5 ? 0.42 : 0.24;
    double open = w + 0.7 * exp(fmin(d, 0) / 0.7);
    /* lobular fissures run in until ~1 cm from the fastigium, leaving the
       central white body; folia are shallow ridges on each lobule */
    double floor_term = depth > 10 ? r - (9 + 5 * hsh) : d + depth;
    double carve = smin(open - s, floor_term, 1.5);
    /* posterior cerebellar notch between the hemispheres */
    double notch = smooth(-72, -86, p.y) * smooth(-24, -34, p.z);
    if (notch > 0) carve = fmax(carve, groove(d, ax, 0.8, 12 * notch, 2.5));
    return fmax(d, carve);
}

static double brainstem(V3 p) {
    double ax = fabs(p.x);
    V3 q = v3(ax, p.y, p.z);
    double mid = sd_capsule(p, v3(0, -20, -4), v3(0, -24, -20), 11, 11);
    double ped = sd_capsule(q, v3(5, -21, -21), v3(12, -11, -5), 6, 7);
    double pons = sd_ellipsoid(p, v3(0, -24, -32), v3(17, 13, 12.5));
    double med = sd_capsule(p, v3(0, -33, -42), v3(0, -41, -84), 9.5, 6.5);
    double olive = sd_ellipsoid(q, v3(6.5, -30, -52), v3(3.2, 3.6, 7.5));
    double d = smin(mid, ped, 4);
    d = smin(d, pons, 5);
    d = smin(d, med, 6);
    d = smin(d, olive, 2);
    /* basilar groove and anterior median fissure */
    d = fmax(d, groove(d, ax, 0.35, 1.6 * smooth(-32, -20, p.y), 0.8));
    return smax(d, -86 - p.z, 1.5);
}

static double thalamus(V3 p) {
    V3 q = v3(fabs(p.x), p.y, p.z);
    double t = sd_ellipsoid(q, v3(10, -17, 6), v3(10.5, 16, 9));
    double hy = sd_ellipsoid(p, v3(0, -6, -9), v3(9, 10, 6));
    double mb = sd_ellipsoid(q, v3(3, -10, -15), v3(2.8, 3, 2.8));     /* mammillary bodies */
    return smin(smin(t, hy, 4), mb, 1.5);
}

static double callosum(V3 p) {
    static const double C[][4] = {   /* y, z, radius; genu -> body -> splenium */
        {14, -2, 2.5}, {24, 3, 4.5}, {28, 12, 5.5}, {22, 20, 4.2}, {6, 24, 3.2},
        {-12, 25, 3.2}, {-28, 21, 4.0}, {-38, 12, 6.5},
    };
    V3 q = v3(p.x / 2.6, p.y, p.z);
    double d = 1e9;
    for (int i = 0; i + 1 < (int)(sizeof C / sizeof C[0]); i++)
        d = fmin(d, sd_capsule(q, v3(0, C[i][0], C[i][1]), v3(0, C[i + 1][0], C[i + 1][1]),
                               C[i][2], C[i + 1][2]));
    return d * 0.8;
}

static double nerves(V3 p) {
    V3 q = v3(fabs(p.x), p.y, p.z);
    double d = sd_ellipsoid(p, v3(0, 2, -13), v3(10, 5, 2.6));                    /* chiasm */
    d = fmin(d, sd_capsule(q, v3(5, 4, -13), v3(12, 24, -17), 2.3, 2.1));           /* optic nerves */
    d = fmin(d, sd_capsule(q, v3(7, -1, -12), v3(19, -13, -7), 2.4, 2.4));          /* optic tracts */
    d = fmin(d, sd_capsule(p, v3(0, 0, -14), v3(0, 2, -23), 1.5, 1.3));             /* infundibulum */
    d = fmin(d, sd_capsule(q, v3(9, 6, -24.4), v3(10, 34, -22.1), 1.4, 1.4));       /* olfactory tracts */
    d = fmin(d, sd_ellipsoid(q, v3(10, 39, -21.6), v3(2.6, 6, 2.2)));               /* olfactory bulbs */
    d = fmin(d, sd_capsule(q, v3(14, -24, -31), v3(25, -15, -34), 2.6, 2.3));       /* trigeminal */
    d = fmin(d, sd_capsule(q, v3(4, -15, -16), v3(10, -3, -20), 1.3, 1.2));         /* oculomotor */
    return d;
}

static double ventricles(V3 p) {
    V3 q = v3(fabs(p.x), p.y, p.z);
    double d = sd_capsule(q, v3(10, 24, 8), v3(11, 2, 16), 3.6, 3.4);               /* frontal horn */
    d = fmin(d, sd_capsule(q, v3(11, 2, 16), v3(13, -24, 18), 3.0, 3.0));          /* body */
    d = fmin(d, sd_capsule(q, v3(13, -24, 18), v3(20, -40, 10), 3.0, 4.8));         /* atrium */
    d = fmin(d, sd_capsule(q, v3(20, -40, 10), v3(17, -60, 6), 3.0, 1.4));          /* occipital horn */
    d = fmin(d, sd_capsule(q, v3(20, -40, 10), v3(30, -24, -8), 3.0, 2.2));         /* temporal horn */
    d = fmin(d, sd_capsule(q, v3(30, -24, -8), v3(30, -8, -20), 2.2, 1.6));
    d = fmin(d, sd_ellipsoid(p, v3(0, -10, 2), v3(1.8, 14, 8)));                    /* third */
    d = fmin(d, sd_capsule(p, v3(0, -24, -5), v3(0, -32, -20), 1.0, 1.0));          /* aqueduct */
    d = fmin(d, sd_ellipsoid(p, v3(0, -37, -29), v3(7, 4.0, 8)));                   /* fourth */
    return d;
}

/* grey nuclei visible in cut-away views */
static int in_grey_nucleus(V3 p) {
    V3 q = v3(fabs(p.x), p.y, p.z);
    if (thalamus(p) < 0) return 1;
    if (sd_ellipsoid(q, v3(14, 14, 10), v3(5.5, 10, 8)) < 0) return 1;             /* caudate head */
    if (sd_capsule(q, v3(14, 4, 16), v3(18, -26, 20), 2.6, 1.4) < 0) return 1;     /* caudate body */
    if (sd_ellipsoid(q, v3(24, 2, 0), v3(5.5, 15, 10)) < 0) return 1;              /* putamen */
    if (sd_ellipsoid(q, v3(18, -2, -2), v3(3.5, 8, 6)) < 0) return 1;              /* globus pallidus */
    if (sd_ellipsoid(q, v3(35, -2, 2), v3(1.2, 16, 12)) < 0) return 1;             /* claustrum */
    if (sd_ellipsoid(q, v3(22, -4, -18), v3(6, 6, 6)) < 0) return 1;               /* amygdala */
    if (sd_capsule(q, v3(26, -14, -14), v3(26, -36, -4), 3.5, 3) < 0) return 1;    /* hippocampus */
    if (sd_ellipsoid(q, v3(7, -18, -12), v3(3, 6, 2)) < 0) return 2;               /* substantia nigra */
    if (sd_ellipsoid(q, v3(12, -55, -32), v3(4.5, 7, 6)) < 0) return 1;            /* dentate nucleus */
    return 0;
}

/* The whole brain: field (mm, negative inside), hull depth, label. */
static double brain_field(V3 p, double *depth_out, int *label_out) {
    double bR, bL, bC;
    int lR, lL;
    double hR = hemisphere(p, 0, &bR, &lR);
    double hL = hemisphere(p, 1, &bL, &lL);
    double cc = callosum(p), th = thalamus(p);
    double cb = cerebellum(p, &bC);
    double bs = brainstem(p);
    double nv = nerves(p);

    double cer = fmin(hR, hL);
    double F = smin(cer, cc, 2.5);
    F = fmin(F, th);
    double hind = smin(cb, bs, 6);
    hind = smin(hind, th, 4);
    F = fmin(F, hind);
    F = smin(F, nv, 1.2);
    F = fmax(F, -ventricles(p));

    int lab;
    double dep = 0;
    double m = fmin(fmin(hR, hL), fmin(fmin(cc, th), fmin(cb, fmin(bs, nv))));
    if (m == hR) { lab = lR; dep = -bR; }
    else if (m == hL) { lab = lL; dep = -bL; }
    else if (m == cb) { lab = T_CEREBELLUM; dep = -bC; }
    else if (m == bs) lab = T_STEM;
    else if (m == cc) lab = T_CALLOSUM;
    else if (m == th) lab = T_THALAMUS;
    else lab = T_NERVE;
    if (lab == T_NONE) lab = T_FRONTAL;
    *depth_out = dep;
    *label_out = lab;
    return F;
}

/* ============================================================== VOLUME === */

static double g_h = 0.8;                 /* voxel size, mm */
static const double VX0 = -76, VX1 = 76, VY0 = -112, VY1 = 82, VZ0 = -90, VZ1 = 84;
static int NX, NY, NZ;
static double g_ih;
static float *g_vol;
static unsigned char *g_lab, *g_dep;     /* label; hull depth in 0.25 mm */

typedef struct { int z0, z1; } VolJob;
static void *vol_worker(void *arg) {
    VolJob *J = arg;
    for (int k = J->z0; k < J->z1; k++)
        for (int j = 0; j < NY; j++)
            for (int i = 0; i < NX; i++) {
                V3 p = v3(VX0 + i * g_h, VY0 + j * g_h, VZ0 + k * g_h);
                double dep;
                int lab;
                double F = brain_field(p, &dep, &lab);
                size_t ix = ((size_t)k * NY + j) * NX + i;
                g_vol[ix] = (float)F;
                g_lab[ix] = (unsigned char)lab;
                g_dep[ix] = (unsigned char)clampd(dep * 4, 0, 255);
            }
    return NULL;
}

static int g_threads = 0;

static void build_volume(void) {
    noise_init(1873);                   /* Broca's year, for luck */
    for (int s = 0; s < 2; s++) {
        build_sulci(s);
        g_map[s] = xmalloc(sizeof(MapT) * MAPW * MAPH);
    }
    int T = g_threads;
    pthread_t th[256];
    MapJob mj[256];
    for (int s = 0; s < 2; s++) {
        for (int t = 0; t < T; t++) {
            mj[t].side = s; mj[t].j0 = MAPH * t / T; mj[t].j1 = MAPH * (t + 1) / T;
            pthread_create(&th[t], NULL, map_worker, &mj[t]);
        }
        for (int t = 0; t < T; t++) pthread_join(th[t], NULL);
    }

    NX = (int)((VX1 - VX0) / g_h) + 1;
    NY = (int)((VY1 - VY0) / g_h) + 1;
    NZ = (int)((VZ1 - VZ0) / g_h) + 1;
    g_ih = 1 / g_h;
    size_t n = (size_t)NX * NY * NZ;
    g_vol = xmalloc(n * sizeof(float));
    g_lab = xmalloc(n);
    g_dep = xmalloc(n);
    VolJob vj[256];
    for (int t = 0; t < T; t++) {
        vj[t].z0 = NZ * t / T; vj[t].z1 = NZ * (t + 1) / T;
        pthread_create(&th[t], NULL, vol_worker, &vj[t]);
    }
    for (int t = 0; t < T; t++) pthread_join(th[t], NULL);
}

static inline double vol_at(double x, double y, double z) {
    double fx = (x - VX0) * g_ih, fy = (y - VY0) * g_ih, fz = (z - VZ0) * g_ih;
    if (fx < 0 || fy < 0 || fz < 0 || fx >= NX - 1 || fy >= NY - 1 || fz >= NZ - 1) return 8;
    int i = (int)fx, j = (int)fy, k = (int)fz;
    double a = fx - i, b = fy - j, c = fz - k;
    size_t sx = 1, sy = (size_t)NX, sz = (size_t)NX * NY;
    const float *v = g_vol + k * sz + j * sy + i;
    double c00 = v[0] + (v[sx] - v[0]) * a;
    double c10 = v[sy] + (v[sy + sx] - v[sy]) * a;
    double c01 = v[sz] + (v[sz + sx] - v[sz]) * a;
    double c11 = v[sz + sy] + (v[sz + sy + sx] - v[sz + sy]) * a;
    double c0 = c00 + (c10 - c00) * b, c1 = c01 + (c11 - c01) * b;
    return c0 + (c1 - c0) * c;
}
static inline double vol_v(V3 p) { return vol_at(p.x, p.y, p.z); }

static inline size_t vox_near(V3 p) {
    int i = (int)clampd((p.x - VX0) * g_ih + 0.5, 0, NX - 1);
    int j = (int)clampd((p.y - VY0) * g_ih + 0.5, 0, NY - 1);
    int k = (int)clampd((p.z - VZ0) * g_ih + 0.5, 0, NZ - 1);
    return ((size_t)k * NY + j) * NX + i;
}

static double dep_at(V3 p) {            /* trilinear hull depth, mm */
    double fx = clampd((p.x - VX0) * g_ih, 0, NX - 1.001);
    double fy = clampd((p.y - VY0) * g_ih, 0, NY - 1.001);
    double fz = clampd((p.z - VZ0) * g_ih, 0, NZ - 1.001);
    int i = (int)fx, j = (int)fy, k = (int)fz;
    double a = fx - i, b = fy - j, c = fz - k;
    size_t sy = (size_t)NX, sz = (size_t)NX * NY;
    const unsigned char *v = g_dep + k * sz + j * sy + i;
    double c00 = v[0] + (v[1] - v[0]) * a, c10 = v[sy] + (v[sy + 1] - v[sy]) * a;
    double c01 = v[sz] + (v[sz + 1] - v[sz]) * a, c11 = v[sz + sy] + (v[sz + sy + 1] - v[sz + sy]) * a;
    double c0 = c00 + (c10 - c00) * b, c1 = c01 + (c11 - c01) * b;
    return (c0 + (c1 - c0) * c) * 0.25;
}

/* ============================================================ RENDERER === */

enum { MAT_FRESH, MAT_FIXED, MAT_LOBES, MAT_MRI, MAT_COUNT };
static const char *MAT_NAME[MAT_COUNT] = {"fresh", "formalin-fixed", "lobes atlas", "MRI surface"};
enum { CUT_NONE, CUT_CORONAL, CUT_AXIAL, CUT_SAGITTAL, CUT_COUNT };
static const char *CUT_NAME[CUT_COUNT] = {"", "coronal cut", "axial cut", "midsagittal cut"};

static const char RAMP[] = " .`'-:;~=+*ixoaO0X#%@";
#define NRAMP ((int)sizeof RAMP - 1)

#define CAM_D 1100.0
static const V3 TARGET = {0, -15, -4};

typedef struct {
    int W, H, aspx;             /* aspx: 1 = square pixels (image output) */
    char *chr;
    float *lum, *cov, *col;     /* per cell: mean luminance, ray coverage, linear colour */
    double expo;                /* auto-exposure, smoothed over frames */
    unsigned char *rgb;
    double t, yaw, spin, pitch, zoom;
    int colour, mat, cut, shadows, activity, paused, hud, ss;
    double fps;
    const char *centre;         /* structure under the screen centre */
    /* per-frame camera */
    V3 cam, f, r, u, Lk, Lf, Lr;
    double K, cx, cy, asp;
    V3 cut_n; double cut_o;
} State;

static inline double field(const State *S, V3 p) {
    double v = vol_v(p);
    if (S->cut) v = fmax(v, vdot(p, S->cut_n) - S->cut_o);
    return v;
}

static int ray_box(V3 o, V3 d, double *t0, double *t1) {
    double lo = 0, hi = 1e9;
    double ob[3] = {o.x, o.y, o.z}, db[3] = {d.x, d.y, d.z};
    double mn[3] = {VX0 + 1, VY0 + 1, VZ0 + 1}, mx[3] = {VX1 - 1, VY1 - 1, VZ1 - 1};
    for (int a = 0; a < 3; a++) {
        if (fabs(db[a]) < 1e-12) {
            if (ob[a] < mn[a] || ob[a] > mx[a]) return 0;
            continue;
        }
        double ta = (mn[a] - ob[a]) / db[a], tb = (mx[a] - ob[a]) / db[a];
        if (ta > tb) { double t = ta; ta = tb; tb = t; }
        if (ta > lo) lo = ta;
        if (tb < hi) hi = tb;
        if (lo > hi) return 0;
    }
    *t0 = lo; *t1 = hi;
    return 1;
}

static int march(const State *S, V3 o, V3 d, double *thit) {
    double t0, t1;
    if (!ray_box(o, d, &t0, &t1)) return 0;
    double t = t0, last = 0;
    for (int i = 0; i < 400 && t < t1; i++) {
        double s = field(S, vadd(o, vmul(d, t)));
        if (s < 0.04) {
            if (s < 0 && last > 0) {           /* overshot: bisect back */
                double lo = t - last, hi = t;
                for (int k = 0; k < 6; k++) {
                    double mid = 0.5 * (lo + hi);
                    if (field(S, vadd(o, vmul(d, mid))) < 0) hi = mid; else lo = mid;
                }
                t = hi;
            }
            *thit = t;
            return 1;
        }
        last = fmax(s * 0.8, 0.07);
        t += last;
    }
    return 0;
}

static double soft_shadow(const State *S, V3 p, V3 L) {
    double res = 1, t = 1.2;
    for (int i = 0; i < 28 && t < 140; i++) {
        double h = field(S, vadd(p, vmul(L, t)));
        if (h < 0.02) return 0.12;
        res = fmin(res, 5 * h / t);
        t += clampd(h * 0.9, 0.4, 9);
    }
    return 0.12 + 0.88 * clampd(res, 0, 1);
}

typedef struct { double r, g, b; } RGB;
static inline RGB rgb(double r, double g, double b) { RGB c = {r, g, b}; return c; }
static inline RGB rmix(RGB a, RGB b, double t) {
    return rgb(mixd(a.r, b.r, t), mixd(a.g, b.g, t), mixd(a.b, b.b, t));
}

static void albedo(const State *S, int lab, double dep, V3 p, RGB *crown, RGB *deep) {
    switch (S->mat) {
    case MAT_FRESH:
        /* living / fresh tissue: pinkish grey from the blood in the pia */
        *crown = rgb(240, 196, 184); *deep = rgb(140, 58, 62);
        if (lab == T_CEREBELLUM) { *crown = rgb(226, 170, 160); *deep = rgb(120, 52, 56); }
        if (lab == T_STEM || lab == T_NERVE || lab == T_CALLOSUM) { *crown = rgb(228, 200, 184); *deep = rgb(150, 100, 96); }
        if (lab == T_THALAMUS) { *crown = rgb(214, 170, 160); *deep = rgb(140, 80, 80); }
        break;
    case MAT_FIXED:
        /* formalin-fixed specimen: beige-grey, as in an anatomy lab */
        *crown = rgb(214, 198, 166); *deep = rgb(112, 92, 70);
        if (lab == T_CEREBELLUM) { *crown = rgb(190, 170, 138); *deep = rgb(96, 78, 58); }
        if (lab == T_STEM || lab == T_NERVE || lab == T_CALLOSUM) { *crown = rgb(226, 214, 184); *deep = rgb(140, 124, 98); }
        break;
    case MAT_LOBES: {
        /* the classic textbook colour key */
        static const double LC[T_COUNT][3] = {
            [T_NONE] = {200, 200, 200}, [T_FRONTAL] = {84, 132, 232}, [T_PARIETAL] = {242, 196, 84},
            [T_TEMPORAL] = {100, 200, 118}, [T_OCCIPITAL] = {232, 104, 96}, [T_INSULA] = {200, 120, 224},
            [T_CEREBELLUM] = {170, 150, 200}, [T_STEM] = {236, 160, 112}, [T_CALLOSUM] = {240, 240, 236},
            [T_THALAMUS] = {120, 214, 220}, [T_NERVE] = {245, 235, 200},
        };
        *crown = rgb(LC[lab][0], LC[lab][1], LC[lab][2]);
        *deep = rgb(LC[lab][0] * 0.42, LC[lab][1] * 0.42, LC[lab][2] * 0.42);
        break;
    }
    default:
        /* FreeSurfer-style binary curvature: gyri light, sulci dark */
        *crown = rgb(196, 196, 200); *deep = rgb(78, 78, 84);
        break;
    }
    (void)dep; (void)p;
}

/* shade one ray; returns luminance in [0,1] and writes the colour */
static double shade_ray(const State *S, V3 o, V3 d, RGB *out) {
    double t;
    if (!march(S, o, d, &t)) return -1;
    V3 p = vadd(o, vmul(d, t));
    V3 V = vmul(d, -1);
    size_t vx = vox_near(p);
    int lab = g_lab[vx];
    double v = vol_v(p);
    int on_cut = S->cut && fabs(vdot(p, S->cut_n) - S->cut_o) < 0.12 && v < -0.05;

    V3 n;
    if (on_cut) n = S->cut_n;
    else {
        const double e = 0.45;
        n = vnorm(v3(vol_at(p.x + e, p.y, p.z) - vol_at(p.x - e, p.y, p.z),
                     vol_at(p.x, p.y + e, p.z) - vol_at(p.x, p.y - e, p.z),
                     vol_at(p.x, p.y, p.z + e) - vol_at(p.x, p.y, p.z - e)));
        if (vdot(n, V) < 0) n = vmul(n, -1);
    }

    double dep = dep_at(p);
    RGB crown, deep, base;
    double cav;

    if (on_cut) {
        /* cut face: grey matter ribbon over white matter, deep grey nuclei */
        int nuc = in_grey_nucleus(p);
        int ribbon = (lab <= T_INSULA && v > -2.7 && ventricles(p) > 2.5) ||
                     (lab == T_CEREBELLUM && v > -1.5);
        RGB gm, wm;
        if (S->mat == MAT_FRESH) { gm = rgb(166, 112, 110); wm = rgb(210, 196, 184); }
        else if (S->mat == MAT_FIXED) { gm = rgb(140, 120, 96); wm = rgb(204, 194, 168); }
        else if (S->mat == MAT_MRI) { gm = rgb(112, 112, 116); wm = rgb(206, 206, 210); }
        else {
            albedo(S, lab, 0, p, &crown, &deep);
            gm = rmix(crown, rgb(60, 60, 60), 0.35);
            wm = rmix(crown, rgb(255, 255, 255), 0.65);
        }
        base = (ribbon || nuc == 1) ? gm : wm;
        if (nuc == 2) base = rgb(46, 36, 40);              /* substantia nigra: dark */
        cav = 0;
        base = rmix(base, rgb(0, 0, 0), 0.18);             /* a cut face is matte */
    } else {
        albedo(S, lab, dep, p, &crown, &deep);
        if (S->mat == MAT_MRI) cav = smooth(1.2, 2.8, dep);
        else cav = smooth(1.0, 10, dep);
        base = rmix(crown, deep, cav * 0.85);
        if (S->mat == MAT_FRESH && lab <= T_INSULA && dep < 2.5) {
            /* pial veins running up over the gyral crowns towards the midline */
            double vn = 1 - fabs(noise3(p.x * 0.045 + 3, p.y * 0.05, p.z * 0.016));
            double vn2 = 1 - fabs(noise3(p.x * 0.03, p.y * 0.07 + 8, p.z * 0.03));
            double vein = smooth(0.972, 0.99, fmax(vn, vn2 * 0.99)) * (1 - smooth(0.8, 2.0, dep));
            base = rmix(base, rgb(150, 52, 70), 0.45 * vein);
        }
    }

    /* ambient occlusion from the distance field, at two scales */
    double ao = 1;
    if (!on_cut) {
        double occ = 0, w = 1;
        for (int k = 1; k <= 5; k++) {
            double hk = 0.7 * k;
            occ += w * fmax(0, hk - vol_v(vadd(p, vmul(n, hk)))) / hk;
            w *= 0.75;
        }
        double occ2 = 0;
        for (int k = 1; k <= 3; k++) {
            double hk = 4.0 * k;
            occ2 += fmax(0, hk - vol_v(vadd(p, vmul(n, hk)))) / hk;
        }
        ao = clampd(1 - 0.26 * occ, 0, 1) * clampd(1 - 0.16 * occ2, 0, 1);
    }

    double shk = S->shadows ? soft_shadow(S, vadd(p, vmul(n, 0.3)), S->Lk) : 1;
    double ndl = vdot(n, S->Lk);
    double diff = clampd((ndl + 0.35) / 1.35, 0, 1) * shk;       /* wrap: soft tissue */
    double fill = clampd((vdot(n, S->Lf) + 0.2) / 1.2, 0, 1);
    double rim = pow(clampd(1 - vdot(n, V), 0, 1), 2.5) * clampd(vdot(n, S->Lr) + 0.6, 0, 1);
    V3 Hh = vnorm(vadd(S->Lk, V));
    double gloss = S->mat == MAT_FRESH ? 0.45 : S->mat == MAT_FIXED ? 0.10 : 0.18;
    double spec = pow(fmax(0, vdot(n, Hh)), 46) * gloss * shk * ao * (on_cut ? 0.3 : 1);

    double light = 0.30 * ao + 0.85 * diff * (0.35 + 0.65 * ao) + 0.26 * fill * ao + 0.30 * rim * ao;
    double R = base.r * light, G = base.g * light, B = base.b * light;

    /* subsurface: light scattering through tissue keeps shadows warm */
    if (S->mat == MAT_FRESH && !on_cut) {
        double sss = (1 - shk * clampd(ndl, 0, 1)) * 0.10 * ao;
        R += 120 * sss; G += 30 * sss; B += 30 * sss;
    }
    R += 255 * spec; G += 245 * spec; B += 235 * spec;

    double lum = (0.299 * base.r + 0.587 * base.g + 0.114 * base.b) / 255.0;
    lum = lum * light + spec;

    if (S->activity && !on_cut && lab <= T_CEREBELLUM) {
        /* travelling waves of activity sweeping across the cortex */
        double ph = 0.10 * (p.y + 0.6 * p.z) - 2.6 * S->t + 2.2 * noise3(p.x * 0.03, p.y * 0.03, p.z * 0.03 + S->t * 0.15);
        double glow = pow(fmax(0, sin(ph)), 14) * (1 - cav) * ao;
        R += 60 * glow; G += 200 * glow; B += 255 * glow;
        lum += 0.45 * glow;
    }
    out->r = R; out->g = G; out->b = B;
    return clampd(lum, 0, 1);
}

typedef struct { State *S; int y0, step; } RowJob;

static void *render_rows(void *arg) {
    RowJob *J = arg;
    State *S = J->S;
    int W = S->W, H = S->H, ss = S->ss;
    int ssy = S->aspx ? ss : ss * 2 > 4 ? 4 : ss * 2;  /* cells are ~2x taller */
    if (ss == 1) ssy = S->aspx ? 1 : 2;
    for (int y = J->y0; y < H; y += J->step)
        for (int x = 0; x < W; x++) {
            double lum = 0, hit = 0;
            RGB acc = {0, 0, 0};
            for (int j = 0; j < ssy; j++)
                for (int i = 0; i < ss; i++) {
                    double px = x + (i + 0.5) / ss, py = y + (j + 0.5) / ssy;
                    double sx = (px - S->cx) / S->K, sy = -(py - S->cy) / (S->K * S->asp);
                    V3 d = vnorm(vadd(vmul(S->f, CAM_D), vadd(vmul(S->r, sx), vmul(S->u, sy))));
                    RGB c;
                    double l = shade_ray(S, S->cam, d, &c);
                    if (l >= 0) { lum += l; hit += 1; acc.r += c.r; acc.g += c.g; acc.b += c.b; }
                }
            int idx = y * W + x;
            double n = ss * ssy;
            S->cov[idx] = (float)(hit / n);
            if (hit == 0) {
                S->lum[idx] = 0;
                S->col[idx * 3] = S->col[idx * 3 + 1] = S->col[idx * 3 + 2] = 0;
                continue;
            }
            S->lum[idx] = (float)(lum / hit);
            double cov = 0.35 + 0.65 * hit / n;
            S->col[idx * 3 + 0] = (float)(acc.r / hit * cov);
            S->col[idx * 3 + 1] = (float)(acc.g / hit * cov);
            S->col[idx * 3 + 2] = (float)(acc.b / hit * cov);
        }
    return NULL;
}

static void setup_camera(State *S) {
    double yaw = S->yaw, pitch = S->pitch;
    pitch += 0.05 * sin(0.6 * S->t);              /* a gentle nod while it turns */
    V3 dir = v3(-cos(pitch) * cos(yaw), -cos(pitch) * sin(yaw), sin(pitch));
    S->cam = vadd(TARGET, vmul(dir, CAM_D));
    S->f = vnorm(vsub(TARGET, S->cam));
    S->r = vnorm(vcross(S->f, v3(0, 0, 1)));
    S->u = vcross(S->r, S->f);
    /* lights, fixed relative to the viewer */
    S->Lk = vnorm(vadd(vadd(vmul(S->r, -0.55), vmul(S->u, 0.75)), vmul(S->f, -0.45)));
    S->Lf = vnorm(vadd(vadd(vmul(S->r, 0.85), vmul(S->u, 0.05)), vmul(S->f, -0.5)));
    S->Lr = vnorm(vadd(vadd(vmul(S->r, 0.25), vmul(S->u, 0.45)), vmul(S->f, 0.85)));

    S->asp = S->aspx ? 1.0 : 0.5;
    double fitw = S->W * 0.97 / (2 * 100.0);
    double fith = S->H * 0.97 / (S->asp * 2 * 94.0);
    S->K = fmin(fitw, fith) * S->zoom;
    S->cx = S->W / 2.0;
    S->cy = S->H / 2.0;

    switch (S->cut) {          /* the removed side is where dot(p,n) > o */
    case CUT_CORONAL:  S->cut_n = v3(0, 1, 0);  S->cut_o = -6;  break;   /* front removed */
    case CUT_AXIAL:    S->cut_n = v3(0, 0, 1);  S->cut_o = 16;  break;   /* top removed */
    case CUT_SAGITTAL: S->cut_n = v3(-1, 0, 0); S->cut_o = 0;   break;   /* left half removed */
    default: break;
    }
}

static void render(State *S) {
    setup_camera(S);
    int T = g_threads;
    pthread_t th[256];
    RowJob jb[256];
    for (int t = 0; t < T; t++) {
        jb[t].S = S; jb[t].y0 = t; jb[t].step = T;
        pthread_create(&th[t], NULL, render_rows, &jb[t]);
    }
    for (int t = 0; t < T; t++) pthread_join(th[t], NULL);

    /* name whatever lies under the centre of the screen, for the HUD */
    {
        V3 d = S->f;
        double t;
        S->centre = march(S, S->cam, d, &t) ? TISSUE_NAME[g_lab[vox_near(vadd(S->cam, vmul(d, t)))]] : "";
    }

    /* auto-exposure: put the brightest few percent of tissue at the top of
       the ramp, so crowns read as dense glyphs and sulci stay dark */
    int N = S->W * S->H, hist[256] = {0}, tot = 0;
    for (int i = 0; i < N; i++)
        if (S->cov[i] > 0) { hist[(int)clampd(S->lum[i] * 255, 0, 255)]++; tot++; }
    double p97 = 1;
    for (int b = 0, acc = 0; b < 256 && tot; b++) {
        acc += hist[b];
        if (acc >= tot * 0.985) { p97 = (b + 0.5) / 255.0; break; }
    }
    double target = 1.0 / fmax(p97, 0.15);
    S->expo = S->expo > 0 ? mixd(S->expo, target, 0.15) : target;
    /* a glyph only lights part of its cell, so colours run hotter than the
       luminance; deep sulci keep a floor so they read as dark red, not void */
    double ce = S->expo * (S->aspx ? 1.0 : 1.25);
    for (int i = 0; i < N; i++) {
        for (int c = 0; c < 3; c++) {
            double v = S->col[i * 3 + c] * ce;
            if (!S->aspx && S->cov[i] > 0) v = 255 * pow(clampd(v / 255, 0, 1), 0.85);
            S->rgb[i * 3 + c] = (unsigned char)clampd(v, 0, 255);
        }
        if (S->cov[i] <= 0) { S->chr[i] = ' '; continue; }
        double L = clampd(S->lum[i] * S->expo, 0, 1);
        int ci = (int)(pow(L, 1.15) * (NRAMP - 1) + 0.5);
        if (ci < 1) ci = 1;
        if (ci > NRAMP - 1) ci = NRAMP - 1;
        if (S->cov[i] < 0.25 && ci > 2) ci = 2;            /* soft silhouette */
        S->chr[i] = RAMP[ci];
    }
}

/* ------------------------------------------------------------ terminal */

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

static void reset_view(State *S) { S->spin = 0.45; S->pitch = 0.32; S->zoom = 1.0; }

static void handle_keys(State *S) {
    unsigned char kb[64];
    ssize_t n = g_raw ? read(STDIN_FILENO, kb, sizeof kb) : 0;
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = kb[i];
        if (c == 27 && i + 2 < n && kb[i + 1] == '[') {
            switch (kb[i + 2]) {
            case 'A': S->pitch = clampd(S->pitch + 0.06, -1.4, 1.4); break;
            case 'B': S->pitch = clampd(S->pitch - 0.06, -1.4, 1.4); break;
            case 'C': S->spin += 0.15; break;
            case 'D': S->spin -= 0.15; break;
            }
            i += 2;
            continue;
        }
        switch (c) {
        case 27: case 'q': case 'Q': g_quit = 1; break;
        case '+': case '=': S->zoom = fmin(S->zoom * 1.1, 5.0); break;
        case '-': case '_': S->zoom = fmax(S->zoom / 1.1, 0.3); break;
        case ' ': S->paused ^= 1; break;
        case 'c': S->colour ^= 1; break;
        case 'm': S->mat = (S->mat + 1) % MAT_COUNT; break;
        case 'x': S->cut = (S->cut + 1) % CUT_COUNT; break;
        case 'h': S->shadows ^= 1; break;
        case 'a': S->activity ^= 1; break;
        case 'r': reset_view(S); break;
        }
    }
}

static const char *view_name(double yaw) {
    /* camera azimuth -> which face of the brain we see */
    static const char *N[8] = {"left lateral", "left posterolateral", "posterior", "right posterolateral",
                               "right lateral", "right anterolateral", "anterior", "left anterolateral"};
    double a = fmod(fmod(yaw, 2 * M_PI) + 2 * M_PI, 2 * M_PI);
    return N[(int)floor(a / (M_PI / 4) + 0.5) % 8];
}

static char *g_out;
static size_t g_outcap;

static void present(State *S, int interactive) {
    int W = S->W, H = S->H;
    size_t need = (size_t)W * (H + 1) * 24 + 1024;
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
        int len = snprintf(hud, sizeof hud,
            " human brain | %s%s%s | centre: %-15s | %s | zoom %.2f %3.0f fps%s | <-> spin  up/dn tilt  "
            "+/- zoom  m material  x cut  h shadow  a activity  space pause  q quit",
            view_name(S->yaw), S->cut ? ", " : "", CUT_NAME[S->cut], S->centre ? S->centre : "",
            MAT_NAME[S->mat], S->zoom, S->fps, S->paused ? "  PAUSED" : "");
        if (len > W) len = W;
        p += sprintf(p, "\r\n%s%.*s%*s%s", S->colour ? "\033[38;2;150;140;150m" : "",
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

static void write_ppm(State *S, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", S->W, S->H);
    fwrite(S->rgb, 3, (size_t)S->W * S->H, f);
    fclose(f);
}

static void usage(const char *argv0) {
    printf("usage: %s [options]\n"
           "  --size WxH       render size in characters (default: the terminal)\n"
           "  --frames N       render N frames to stdout, then exit\n"
           "  --at T           start at animation time T seconds\n"
           "  --yaw DEG        start azimuth (0 = left lateral view)\n"
           "  --pitch DEG      camera elevation (default 18)\n"
           "  --spin R         spin speed in rad/s (default 0.45)\n"
           "  --material M     fresh | fixed | lobes | mri\n"
           "  --cut C          none | coronal | axial | sagittal\n"
           "  --aa N           supersampling per cell axis (1-3, default 3)\n"
           "  --res MM         voxel size in mm (default 0.8; smaller = finer, slower start)\n"
           "  --threads N      worker threads (default: all cores)\n"
           "  --fps N          target frame rate (default 30)\n"
           "  --no-shadows  --activity  --mono\n"
           "  --ppm FILE       render one square-pixel image of --size pixels and exit\n"
           "keys: <- -> spin, up/down tilt, +/- zoom, m material, x cut-away,\n"
           "      h shadows, a activity, c colour, space pause, r reset, q quit\n", argv0);
}

int main(int argc, char **argv) {
    State S = {0};
    reset_view(&S);
    S.colour = 1; S.shadows = 1; S.ss = 3;
    int fixW = 0, fixH = 0, frames = -1;
    double fps = 30;
    const char *ppm = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "--mono")) S.colour = 0;
        else if (!strcmp(a, "--no-shadows")) S.shadows = 0;
        else if (!strcmp(a, "--activity")) S.activity = 1;
        else if (v && !strcmp(a, "--size")) { sscanf(v, "%dx%d", &fixW, &fixH); i++; }
        else if (v && !strcmp(a, "--frames")) { frames = atoi(v); i++; }
        else if (v && !strcmp(a, "--at")) { S.t = atof(v); i++; }
        else if (v && !strcmp(a, "--yaw")) { S.yaw = atof(v) * M_PI / 180; i++; }
        else if (v && !strcmp(a, "--pitch")) { S.pitch = atof(v) * M_PI / 180; i++; }
        else if (v && !strcmp(a, "--spin")) { S.spin = atof(v); i++; }
        else if (v && !strcmp(a, "--fps")) { fps = fmax(1, atof(v)); i++; }
        else if (v && !strcmp(a, "--aa")) { S.ss = (int)clampd(atoi(v), 1, 3); i++; }
        else if (v && !strcmp(a, "--res")) { g_h = clampd(atof(v), 0.4, 2.0); i++; }
        else if (v && !strcmp(a, "--threads")) { g_threads = atoi(v); i++; }
        else if (v && !strcmp(a, "--ppm")) { ppm = v; i++; }
        else if (v && !strcmp(a, "--material")) {
            S.mat = !strcmp(v, "fixed") ? MAT_FIXED : !strcmp(v, "lobes") ? MAT_LOBES
                  : !strcmp(v, "mri") ? MAT_MRI : MAT_FRESH;
            i++;
        } else if (v && !strcmp(a, "--cut")) {
            S.cut = !strcmp(v, "coronal") ? CUT_CORONAL : !strcmp(v, "axial") ? CUT_AXIAL
                  : !strcmp(v, "sagittal") ? CUT_SAGITTAL : CUT_NONE;
            i++;
        } else { fprintf(stderr, "unknown option: %s\n", a); usage(argv[0]); return 1; }
    }
    if (g_threads <= 0) g_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_threads < 1) g_threads = 1;
    if (g_threads > 256) g_threads = 256;

    int interactive = !ppm && isatty(STDOUT_FILENO);
    S.hud = interactive;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (interactive) {
        term_setup();
        fputs("\033[H  growing cortex: carving 22 named sulci, folding gyri, laying folia ...", stdout);
        fflush(stdout);
    }
    double tb = now_s();
    build_volume();
    double build_s = now_s() - tb;
    if (!interactive && !ppm && getenv("BRAIN3D_TIMING"))
        fprintf(stderr, "volume %dx%dx%d built in %.2f s\n", NX, NY, NZ, build_s);

    if (ppm) {
        S.W = fixW > 0 ? fixW : 800; S.H = fixH > 0 ? fixH : 600; S.aspx = 1;
        S.chr = xmalloc((size_t)S.W * S.H);
        S.rgb = xmalloc((size_t)S.W * S.H * 3);
        S.lum = xmalloc((size_t)S.W * S.H * sizeof(float));
        S.cov = xmalloc((size_t)S.W * S.H * sizeof(float));
        S.col = xmalloc((size_t)S.W * S.H * 3 * sizeof(float));
        render(&S);
        write_ppm(&S, ppm);
        fprintf(stderr, "wrote %s (volume built in %.2f s)\n", ppm, build_s);
        return 0;
    }

    double prev = now_s(), fps_acc = 0;
    int fps_n = 0;
    for (int frame = 0; !g_quit && frame != frames; frame++) {
        int W = fixW, H = fixH;
        if (W <= 0 || H <= 0) {
            struct winsize ws;
            if (interactive && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
                W = ws.ws_col; H = ws.ws_row;
            } else { W = 120; H = 50; }
        }
        if (S.hud) H -= 1;
        if (W < 20) W = 20;
        if (H < 10) H = 10;
        if (W != S.W || H != S.H) {
            S.W = W; S.H = H;
            S.chr = xrealloc(S.chr, (size_t)W * H);
            S.rgb = xrealloc(S.rgb, (size_t)W * H * 3);
            S.lum = xrealloc(S.lum, (size_t)W * H * sizeof(float));
            S.cov = xrealloc(S.cov, (size_t)W * H * sizeof(float));
            S.col = xrealloc(S.col, (size_t)W * H * 3 * sizeof(float));
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
    free(S.chr); free(S.rgb); free(S.lum); free(S.cov); free(S.col); free(g_out);
    free(g_vol); free(g_lab); free(g_dep); free(g_map[0]); free(g_map[1]);
    return 0;
}
