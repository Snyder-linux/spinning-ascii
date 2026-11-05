// station.c - Space Station V from "2001: A Space Odyssey", spinning among the stars
//
// A high-detail, shaded ASCII model of the double-wheel station. The finished
// wheel has a wide rim with panel joints and rows of windows; the wheel still
// under construction shows a skinned arc, an open lattice arc and an arc of
// bare C-shaped ribs. Four spokes per wheel run to hub modules on a central
// axle with a bearing between the wheels and a docking port at the front.
// The station spins about its axis while the view sways slowly against a
// starfield. Everything is ray-marched distance fields with soft shadows,
// a cool fill light from below, ambient occlusion and glowing windows on the dark side.
// Silhouette cells are antialiased and drawn with / \ | _ - .
// A fan rendering built from simple shapes, not a copy of the studio model.
//
// build: cc -O2 station.c -o station -lm
// run:   ./station                  full colour (24-bit terminal)
//        ./station ice              one 256-colour palette
//                                   (white purple orange gold ice ember)
//        ./station --mono           plain text, no colour (also: ./station mono)
//        ./station 4.0              print the frame at t = 4.0 s and exit
//        ./station 4.0 160 50       ... at a fixed size
//        ./station --bench          time 120 frames at 200x60 (or --bench COLS ROWS)
//        flags: --size WxH --frames N --fps N --at T --no-stars --no-edges
//        Piped into another program it prints plain frames, rows ending in \r\n.
// quit:  q, Esc or Ctrl+C
//
// Shrink the terminal font for more detail; the station fills whatever size it gets.
#define _DEFAULT_SOURCE
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>

#define TAU 6.28318530718f
#define NL 12
static const char RAMP[] = ".,-~:;=!*#$@";

// ---------- motion and camera ----------
#define SPIN_PERIOD 24.0f   // seconds per turn of the station about its axis
#define YAW0        0.48f   // view angle onto the wheels (0 = edge-on, pi/2 = face-on), as in the film still
#define YAW_SWAY    0.14f   // the view sways this far either side of YAW0 ...
#define SWAY_PERIOD 80.0f   // ... over this many seconds
#define TILT        0.32f   // camera elevation
#define CAM_DIST    55.0f   // eye distance from the station's centre
#define VIEW_HW     10.9f   // half-width the frame must hold (world units)
#define VIEW_HH     11.65f  // half-height the frame must hold

// ---------- station layout ----------
// units: rim radius = 10; the spin axis is X, the docking port faces +X.
// The wheel under construction is at -X, the finished wheel at +X.
#define RIM_R      10.0f    // radius of the rim's centre line
#define RIM_W       0.90f   // rim half-width along the axis
#define RIM_T       0.42f   // rim half-thickness, radial
#define WHEEL_X     3.60f   // wheel planes at x = -WHEEL_X and +WHEEL_X
#define PANEL       1.31f   // rim panel length (arc units)
#define SPOKE_R0    1.25f   // spokes run from the hub ...
#define SPOKE_R1   (RIM_R - RIM_T + 0.05f)   // ... into the rim
#define SPOKE_RAD   0.30f
#define HUB_R       1.30f   // hub module at each wheel
#define HUB_HX      0.95f
#define AXLE_R      0.55f
#define NOSE_X      6.50f   // docking face
#define TAIL_X     -5.90f
// the unfinished arc of the -X wheel, as angles around the axis (radians)
#define GAP_A0     -2.30f   // open lattice from here ...
#define GAP_A1     -1.45f   // ... bare ribs from here ...
#define GAP_A2     -0.45f   // ... skin again from here

static const float BMIN[3] = {-6.05f, -10.55f, -10.55f};   // whole-station box
static const float BMAX[3] = { 6.60f,  10.55f,  10.55f};

// ---------- materials ----------
enum { M_RIM, M_FRAME, M_SPOKE, M_HUB, M_DETAIL, M_WIN_LIT, M_WIN_DARK, M_DOCK };
static const struct { float r, g, b, ks, shin, er, eg, eb; } MATS[] = {
    [M_RIM]      = {0.90f, 0.90f, 0.88f, 0.30f, 22, 0, 0, 0},
    [M_FRAME]    = {0.74f, 0.73f, 0.70f, 0.20f, 12, 0, 0, 0},
    [M_SPOKE]    = {0.82f, 0.82f, 0.83f, 0.30f, 20, 0, 0, 0},
    [M_HUB]      = {0.93f, 0.93f, 0.91f, 0.35f, 26, 0, 0, 0},
    [M_DETAIL]   = {0.50f, 0.51f, 0.54f, 0.20f, 12, 0, 0, 0},
    [M_WIN_LIT]  = {0.10f, 0.10f, 0.11f, 0.80f, 60, 0.60f, 0.50f, 0.32f},
    [M_WIN_DARK] = {0.06f, 0.07f, 0.09f, 0.80f, 60, 0, 0, 0},
    [M_DOCK]     = {0.08f, 0.09f, 0.11f, 0.30f, 20, 0.20f, 0.26f, 0.34f},
};

