/*
 * ascii_rot.c - turn a photo of an object into rotating ASCII art.
 *
 *   ascii_rot <image> [angle_deg] [width=80] [--spin | --turntable] [--invert]
 *
 *   angle_deg    in-plane rotation, clockwise, in degrees (default 45, or 0
 *                as the fixed tilt under --turntable)
 *   width        output width in characters (height is width/2)
 *   --spin       animate an in-plane rotation forever (Ctrl-C to stop)
 *   --turntable  animate a spin around the vertical axis (Ctrl-C to stop)
 *   --invert     dense characters for bright pixels (nicer on light terminals)
 *
 * Any format ImageMagick reads is accepted (piped through `magick ... ppm:-`).
 * The background is modelled per row from the left/right image edges, so a
 * studio backdrop that shades from wall to floor is removed cleanly.
 *
 * Build:  cc -O2 -o ascii_rot ascii_rot.c -lm      (needs ImageMagick at runtime)
 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int w, h;
    unsigned char *px;   /* RGB */
    float *shade;        /* 0..1 brightness, normalised over the subject */
    unsigned char *fg;   /* 1 = subject, 0 = background */
} Image;

static const char RAMP[] = "@%#*+=-:.";   /* dense -> sparse */
#define NRAMP ((int)sizeof RAMP - 1)

static int read_token(FILE *f) {
    int c, v = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n'); continue; }
        if (c >= '0' && c <= '9') break;
    }
    if (c == EOF) return -1;
    do v = v * 10 + (c - '0'); while ((c = fgetc(f)) >= '0' && c <= '9');
    return v;
}

static int load_image(const char *path, int maxdim, Image *im) {
    char cmd[4096];
    if (strchr(path, '\'')) return 0;   /* keep the shell quoting simple */
    snprintf(cmd, sizeof cmd,
             "magick '%s' -flatten -resize '%dx%d>' -depth 8 ppm:-",
             path, maxdim, maxdim);
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    if (fgetc(f) != 'P' || fgetc(f) != '6') { pclose(f); return 0; }
    im->w = read_token(f); im->h = read_token(f);
    if (im->w <= 0 || im->h <= 0 || read_token(f) != 255) { pclose(f); return 0; }
    size_t n = (size_t)im->w * im->h * 3;
    im->px = malloc(n);
    size_t got = fread(im->px, 1, n, f);
    pclose(f);
    return got == n;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Separate the subject from the backdrop and normalise its shading. */
static void segment(Image *im) {
    int w = im->w, h = im->h, edge = w / 40 + 1;
    im->fg = calloc((size_t)w * h, 1);
    im->shade = calloc((size_t)w * h, sizeof(float));
    float *lums = malloc((size_t)w * h * sizeof(float));
    size_t nl = 0;

    for (int y = 0; y < h; y++) {
        /* background colour at this row: mean of a strip on each side */
        double L[3] = {0}, R[3] = {0};
        for (int x = 0; x < edge; x++)
            for (int k = 0; k < 3; k++) {
                L[k] += im->px[((size_t)y * w + x) * 3 + k];
                R[k] += im->px[((size_t)y * w + (w - 1 - x)) * 3 + k];
            }
        for (int x = 0; x < w; x++) {
            double t = (double)x / (w - 1), bg[3];
            for (int k = 0; k < 3; k++) bg[k] = ((1 - t) * L[k] + t * R[k]) / edge;
            unsigned char *p = im->px + ((size_t)y * w + x) * 3;
            double pl = (p[0] + p[1] + p[2]) / 3.0, bl = (bg[0] + bg[1] + bg[2]) / 3.0;
            /* colour (chroma) difference counts fully, brightness only
               partly, so soft shadows on the backdrop stay background */
            double chroma = 0;
            for (int k = 0; k < 3; k++) {
                double d = (p[k] - pl) - (bg[k] - bl);
                chroma += d * d;
            }
            double dist = sqrt(chroma) + 0.35 * fabs(pl - bl);
            if (dist > 30) {
                size_t i = (size_t)y * w + x;
                im->fg[i] = 1;
                im->shade[i] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.0f;
                lums[nl++] = im->shade[i];
            }
        }
    }
    if (nl) {   /* stretch the 2nd..98th percentile to the full ramp */
        qsort(lums, nl, sizeof(float), cmp_float);
        float lo = lums[nl / 50], hi = lums[nl - 1 - nl / 50];
        if (hi - lo < 1e-3f) hi = lo + 1e-3f;
        for (size_t i = 0; i < (size_t)w * h; i++)
            if (im->fg[i]) {
                float s = (im->shade[i] - lo) / (hi - lo);
                im->shade[i] = s < 0 ? 0 : s > 1 ? 1 : s;
            }
    }
    free(lums);
}

/*
 * Render one frame. `deg` rotates in the image plane; `xscale` squashes the
 * picture horizontally (negative mirrors it) to fake a turn around the
 * vertical axis.
 */
static void render(const Image *im, double deg, double xscale, int invert,
                   int cols, int rows, char *buf) {
    double a = deg * M_PI / 180.0, ca = cos(a), sa = sin(a);
    double cx = im->w / 2.0, cy = im->h / 2.0;
    double diag = sqrt((double)im->w * im->w + (double)im->h * im->h);
    double sx = diag / cols, sy = diag / rows;
    char *o = buf;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            double x = (c + 0.5) * sx - diag / 2, y = (r + 0.5) * sy - diag / 2;
            double u = ca * x + sa * y, v = -sa * x + ca * y;  /* inverse rotate */
            char ch = ' ';
            if (fabs(xscale) > 1e-3) {
                int ix = (int)floor(u / xscale + cx), iy = (int)floor(v + cy);
                if (ix >= 0 && iy >= 0 && ix < im->w && iy < im->h) {
                    size_t i = (size_t)iy * im->w + ix;
                    if (im->fg[i]) {
                        float s = invert ? 1 - im->shade[i] : im->shade[i];
                        int k = (int)(s * (NRAMP - 1) + 0.5f);
                        ch = RAMP[k];
                    }
                }
            }
            *o++ = ch;
        }
        *o++ = '\n';
    }
    *o = 0;
}

