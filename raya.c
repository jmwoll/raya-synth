// raya — ambient modular synth environment
// Single file, C99, raylib. Build for the web with `make`, see Makefile.
//
// Signal conventions (all cables carry one float per sample):
//   audio  : roughly -1..1
//   gate   : 0 or 1, rising edge = trigger
//   pitch  : octaves relative to C4 (0 = C4 = 261.63 Hz, 1/12 per semitone)
//   cv/mod : 0..1 unipolar
// Several cables into one input are summed.

#include "raylib.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif

#define SR           48000.0f
#define TAU          6.28318530718f
#define MAX_MODULES  48
#define MAX_IO       8
#define MAX_KNOBS    12
#define MAX_CABLES   160
#define MAX_SRC      8

static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static int   clampi(int x, int a, int b)       { return x < a ? a : x > b ? b : x; }
static float fractf(float x)                   { return x - floorf(x); }

static unsigned g_rng = 0x9e3779b9u;
static float frand(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (g_rng >> 8) / 16777216.0f;
}

static bool rising(float x, float *prev)
{
    bool r = x > 0.5f && *prev <= 0.5f;
    *prev = x;
    return r;
}

// ---------------------------------------------------------------------------
// Style tokens (see STYLE.md)
// ---------------------------------------------------------------------------

static const Color BG_TOP     = {  4,   9,   9, 255 };
static const Color BG_BOT     = {  8,  34,  29, 255 };
static const Color PANEL      = { 40,  42,  44, 255 };
static const Color PANEL_HI   = { 60,  63,  66, 255 };
static const Color PANEL_LO   = { 22,  23,  25, 255 };
static const Color INSET      = { 14,  17,  17, 255 };
static const Color WOOD       = {122,  68,  36, 255 };
static const Color WOOD_DARK  = { 74,  39,  20, 255 };
static const Color TEXT       = {226, 232, 228, 255 };
static const Color TEXT_DIM   = {128, 138, 134, 255 };
static const Color ACCENT     = {112, 228, 162, 255 };
static const Color ACCENT_DIM = { 52, 110,  82, 255 };
static const Color PHOSPHOR   = { 96, 255, 150, 255 };

static const Color CABLE_COLORS[] = {
    {214, 160,  72, 255 },  // amber
    {198, 118, 128, 255 },  // rose
    {140, 172, 118, 255 },  // sage
    {112, 142, 182, 255 },  // dusty blue
    {156, 132, 182, 255 },  // muted violet
};
#define N_CABLE_COLORS 5

static Color fade(Color c, float a) { c.a = (unsigned char)(c.a * clampf(a, 0, 1)); return c; }

static void txt(const char *s, float x, float y, float size, Color c)
{
    DrawTextEx(GetFontDefault(), s, (Vector2){ roundf(x), roundf(y) }, size, size / 5.0f, c);
}
static float txt_w(const char *s, float size)
{
    return MeasureTextEx(GetFontDefault(), s, size, size / 5.0f).x;
}
static void txt_c(const char *s, float cx, float y, float size, Color c)
{
    txt(s, cx - txt_w(s, size) / 2, y, size, c);
}

