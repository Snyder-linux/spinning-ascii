/* morph.c - a donut.c-style ASCII shape that morphs as it tumbles
 *
 * A single ray-marched surface flows through four forms - a torus, a twisted
 * rounded cube, a twisted octahedron and a rippling sphere - pausing briefly
 * on each one while it spins on two axes.
 *
 * build: cc -O2 morph.c -o morph -lm
 * run:   ./morph            (q, Esc or Ctrl+C to quit)
 *        ./morph 3.0        (print one frame at time 3.0 and exit)
 *        flags: --size WxH --frames N --fps N (--mono is accepted; it has no colour)
 *        Piped into another program it prints plain frames, rows ending in \r\n.
 *
 * It fills the terminal; shrink your terminal font for finer detail.
 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define VIEW 1.75f            /* half-height of the view in world units */
static int COLS = 100, ROWS = 50;

static const char *ramp = ".,-~:;=!*#$@";

static float mixf(float a, float b, float f) { return a + (b - a) * f; }
static float smooth(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* the four forms, each roughly 2 units across */
static float torus(float x, float y, float z) {
    float q = sqrtf(x * x + z * z) - 0.9f;
    return sqrtf(q * q + y * y) - 0.38f;
}
static float cube(float x, float y, float z) {
    float h = 0.68f, r = 0.14f;
    float qx = fabsf(x) - h, qy = fabsf(y) - h, qz = fabsf(z) - h;
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0), oz = fmaxf(qz, 0);
    return sqrtf(ox * ox + oy * oy + oz * oz) + fminf(fmaxf(qx, fmaxf(qy, qz)), 0) - r;
}
static float octa(float x, float y, float z) {
    return (fabsf(x) + fabsf(y) + fabsf(z) - 1.25f) * 0.57735f;
}
static float blob(float x, float y, float z) {
    return sqrtf(x * x + y * y + z * z) - 0.95f + 0.1f * sinf(4 * x) * sinf(4 * y) * sinf(4 * z);
}

static float shape(int i, float x, float y, float z, float t) {
    if (i == 1 || i == 2) {                 /* twist the cube and octahedron */
        float a = y * 0.9f * sinf(t * 0.5f);
        float c = cosf(a), s = sinf(a), nx = c * x - s * z, nz = s * x + c * z;
        x = nx; z = nz;
    }
    switch (i) {
        case 0:  return torus(x, y, z);
        case 1:  return cube(x, y, z);
        case 2:  return octa(x, y, z);
        default: return blob(x, y, z);
    }
}

static float g_t;                           /* current time, used by scene() */
static float scene(float x, float y, float z) {
    float phase = g_t * 0.22f;
    int i = (int)floorf(phase) % 4;
    float f = smooth(0.45f, 0.95f, phase - floorf(phase));   /* hold, then flow */
    return mixf(shape(i, x, y, z, g_t), shape((i + 1) % 4, x, y, z, g_t), f);
}

static char *screen;                        /* ROWS rows of COLS + 1 chars */
#define SCREEN(r, c) screen[(r) * (COLS + 1) + (c)]

static void resize(int cols, int rows) {
    COLS = cols; ROWS = rows;
    screen = realloc(screen, (size_t)rows * (cols + 1));
}

static void render(float t) {
    g_t = t;
    float A = t * 0.6f, B = t * 0.37f;        /* tumble on two axes, like donut.c */
    float cA = cosf(A), sA = sinf(A), cB = cosf(B), sB = sinf(B);
    /* R = Rz(B) * Rx(A); rows below are R, used as view = R * obj */
    float R[3][3] = {
        { cB, -sB * cA,  sB * sA },
        { sB,  cB * cA, -cB * sA },
        { 0,   sA,       cA      }
    };
    const float cam = 4.0f, bound = 1.7f;
    const float lx0 = -3, ly0 = 4, lz0 = 6;
    float uy = 2.0f * VIEW / ROWS, ux = uy / 2.0f;
    float dx = -R[2][0], dy = -R[2][1], dz = -R[2][2];   /* view ray (0,0,-1) in object space */

    for (int r = 0; r < ROWS; r++) {
        float Y = (ROWS / 2 - r - 0.5f) * uy;
        for (int c = 0; c < COLS; c++) {
            float X = (c - COLS / 2 + 0.5f) * ux;
            SCREEN(r, c) = ' ';
            if (X * X + Y * Y > bound * bound) continue;
            float ox = R[0][0]*X + R[1][0]*Y + R[2][0]*cam;
            float oy = R[0][1]*X + R[1][1]*Y + R[2][1]*cam;
            float oz = R[0][2]*X + R[1][2]*Y + R[2][2]*cam;
            float d = cam - bound, px = 0, py = 0, pz = 0;
            int hit = 0;
            for (int i = 0; i < 120 && d < cam + bound; i++) {
                px = ox + dx * d; py = oy + dy * d; pz = oz + dz * d;
                float s = scene(px, py, pz);
                if (s < 0.002f) { hit = 1; break; }
                d += s * 0.7f;                  /* smaller steps: morphs aren't exact distances */
            }
            if (!hit) continue;

            const float e = 0.002f;
            float nx = scene(px + e, py, pz) - scene(px - e, py, pz);
            float ny = scene(px, py + e, pz) - scene(px, py - e, pz);
            float nz = scene(px, py, pz + e) - scene(px, py, pz - e);
            float vx = R[0][0]*nx + R[0][1]*ny + R[0][2]*nz;
            float vy = R[1][0]*nx + R[1][1]*ny + R[1][2]*nz;
            float vz = R[2][0]*nx + R[2][1]*ny + R[2][2]*nz;
            float nl = sqrtf(vx*vx + vy*vy + vz*vz);
            vx /= nl; vy /= nl; vz /= nl;

            float lx = lx0 - X, ly = ly0 - Y, lz = lz0 - (cam - d);
            float ll = sqrtf(lx*lx + ly*ly + lz*lz);
            lx /= ll; ly /= ll; lz /= ll;
            float hx = lx, hy = ly, hz = lz + 1, hl = sqrtf(hx*hx + hy*hy + hz*hz);
            float diff = fmaxf(vx*lx + vy*ly + vz*lz, 0);
            float spec = powf(fmaxf((vx*hx + vy*hy + vz*hz) / hl, 0), 30);
            float lum = fminf(1, 0.12f + 0.68f * diff + 0.5f * spec);
            SCREEN(r, c) = ramp[(int)(lum * 11.99f)];
        }
        SCREEN(r, COLS) = 0;
    }
}