// ---------- 256-colour palettes (dark to bright), as in spin.c ----------
typedef struct { const char *name; int c[NL]; } Palette;
static const Palette PALETTES[] = {
    {"white",  {236,238,240,242,244,246,248,250,252,254,255,231}},
    {"purple", { 54, 55, 56, 57, 93, 99,105,141,147,183,189,231}},
    {"orange", { 52, 94,130,166,202,208,214,215,216,222,223,230}},
    {"gold",   { 58, 94,136,142,178,184,220,221,222,227,229,230}},
    {"ice",    { 23, 24, 30, 31, 37, 38, 44, 45, 51, 87,123,195}},
    {"ember",  { 52, 88,124,160,196,202,208,214,220,226,228,230}},
};
#define NPAL (int)(sizeof(PALETTES)/sizeof(PALETTES[0]))

// ---------- small helpers ----------
static float sq(float x) { return x * x; }
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

static float hash1(unsigned n) {
    n *= 0x9E3779B1u; n ^= n >> 15; n *= 0x85EBCA77u; n ^= n >> 13; n *= 0xC2B2AE3Du; n ^= n >> 16;
    return (n & 0xFFFFFF) / 16777216.0f;
}

static float sdbox(float x, float y, float z, float hx, float hy, float hz) {
    float qx = fabsf(x) - hx, qy = fabsf(y) - hy, qz = fabsf(z) - hz;
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0), oz = fmaxf(qz, 0);
    return sqrtf(ox*ox + oy*oy + oz*oz) + fminf(fmaxf(qx, fmaxf(qy, qz)), 0);
}
static float sdbox2(float x, float y, float hx, float hy) {
    float qx = fabsf(x) - hx, qy = fabsf(y) - hy;
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
    return sqrtf(ox*ox + oy*oy) + fminf(fmaxf(qx, qy), 0);
}
static float sdrbox2(float x, float y, float hx, float hy, float r) { return sdbox2(x, y, hx - r, hy - r) - r; }
// cut `region` (negative inside) into the shape d, at most `depth` below its skin
static float carve(float d, float region, float depth) { return fmaxf(d, -fmaxf(region, -(d + depth))); }
// distance from a point at angle th, radius rho, to the angular range [a, b] (negative inside)
static float wedge(float th, float a, float b, float rho) {
    float dt = th - 0.5f * (a + b);
    dt -= TAU * roundf(dt / TAU);
    float e = fabsf(dt) - 0.5f * (b - a);
    return (e < 0 ? -1 : 1) * rho * sinf(fminf(fabsf(e), 1.5707963f));
}

// ---------- level of detail ----------
// Detail finer than a character only adds speckle, so it is dropped on coarse
// grids, and windows and lattice grow so they stay one clean character apiece.
#define FINE_CELL 0.10f            // world units per column where every detail shows
static float g_cell = FINE_CELL;   // this frame's units per column
static int g_fine = 1;             // sub-character detail on
static float lod(void) { return fmaxf(1, g_cell / FINE_CELL); }

// ---------- rims ----------
// (u, v) = axial and radial offset from the rim's centre line, s = arc length
static float rim_solid(float u, float v) { return sdrbox2(u, v, RIM_W, RIM_T, 0.06f); }

static float rim_skin(float u, float v, float s, float base) {
    (void)u;
    if (!g_fine) return base;
    float d = carve(base, fabsf(s - PANEL * roundf(s / PANEL)) - 0.03f, 0.04f);   // panel joints
    return carve(d, fabsf(v + 0.06f) - 0.022f, 0.03f);                          // seam along each side
}

static float rim_lattice(float u, float v, float s, float base) {
    float shell = fabsf(base) - 0.07f;                     // beams sit on the skin's surface
    float k = lod(), sp = 0.55f * k, bw = 0.065f * fminf(k, 1.8f);
    float gs = fabsf(s - sp * roundf(s / sp)) - bw;
    float gu = fabsf(u - RIM_W * roundf(u / RIM_W)) - bw;
    float gv = fabsf(v - RIM_T * roundf(v / RIM_T)) - bw;
    return fmaxf(shell, fminf(gs, fmaxf(gu, gv)));
}

static float rim_ribs(float u, float v, float s) {
    float k = lod(), sp = 0.9f * k;
    float ls = s - sp * roundf(s / sp);
    float frame = fabsf(sdbox2(u, v, RIM_W - 0.05f, RIM_T - 0.05f)) - 0.05f;
    frame = fmaxf(frame, -(v + RIM_T - 0.15f));            // open toward the hub: C-shaped
    float rib = fmaxf(frame, fabsf(ls) - 0.08f * fminf(k, 1.8f));
    float str = fminf(sqrtf(sq(fabsf(u) - RIM_W + 0.06f) + sq(v - RIM_T + 0.06f)) - 0.05f,
                      sqrtf(u*u + sq(v - RIM_T + 0.06f)) - 0.035f);   // stringers
    return fminf(rib, str);
}