static const char *NOTE_NAMES[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

// ---------------------------------------------------------------------------
// Module framework
// ---------------------------------------------------------------------------

enum { K_KNOB, K_EXP, K_STEP, K_SEG };
enum { F_NUM, F_INT, F_HZ, F_SEC, F_BPM, F_SEMI, F_PCT, F_PROG, F_OCT };

typedef struct {
    const char  *name;
    float        min, max, def;
    int          kind;
    int          fmt;
    const char **labels;   // K_SEG
} KnobDef;

typedef struct Module Module;

typedef struct {
    const char *name;
    int         w, customH;
    int         nin, nout, nknob;
    const char *ins[MAX_IO];
    const char *outs[MAX_IO];
    KnobDef     knobs[MAX_KNOBS];
    void (*init)(Module *);
    void (*process)(Module *);
    void (*draw)(Module *, Rectangle area);
    bool (*press)(Module *, Vector2 mouse, Rectangle area);
    void (*drag)(Module *, Vector2 mouse, Rectangle area);
} ModDef;

struct Module {
    int     type;
    bool    alive;
    Vector2 pos;
    float   p[MAX_KNOBS];
    float   out[MAX_IO];
    void   *st;
    // Routing, double buffered: the UI writes one copy, the audio reads the other.
    short         src[2][MAX_IO][MAX_SRC];
    unsigned char nsrc[2][MAX_IO];
};

typedef struct { int fm, fo, tm, ti, color; } Cable;

static Module mods[MAX_MODULES];
static Cable  cables[MAX_CABLES];
static int    ncables;
static int    next_color;
static volatile int g_route;  // routing copy the audio thread should use
static int    g_cur;          // routing copy used during the current audio block

enum { M_CLOCK, M_SEQ, M_CHORD, M_OSC3, M_LFO, M_REVERB, M_OUT, M_COUNT };
static const ModDef DEFS[M_COUNT];

static float IN(Module *m, int i)
{
    float s = 0;
    int n = m->nsrc[g_cur][i];
    for (int k = 0; k < n; k++) {
        int src = m->src[g_cur][i][k];
        s += mods[src / MAX_IO].out[src % MAX_IO];
    }
    return s;
}
static bool HAS(Module *m, int i) { return m->nsrc[g_cur][i] > 0; }

static void rebuild_routing(void)
{
    int r = 1 - g_route;
    for (int i = 0; i < MAX_MODULES; i++) memset(mods[i].nsrc[r], 0, sizeof mods[i].nsrc[r]);
    for (int c = 0; c < ncables; c++) {
        Cable *k = &cables[c];
        Module *m = &mods[k->tm];
        if (m->nsrc[r][k->ti] < MAX_SRC)
            m->src[r][k->ti][m->nsrc[r][k->ti]++] = (short)(k->fm * MAX_IO + k->fo);
    }
    g_route = r;
}

// ---------------------------------------------------------------------------
// CLOCK — tempo source, pulses on 16ths, beats, bars and 4-bar phrases
// ---------------------------------------------------------------------------

typedef struct { double ph; long idx; } ClockSt;

static void clock_init(Module *m) { m->st = calloc(1, sizeof(ClockSt)); }

static void clock_process(Module *m)
{
    ClockSt *s = m->st;
    s->ph += m->p[0] * 4.0 / 60.0 / SR;
    if (s->ph >= 6400.0) s->ph -= 6400.0;
    long i = (long)s->ph;
    float hi = (s->ph - i) < 0.5 ? 1.0f : 0.0f;
    s->idx = i;
    m->out[0] = hi;
    m->out[1] = (i % 4  == 0) ? hi : 0;
    m->out[2] = (i % 16 == 0) ? hi : 0;
    m->out[3] = (i % 64 == 0) ? hi : 0;
}

static void clock_draw(Module *m, Rectangle a)
{
    ClockSt *s = m->st;
    int beat = (int)((s->idx / 4) % 4);
    for (int k = 0; k < 4; k++) {
        Vector2 c = { a.x + 12 + k * 18, a.y + a.height / 2 };
        DrawCircleV(c, 5, INSET);
        if (k == beat) {
            DrawCircleV(c, 9, fade(ACCENT, 0.15f));
            DrawCircleV(c, 4, ACCENT);
        } else DrawCircleV(c, 3, ACCENT_DIM);
    }
    txt(TextFormat("BAR %ld", (s->idx / 16) % 100 + 1), a.x + a.width - 52, a.y + a.height / 2 - 5, 10, TEXT_DIM);
}

// ---------------------------------------------------------------------------
// SEQ — 16 programmable steps (gate on/off + value per step)
// ---------------------------------------------------------------------------

typedef struct {
    bool  on[16];
    float val[16];
    int   step;
    float pclk, prst, cv;
    bool  fire;
    float iph;
    int   dragMode;   // 0 none, 1 painting values
} SeqSt;

static void seq_init(Module *m)
{
    SeqSt *s = calloc(1, sizeof(SeqSt));
    static const bool  on[16]  = { 1,0,0,1, 0,0,1,0, 0,1,0,0, 1,0,0,0 };
    static const float val[16] = { .10f,.30f,.55f,.70f, .25f,.85f,.40f,.65f, .15f,.90f,.35f,.50f, .60f,.20f,.75f,.45f };
    memcpy(s->on, on, sizeof on);
    memcpy(s->val, val, sizeof val);
    s->step = -1;
    m->st = s;
}

static void seq_process(Module *m)
{
    SeqSt *s = m->st;
    float clk;
    if (HAS(m, 0)) clk = IN(m, 0);
    else {  // free-running eighths at 72 BPM so it does something on its own
        s->iph += 72.0f * 2.0f / 60.0f / SR;
        if (s->iph >= 1) s->iph -= 1;
        clk = s->iph < 0.5f;
    }
    if (rising(IN(m, 1), &s->prst)) s->step = -1;
    if (rising(clk, &s->pclk)) {
        int len = clampi((int)roundf(m->p[0]), 1, 16);
        s->step = (s->step + 1) % len;
        s->fire = s->on[s->step] && frand() < m->p[1];
    }
    float target = s->val[s->step < 0 ? 0 : s->step];
    float g = m->p[2];
    float coef = g < 0.001f ? 1.0f : 1.0f - expf(-1.0f / (g * SR));
    s->cv += (target - s->cv) * coef;
    m->out[0] = (s->fire && clk > 0.5f) ? 1.0f : 0.0f;
    m->out[1] = s->cv;
}

#define SEQ_LED_H 12

static void seq_draw(Module *m, Rectangle a)
{
    SeqSt *s = m->st;
    int len = clampi((int)roundf(m->p[0]), 1, 16);
    float cw = a.width / 16;
    for (int i = 0; i < 16; i++) {
        float x = a.x + i * cw;
        bool active = i < len;
        Rectangle led = { x + 2, a.y, cw - 4, SEQ_LED_H };
        Rectangle bar = { x + 2, a.y + SEQ_LED_H + 6, cw - 4, a.height - SEQ_LED_H - 6 };
        DrawRectangleRec(led, INSET);
        if (s->on[i]) DrawRectangleRec((Rectangle){ led.x + 2, led.y + 2, led.width - 4, led.height - 4 }, active ? ACCENT : ACCENT_DIM);
        DrawRectangleRec(bar, INSET);
        float h = (bar.height - 4) * s->val[i];
        Color fill = active ? (s->on[i] ? (Color){ 150, 190, 170, 255 } : (Color){ 80, 96, 90, 255 }) : (Color){ 50, 56, 54, 255 };
        DrawRectangleRec((Rectangle){ bar.x + 2, bar.y + bar.height - 2 - h, bar.width - 4, h }, fill);
        if (i == s->step) {
            DrawRectangleLinesEx((Rectangle){ x + 1, a.y - 1, cw - 2, a.height + 2 }, 1, ACCENT);
        }
        if (i % 4 == 0) txt(TextFormat("%d", i + 1), x + 3, a.y + a.height + 3, 10, TEXT_DIM);
    }
}

static void seq_set_val(Module *m, Vector2 mp, Rectangle a)
{
    SeqSt *s = m->st;
    int i = clampi((int)((mp.x - a.x) / (a.width / 16)), 0, 15);
    float top = a.y + SEQ_LED_H + 8, bot = a.y + a.height - 2;
    s->val[i] = clampf((bot - mp.y) / (bot - top), 0, 1);
}

static bool seq_press(Module *m, Vector2 mp, Rectangle a)
{
    SeqSt *s = m->st;
    if (!CheckCollisionPointRec(mp, a)) return false;
    int i = clampi((int)((mp.x - a.x) / (a.width / 16)), 0, 15);
    if (mp.y < a.y + SEQ_LED_H + 3) { s->on[i] = !s->on[i]; s->dragMode = 0; }
    else { s->dragMode = 1; seq_set_val(m, mp, a); }
    return true;
}

static void seq_drag(Module *m, Vector2 mp, Rectangle a)
{
    SeqSt *s = m->st;
    if (s->dragMode == 1) seq_set_val(m, mp, a);
}

// ---------------------------------------------------------------------------
// CHORDS — 92 progressions, advanced by clock every 1x/2x/4x/8x pulses
// ---------------------------------------------------------------------------

static const char *PROGRESSIONS[] = {
    "Fmaj7 Am C G",          "C G Am F",              "Am F C G",              "C Am F G",
    "Dm7 G7 Cmaj7 Cmaj7",    "Cmaj7 Fmaj7",           "Am7 Dm7",               "Fmaj7 G Em7 Am7",
    "Dm Bb F C",             "Em C G D",              "G D Em C",              "D A Bm G",
    "Am G F E",              "Cmaj7 Em7 Fmaj7 G6",    "Ebmaj7 Bbmaj7 Fm7 Abmaj7", "Am Em F C",
    "F G Am Am",             "C Em Am F",             "Dm F C G",              "Em7 Cmaj7 G D",
    "Fmaj7 Fm6 Cmaj7 Cmaj7", "Cmaj7 Am7 Dm7 G7",      "Am7 Fmaj7 Cmaj7 G",     "Bbmaj7 Am7 Gm7 Fmaj7",
    "Dm9 Gsus4 Cmaj9 Fmaj7", "Fmaj9 Em7 Dm7 Cmaj7",   "Am Fmaj7 C/E G",        "C C/B Am G",
    "Csus2 Am7 Fmaj7 Gsus4", "Asus2 E/G# F#m7 Dmaj7", "Dmaj7 Bm7 Gmaj7 A",     "Gmaj7 F#m7 Em7 Dmaj7",
    "Emaj7 C#m7 Amaj7 B",    "Abmaj7 Fm7 Dbmaj7 Eb",  "Bbmaj7 Gm7 Ebmaj7 F",   "Am C D F",
    "Em G D A",              "Bm G D A",              "F#m D A E",             "Cm Ab Eb Bb",
    "Fm Db Ab Eb",           "Gm Eb Bb F",            "Dm C Bb A",             "Am G F G",
    "Em D C D",              "Cm Bb Ab G",            "C F C G",               "G C G D",
    "F C Dm Bb",             "C G/B Am Em F C/E Dm G", "D A/C# Bm F#m G D/F# Em A", "Cmaj7 Dm7 Em7 Fmaj7",
    "Fmaj7 Em7 Dm7 Cmaj7",   "Am7 Gmaj7 Fmaj7 Em7",   "Dm7 Em7 Fmaj7 Gsus4",   "Cadd9 Fadd9 Am7 Gsus4",
    "Gadd9 Cadd9 Em7 Dsus4", "Emadd9 Cmaj7 Gadd9 Dsus2", "Am9 Fmaj9",          "Cmaj9 Bbmaj9",
    "Ebmaj9 Dbmaj9",         "Dm9 Ebmaj7",            "Fmaj7 Bbmaj7",          "Gmaj7 Cmaj7",
    "Em9 Am9",               "Bm7 E7 Amaj7 Dmaj7",    "Am7 D7 Gmaj7 Cmaj7",    "Dm7 G7 Em7 A7",
    "Cmaj7 C7 Fmaj7 Fm6",    "Fm7 Bb7 Ebmaj7 Abmaj7", "Am Am/G Fmaj7 E7",      "Dm Dm/C Bbmaj7 A7",
    "Em Em/D Cmaj7 B7",      "C Cmaj7 C7 F",          "Am Dm E Am",            "Dsus2 Dsus4 Gsus2 Asus4",
    "Fsus2 Csus2 Gsus2 Dsus2", "Am7 Em7",             "Dmaj7 Gmaj7",           "Ebmaj7 Abmaj7",
    "Bbmaj7 Ebmaj7",         "C Ab Bb C",             "Am Fmaj7 Dm7 E7",       "F Am Bb C",
    "G Bm C Cm",             "C E Am F",              "Dbmaj7 Cm7 Bbm7 Abmaj7", "Cm7 Fm7 Abmaj7 G7",
    "Gm9 Cm9 Ebmaj7 D7sus4", "Em7 A7 Dmaj7 Bm7",      "Fmaj7 Am7 Dm7 Bbmaj7",  "C Fm6 C Fm6",
};
#define NPROG ((int)(sizeof PROGRESSIONS / sizeof PROGRESSIONS[0]))

typedef struct { char name[12]; int root, bass, n, iv[4]; } Chord;
typedef struct { int n; Chord ch[8]; } Prog;
static Prog progs[NPROG];

static const struct { const char *q; int n, iv[4]; } QUALITIES[] = {
    { "",      3, { 0, 4, 7 } },      { "m",     3, { 0, 3, 7 } },
    { "7",     4, { 0, 4, 7, 10 } },  { "maj7",  4, { 0, 4, 7, 11 } },
    { "m7",    4, { 0, 3, 7, 10 } },  { "m9",    4, { 0, 3, 10, 14 } },
    { "maj9",  4, { 0, 4, 11, 14 } }, { "9",     4, { 0, 4, 10, 14 } },
    { "add9",  4, { 0, 4, 7, 14 } },  { "madd9", 4, { 0, 3, 7, 14 } },
    { "sus2",  3, { 0, 2, 7 } },      { "sus4",  3, { 0, 5, 7 } },
    { "7sus4", 4, { 0, 5, 7, 10 } },  { "6",     4, { 0, 4, 7, 9 } },
    { "m6",    4, { 0, 3, 7, 9 } },   { "dim",   3, { 0, 3, 6 } },
    { "m7b5",  4, { 0, 3, 6, 10 } },  { "aug",   3, { 0, 4, 8 } },
};

static int parse_note(const char **s)
{
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };  // A..G
    int n = base[(**s) - 'A'];
    (*s)++;
    if (**s == '#') { n++; (*s)++; }
    else if (**s == 'b') { n--; (*s)++; }
    return (n + 12) % 12;
}

