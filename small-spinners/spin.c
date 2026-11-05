// spin.c - rotating shaded ASCII shapes for the terminal
//
// Cycles through five scenes, 12 seconds each, each in its own color:
//   knot (purple), ribbon (orange), shell (gold), blob (white),
//   triangle solids morphing into each other (ice)
//
// build: gcc -O2 -o spin spin.c -lm
// run:   ./spin                 each scene uses its own color
//        ./spin gold            every scene in one color
//        (colors: white purple orange gold ice ember)
//        ./spin --mono          no color
//        flags: --size WxH --frames N --fps N
//        Piped into another program it prints plain frames, rows ending in \r\n.
// quit:  q, Esc or Ctrl+C
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>

#define TAU 6.28318530718
#define NL 12
#define SCENE_SECONDS 12.0
#define FADE_SECONDS 0.5
#define K2 5.0

static const char RAMP[] = ".,-~:;=!*#$@";

// ---------- colors (256-color terminal codes, dark to bright) ----------
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

// ---------- smooth shapes: parametric surfaces drawn as dense points ----------
typedef void (*surf_fn)(double u, double v, double t, double *o);

static void trefoil(double t, double *p, double *N, double *B) {
    double s1=sin(t), c1=cos(t), s2=sin(2*t), c2=cos(2*t), s3=sin(3*t), c3=cos(3*t);
    p[0]=s1+2*s2; p[1]=c1-2*c2; p[2]=-s3;
    double d[3]={c1+4*c2, -s1+4*s2, -3*c3}, dd[3]={-s1-8*s2, -c1+8*c2, 9*s3};
    double l=sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    double T[3]={d[0]/l, d[1]/l, d[2]/l};
    double k=dd[0]*T[0]+dd[1]*T[1]+dd[2]*T[2];
    for (int i=0;i<3;i++) N[i]=dd[i]-k*T[i];
    l=sqrt(N[0]*N[0]+N[1]*N[1]+N[2]*N[2]);
    for (int i=0;i<3;i++) N[i]/=l;
    B[0]=T[1]*N[2]-T[2]*N[1]; B[1]=T[2]*N[0]-T[0]*N[2]; B[2]=T[0]*N[1]-T[1]*N[0];
}

static void knot(double u, double v, double t, double *o) {
    (void)t; double p[3], N[3], B[3]; trefoil(u, p, N, B);
    double r=0.62, cv=cos(v)*r, sv=sin(v)*r;
    for (int i=0;i<3;i++) o[i]=p[i]+cv*N[i]+sv*B[i];
}

static void ribbon(double u, double v, double t, double *o) {
    (void)t; double x=0.52*cos(v), y=0.16*sin(v), a=1.5*u;
    double xr=x*cos(a)-y*sin(a), yr=x*sin(a)+y*cos(a), R=1+xr;
    o[0]=R*cos(u); o[1]=yr; o[2]=R*sin(u);
}

static void shell(double u, double v, double t, double *o) {
    (void)t; double s=exp(0.15*u), rho=0.36*s, R=0.55*s, rr=R+rho*cos(v);
    o[0]=rr*cos(u); o[1]=-0.9*s+rho*sin(v); o[2]=rr*sin(u);
}

static void blob(double u, double v, double t, double *o) {
    double sv=sin(v), x=sv*cos(u), y=cos(v), z=sv*sin(u);
    double r=1+0.2*sin(3*x+1.1*t)*sin(3*y+0.8*t+1)*sin(3*z+1.3*t+2)
             +0.09*sin(5*y+1.7*t)+0.06*sin(4*x-6*z+t);
    o[0]=x*r; o[1]=y*r; o[2]=z*r;
}

typedef struct {
    surf_fn f; int nu, nv; double u0, u1, v0, v1; int animated; double scale;
    float *P, *N; int count; double built_density;
} Surf;