// windows and panel shading on a finished rim surface
static int g_want;         // a material is being asked for (not just a distance)
static float g_tint = 1;   // panel shade, set with the material
static int rim_material(float u, float v, float s, unsigned seed) {
    float pi = roundf(s / PANEL);
    g_tint = 0.90f + 0.14f * hash1(seed + (unsigned)(int)(pi + 1000));
    float fu = RIM_W - fabsf(u), fv = RIM_T - fabsf(v);
    float sp, hw, hh, dv;
    unsigned face;
    if (fu < fv) { sp = 0.38f; hw = 0.11f; hh = 0.07f; dv = fabsf(v - 0.22f); face = u > 0 ? 1 : 2; }
    else if (v > 0) { sp = 0.32f; hw = 0.09f; hh = 0.08f; dv = fabsf(fabsf(u) - 0.5f); face = u > 0 ? 3 : 4; }
    else { sp = 0.60f; hw = 0.12f; hh = 0.08f; dv = fabsf(u); face = 5; }
    float z = lod();                                     // windows grow on coarse grids
    sp *= z; hw = fminf(hw * z, sp * 0.3f); hh = fminf(hh * z, face <= 2 ? 0.16f : 0.2f);
    float k = roundf(s / sp), ls = s - sp * k;
    if (fabsf(ls) > hw || dv > hh) return M_RIM;
    return hash1(seed * 31u + face * 7919u + (unsigned)(int)(k + 5000)) < 0.72f ? M_WIN_LIT : M_WIN_DARK;
}

// ---------- spokes: four per wheel, along +-Y and +-Z ----------
static float spoke1(float u, float a, float b, int *m) {
    float mid = 0.5f * (SPOKE_R0 + SPOKE_R1), hl = 0.5f * (SPOKE_R1 - SPOKE_R0);
    float cap = fabsf(a - mid) - hl;
    float r = sqrtf(u*u + b*b);
    float d = fmaxf(r - SPOKE_RAD, cap);
    *m = M_SPOKE;
    float sleeve = fmaxf(r - 0.44f, fabsf(a - (SPOKE_R1 - 0.7f)) - 0.6f);   // housing at the rim
    sleeve = fminf(sleeve, fmaxf(r - 0.46f, fabsf(a - SPOKE_R0 - 0.35f) - 0.35f));   // and at the hub
    if (sleeve < d) { d = sleeve; *m = M_HUB; }
    if (!g_fine) return d;                                   // rails and ties are sub-character
    float rails = fmaxf(sqrtf(sq(fabsf(u) - 0.55f) + b*b) - 0.07f, cap);
    float la = a - 1.1f * roundf(a / 1.1f);
    float ties = fmaxf(sdbox(u, la, b, 0.55f, 0.05f, 0.05f), cap);
    float t = fminf(rails, ties);
    if (t < d) { d = t; *m = M_DETAIL; }
    return d;
}

static float part_wheel(int w, float u, float y, float z, float rho, int *m) {
    float d = 1e9f;
    int sm;
    if (rho < RIM_R) {                              // each spoke lies within 0.62 of its axis
        float au = fabsf(u), b1 = fmaxf(au, fabsf(z)) - 0.62f, b2 = fmaxf(au, fabsf(y)) - 0.62f;
        if (b1 > 0.25f) d = b1;
        else { float s1 = spoke1(u, fabsf(y), z, &sm); if (s1 < d) { d = s1; *m = sm; } }
        if (b2 < d) {
            if (b2 > 0.25f) d = b2;
            else { float s2 = spoke1(u, fabsf(z), y, &sm); if (s2 < d) { d = s2; *m = sm; } }
        }
    }
    float v = rho - RIM_R, base = rim_solid(u, v);
    if (base >= d) return d;
    if (base > 0.3f) { *m = M_RIM; return base; }   // far: the solid rim stands in

    float th = atan2f(z, y), s = th * RIM_R, r;
    if (w == 1) {
        r = rim_skin(u, v, s, base);
        if (r < d) { d = r; *m = g_want ? rim_material(u, v, s, 101) : M_RIM; }
        return d;
    }
    float gap = wedge(th, GAP_A0, GAP_A2, rho);
    float skin = fmaxf(rim_skin(u, v, s, base), -gap);
    if (skin < d) { d = skin; *m = g_want ? rim_material(u, v, s, 202) : M_RIM; }
    if (gap > 1.0f) return d;                       // well away from the unfinished arc
    float lat  = fmaxf(rim_lattice(u, v, s, base), wedge(th, GAP_A0, GAP_A1, rho));
    float ribs = fmaxf(rim_ribs(u, v, s), wedge(th, GAP_A1, GAP_A2, rho));
    r = fminf(lat, ribs);
    if (r < d) { d = r; *m = M_FRAME; }
    return d;
}