static void parse_progressions(void)
{
    for (int p = 0; p < NPROG; p++) {
        char buf[128];
        strncpy(buf, PROGRESSIONS[p], sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
        Prog *pr = &progs[p];
        pr->n = 0;
        for (char *tok = strtok(buf, " "); tok && pr->n < 8; tok = strtok(NULL, " ")) {
            Chord *c = &pr->ch[pr->n++];
            strncpy(c->name, tok, sizeof c->name - 1);
            const char *s = tok;
            c->root = parse_note(&s);
            c->bass = c->root;
            char q[8] = { 0 };
            int k = 0;
            while (*s && *s != '/' && k < 7) q[k++] = *s++;
            if (*s == '/') { s++; c->bass = parse_note(&s); }
            c->n = 3; c->iv[0] = 0; c->iv[1] = 4; c->iv[2] = 7;
            for (size_t i = 0; i < sizeof QUALITIES / sizeof QUALITIES[0]; i++)
                if (strcmp(q, QUALITIES[i].q) == 0) {
                    c->n = QUALITIES[i].n;
                    memcpy(c->iv, QUALITIES[i].iv, sizeof c->iv);
                }
        }
    }
}

static const char *RATE_LABELS[] = { "1X", "2X", "4X", "8X" };

typedef struct {
    int   idx, cnt, lastProg, lastTrans, lastIdx;
    float pclk, prst;
    float t;
    int   tones[4], nt;
    float bass;
} ChordSt;

static void chord_init(Module *m)
{
    ChordSt *s = calloc(1, sizeof(ChordSt));
    s->lastProg = -1;
    m->st = s;
}

static void chord_voice(ChordSt *s, const Chord *c, int trans)
{
    int t[4];
    for (int k = 0; k < c->n; k++) {
        int nn = c->root + c->iv[k] + trans;
        while (nn < 53) nn += 12;      // keep voices in F3..E4: smooth voice leading
        while (nn >= 65) nn -= 12;
        t[k] = nn;
    }
    for (int i = 0; i < c->n; i++)
        for (int j = i + 1; j < c->n; j++)
            if (t[j] < t[i]) { int x = t[i]; t[i] = t[j]; t[j] = x; }
    memcpy(s->tones, t, sizeof t);
    s->nt = c->n;
    int b = c->bass + trans;
    while (b < 36) b += 12;
    while (b >= 48) b -= 12;
    s->bass = (float)b;
}

static void chord_process(Module *m)
{
    ChordSt *s = m->st;
    int prog  = clampi((int)roundf(m->p[0]), 0, NPROG - 1);
    int rate  = 1 << clampi((int)roundf(m->p[1]), 0, 3);
    int trans = (int)roundf(m->p[2]);
    float oct = roundf(m->p[3]);
    const Prog *pr = &progs[prog];

    if (prog != s->lastProg) { s->idx = 0; s->cnt = 0; s->t = 0; }
    if (rising(IN(m, 2), &s->prst)) { s->idx = 0; s->cnt = 0; }
    if (HAS(m, 0)) {
        if (rising(IN(m, 0), &s->pclk) && ++s->cnt >= rate) { s->cnt = 0; s->idx = (s->idx + 1) % pr->n; }
    } else {  // free-running: one chord every rate*4 seconds
        s->t += 1.0f / SR;
        if (s->t >= rate * 4.0f) { s->t = 0; s->idx = (s->idx + 1) % pr->n; }
    }
    if (s->idx >= pr->n) s->idx = 0;
    if (prog != s->lastProg || trans != s->lastTrans || s->idx != s->lastIdx) {
        chord_voice(s, &pr->ch[s->idx], trans);
        s->lastProg = prog; s->lastTrans = trans; s->lastIdx = s->idx;
    }
    for (int k = 0; k < 4; k++) {
        int note = k < s->nt ? s->tones[k] : s->tones[k - s->nt] + 12;
        m->out[k] = (note - 60) / 12.0f + oct;
    }
    m->out[4] = (s->bass - 60) / 12.0f + oct;
    int ai = clampi((int)(clampf(IN(m, 1), 0, 0.999f) * s->nt * 2), 0, s->nt * 2 - 1);
    int an = s->tones[ai % s->nt] + 12 * (ai / s->nt) + 12;
    m->out[5] = (an - 60) / 12.0f + oct;
}

static void chord_draw(Module *m, Rectangle a)
{
    ChordSt *s = m->st;
    int prog = clampi((int)roundf(m->p[0]), 0, NPROG - 1);
    const Prog *pr = &progs[prog];
    DrawRectangleRec(a, INSET);
    txt(TextFormat("PROGRESSION %02d / %d", prog + 1, NPROG), a.x + 8, a.y + 7, 10, TEXT_DIM);
    float x = a.x + 8, y = a.y + 26;
    for (int i = 0; i < pr->n; i++) {
        float size = 20;
        float w = txt_w(pr->ch[i].name, size);
        if (x + w > a.x + a.width - 8) { x = a.x + 8; y += 22; size = 20; }
        bool cur = i == s->idx;
        if (cur) DrawRectangleRec((Rectangle){ x - 4, y - 3, w + 8, size + 4 }, fade(ACCENT, 0.12f));
        txt(pr->ch[i].name, x, y, size, cur ? ACCENT : TEXT_DIM);
        x += w + 16;
    }
}

// ---------------------------------------------------------------------------
// 3X OSC — three detuned oscillator pairs, filter, swell envelope, stereo
// ---------------------------------------------------------------------------

typedef struct {
    float ph[6];
    float pitch[3];
    float drift[3], driftT[3];
    float env, pgate;
    int   stage;      // 0 idle, 1 attack, 2 sustain (drone), 3 release
    float ic1[2], ic2[2];
    float breath;
    bool  started;
} Osc3St;

static void osc3_init(Module *m) { m->st = calloc(1, sizeof(Osc3St)); }

static float polyblep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0;
}

static float osc_wave(float t, float dt, float w)
{
    float s   = sinf(TAU * t);
    float tri = 1.0f - 4.0f * fabsf(t - 0.5f);
    if (w < 0.5f) return s + (tri - s) * (w * 2);
    float saw = 2.0f * t - 1.0f - polyblep(t, dt);
    return tri + (saw - tri) * ((w - 0.5f) * 2);
}

static float svf_lp(float in, float g, float k, float *ic1, float *ic2)
{
    float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float v3 = in - *ic2;
    float v1 = a1 * *ic1 + a2 * v3;
    float v2 = *ic2 + a2 * *ic1 + a3 * v3;
    *ic1 = 2 * v1 - *ic1;
    *ic2 = 2 * v2 - *ic2;
    return v2;
}

static void osc3_process(Module *m)
{
    Osc3St *s = m->st;
    float *p = m->p;
    float t1 = HAS(m, 0) ? IN(m, 0) : -1.0f;              // C3 on its own
    float t2 = HAS(m, 1) ? IN(m, 1) : t1 + 7.0f / 12.0f;  // a fifth above
    float t3 = HAS(m, 2) ? IN(m, 2) : t1 + 1.0f;          // an octave above
    float tg[3] = { t1, t2, t3 };
    float gc = p[7] < 0.001f ? 1.0f : 1.0f - expf(-1.0f / (p[7] * 0.3f * SR));
    for (int k = 0; k < 3; k++) {
        if (!s->started) s->pitch[k] = tg[k];
        s->pitch[k] += (tg[k] - s->pitch[k]) * gc;
        if (frand() < 1.0f / SR) s->driftT[k] = (frand() - 0.5f) * 0.012f;
        s->drift[k] += (s->driftT[k] - s->drift[k]) * 0.00003f;
    }
    s->started = true;

    // envelope: triggered swell, or a drone when GATE is unpatched
    bool gated = HAS(m, 3);
    if (gated) { if (rising(IN(m, 3), &s->pgate)) s->stage = 1; }
    else if (s->stage != 2) s->stage = 1;
    if (s->stage == 1) {
        s->env += 1.0f / (p[5] * SR);
        if (s->env >= 1) { s->env = 1; s->stage = gated ? 3 : 2; }
    } else if (s->stage == 3) {
        s->env *= expf(-4.6f / (p[6] * SR));
    }

    s->breath += 0.05f / SR;
    if (s->breath >= 1) s->breath -= 1;
    float wave = clampf(p[1] + (HAS(m, 5) ? IN(m, 5) * p[8] : 0), 0, 1);
    float cut = log2f(p[3]) + 0.4f * sinf(TAU * s->breath) + IN(m, 4) * p[8] * 4.0f + s->env * 0.8f;
    float fc = clampf(exp2f(cut), 20.0f, SR * 0.42f);
    float g = tanf(3.14159265f * fc / SR);
    float kres = 2.0f - 2.0f * p[4];

    float oct = roundf(p[0]);
    float L = 0, R = 0;
    for (int k = 0; k < 3; k++) {
        float f = 261.6256f * exp2f(s->pitch[k] + oct + s->drift[k]);
        for (int v = 0; v < 2; v++) {
            float det = (v ? 1.0f : -1.0f) * p[2] * 0.012f * (1.0f + 0.3f * k);
            float dt = clampf(f * exp2f(det) / SR, 0, 0.45f);
            float *ph = &s->ph[k * 2 + v];
            *ph += dt;
            if (*ph >= 1) *ph -= 1;
            float x = osc_wave(*ph, dt, wave);
            float pan = (k - 1) * 0.35f + (v ? 0.25f : -0.25f);   // -0.6 .. 0.6
            L += x * (0.5f - pan * 0.5f);
            R += x * (0.5f + pan * 0.5f);
        }
    }
    float amp = s->env * p[9] * 0.28f;
    m->out[0] = svf_lp(L, g, kres, &s->ic1[0], &s->ic2[0]) * amp;
    m->out[1] = svf_lp(R, g, kres, &s->ic1[1], &s->ic2[1]) * amp;
}