static Surf surfs[] = {
    {knot,   500, 28, 0, TAU,   0, TAU,  0, 0,       NULL, NULL, 0, 0},
    {ribbon, 320, 44, 0, TAU,   0, TAU,  0, 0,       NULL, NULL, 0, 0},
    {shell,  320, 36, 0, 2*TAU, 0, TAU,  0, 0,       NULL, NULL, 0, 0},
    {blob,   120, 64, 0, TAU,   0, M_PI, 1, 1/1.36,  NULL, NULL, 0, 0},
};

static void build_surf(Surf *s, double t, double density) {
    double dens = s->animated ? fmin(density, 1.5) : density;
    int nu=(int)(s->nu*dens), nv=(int)(s->nv*dens);
    s->count = nu*nv;
    s->P = realloc(s->P, sizeof(float)*s->count*3);
    s->N = realloc(s->N, sizeof(float)*s->count*3);
    double du=(s->u1-s->u0)/nu, dv=(s->v1-s->v0)/nv, e=1e-3;
    int k=0;
    for (int i=0;i<nu;i++) {
        double u=s->u0+(i+0.5)*du;
        for (int j=0;j<nv;j++) {
            double v=s->v0+(j+0.5)*dv, p[3], a[3], b[3];
            s->f(u,v,t,p); s->f(u+e,v,t,a); s->f(u,v+e,t,b);
            double ux=a[0]-p[0], uy=a[1]-p[1], uz=a[2]-p[2];
            double vx=b[0]-p[0], vy=b[1]-p[1], vz=b[2]-p[2];
            double nx=uy*vz-uz*vy, ny=uz*vx-ux*vz, nz=ux*vy-uy*vx;
            double len=sqrt(nx*nx+ny*ny+nz*nz);
            if (len<1e-14) { nx=p[0]; ny=p[1]; nz=p[2]; len=sqrt(nx*nx+ny*ny+nz*nz); if (len==0) len=1; }
            s->P[k]=p[0]; s->P[k+1]=p[1]; s->P[k+2]=p[2];
            s->N[k]=nx/len; s->N[k+1]=ny/len; s->N[k+2]=nz/len;
            k+=3;
        }
    }
    float *P=s->P; int n=s->count*3;
    if (s->scale > 0) {
        for (int i=0;i<n;i++) P[i]*=s->scale;
    } else {   // center it and fit it inside the unit sphere
        double cx=0, cy=0, cz=0, m=0;
        for (int i=0;i<n;i+=3) { cx+=P[i]; cy+=P[i+1]; cz+=P[i+2]; }
        cx/=s->count; cy/=s->count; cz/=s->count;
        for (int i=0;i<n;i+=3) {
            double x=P[i]-cx, y=P[i+1]-cy, z=P[i+2]-cz, d=x*x+y*y+z*z;
            if (d>m) m=d;
        }
        double sc=1/sqrt(m);
        for (int i=0;i<n;i+=3) { P[i]=(P[i]-cx)*sc; P[i+1]=(P[i+1]-cy)*sc; P[i+2]=(P[i+2]-cz)*sc; }
    }
    s->built_density = density;
}

// ---------- triangle solids: drawn by ray marching ----------
typedef struct { int n; double h; double N[20][3]; } Poly;
static Poly polys[4];

static void add(Poly *p, double x, double y, double z) {
    double l=sqrt(x*x+y*y+z*z);
    p->N[p->n][0]=x/l; p->N[p->n][1]=y/l; p->N[p->n][2]=z/l;
    p->n++;
}