// ---------- hub, axle, bearing, docking port ----------
static float part_hub(float x, float y, float z, float rho, int *m) {
    float d = fmaxf(rho - AXLE_R, fabsf(x - 0.5f * (NOSE_X + TAIL_X)) - 0.5f * (NOSE_X - TAIL_X));
    *m = M_DETAIL;
    float ux = fabsf(fabsf(x) - WHEEL_X);                  // hub modules on both wheels
    float hub = fminf(sdrbox2(ux, rho, HUB_HX, HUB_R, 0.25f), sdrbox2(ux, rho, 0.18f, HUB_R + 0.18f, 0.05f));
    if (g_fine) hub = carve(hub, fabsf(ux - 0.55f) - 0.03f, 0.04f);
    float mid = sdrbox2(x, rho, 0.80f, 0.90f, 0.15f);     // bearing between the wheels
    if (g_fine) mid = carve(mid, fabsf(fabsf(x) - 0.4f) - 0.04f, 0.05f);
    hub = fminf(hub, mid);
    // docking hub ahead of the finished wheel: a tapered drum with a slot in its face
    float L = NOSE_X - WHEEL_X - HUB_HX, nx = x - WHEEL_X - HUB_HX;
    float nose = fmaxf(rho - (0.95f - 0.2f * clampf(nx / L, 0, 1)), fabsf(nx - 0.5f * L) - 0.5f * L);
    if (g_fine) nose = carve(nose, fabsf(nx - 0.35f * L) - 0.03f, 0.04f);
    float slot = fmaxf(fabsf(y) - 0.50f, fabsf(z) - 0.16f);
    nose = carve(nose, slot, 0.35f);
    hub = fminf(hub, nose);
    if (hub < d) { d = hub; *m = M_HUB; }
    if (nose <= d + 1e-4f && slot < 0 && x > NOSE_X - 0.45f) *m = M_DOCK;
    // tail stub behind the unfinished wheel
    float tx = x + WHEEL_X + HUB_HX + 0.55f;
    float tail = carve(sdrbox2(tx, rho, 0.55f, 0.62f, 0.05f), fabsf(fabsf(tx) - 0.25f) - 0.04f, 0.05f);
    if (tail < d) { d = tail; *m = M_FRAME; }
    return d;
}

// ---------- the whole station ----------
// A far bounding box stands in for its part (a safe lower bound).
static float scene(float x, float y, float z, int *m) {
    float d = 1e9f, b, pd, rho = sqrtf(y*y + z*z);
    int pm = M_HUB;
    g_want = m != NULL;
#define PART(bound, call)                                              \
    b = (bound);                                                       \
    if (b < d) {                                                       \
        if (b > 0.25f) d = b;                                          \
        else { pd = call; if (pd < d) { d = pd; if (m) *m = pm; } }    \
    }
    PART(fmaxf(fabsf(x - 0.3f) - 6.35f, rho - 1.55f),   part_hub(x, y, z, rho, &pm))
    PART(fmaxf(fabsf(x - WHEEL_X) - 1.0f, rho - 10.5f), part_wheel(1, x - WHEEL_X, y, z, rho, &pm))
    PART(fmaxf(fabsf(x + WHEEL_X) - 1.0f, rho - 10.5f), part_wheel(0, x + WHEEL_X, y, z, rho, &pm))
#undef PART
    return d;
}

// ---------- camera, lights, tracing ----------
typedef struct { float m[9]; } Mat;     // row-major rotation, object -> camera

static Mat mmul(Mat a, Mat b) {
    Mat r;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        r.m[i*3+j] = a.m[i*3]*b.m[j] + a.m[i*3+1]*b.m[3+j] + a.m[i*3+2]*b.m[6+j];
    return r;
}
static Mat rotx(float a) { float c = cosf(a), s = sinf(a); return (Mat){{1,0,0, 0,c,-s, 0,s,c}}; }
static Mat roty(float a) { float c = cosf(a), s = sinf(a); return (Mat){{c,0,s, 0,1,0, -s,0,c}}; }

static Mat R;                 // object -> camera
static float EYE[3];          // eye in object space
static float LO[3];           // sun direction in object space (shadow rays)
static float LC[3], FC[3];    // sun and fill-light directions in camera space
static float cellw;           // world units per column at the station's centre plane

static const float SUN[3]  = {1.00f, 0.97f, 0.92f};
static const float FILL[3] = {0.50f, 0.66f, 1.00f};
#define AMB 0.06f

typedef struct { int hit, mat; float t, minD, lum, rgb[3]; } Sample;

// windows are rim for edge detection: resampling every window costs more than it shows
static int mat_class(int m) { return m == M_WIN_LIT || m == M_WIN_DARK ? M_RIM : m; }

static int slab(const float *o, const float *d, float *t0, float *t1) {
    float tn = -1e9f, tf = 1e9f;
    for (int k = 0; k < 3; k++) {
        float inv = 1.0f / d[k];
        float a = (BMIN[k] - o[k]) * inv, b = (BMAX[k] - o[k]) * inv;
        if (a > b) { float tmp = a; a = b; b = tmp; }
        if (a > tn) tn = a;
        if (b < tf) tf = b;
    }
    if (tn > tf || tf < 0) return 0;
    *t0 = fmaxf(tn, 0); *t1 = tf;
    return 1;
}