static void osc3_draw(Module *m, Rectangle a)
{
    Osc3St *s = m->st;
    float oct = roundf(m->p[0]);
    for (int k = 0; k < 3; k++) {
        int midi = (int)roundf(60 + 12 * (s->pitch[k] + oct));
        float x = a.x + k * (a.width / 3);
        txt(TextFormat("OSC %d", k + 1), x, a.y + 6, 10, TEXT_DIM);
        txt(TextFormat("%s%d", NOTE_NAMES[((midi % 12) + 12) % 12], midi / 12 - 1), x + 44, a.y + 2, 20, TEXT);
    }
    DrawRectangleRec((Rectangle){ a.x, a.y + a.height - 4, a.width * clampf(s->env, 0, 1), 2 }, ACCENT);
}

// ---------------------------------------------------------------------------
// LFO — slow modulation: sine, triangle, smooth drift, sample & hold
// ---------------------------------------------------------------------------

typedef struct { float ph, drift, driftT, sh; } LfoSt;

static void lfo_init(Module *m)
{
    LfoSt *s = calloc(1, sizeof(LfoSt));
    s->ph = frand(); s->drift = s->driftT = s->sh = frand();
    m->st = s;
}

static void lfo_process(Module *m)
{
    LfoSt *s = m->st;
    float rate = m->p[0], depth = m->p[1];
    s->ph += rate / SR;
    if (s->ph >= 1) { s->ph -= 1; s->sh = frand(); s->driftT = frand(); }
    s->drift += (s->driftT - s->drift) * (rate * 4.0f / SR);
    float x[4] = {
        0.5f + 0.5f * sinf(TAU * s->ph),
        s->ph < 0.5f ? 2 * s->ph : 2 - 2 * s->ph,
        s->drift,
        s->sh,
    };
    for (int i = 0; i < 4; i++) m->out[i] = 0.5f + (x[i] - 0.5f) * depth;
}

static void lfo_draw(Module *m, Rectangle a)
{
    LfoSt *s = m->st;
    DrawRectangleRec(a, INSET);
    Vector2 prev = { 0 };
    for (int i = 0; i <= 64; i++) {
        float t = i / 64.0f;
        Vector2 p = { a.x + 4 + t * (a.width - 8), a.y + a.height / 2 - sinf(TAU * t) * (a.height / 2 - 6) };
        if (i) DrawLineEx(prev, p, 1, ACCENT_DIM);
        prev = p;
    }
    Vector2 dot = { a.x + 4 + s->ph * (a.width - 8), a.y + a.height / 2 - sinf(TAU * s->ph) * (a.height / 2 - 6) };
    DrawCircleV(dot, 7, fade(PHOSPHOR, 0.18f));
    DrawCircleV(dot, 3, PHOSPHOR);
    DrawCircleV((Vector2){ a.x + a.width - 8, a.y + a.height - 6 - m->out[2] * (a.height - 12) }, 2, CABLE_COLORS[0]);
}

// ---------------------------------------------------------------------------
// REVERB — 8-line feedback delay network with gentle modulation
// ---------------------------------------------------------------------------

#define RV_N    8
#define RV_LEN  16384
#define RV_MASK (RV_LEN - 1)

typedef struct {
    float buf[RV_N][RV_LEN];
    float lp[RV_N];
    float gain[RV_N];
    int   w;
    float mph;
    float cSize, cDecay;
} RevSt;

static const float RV_BASE[RV_N] = { 2473, 2767, 3217, 3557, 4013, 4637, 5167, 5897 };

static void rev_init(Module *m) { m->st = calloc(1, sizeof(RevSt)); }

static void rev_process(Module *m)
{
    RevSt *s = m->st;
    float size = m->p[0], decay = m->p[1], damp = m->p[2], mix = m->p[3];
    if (size != s->cSize || decay != s->cDecay) {
        for (int i = 0; i < RV_N; i++) s->gain[i] = powf(10.0f, -3.0f * RV_BASE[i] * size / (decay * SR));
        s->cSize = size; s->cDecay = decay;
    }
    float inL = IN(m, 0);
    float inR = HAS(m, 1) ? IN(m, 1) : inL;
    if (!HAS(m, 0)) inL = inR;
    s->mph += TAU * 0.27f / SR;
    if (s->mph > TAU) s->mph -= TAU;

    float x[RV_N];
    float lpc = 1.0f - damp * 0.85f;
    for (int i = 0; i < RV_N; i++) {
        float d = RV_BASE[i] * size + 7.0f * sinf(s->mph + i * 0.79f);
        float rp = (float)s->w - d;
        while (rp < 0) rp += RV_LEN;
        int i0 = (int)rp;
        float fr = rp - i0;
        float v = s->buf[i][i0 & RV_MASK] * (1 - fr) + s->buf[i][(i0 + 1) & RV_MASK] * fr;
        s->lp[i] += (v - s->lp[i]) * lpc;
        x[i] = s->lp[i];
    }
    float outL = (x[0] + x[2] + x[4] + x[6]) * 0.35f;
    float outR = (x[1] + x[3] + x[5] + x[7]) * 0.35f;
    for (int h = 1; h < RV_N; h <<= 1)            // Hadamard mixing
        for (int i = 0; i < RV_N; i += h * 2)
            for (int j = i; j < i + h; j++) { float a = x[j], b = x[j + h]; x[j] = a + b; x[j + h] = a - b; }
    for (int i = 0; i < RV_N; i++)
        s->buf[i][s->w] = x[i] * 0.35355339f * s->gain[i] + ((i & 1) ? inR : inL) * 0.5f;
    s->w = (s->w + 1) & RV_MASK;
    m->out[0] = inL * (1 - mix) + outL * mix;
    m->out[1] = inR * (1 - mix) + outR * mix;
}

// ---------------------------------------------------------------------------
// OUTPUT — everything ends here; soft limiter + phosphor scope
// ---------------------------------------------------------------------------

#define SCOPE_N 256

typedef struct { float l, r, peakL, peakR; float scope[SCOPE_N]; int sp, dec; } OutSt;

static void out_init(Module *m) { m->st = calloc(1, sizeof(OutSt)); }

static void out_process(Module *m)
{
    OutSt *s = m->st;
    float L = IN(m, 0), R = IN(m, 1);
    if (!HAS(m, 1)) R = L;
    if (!HAS(m, 0)) L = R;
    float v = m->p[0];
    s->l = tanhf(L * v);
    s->r = tanhf(R * v);
    s->peakL = fmaxf(fabsf(s->l), s->peakL * 0.99995f);
    s->peakR = fmaxf(fabsf(s->r), s->peakR * 0.99995f);
    if (++s->dec >= 4) { s->dec = 0; s->scope[s->sp] = (s->l + s->r) * 0.5f; s->sp = (s->sp + 1) % SCOPE_N; }
}

static void out_draw(Module *m, Rectangle a)
{
    OutSt *s = m->st;
    Rectangle sc = { a.x, a.y, a.width - 22, a.height };
    DrawRectangleRec(sc, INSET);
    for (int g = 1; g < 4; g++)
        DrawLineEx((Vector2){ sc.x + g * sc.width / 4, sc.y }, (Vector2){ sc.x + g * sc.width / 4, sc.y + sc.height }, 1, fade(ACCENT_DIM, 0.3f));
    DrawLineEx((Vector2){ sc.x, sc.y + sc.height / 2 }, (Vector2){ sc.x + sc.width, sc.y + sc.height / 2 }, 1, fade(ACCENT_DIM, 0.4f));
    Vector2 prev = { 0 };
    for (int i = 0; i < SCOPE_N; i++) {
        float v = clampf(s->scope[(s->sp + i) % SCOPE_N] * 2.0f, -1, 1);
        Vector2 p = { sc.x + 2 + i * (sc.width - 4) / (SCOPE_N - 1), sc.y + sc.height / 2 - v * (sc.height / 2 - 4) };
        if (i) {
            DrawLineEx(prev, p, 4, fade(PHOSPHOR, 0.12f));
            DrawLineEx(prev, p, 1.2f, PHOSPHOR);
        }
        prev = p;
    }
    float pk[2] = { s->peakL, s->peakR };
    for (int c = 0; c < 2; c++) {
        Rectangle bar = { a.x + a.width - 18 + c * 10, a.y, 7, a.height };
        DrawRectangleRec(bar, INSET);
        float h = clampf(pk[c], 0, 1) * a.height;
        DrawRectangleRec((Rectangle){ bar.x + 1, bar.y + bar.height - h, bar.width - 2, h }, pk[c] > 0.9f ? CABLE_COLORS[1] : ACCENT);
    }
}