/* terminal: own screen, keys without Enter, restored on exit */
static struct termios old_tio;
static int raw_on, alt_on;
static volatile sig_atomic_t quit;

static void on_signal(int sig) { (void)sig; quit = 1; }

static void term_restore(void) {
    if (raw_on) { tcsetattr(STDIN_FILENO, TCSANOW, &old_tio); raw_on = 0; }
    if (alt_on) { printf("\x1b[?7h\x1b[?25h\x1b[?1049l"); fflush(stdout); alt_on = 0; }
}

static void term_setup(void) {
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old_tio) == 0) {
        struct termios raw = old_tio;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        raw_on = 1;
    }
    printf("\x1b[?1049h\x1b[?7l\x1b[?25l\x1b[2J");   /* own screen, no wrap, no cursor */
    alt_on = 1;
    atexit(term_restore);
}

static void read_keys(void) {
    unsigned char kb[64];
    ssize_t n = raw_on ? read(STDIN_FILENO, kb, sizeof kb) : 0;
    for (ssize_t i = 0; i < n; i++) {
        if (kb[i] == 27 && i + 1 < n && kb[i + 1] == '[') { i += 2; continue; }   /* arrow keys etc. */
        if (kb[i] == 'q' || kb[i] == 'Q' || kb[i] == 27) quit = 1;
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [time] [--size WxH] [--frames N] [--fps N] [--mono]\n", prog);
}

int main(int argc, char **argv) {
    int fix_cols = 0, fix_rows = 0, frames = -1, one = 0;
    float fps = 30, t0 = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        char *end;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "--mono")) continue;
        else if (v && !strcmp(a, "--size")) {
            if (sscanf(v, "%dx%d", &fix_cols, &fix_rows) != 2 || fix_cols < 8 || fix_rows < 4) { usage(argv[0]); return 1; }
            i++;
        }
        else if (v && !strcmp(a, "--frames")) { frames = atoi(v); i++; }
        else if (v && !strcmp(a, "--fps")) { fps = fmaxf(1, (float)atof(v)); i++; }
        else {
            t0 = strtof(a, &end);
            if (end == a || *end) { usage(argv[0]); return 1; }
            one = 1;
        }
    }
    if (one) {                                  /* one frame and exit */
        resize(fix_cols ? fix_cols : 100, fix_cols ? fix_rows : 50);
        render(t0);
        for (int r = 0; r < ROWS; r++) puts(&SCREEN(r, 0));
        return 0;
    }
    int live = isatty(STDOUT_FILENO);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (live) term_setup();
    float step = 0.9f / fps;                    /* 0.03 a frame at 30 fps */
    for (int f = 0; !quit && f != frames; f++) {
        int nc = fix_cols, nr = fix_rows;
        if (!nc) {
            struct winsize w;
            nc = 100; nr = 50;
            if (live && ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) { nc = w.ws_col; nr = w.ws_row; }
        }
        if (nr < 4) nr = 4;
        if (nc < 8) nc = 8;
        if (nc != COLS || nr != ROWS || !screen) {
            resize(nc, nr);
            if (live) printf("\x1b[2J");
        }
        read_keys();
        render(f * step);
        if (live) printf("\x1b[H");
        for (int r = 0; r < ROWS; r++) {
            fputs(&SCREEN(r, 0), stdout);
            if (!live) fputs("\r\n", stdout);
            else if (r < ROWS - 1) putchar('\n');
        }
        fflush(stdout);
        if (live) usleep((useconds_t)(1e6f / fps));
    }
    term_restore();
    return 0;
}