static void restore_cursor(int sig) {
    static const char msg[] = "\033[?25h\n";     /* show the cursor again */
    (void)sig;
    if (write(STDOUT_FILENO, msg, sizeof msg - 1) < 0) { /* nothing to do */ }
    _exit(0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image> [angle_deg] [width=80] "
                        "[--spin|--turntable] [--invert]\n", argv[0]);
        return 1;
    }
    int spin = 0, turn = 0, invert = 0, npos = 0, cols = 80;
    double angle = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--spin")) spin = 1;
        else if (!strcmp(argv[i], "--turntable")) turn = 1;
        else if (!strcmp(argv[i], "--invert")) invert = 1;
        else if (npos++ == 0) angle = atof(argv[i]);
        else cols = atoi(argv[i]);
    }
    if (npos == 0 && !turn) angle = 45;
    if (cols < 10) cols = 10;
    int rows = cols / 2;   /* terminal cells are ~2x taller than wide */

    /* The image spans ~cols/sqrt(2) characters across (the canvas is sized
       to its diagonal), so shrinking it to `cols` pixels makes each character
       an area average of ~1.4 source pixels instead of a noisy point sample. */
    Image im;
    if (!load_image(argv[1], cols, &im)) {
        fprintf(stderr, "could not read image '%s' (needs ImageMagick `magick`)\n", argv[1]);
        return 1;
    }
    segment(&im);
    char *buf = malloc((size_t)(cols + 1) * rows + 1);

    if (!spin && !turn) {
        render(&im, angle, 1.0, invert, cols, rows, buf);
        fputs(buf, stdout);
    } else {
        signal(SIGINT, restore_cursor);
        fputs("\033[2J\033[?25l", stdout);   /* clear screen, hide cursor */
        for (double t = 0;; t += 6) {
            if (spin) render(&im, t, 1.0, invert, cols, rows, buf);
            else      render(&im, angle, cos(t * M_PI / 180.0), invert, cols, rows, buf);
            printf("\033[H%s", buf);
            fflush(stdout);
            usleep(50000);
        }
    }
    free(buf); free(im.px); free(im.fg); free(im.shade);
    return 0;
}