static void shade(float px, float py, float pz, const float *vc, Sample *s) {
    const float e = 0.004f;
    float f0 = scene(px + e, py - e, pz - e, NULL), f1 = scene(px - e, py - e, pz + e, NULL);
    float f2 = scene(px - e, py + e, pz - e, NULL), f3 = scene(px + e, py + e, pz + e, NULL);
    float nx = f0 - f1 - f2 + f3, ny = -f0 - f1 + f2 + f3, nz = -f0 + f1 - f2 + f3;
    float nl = sqrtf(nx*nx + ny*ny + nz*nz); if (nl == 0) nl = 1;
    nx /= nl; ny /= nl; nz /= nl;
    g_tint = 1;
    scene(px, py, pz, &s->mat);
    float tint = g_tint;

    // ambient occlusion: creases, joints, the lattice and the hub's corners
    float ao = 1;
    for (int k = 1; k <= 4; k++) {
        float h = 0.08f * k;
        ao -= (h - scene(px + nx*h, py + ny*h, pz + nz*h, NULL)) * (2.2f / k);
    }
    ao = clampf(ao, 0.15f, 1);

    // soft shadow toward the sun: spokes and the near wheel shade the rest
    float sh = 1, o[3] = {px + nx*0.015f, py + ny*0.015f, pz + nz*0.015f}, t0, t1;
    if (slab(o, LO, &t0, &t1)) {
        float t = 0.05f;
        for (int i = 0; i < 36 && t < t1; i++) {
            float h = scene(o[0] + LO[0]*t, o[1] + LO[1]*t, o[2] + LO[2]*t, NULL);
            if (h < 0.001f) { sh = 0; break; }
            sh = fminf(sh, 10 * h / t);
            t += fmaxf(h, 0.05f);
        }
    }
    sh = clampf(sh, 0, 1);

    const float *m = R.m;
    float cx = m[0]*nx + m[1]*ny + m[2]*nz, cy = m[3]*nx + m[4]*ny + m[5]*nz, cz = m[6]*nx + m[7]*ny + m[8]*nz;
    float key = fmaxf(cx*LC[0] + cy*LC[1] + cz*LC[2], 0) * sh;
    float fill = fmaxf(cx*FC[0] + cy*FC[1] + cz*FC[2], 0);
    float hx = LC[0] + vc[0], hy = LC[1] + vc[1], hz = LC[2] + vc[2], hl = sqrtf(hx*hx + hy*hy + hz*hz);
    float nh = fmaxf((cx*hx + cy*hy + cz*hz) / hl, 0);
    const int mt = s->mat;
    float spec = powf(nh, MATS[mt].shin) * MATS[mt].ks * sh;
    float alb[3] = {MATS[mt].r * tint, MATS[mt].g * tint, MATS[mt].b * tint};
    float emit[3] = {MATS[mt].er, MATS[mt].eg, MATS[mt].eb};
    for (int j = 0; j < 3; j++)
        s->rgb[j] = alb[j] * (SUN[j]*key*1.15f + FILL[j]*fill*0.30f*ao + AMB*ao) + spec * SUN[j] + emit[j];
    s->lum = clampf(0.2126f*s->rgb[0] + 0.7152f*s->rgb[1] + 0.0722f*s->rgb[2], 0, 1);
}

// one ray through the point (X, Y) of the plane through the station's centre
static void trace(float X, float Y, Sample *s) {
    float il = 1 / sqrtf(X*X + Y*Y + CAM_DIST*CAM_DIST);
    float dc[3] = {X*il, Y*il, -CAM_DIST*il};
    const float *m = R.m;
    float d[3] = {m[0]*dc[0] + m[3]*dc[1] + m[6]*dc[2],
                  m[1]*dc[0] + m[4]*dc[1] + m[7]*dc[2],
                  m[2]*dc[0] + m[5]*dc[1] + m[8]*dc[2]};
    s->hit = 0; s->minD = 1e9f; s->t = 1e9f; s->mat = -1;
    float t, t1;
    if (!slab(EYE, d, &t, &t1)) return;
    for (int i = 0; i < 140 && t < t1; i++) {
        float px = EYE[0] + d[0]*t, py = EYE[1] + d[1]*t, pz = EYE[2] + d[2]*t;
        float h = scene(px, py, pz, NULL);
        if (h < s->minD) s->minD = h;
        if (h < 0.003f) {
            float vc[3] = {-dc[0], -dc[1], -dc[2]};
            s->hit = 1; s->t = t;
            shade(px, py, pz, vc, s);
            float k = clampf(1.18f - 0.024f * (t - (CAM_DIST - RIM_R)), 0.55f, 1.18f);   // depth: far parts recede
            for (int j = 0; j < 3; j++) s->rgb[j] *= k;
            s->lum = clampf(s->lum * k, 0, 1);
            return;
        }
        t += h * 0.85f;
    }
}

// ---------- frame ----------
enum { MODE_TRUE, MODE_PAL, MODE_MONO };
static int mode = MODE_TRUE, pal = 0, stars_on = 1, edges_on = 1;

static Sample *S;                      // centre ray per cell
static float *COV, *LUM, *RGB;          // per cell after antialiasing
static char *CH;                       // final character per cell
static unsigned char *OUTC;            // final colour per cell (r,g,b)
static int cap_cells;