static void init_polys(void) {
    Poly *t=&polys[0]; t->h=1.0/3.0;                  // tetrahedron
    add(t,1,1,1); add(t,-1,-1,1); add(t,-1,1,-1); add(t,1,-1,-1);
    Poly *b=&polys[1]; b->h=1.0/sqrt(5.0);            // triangular bipyramid
    for (int k=0;k<3;k++) {
        double a=k*TAU/3+TAU/6;
        add(b,2*cos(a),1,2*sin(a)); add(b,2*cos(a),-1,2*sin(a));
    }
    Poly *o=&polys[2]; o->h=1.0/sqrt(3.0);            // octahedron
    for (int i=0;i<8;i++) add(o,(i&1)?1:-1,(i&2)?1:-1,(i&4)?1:-1);
    Poly *c=&polys[3]; c->h=0.794654;                 // icosahedron
    double ph=1.618033988749895, ip=1/ph;
    for (int i=0;i<8;i++) add(c,(i&1)?1:-1,(i&2)?1:-1,(i&4)?1:-1);
    for (int i=0;i<4;i++) {
        double s1=(i&1)?1:-1, s2=(i&2)?1:-1;
        add(c,0,s1*ip,s2*ph); add(c,s1*ip,s2*ph,0); add(c,s1*ph,0,s2*ip);
    }
}

static double sd(const Poly *p, double x, double y, double z, int *face) {
    double best=-1e9; int bi=0;
    for (int i=0;i<p->n;i++) {
        double d=p->N[i][0]*x+p->N[i][1]*y+p->N[i][2]*z-p->h;
        if (d>best) { best=d; bi=i; }
    }
    *face=bi;
    return best;
}

// ---------- shared rendering ----------
typedef struct { double m[9]; } Mat;   // row-major rotation, object -> camera

static double lx, ly, lz;              // main light (camera space)

static int shade(double qx, double qy, double qz) {
    double key =fmax(0, qx*lx+qy*ly+qz*lz);
    double fill=fmax(0, qx*0.72+qy*0.3-qz*0.62);
    int level=(int)((0.06+0.7*key+0.24*fill)*NL);
    return level>NL-1 ? NL-1 : level;
}

static void draw_surf(const Surf *s, const Mat *R, double K1, int cols, int rows,
                      float *zb, signed char *lb) {
    const double *m=R->m, cw=0.5, ch=1, cx=cols*cw/2, cy=rows*ch/2;
    for (int k=0;k<s->count*3;k+=3) {
        double x=s->P[k], y=s->P[k+1], z=s->P[k+2];
        double rx=m[0]*x+m[1]*y+m[2]*z, ry=m[3]*x+m[4]*y+m[5]*z, rz=m[6]*x+m[7]*y+m[8]*z+K2;
        double ooz=1/rz, sx=cx+K1*ooz*rx, sy=cy+K1*ooz*ry;
        if (sx<0 || sy<0) continue;
        int col=(int)(sx/cw), row=(int)(sy/ch);
        if (col>=cols || row>=rows) continue;
        int idx=row*cols+col;
        if (ooz<=zb[idx]) continue;
        double nx=m[0]*s->N[k]+m[1]*s->N[k+1]+m[2]*s->N[k+2];
        double ny=m[3]*s->N[k]+m[4]*s->N[k+1]+m[5]*s->N[k+2];
        double nz=m[6]*s->N[k]+m[7]*s->N[k+1]+m[8]*s->N[k+2];
        if (nx*rx+ny*ry+nz*rz>0) { nx=-nx; ny=-ny; nz=-nz; }
        zb[idx]=ooz; lb[idx]=(signed char)shade(nx,ny,nz);
    }
}