// ---------------------------------------------------------------------------
// Module registry
// ---------------------------------------------------------------------------

static const ModDef DEFS[M_COUNT] = {
    [M_CLOCK] = { "CLOCK", 232, 22, 0, 4, 1, {0}, { "1/16", "BEAT", "BAR", "4 BAR" },
        { { "BPM", 30, 180, 72, K_STEP, F_BPM } },
        clock_init, clock_process, clock_draw, NULL, NULL },
    [M_SEQ] = { "SEQUENCER", 388, 112, 2, 2, 3, { "CLOCK", "RESET" }, { "GATE", "CV" },
        { { "LENGTH", 1, 16, 16, K_STEP, F_INT }, { "CHANCE", 0, 1, 0.8f, K_KNOB, F_PCT }, { "GLIDE", 0, 1, 0.15f, K_KNOB, F_SEC } },
        seq_init, seq_process, seq_draw, seq_press, seq_drag },
    [M_CHORD] = { "CHORDS", 372, 72, 3, 6, 4, { "CLOCK", "ARP", "RESET" }, { "N1", "N2", "N3", "N4", "BASS", "ARP" },
        { { "PROG", 0, NPROG - 1, 0, K_STEP, F_PROG }, { "CHORD LEN", 0, 3, 2, K_SEG, F_INT, RATE_LABELS },
          { "TRANSPOSE", -12, 12, 0, K_STEP, F_SEMI }, { "OCTAVE", -2, 2, 0, K_STEP, F_OCT } },
        chord_init, chord_process, chord_draw, NULL, NULL },
    [M_OSC3] = { "3X OSC", 412, 26, 6, 2, 10, { "P1", "P2", "P3", "GATE", "CUTOFF", "WAVE" }, { "L", "R" },
        { { "OCTAVE", -2, 2, 0, K_STEP, F_OCT },      { "WAVE", 0, 1, 0.35f, K_KNOB, F_PCT },
          { "DETUNE", 0, 1, 0.35f, K_KNOB, F_PCT },   { "CUTOFF", 40, 12000, 900, K_EXP, F_HZ },
          { "RESO", 0, 0.9f, 0.2f, K_KNOB, F_PCT },   { "ATTACK", 0.01f, 12, 2.0f, K_EXP, F_SEC },
          { "RELEASE", 0.05f, 20, 6.0f, K_EXP, F_SEC }, { "GLIDE", 0, 2, 0.4f, K_KNOB, F_SEC },
          { "MOD AMT", 0, 1, 0.5f, K_KNOB, F_PCT },   { "LEVEL", 0, 1, 0.7f, K_KNOB, F_PCT } },
        osc3_init, osc3_process, osc3_draw, NULL, NULL },
    [M_LFO] = { "LFO", 232, 44, 0, 4, 2, {0}, { "SINE", "TRI", "DRIFT", "S+H" },
        { { "RATE", 0.005f, 4, 0.08f, K_EXP, F_HZ }, { "DEPTH", 0, 1, 1, K_KNOB, F_PCT } },
        lfo_init, lfo_process, lfo_draw, NULL, NULL },
    [M_REVERB] = { "REVERB", 236, 0, 2, 2, 4, { "IN L", "IN R" }, { "L", "R" },
        { { "SIZE", 0.25f, 1.9f, 1.1f, K_KNOB, F_PCT }, { "DECAY", 0.5f, 40, 12, K_EXP, F_SEC },
          { "DAMP", 0, 1, 0.45f, K_KNOB, F_PCT },      { "MIX", 0, 1, 0.5f, K_KNOB, F_PCT } },
        rev_init, rev_process, NULL, NULL, NULL },
    [M_OUT] = { "OUTPUT", 236, 76, 2, 0, 1, { "L", "R" }, {0},
        { { "VOLUME", 0, 1.5f, 0.8f, K_KNOB, F_PCT } },
        out_init, out_process, out_draw, NULL, NULL },
};

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