static void ensure(int n) {
    if (n <= cap_cells) return;
    S = realloc(S, sizeof(Sample) * n);
    COV = realloc(COV, sizeof(float) * n);
    LUM = realloc(LUM, sizeof(float) * n);
    RGB = realloc(RGB, sizeof(float) * n * 3);
    CH = realloc(CH, n);
    OUTC = realloc(OUTC, (size_t)n * 3);
    cap_cells = n;
}

// silhouette character from the shape of the coverage around a partial cell
static char edge_glyph(int cols, int rows, int c, int r) {
    float W = 0, mx = 0, my = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -2; dx <= 2; dx++) {
        int cc = c + dx, rr = r + dy;
        if (cc < 0 || rr < 0 || cc >= cols || rr >= rows) continue;
        float w = COV[rr*cols + cc];
        W += w; mx += w * dx; my += w * 2.0f * dy;
    }
    if (W < 1e-3f) return 0;
    mx /= W; my /= W;
    float sxx = 0, syy = 0, sxy = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -2; dx <= 2; dx++) {
        int cc = c + dx, rr = r + dy;
        if (cc < 0 || rr < 0 || cc >= cols || rr >= rows) continue;
        float w = COV[rr*cols + cc], px = dx - mx, py = 2.0f*dy - my;
        sxx += w*px*px; syy += w*py*py; sxy += w*px*py;
    }
    float tr = sxx + syy, diff = sqrtf(sq(sxx - syy) + 4*sxy*sxy);
    if (tr <= 0 || diff < 0.35f * tr) return 0;        // no clear direction
    float ang = 0.5f * atan2f(2*sxy, sxx - syy);       // line direction, screen y down
    float a = fabsf(ang);
    if (a < 0.52f) return my > 0.25f ? '_' : '-';
    if (a > 1.34f) return '|';
    return ang > 0 ? '\\' : '/';
}

static void tone(const float *rgb, float k, unsigned char *o) {
    for (int j = 0; j < 3; j++) {
        int v = (int)(255 * powf(clampf(rgb[j] * k, 0, 1), 0.75f));
        o[j] = (unsigned char)(v & ~7);                // coarser steps, fewer escapes
    }
}

static int ramp_level(float l) {
    int level = (int)(powf(clampf(l, 0, 1), 0.65f) * 11.99f);
    return level > NL - 1 ? NL - 1 : level;
}

static void render(float T, int cols, int rows) {
    int n = cols * rows;
    ensure(n);

    float yaw = YAW0 + YAW_SWAY * sinf(T * TAU / SWAY_PERIOD), spin = T * TAU / SPIN_PERIOD;
    R = mmul(rotx(TILT), mmul(roty(yaw), rotx(spin)));
    const float *m = R.m;
    EYE[0] = m[6]*CAM_DIST; EYE[1] = m[7]*CAM_DIST; EYE[2] = m[8]*CAM_DIST;
    LO[0] = m[0]*LC[0] + m[3]*LC[1] + m[6]*LC[2];
    LO[1] = m[1]*LC[0] + m[4]*LC[1] + m[7]*LC[2];
    LO[2] = m[2]*LC[0] + m[5]*LC[1] + m[8]*LC[2];
    cellw = fmaxf(2 * VIEW_HW / cols, 2 * VIEW_HH / (rows * 2.0f));
    g_cell = cellw; g_fine = cellw <= FINE_CELL * 1.2f;
    float cw = cellw, ch = 2 * cellw, x0 = cols * 0.5f, y0 = rows * 0.5f;

    // pass 1: one ray per cell
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            trace((c - x0 + 0.5f) * cw, (y0 - r - 0.5f) * ch, &S[r*cols + c]);

    // pass 2: resample silhouettes, material edges, depth steps and near misses
    static const float OFF[4][2] = {{-0.25f,-0.25f},{0.25f,-0.25f},{-0.25f,0.25f},{0.25f,0.25f}};
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
        int i = r*cols + c;
        const Sample *s = &S[i];
        int need = !s->hit && s->minD < cellw * 0.9f;
        const int nb[4][2] = {{c-1,r},{c+1,r},{c,r-1},{c,r+1}};
        for (int k = 0; k < 4 && !need; k++) {
            int cc = nb[k][0], rr = nb[k][1];
            if (cc < 0 || rr < 0 || cc >= cols || rr >= rows) continue;
            const Sample *q = &S[rr*cols + cc];
            if (q->hit != s->hit) need = 1;
            else if (s->hit && (mat_class(q->mat) != mat_class(s->mat) || fabsf(q->t - s->t) > 0.5f)) need = 1;
        }
        int hits = s->hit, total = 1;
        float lum = s->hit ? s->lum : 0, rgb[3] = {0, 0, 0};
        if (s->hit) for (int j = 0; j < 3; j++) rgb[j] = s->rgb[j];
        if (need) {
            float X = (c - x0 + 0.5f) * cw, Y = (y0 - r - 0.5f) * ch;
            for (int k = 0; k < 4; k++) {
                Sample q;
                trace(X + OFF[k][0]*cw, Y - OFF[k][1]*ch, &q);
                total++;
                if (!q.hit) continue;
                hits++; lum += q.lum;
                for (int j = 0; j < 3; j++) rgb[j] += q.rgb[j];
            }
        }
        COV[i] = (float)hits / total;
        LUM[i] = hits ? lum / hits : 0;
        for (int j = 0; j < 3; j++) RGB[i*3 + j] = hits ? rgb[j] / hits : 0;
    }

    // pass 3: characters and colours
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
        int i = r*cols + c;
        unsigned char *k = &OUTC[i*3];
        float cov = COV[i];
        if (cov <= 0) {
            CH[i] = ' ';
            if (!stars_on) continue;
            float h = hash1((unsigned)c * 73856093u ^ (unsigned)r * 19349663u);
            if (h > 0.006f) continue;
            float b = hash1((unsigned)c * 83492791u + (unsigned)r * 2654435761u);
            b = 0.25f + 0.75f * b * b * b;
            b *= 0.8f + 0.2f * sinf(T * (1.5f + 3 * h * 100) + h * 1000);
            CH[i] = b > 0.75f ? '+' : '.';
            float tint = hash1((unsigned)i * 2246822519u);
            float srgb[3] = {b * (tint > 0.7f ? 1.0f : 0.85f), b * 0.9f, b * (tint < 0.3f ? 1.0f : 0.85f)};
            tone(srgb, 0.85f, k);
            if (mode == MODE_PAL) k[1] = (unsigned char)(b * 23);   // grey step
            k[2] |= 1;                                              // mark: star
            continue;
        }
        char g = (edges_on && cov < 0.99f) ? edge_glyph(cols, rows, c, r) : 0;
        int level = ramp_level(cov < 0.99f && !g ? LUM[i] * (0.35f + 0.65f * cov) : LUM[i]);
        CH[i] = g ? g : RAMP[level];
        tone(&RGB[i*3], g ? 0.6f + 0.4f * cov : 1.0f, k);
        k[2] &= ~1;
        if (mode == MODE_PAL) k[1] = (unsigned char)level;
    }
}