static void draw_triangles(double local, const Mat *R, double K1, int cols, int rows,
                           signed char *lb) {
    // 3 seconds per solid: hold 1.2 s, then morph for 1.8 s
    const double HOLD=1.2, MORPH=1.8, SEG=HOLD+MORPH, RR=1.02*1.02;
    double phs=fmod(local, SEG*4); if (phs<0) phs+=SEG*4;
    int a=(int)(phs/SEG); if (a>3) a=3;
    double mm=phs-a*SEG; mm = mm<HOLD ? 0 : (mm-HOLD)/MORPH; mm=mm*mm*(3-2*mm);
    const Poly *cur=&polys[a], *nxt=&polys[(a+1)%4];
    const double *m=R->m, cw=0.5, ch=1, cx=cols*cw/2, cy=rows*ch/2;
    double ox=-K2*m[6], oy=-K2*m[7], oz=-K2*m[8], oo=ox*ox+oy*oy+oz*oz;

    for (int r=0;r<rows;r++) for (int c=0;c<cols;c++) {
        double dx=((c+0.5)*cw-cx)/K1, dy=((r+0.5)*ch-cy)/K1;
        double il=1/sqrt(dx*dx+dy*dy+1), dz=il; dx*=il; dy*=il;
        double ex=m[0]*dx+m[3]*dy+m[6]*dz, ey=m[1]*dx+m[4]*dy+m[7]*dz, ez=m[2]*dx+m[5]*dy+m[8]*dz;
        double b=ox*ex+oy*ey+oz*ez, disc=b*b-(oo-RR);
        if (disc<0) continue;
        double sq=sqrt(disc), tt=-b-sq, tmax=-b+sq;
        int hit=0, f1=0, f2=0;
        for (int s=0;s<80;s++) {
            double px=ox+tt*ex, py=oy+tt*ey, pz=oz+tt*ez;
            double d=sd(cur,px,py,pz,&f1);
            if (mm>0) d=(1-mm)*d+mm*sd(nxt,px,py,pz,&f2);
            if (d<1e-3) { hit=1; break; }
            tt+=fmax(d*0.9, 2e-4);
            if (tt>tmax) break;
        }
        if (!hit) continue;
        double nx=cur->N[f1][0], ny=cur->N[f1][1], nz=cur->N[f1][2];
        if (mm>0) {
            nx=(1-mm)*nx+mm*nxt->N[f2][0]; ny=(1-mm)*ny+mm*nxt->N[f2][1]; nz=(1-mm)*nz+mm*nxt->N[f2][2];
            double l=sqrt(nx*nx+ny*ny+nz*nz); nx/=l; ny/=l; nz/=l;
        }
        double qx=m[0]*nx+m[1]*ny+m[2]*nz, qy=m[3]*nx+m[4]*ny+m[5]*nz, qz=m[6]*nx+m[7]*ny+m[8]*nz;
        lb[r*cols+c]=(signed char)shade(qx,qy,qz);
    }
}

// ---------- scenes ----------
typedef struct { int surf; const char *palette; } Scene;   // surf -1 = triangles
static const Scene SCENES[] = {
    {0, "purple"}, {1, "orange"}, {2, "gold"}, {3, "white"}, {-1, "ice"},
};
#define NSCENES (int)(sizeof(SCENES)/sizeof(SCENES[0]))

static int find_palette(const char *name) {
    for (int i=0;i<NPAL;i++) if (strcmp(PALETTES[i].name, name)==0) return i;
    return -1;
}

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec*1e-9;
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
    fprintf(stderr, "usage: %s [color] [--mono] [--size WxH] [--frames N] [--fps N]\ncolors:", prog);
    for (int i=0;i<NPAL;i++) fprintf(stderr, " %s", PALETTES[i].name);
    fprintf(stderr, "\n");
}