static void audio_callback(void *buffer, unsigned int frames)
{
    float *o = buffer;
    g_cur = g_route;
    for (unsigned int f = 0; f < frames; f++) {
        for (int i = 0; i < MAX_MODULES; i++)
            if (mods[i].alive) DEFS[mods[i].type].process(&mods[i]);
        float L = 0, R = 0;
        for (int i = 0; i < MAX_MODULES; i++)
            if (mods[i].alive && mods[i].type == M_OUT) {
                OutSt *s = mods[i].st;
                L += s->l; R += s->r;
            }
        if (!isfinite(L)) L = 0;
        if (!isfinite(R)) R = 0;
        o[2 * f]     = clampf(L, -1, 1);
        o[2 * f + 1] = clampf(R, -1, 1);
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

#define HEADER_H    28
#define CHEEK       9
#define PAD         10
#define KNOB_CELL_W 62
#define KNOB_CELL_H 62
#define PORT_CELL_W 46
#define PORT_CELL_H 48

static Vector2 pan = { 0, 0 };

static float inner_w(const ModDef *d) { return d->w - 2 * CHEEK - 2 * PAD; }
static int   knob_cols(const ModDef *d) { int c = (int)(inner_w(d) / KNOB_CELL_W); return c < 1 ? 1 : c; }
static int   port_cols(const ModDef *d) { int c = (int)(inner_w(d) / PORT_CELL_W); return c < 1 ? 1 : c; }
static int   knob_span(const KnobDef *k) { return k->kind == K_SEG ? 2 : 1; }

static int knob_rows(const ModDef *d)
{
    int cols = knob_cols(d), col = 0, row = 0;
    for (int k = 0; k < d->nknob; k++) {
        int sp = knob_span(&d->knobs[k]);
        if (col + sp > cols) { col = 0; row++; }
        col += sp;
    }
    return d->nknob ? row + 1 : 0;
}
static int port_rows(const ModDef *d) { int n = d->nin + d->nout; return n ? (n + port_cols(d) - 1) / port_cols(d) : 0; }

static float custom_top(const Module *m) { return m->pos.y + pan.y + HEADER_H + 4; }
static float knobs_top(const Module *m)
{
    const ModDef *d = &DEFS[m->type];
    return custom_top(m) + (d->customH ? d->customH + PAD + (m->type == M_SEQ ? 10 : 0) : 0);
}
static float ports_top(const Module *m) { return knobs_top(m) + knob_rows(&DEFS[m->type]) * KNOB_CELL_H; }

static Rectangle module_rect(const Module *m)
{
    const ModDef *d = &DEFS[m->type];
    float h = ports_top(m) - (m->pos.y + pan.y) + port_rows(d) * PORT_CELL_H + 4;
    return (Rectangle){ m->pos.x + pan.x, m->pos.y + pan.y, (float)d->w, h };
}

static Rectangle custom_rect(const Module *m)
{
    const ModDef *d = &DEFS[m->type];
    return (Rectangle){ m->pos.x + pan.x + CHEEK + PAD, custom_top(m), inner_w(d), (float)d->customH };
}

static Rectangle knob_cell(const Module *m, int k)
{
    const ModDef *d = &DEFS[m->type];
    int cols = knob_cols(d), col = 0, row = 0;
    float cw = inner_w(d) / cols;
    for (int i = 0; i <= k; i++) {
        int sp = knob_span(&d->knobs[i]);
        if (col + sp > cols) { col = 0; row++; }
        if (i == k) return (Rectangle){ m->pos.x + pan.x + CHEEK + PAD + col * cw, knobs_top(m) + row * KNOB_CELL_H, cw * sp, KNOB_CELL_H };
        col += sp;
    }
    return (Rectangle){ 0 };
}

static Vector2 port_pos(const Module *m, bool isOut, int i)
{
    const ModDef *d = &DEFS[m->type];
    int cols = port_cols(d);
    int idx = isOut ? d->nin + i : i;
    float cw = inner_w(d) / cols;
    return (Vector2){ m->pos.x + pan.x + CHEEK + PAD + (idx % cols) * cw + cw / 2, ports_top(m) + (idx / cols) * PORT_CELL_H + 18 };
}

static Vector2 knob_center(Rectangle c) { return (Vector2){ c.x + c.width / 2, c.y + 20 }; }

// ---------------------------------------------------------------------------
// Knob value mapping
// ---------------------------------------------------------------------------

static float knob_to_t(const KnobDef *k, float v)
{
    if (k->kind == K_EXP) return logf(v / k->min) / logf(k->max / k->min);
    return (v - k->min) / (k->max - k->min);
}
static float knob_from_t(const KnobDef *k, float t)
{
    t = clampf(t, 0, 1);
    if (k->kind == K_EXP) return k->min * powf(k->max / k->min, t);
    float v = k->min + t * (k->max - k->min);
    return (k->kind == K_STEP || k->kind == K_SEG) ? roundf(v) : v;
}

static const char *knob_text(const KnobDef *k, float v)
{
    switch (k->fmt) {
    case F_INT:  return TextFormat("%d", (int)roundf(v));
    case F_HZ:   return v < 1 ? TextFormat("%.3f HZ", v) : v < 1000 ? TextFormat("%.0f HZ", v) : TextFormat("%.1f KHZ", v / 1000);
    case F_SEC:  return v < 1 ? TextFormat("%.0f MS", v * 1000) : TextFormat("%.1f S", v);
    case F_BPM:  return TextFormat("%.0f BPM", v);
    case F_SEMI: return TextFormat("%+d ST", (int)roundf(v));
    case F_OCT:  return TextFormat("%+d OCT", (int)roundf(v));
    case F_PCT:  return TextFormat("%.0f%%", knob_to_t(k, v) * 100);
    case F_PROG: return TextFormat("#%02d", (int)roundf(v) + 1);
    default:     return TextFormat("%.2f", v);
    }
}

// ---------------------------------------------------------------------------
// Module + cable management
// ---------------------------------------------------------------------------

static int add_module(int type, float x, float y)
{
    for (int i = 0; i < MAX_MODULES; i++) {
        Module *m = &mods[i];
        if (m->alive) continue;
        free(m->st);
        memset(m, 0, sizeof *m);
        m->type = type;
        m->pos = (Vector2){ x, y };
        const ModDef *d = &DEFS[type];
        for (int k = 0; k < d->nknob; k++) m->p[k] = d->knobs[k].def;
        d->init(m);
        m->alive = true;
        return i;
    }
    return -1;
}

static void remove_cable(int c)
{
    cables[c] = cables[--ncables];
}

static void remove_module(int i)
{
    mods[i].alive = false;
    for (int c = ncables - 1; c >= 0; c--)
        if (cables[c].fm == i || cables[c].tm == i) remove_cable(c);
    rebuild_routing();
}

static void add_cable(int fm, int fo, int tm, int ti, int color)
{
    for (int c = 0; c < ncables; c++)
        if (cables[c].fm == fm && cables[c].fo == fo && cables[c].tm == tm && cables[c].ti == ti) return;
    if (ncables >= MAX_CABLES) return;
    cables[ncables++] = (Cable){ fm, fo, tm, ti, color };
    rebuild_routing();
}

static bool find_port(Vector2 mp, int *mod, int *idx, bool *isOut)
{
    for (int i = MAX_MODULES - 1; i >= 0; i--) {
        Module *m = &mods[i];
        if (!m->alive) continue;
        const ModDef *d = &DEFS[m->type];
        for (int o = 0; o < 2; o++) {
            int n = o ? d->nout : d->nin;
            for (int k = 0; k < n; k++)
                if (CheckCollisionPointCircle(mp, port_pos(m, o, k), 12)) {
                    *mod = i; *idx = k; *isOut = o;
                    return true;
                }
        }
    }
    return false;
}

static int module_at(Vector2 mp)
{
    for (int i = MAX_MODULES - 1; i >= 0; i--)
        if (mods[i].alive && CheckCollisionPointRec(mp, module_rect(&mods[i]))) return i;
    return -1;
}

static bool knob_at(Vector2 mp, int *mod, int *k)
{
    int i = module_at(mp);
    if (i < 0) return false;
    const ModDef *d = &DEFS[mods[i].type];
    for (int j = 0; j < d->nknob; j++) {
        Rectangle c = knob_cell(&mods[i], j);
        Rectangle hit = { c.x, c.y, c.width, 40 };
        if (CheckCollisionPointRec(mp, hit)) { *mod = i; *k = j; return true; }
    }
    return false;
}

static Rectangle close_rect(const Module *m)
{
    Rectangle r = module_rect(m);
    return (Rectangle){ r.x + r.width - CHEEK - 22, r.y + 7, 14, 14 };
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void draw_background(float t)
{
    int W = GetScreenWidth(), H = GetScreenHeight();
    DrawRectangleGradientV(0, 0, W, H, BG_TOP, BG_BOT);
    DrawCircleGradient((int)(W * 0.78f), (int)(H * 0.95f), H * 0.9f, (Color){ 22, 78, 64, 70 }, (Color){ 0, 0, 0, 0 });
    DrawCircleGradient((int)(W * 0.12f), (int)(H * 0.1f), H * 0.6f, (Color){ 30, 60, 70, 40 }, (Color){ 0, 0, 0, 0 });
    for (int i = 0; i < 30; i++) {
        float s = i * 1.37f;
        Vector2 p1 = { -60, H * (0.15f + 0.7f * fractf(s * 0.31f)) + sinf(t * 0.05f + s) * 60 };
        Vector2 p4 = { W + 60.0f, H * (0.1f + 0.8f * fractf(s * 0.53f)) + cosf(t * 0.04f + s * 1.3f) * 80 };
        Vector2 c2 = { W * 0.33f, H * fractf(s * 0.71f) + sinf(t * 0.07f + s * 2) * H * 0.3f };
        Vector2 c3 = { W * 0.66f, H * fractf(s * 0.97f) + cosf(t * 0.06f + s) * H * 0.3f };
        DrawSplineSegmentBezierCubic(p1, c2, c3, p4, 1.0f, (Color){ 120, 210, 185, (unsigned char)(8 + (i % 4) * 4) });
    }
    for (int i = 0; i < 90; i++) {
        float s = i * 7.31f;
        float x = fractf(fractf(s * 0.137f) + t * 0.004f * (0.5f + fractf(s * 0.71f))) * W;
        float y = fractf(s * 0.291f) * H + sinf(t * 0.2f + s) * 12;
        float a = 0.25f + 0.25f * sinf(t * 0.5f + s);
        DrawCircleV((Vector2){ x, y }, 1.2f, (Color){ 170, 240, 215, (unsigned char)(a * 120) });
    }
}

static void draw_port(Vector2 p, const char *label, bool isOut, bool hover)
{
    if (isOut) DrawRectangleRounded((Rectangle){ p.x - 19, p.y - 14, 38, 42 }, 0.25f, 6, PANEL_HI);
    DrawCircleV(p, 10, PANEL_LO);
    DrawCircleV(p, 8.5f, (Color){ 120, 124, 126, 255 });
    DrawCircleV(p, 6.5f, (Color){ 70, 72, 74, 255 });
    DrawCircleV(p, 4.5f, (Color){ 6, 6, 7, 255 });
    if (hover) DrawRing(p, 11, 13, 0, 360, 32, ACCENT);
    txt_c(label, p.x, p.y + 15, 10, isOut ? TEXT : TEXT_DIM);
}

static void draw_knob(const Module *m, int k, bool hover, bool active)
{
    const KnobDef *kd = &DEFS[m->type].knobs[k];
    Rectangle c = knob_cell(m, k);
    float v = m->p[k];
    if (kd->kind == K_SEG) {
        int n = (int)(kd->max - kd->min) + 1;
        float bw = (c.width - 8) / n;
        for (int i = 0; i < n; i++) {
            Rectangle b = { c.x + 4 + i * bw + 1, c.y + 10, bw - 2, 20 };
            bool sel = (int)roundf(v) == (int)kd->min + i;
            DrawRectangleRec(b, sel ? fade(ACCENT, 0.18f) : INSET);
            DrawRectangleLinesEx(b, 1, sel ? ACCENT : PANEL_HI);
            txt_c(kd->labels[i], b.x + b.width / 2, b.y + 6, 10, sel ? ACCENT : TEXT_DIM);
        }
        txt_c(kd->name, c.x + c.width / 2, c.y + 40, 10, TEXT_DIM);
        return;
    }
    Vector2 ctr = knob_center(c);
    float t = clampf(knob_to_t(kd, v), 0, 1);
    float a0 = 135, a1 = 135 + 270 * t;
    DrawRing(ctr, 17, 19, 135, 405, 36, PANEL_LO);
    if (kd->min < 0 && kd->max > 0) {
        float az = 135 + 270 * knob_to_t(kd, 0);
        DrawRing(ctr, 17, 19, fminf(az, a1), fmaxf(az, a1), 24, ACCENT_DIM);
    } else DrawRing(ctr, 17, 19, a0, a1, 36, ACCENT_DIM);
    DrawCircleV((Vector2){ ctr.x, ctr.y + 2 }, 14, (Color){ 0, 0, 0, 90 });
    DrawCircleV(ctr, 14, (Color){ 30, 31, 33, 255 });
    DrawCircleV(ctr, 12, (Color){ 44, 46, 48, 255 });
    float ar = a1 * DEG2RAD;
    DrawLineEx((Vector2){ ctr.x + cosf(ar) * 4, ctr.y + sinf(ar) * 4 },
               (Vector2){ ctr.x + cosf(ar) * 12, ctr.y + sinf(ar) * 12 }, 2.2f, TEXT);
    if (hover || active) {
        DrawRing(ctr, 19.5f, 20.5f, 0, 360, 36, fade(ACCENT, 0.5f));
        txt_c(knob_text(kd, v), ctr.x, c.y + 40, 10, ACCENT);
    } else txt_c(kd->name, ctr.x, c.y + 40, 10, TEXT_DIM);
}

static void draw_module(int i, int hoverMod, int hoverKnob, int activeKnob, int hpMod, int hpIdx, bool hpOut)
{
    Module *m = &mods[i];
    const ModDef *d = &DEFS[m->type];
    Rectangle r = module_rect(m);

    DrawRectangleRec((Rectangle){ r.x + 6, r.y + 8, r.width, r.height }, (Color){ 0, 0, 0, 90 });
    // wooden cheeks
    DrawRectangleGradientH((int)r.x, (int)r.y, CHEEK, (int)r.height, WOOD_DARK, WOOD);
    DrawRectangleGradientH((int)(r.x + r.width - CHEEK), (int)r.y, CHEEK, (int)r.height, WOOD, WOOD_DARK);
    for (int g = 0; g < 3; g++) {
        DrawLineEx((Vector2){ r.x + 2.5f + g * 2, r.y }, (Vector2){ r.x + 2.5f + g * 2, r.y + r.height }, 1, fade(WOOD_DARK, 0.5f));
        DrawLineEx((Vector2){ r.x + r.width - 3 - g * 2, r.y }, (Vector2){ r.x + r.width - 3 - g * 2, r.y + r.height }, 1, fade(WOOD_DARK, 0.5f));
    }
    // panel
    Rectangle pr = { r.x + CHEEK, r.y, r.width - 2 * CHEEK, r.height };
    DrawRectangleRec(pr, PANEL);
    DrawLineEx((Vector2){ pr.x, pr.y + 0.5f }, (Vector2){ pr.x + pr.width, pr.y + 0.5f }, 1, PANEL_HI);
    DrawLineEx((Vector2){ pr.x, pr.y + pr.height - 0.5f }, (Vector2){ pr.x + pr.width, pr.y + pr.height - 0.5f }, 1, PANEL_LO);
    // header
    DrawRectangleRec((Rectangle){ pr.x, pr.y, pr.width, HEADER_H - 4 }, (Color){ 33, 35, 37, 255 });
    txt(d->name, pr.x + PAD, r.y + 8, 10, TEXT);
    Rectangle cr = close_rect(m);
    bool hc = CheckCollisionPointRec(GetMousePosition(), cr);
    txt_c("x", cr.x + cr.width / 2, cr.y + 2, 10, hc ? CABLE_COLORS[1] : (i == hoverMod ? TEXT_DIM : fade(TEXT_DIM, 0.4f)));
    // screws
    DrawCircleV((Vector2){ pr.x + pr.width - 40, r.y + 13 }, 3, PANEL_LO);
    // custom area
    if (d->draw) d->draw(m, custom_rect(m));
    // knobs
    for (int k = 0; k < d->nknob; k++) draw_knob(m, k, i == hoverMod && k == hoverKnob, i == hoverMod && k == activeKnob);
    // ports
    if (d->nin + d->nout) {
        float y = ports_top(m) - 2;
        DrawLineEx((Vector2){ pr.x + PAD, y }, (Vector2){ pr.x + pr.width - PAD, y }, 1, PANEL_LO);
    }
    for (int k = 0; k < d->nin; k++)  draw_port(port_pos(m, false, k), d->ins[k], false, hpMod == i && !hpOut && hpIdx == k);
    for (int k = 0; k < d->nout; k++) draw_port(port_pos(m, true, k), d->outs[k], true, hpMod == i && hpOut && hpIdx == k);
}

static void draw_cable(Vector2 a, Vector2 b, Color c, float alpha)
{
    float dist = hypotf(b.x - a.x, b.y - a.y);
    float sag = 30 + dist * 0.3f;
    Vector2 c1 = { a.x + (b.x - a.x) * 0.15f, a.y + sag };
    Vector2 c2 = { b.x - (b.x - a.x) * 0.15f, b.y + sag };
    DrawSplineSegmentBezierCubic((Vector2){ a.x + 2, a.y + 4 }, (Vector2){ c1.x + 2, c1.y + 4 }, (Vector2){ c2.x + 2, c2.y + 4 }, (Vector2){ b.x + 2, b.y + 4 }, 5, (Color){ 0, 0, 0, (unsigned char)(70 * alpha) });
    DrawSplineSegmentBezierCubic(a, c1, c2, b, 4.5f, fade(c, alpha));
    DrawSplineSegmentBezierCubic((Vector2){ a.x, a.y - 1 }, (Vector2){ c1.x, c1.y - 1 }, (Vector2){ c2.x, c2.y - 1 }, (Vector2){ b.x, b.y - 1 }, 1.2f, (Color){ 255, 255, 255, (unsigned char)(40 * alpha) });
    DrawCircleV(a, 6, fade(c, alpha));
    DrawCircleV(b, 6, fade(c, alpha));
    DrawCircleV(a, 2.5f, (Color){ 20, 20, 20, (unsigned char)(200 * alpha) });
    DrawCircleV(b, 2.5f, (Color){ 20, 20, 20, (unsigned char)(200 * alpha) });
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

enum { D_NONE, D_MODULE, D_KNOB, D_CABLE, D_PAN, D_CUSTOM };

static struct {
    int     mode;
    int     mod, idx;
    bool    fromOut;
    int     color;
    Vector2 last;
    float   t;
    bool    menu;
    Vector2 menuPos;
    double  lastClick;
    int     lastKnobMod, lastKnobIdx;
} ui = { .lastKnobMod = -1 };

static const int MENU_ORDER[M_COUNT] = { M_CLOCK, M_LFO, M_SEQ, M_CHORD, M_OSC3, M_REVERB, M_OUT };
#define MENU_W 160
#define MENU_ITEM_H 24

static Rectangle menu_item(int i) { return (Rectangle){ ui.menuPos.x, ui.menuPos.y + 26 + i * MENU_ITEM_H, MENU_W, MENU_ITEM_H }; }

static void handle_input(void)
{
    Vector2 mp = GetMousePosition();
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (ui.menu) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            for (int i = 0; i < M_COUNT; i++)
                if (CheckCollisionPointRec(mp, menu_item(i)) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    add_module(MENU_ORDER[i], ui.menuPos.x - pan.x, ui.menuPos.y - pan.y);
            ui.menu = false;
        }
        if (IsKeyPressed(KEY_ESCAPE)) ui.menu = false;
        return;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        int pm, pi; bool po;
        int ki, kk;
        int mi = module_at(mp);
        if (find_port(mp, &pm, &pi, &po)) {
            ui.mode = D_CABLE;
            if (po) { ui.mod = pm; ui.idx = pi; ui.fromOut = true; ui.color = next_color++ % N_CABLE_COLORS; }
            else {
                int found = -1;
                for (int c = ncables - 1; c >= 0 && found < 0; c--)
                    if (cables[c].tm == pm && cables[c].ti == pi) found = c;
                if (found >= 0) {   // pick up the cable plugged into this input
                    ui.mod = cables[found].fm; ui.idx = cables[found].fo; ui.fromOut = true; ui.color = cables[found].color;
                    remove_cable(found);
                    rebuild_routing();
                } else { ui.mod = pm; ui.idx = pi; ui.fromOut = false; ui.color = next_color++ % N_CABLE_COLORS; }
            }
        } else if (mi >= 0 && CheckCollisionPointRec(mp, close_rect(&mods[mi]))) {
            remove_module(mi);
        } else if (knob_at(mp, &ki, &kk)) {
            const KnobDef *kd = &DEFS[mods[ki].type].knobs[kk];
            double now = GetTime();
            if (kd->kind == K_SEG) {
                Rectangle c = knob_cell(&mods[ki], kk);
                int n = (int)(kd->max - kd->min) + 1;
                int s = clampi((int)((mp.x - c.x - 4) / ((c.width - 8) / n)), 0, n - 1);
                mods[ki].p[kk] = kd->min + s;
            } else if (ui.lastKnobMod == ki && ui.lastKnobIdx == kk && now - ui.lastClick < 0.3) {
                mods[ki].p[kk] = kd->def;   // double-click resets
            } else {
                ui.mode = D_KNOB; ui.mod = ki; ui.idx = kk;
                ui.t = knob_to_t(kd, mods[ki].p[kk]);
            }
            ui.lastClick = now; ui.lastKnobMod = ki; ui.lastKnobIdx = kk;
        } else if (mi >= 0) {
            const ModDef *d = &DEFS[mods[mi].type];
            if (d->press && d->press(&mods[mi], mp, custom_rect(&mods[mi]))) { ui.mode = D_CUSTOM; ui.mod = mi; }
            else if (mp.y < mods[mi].pos.y + pan.y + HEADER_H) { ui.mode = D_MODULE; ui.mod = mi; }
            else ui.mode = D_NONE;
        } else ui.mode = D_PAN;
        ui.last = mp;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        int pm, pi; bool po;
        if (find_port(mp, &pm, &pi, &po)) {  // unplug everything at this jack
            for (int c = ncables - 1; c >= 0; c--)
                if ((po && cables[c].fm == pm && cables[c].fo == pi) || (!po && cables[c].tm == pm && cables[c].ti == pi))
                    remove_cable(c);
            rebuild_routing();
        } else if (module_at(mp) < 0) {
            ui.menu = true;
            ui.menuPos = mp;
            float maxY = GetScreenHeight() - 30 - M_COUNT * MENU_ITEM_H;
            if (ui.menuPos.y > maxY) ui.menuPos.y = maxY;
            if (ui.menuPos.x > GetScreenWidth() - MENU_W - 4) ui.menuPos.x = GetScreenWidth() - MENU_W - 4;
        }
    }

    Vector2 delta = { mp.x - ui.last.x, mp.y - ui.last.y };
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        switch (ui.mode) {
        case D_MODULE: mods[ui.mod].pos.x += delta.x; mods[ui.mod].pos.y += delta.y; break;
        case D_PAN:    pan.x += delta.x; pan.y += delta.y; break;
        case D_KNOB: {
            const KnobDef *kd = &DEFS[mods[ui.mod].type].knobs[ui.idx];
            float range = kd->kind == K_STEP ? fmaxf(200.0f, (kd->max - kd->min) * 6.0f) : 200.0f;
            ui.t = clampf(ui.t - delta.y / range * (shift ? 0.1f : 1.0f), 0, 1);
            mods[ui.mod].p[ui.idx] = knob_from_t(kd, ui.t);
        } break;
        case D_CUSTOM: {
            const ModDef *d = &DEFS[mods[ui.mod].type];
            if (d->drag) d->drag(&mods[ui.mod], mp, custom_rect(&mods[ui.mod]));
        } break;
        }
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (ui.mode == D_CABLE) {
            int pm, pi; bool po;
            if (find_port(mp, &pm, &pi, &po) && po != ui.fromOut) {
                if (ui.fromOut) add_cable(ui.mod, ui.idx, pm, pi, ui.color);
                else            add_cable(pm, pi, ui.mod, ui.idx, ui.color);
            }
        }
        ui.mode = D_NONE;
    }

    float wheel = GetMouseWheelMove();
    int ki, kk;
    if (wheel != 0 && ui.mode == D_NONE && knob_at(mp, &ki, &kk)) {
        const KnobDef *kd = &DEFS[mods[ki].type].knobs[kk];
        if (kd->kind == K_STEP || kd->kind == K_SEG)
            mods[ki].p[kk] = clampf(mods[ki].p[kk] + (wheel > 0 ? 1 : -1), kd->min, kd->max);
        else
            mods[ki].p[kk] = knob_from_t(kd, knob_to_t(kd, mods[ki].p[kk]) + wheel * (shift ? 0.005f : 0.03f));
    }
    ui.last = mp;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

static void frame(void)
{
#if defined(PLATFORM_WEB)
    // raylib resizes the canvas to the browser window behind GLFW's back; GLFW
    // then scales mouse coordinates by its stale window size. Keep them in sync.
    static int syncW, syncH;
    if (GetScreenWidth() != syncW || GetScreenHeight() != syncH) {
        syncW = GetScreenWidth(); syncH = GetScreenHeight();
        SetWindowSize(syncW, syncH);
    }
#endif
    handle_input();
    Vector2 mp = GetMousePosition();

    int hoverMod = module_at(mp), hoverKnob = -1, km, kk;
    if (knob_at(mp, &km, &kk) && km == hoverMod) hoverKnob = kk;
    int hpMod = -1, hpIdx = -1; bool hpOut = false;
    find_port(mp, &hpMod, &hpIdx, &hpOut);

    BeginDrawing();
    draw_background((float)GetTime());

    for (int i = 0; i < MAX_MODULES; i++)
        if (mods[i].alive)
            draw_module(i, hoverMod, hoverKnob, (ui.mode == D_KNOB && ui.mod == i) ? ui.idx : -1, hpMod, hpIdx, hpOut);

    for (int c = 0; c < ncables; c++) {
        Cable *k = &cables[c];
        draw_cable(port_pos(&mods[k->fm], true, k->fo), port_pos(&mods[k->tm], false, k->ti), CABLE_COLORS[k->color], 0.92f);
    }
    if (ui.mode == D_CABLE) {
        Vector2 a = port_pos(&mods[ui.mod], ui.fromOut, ui.idx);
        draw_cable(a, mp, CABLE_COLORS[ui.color], 0.75f);
    }

    // brand + hints
    txt("R A Y A", 18, 14, 20, TEXT);
    txt("AMBIENT MODULAR", 18, 38, 10, TEXT_DIM);
    txt("DRAG JACK TO JACK TO PATCH   RIGHT-CLICK JACK: UNPLUG   RIGHT-CLICK SPACE: ADD MODULE   DRAG SPACE: PAN   DOUBLE-CLICK KNOB: RESET   SHIFT: FINE",
        18, GetScreenHeight() - 22, 10, fade(TEXT_DIM, 0.8f));

    if (ui.menu) {
        Rectangle box = { ui.menuPos.x, ui.menuPos.y, MENU_W, 30 + M_COUNT * MENU_ITEM_H };
        DrawRectangleRec((Rectangle){ box.x + 5, box.y + 6, box.width, box.height }, (Color){ 0, 0, 0, 110 });
        DrawRectangleRec(box, PANEL);
        DrawRectangleLinesEx(box, 1, PANEL_HI);
        txt("ADD MODULE", box.x + 10, box.y + 9, 10, TEXT_DIM);
        for (int i = 0; i < M_COUNT; i++) {
            Rectangle it = menu_item(i);
            bool h = CheckCollisionPointRec(mp, it);
            if (h) DrawRectangleRec(it, fade(ACCENT, 0.12f));
            txt(DEFS[MENU_ORDER[i]].name, it.x + 12, it.y + 7, 10, h ? ACCENT : TEXT);
        }
    }
    EndDrawing();
}

static void default_layout(void)
{
    int clk = add_module(M_CLOCK,  24,  70);
    int lfo = add_module(M_LFO,    24, 270);
    int seq = add_module(M_SEQ,   276,  70);
    int chd = add_module(M_CHORD, 684,  70);
    int osc = add_module(M_OSC3,  276, 360);
    int rev = add_module(M_REVERB, 708, 360);
    int out = add_module(M_OUT,   964, 360);

    /* A starting patch that sounds right away: a drifting chord pad through
       the reverb. Every cable can be pulled out and repatched. */
    add_cable(clk, 0, seq, 0, 3);   /* 1/16 -> SEQ CLOCK      */
    add_cable(clk, 2, chd, 0, 0);   /* BAR  -> CHORDS CLOCK   */
    add_cable(chd, 0, osc, 0, 1);   /* N1..N3 -> P1..P3       */
    add_cable(chd, 1, osc, 1, 1);
    add_cable(chd, 2, osc, 2, 1);
    add_cable(seq, 1, osc, 5, 3);   /* SEQ CV -> WAVE         */
    add_cable(lfo, 0, osc, 4, 2);   /* SINE -> CUTOFF         */
    add_cable(osc, 0, rev, 0, 4);   /* OSC -> REVERB -> OUT   */
    add_cable(osc, 1, rev, 1, 4);
    add_cable(rev, 0, out, 0, 0);
    add_cable(rev, 1, out, 1, 0);
}

int main(void)
{
    parse_progressions();

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 760, "raya");
    SetExitKey(KEY_NULL);

    default_layout();
    rebuild_routing();

    InitAudioDevice();
    SetAudioStreamBufferSizeDefault(2048);
    AudioStream stream = LoadAudioStream((unsigned int)SR, 32, 2);
    SetAudioStreamCallback(stream, audio_callback);
    PlayAudioStream(stream);

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(frame, 0, 1);
#else
    SetTargetFPS(60);
    while (!WindowShouldClose()) frame();
    UnloadAudioStream(stream);
    CloseAudioDevice();
#endif

    CloseWindow();
    return 0;
}