// build the frame text; returns its length. Live frames place every row with an
// absolute cursor move instead of a newline, so the terminal can never scroll.
// live: 0 = newlines between rows, 1 = cursor moves, 2 = "\r\n" after every row
static size_t emit(char *out, int cols, int rows, int live) {
    char *o = out;
    int last = -1;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i = r*cols + c;
            char chr = CH[i];
            if (chr == ' ' || mode == MODE_MONO) { *o++ = chr; continue; }
            const unsigned char *k = &OUTC[i*3];
            int code;
            if (mode == MODE_TRUE) {
                code = k[0] << 16 | k[1] << 8 | (k[2] & ~1);
                if (code != last) o += sprintf(o, "\x1b[38;2;%d;%d;%dm", k[0], k[1], k[2] & ~1);
            } else {
                code = (k[2] & 1) ? 232 + k[1] : PALETTES[pal].c[k[1]];
                if (code != last) o += sprintf(o, "\x1b[38;5;%dm", code);
            }
            last = code;
            *o++ = chr;
        }
        if (live == 2) { *o++ = '\r'; *o++ = '\n'; }
        else if (r < rows - 1) {
            if (live) o += sprintf(o, "\x1b[%d;1H", r + 2);
            else *o++ = '\n';
        }
    }
    return (size_t)(o - out);
}

// ---------- program ----------
static int find_palette(const char *name) {
    for (int i = 0; i < NPAL; i++) if (strcmp(PALETTES[i].name, name) == 0) return i;
    return -1;
}

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---------- terminal: own screen, keys without Enter, restored on exit ----------
static struct termios old_tio;
static int raw_on, alt_on;
static volatile sig_atomic_t quit;

static void on_signal(int sig) { (void)sig; quit = 1; }

static void term_restore(void) {
    if (raw_on) { tcsetattr(STDIN_FILENO, TCSANOW, &old_tio); raw_on = 0; }
    if (alt_on) { printf("\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l"); fflush(stdout); alt_on = 0; }
}

static void term_setup(void) {
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old_tio) == 0) {
        struct termios raw = old_tio;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        raw_on = 1;
    }
    printf("\x1b[?1049h\x1b[?7l\x1b[?25l\x1b[2J");   // own screen, no wrap, no cursor
    alt_on = 1;
    atexit(term_restore);
}

static void read_keys(void) {
    unsigned char kb[64];
    ssize_t n = raw_on ? read(STDIN_FILENO, kb, sizeof kb) : 0;
    for (ssize_t i = 0; i < n; i++) {
        if (kb[i] == 27 && i + 1 < n && kb[i + 1] == '[') { i += 2; continue; }   // arrow keys etc.
        if (kb[i] == 'q' || kb[i] == 'Q' || kb[i] == 27) quit = 1;
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [color|mono|--mono] [time [cols rows]] [--size WxH] [--frames N] [--fps N]\n"
                    "       [--at T] [--bench [cols rows]] [--no-stars] [--no-edges]\ncolors:", prog);
    for (int i = 0; i < NPAL; i++) fprintf(stderr, " %s", PALETTES[i].name);
    fprintf(stderr, "\n");
}