int main(int argc, char **argv) {
    int forced=-1, mono=0, fix_cols=0, fix_rows=0, frames=-1;
    double fps=30;
    for (int i=1;i<argc;i++) {
        const char *a=argv[i], *v=i+1<argc ? argv[i+1] : NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "--mono")) mono=1;
        else if (v && !strcmp(a, "--size")) {
            if (sscanf(v, "%dx%d", &fix_cols, &fix_rows)!=2 || fix_cols<8 || fix_rows<4) { usage(argv[0]); return 1; }
            i++;
        }
        else if (v && !strcmp(a, "--frames")) { frames=atoi(v); i++; }
        else if (v && !strcmp(a, "--fps")) { fps=fmax(1, atof(v)); i++; }
        else if ((forced=find_palette(a))<0) { usage(argv[0]); return 1; }
    }
    init_polys();
    { double l=sqrt(0.45*0.45+0.65*0.65+0.62*0.62); lx=-0.45/l; ly=-0.65/l; lz=-0.62/l; }
    int live=isatty(STDOUT_FILENO);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (live) term_setup();

    double A=-0.5, B=0.3, C=0, T=0, last=now_sec();
    int cols=0, rows=0;
    float *zb=NULL; signed char *lb=NULL; char *out=NULL;

    for (int f=0; !quit && f!=frames; f++) {
        double frame_start=now_sec(), dt=frame_start-last;
        if (dt<0) dt=0;
        if (dt>0.1) dt=0.1;
        if (!live) dt=1/fps;          // piped: a steady clock, whatever the render time
        if (f==0) dt=0;
        last=frame_start;
        T+=dt; A+=dt*1.05; B+=dt*1.5; C+=dt*0.45;

        int nc=fix_cols, nr=fix_rows;
        if (!nc) {
            struct winsize w;
            nc=100; nr=45;
            if (live && ioctl(STDOUT_FILENO, TIOCGWINSZ, &w)==0 && w.ws_col>0) { nc=w.ws_col; nr=w.ws_row-1; }
        }
        if (nr<4) nr=4;
        if (nc!=cols || nr!=rows) {
            cols=nc; rows=nr;
            zb=realloc(zb, sizeof(float)*cols*rows);
            lb=realloc(lb, cols*rows);
            out=realloc(out, cols*rows*12 + rows*8 + 64);
            if (live) printf("\x1b[2J");
        }
        read_keys();
        double density=fmax(1.0, fmin(2.5, sqrt((double)cols*rows)/60.0));

        int si=((int)(T/SCENE_SECONDS)) % NSCENES;
        double local=fmod(T, SCENE_SECONDS);
        // shrink out and grow in between scenes
        double g=1;
        if (local<FADE_SECONDS) g=local/FADE_SECONDS;
        else if (local>SCENE_SECONDS-FADE_SECONDS) g=(SCENE_SECONDS-local)/FADE_SECONDS;
        g=g*g*(3-2*g); if (g<0.02) g=0.02;

        double cA=cos(A), sA=sin(A), cB=cos(B), sB=sin(B), cC=cos(C), sC=sin(C);
        Mat R = {{ cC*cB, cC*sB*sA-sC*cA, cC*sB*cA+sC*sA,
                   sC*cB, sC*sB*sA+cC*cA, sC*sB*cA-cC*sA,
                   -sB,   cB*sA,          cB*cA }};
        double K1=fmin(cols*0.5, rows)*0.45*(K2-1)*g;

        memset(zb, 0, sizeof(float)*cols*rows);
        memset(lb, -1, cols*rows);
        const Scene *sc=&SCENES[si];
        if (sc->surf<0) {
            draw_triangles(local, &R, K1, cols, rows, lb);
        } else {
            Surf *s=&surfs[sc->surf];
            if (s->animated) build_surf(s, T, density);
            else if (!s->P || s->built_density!=density) build_surf(s, 0, density);
            draw_surf(s, &R, K1, cols, rows, zb, lb);
        }

        const int *pal=PALETTES[forced>=0 ? forced : find_palette(sc->palette)].c;
        char *o=out;
        if (live) o+=sprintf(o, "\x1b[H");
        int lastc=-1;
        for (int r=0;r<rows;r++) {
            for (int c=0;c<cols;c++) {
                int l=lb[r*cols+c];
                if (l<0) { *o++=' '; continue; }
                if (!mono && pal[l]!=lastc) { o+=sprintf(o, "\x1b[38;5;%dm", pal[l]); lastc=pal[l]; }
                *o++=RAMP[l];
            }
            if (!live) { *o++='\r'; *o++='\n'; }
            else if (r<rows-1) *o++='\n';
        }
        if (!live && !mono) o+=sprintf(o, "\x1b[0m");
        fwrite(out, 1, o-out, stdout);
        fflush(stdout);

        double spent=now_sec()-frame_start;
        if (live && spent<1/fps) usleep((useconds_t)((1/fps-spent)*1e6));
    }
    term_restore();
    return 0;
}