static void norm3(float *v, float x, float y, float z) {
    float l = sqrtf(x*x + y*y + z*z); v[0] = x / l; v[1] = y / l; v[2] = z / l;
}

int main(int argc, char **argv) {
    int bench = 0, nnum = 0, fix_cols = 0, fix_rows = 0, frames = -1;
    double num[3] = {0, 0, 0}, fps = 30, T = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        char *end;
        int p;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "--bench")) bench = 1;
        else if (!strcmp(a, "--no-stars")) stars_on = 0;
        else if (!strcmp(a, "--no-edges")) edges_on = 0;
        else if (!strcmp(a, "mono") || !strcmp(a, "--mono")) mode = MODE_MONO;
        else if (v && !strcmp(a, "--size")) {
            if (sscanf(v, "%dx%d", &fix_cols, &fix_rows) != 2 || fix_cols < 8 || fix_rows < 4) { usage(argv[0]); return 1; }
            i++;
        }
        else if (v && !strcmp(a, "--frames")) { frames = atoi(v); i++; }
        else if (v && !strcmp(a, "--fps")) { fps = fmax(1, atof(v)); i++; }
        else if (v && !strcmp(a, "--at")) { T = atof(v); i++; }
        else if ((p = find_palette(a)) >= 0) { mode = MODE_PAL; pal = p; }
        else {
            double v = strtod(a, &end);
            if (end == a || *end || nnum >= 3) { usage(argv[0]); return 1; }
            num[nnum++] = v;
        }
    }
    norm3(LC, -0.50f, 0.55f, 0.67f);    // sun: upper left, in front
    norm3(FC, 0.15f, -0.85f, 0.35f);    // cool fill from below

    int cols = 0, rows = 0;
    if (nnum >= 3) { cols = (int)num[1]; rows = (int)num[2]; }
    else if (fix_cols) { cols = fix_cols; rows = fix_rows; }
    if (bench) {
        if (nnum == 2) { cols = (int)num[0]; rows = (int)num[1]; }
        if (!cols) { cols = 200; rows = 60; }
        if (cols < 8 || rows < 4) { usage(argv[0]); return 1; }
        char *out = malloc((size_t)cols * rows * 24 + rows * 8 + 64);
        const int frames = 120;
        double t0 = now_sec();
        size_t bytes = 0;
        for (int f = 0; f < frames; f++) { render(f * 0.25f, cols, rows); bytes += emit(out, cols, rows, 1); }
        double dt = now_sec() - t0;
        printf("%d frames at %dx%d: %.2f ms/frame, %.0f KB/frame\n",
               frames, cols, rows, dt * 1000 / frames, bytes / 1024.0 / frames);
        free(out);
        return 0;
    }
    if (nnum >= 1) {                     // one frame and exit
        if (!cols) {
            struct winsize w;
            if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
                cols = w.ws_col; rows = w.ws_row - 1;
            } else { cols = 160; rows = 50; }
        }
        if (cols < 8 || rows < 4) { usage(argv[0]); return 1; }
        char *out = malloc((size_t)cols * rows * 24 + rows * 8 + 64);
        render((float)num[0], cols, rows);
        size_t len = emit(out, cols, rows, 0);
        fwrite(out, 1, len, stdout);
        fputs(mode == MODE_MONO ? "\n" : "\x1b[0m\n", stdout);
        free(out);
        return 0;
    }

    int live = isatty(STDOUT_FILENO);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (live) term_setup();
    double last = now_sec();
    char *out = NULL;
    cols = rows = 0;
    for (int f = 0; !quit && f != frames; f++) {
        double frame_start = now_sec(), dt = frame_start - last;
        if (dt < 0) dt = 0;
        if (dt > 0.1) dt = 0.1;
        if (!live) dt = 1 / fps;          // piped: a steady clock, whatever the render time
        last = frame_start;
        if (f > 0) T += dt;

        int nc = fix_cols, nr = fix_rows;
        if (!nc) {
            struct winsize w;
            nc = 100; nr = 45;
            if (live && ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) { nc = w.ws_col; nr = w.ws_row - 1; }
        }
        if (nr < 4) nr = 4;
        if (nc < 8) nc = 8;
        if (nc != cols || nr != rows) {
            cols = nc; rows = nr;
            out = realloc(out, (size_t)cols * rows * 24 + rows * 8 + 64);
            if (live) printf("\x1b[2J");
        }
        read_keys();
        render((float)T, cols, rows);
        char *o = out;
        if (live) o += sprintf(o, "\x1b[H");
        o += emit(o, cols, rows, live ? 1 : 2);
        if (!live && mode != MODE_MONO) o += sprintf(o, "\x1b[0m");
        fwrite(out, 1, (size_t)(o - out), stdout);
        fflush(stdout);

        double spent = now_sec() - frame_start;
        if (live && spent < 1 / fps) usleep((useconds_t)((1 / fps - spent) * 1e6));
    }
    term_restore();
    free(out);
    return 0;
}
