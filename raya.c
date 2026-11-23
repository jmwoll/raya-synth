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
#include <stdarg.h>
#include <ctype.h>

#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif

#define SR           48000.0f
#define TAU          6.28318530718f
#define MAX_MODULES  64
#define MAX_IO       8
#define MAX_KNOBS    12
#define MAX_CABLES   256
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
    // optional: extra state stored in presets (beyond knob values)
    void (*save)(const Module *, char *out, int cap);
    void (*load)(Module *, const char *data);
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
static bool   sel[MAX_MODULES];   // selected in the UI
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

static void seq_save(const Module *m, char *out, int cap)
{
    const SeqSt *s = m->st;
    int n = 0;
    for (int i = 0; i < 16 && n < cap; i++)
        n += snprintf(out + n, cap - n, "%s%d:%.3f", i ? " " : "", s->on[i], s->val[i]);
}

static void seq_load(Module *m, const char *str)
{
    SeqSt *s = m->st;
    for (int i = 0; i < 16; i++) {
        int on, used; float v;
        if (sscanf(str, " %d:%f%n", &on, &v, &used) != 2) break;
        s->on[i] = on != 0; s->val[i] = clampf(v, 0, 1);
        str += used;
    }
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
        seq_init, seq_process, seq_draw, seq_press, seq_drag, seq_save, seq_load },
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

// Stable ids used in preset files.
static const char *MOD_ID[M_COUNT] = {
    [M_CLOCK] = "clock", [M_SEQ] = "seq", [M_CHORD] = "chords", [M_OSC3] = "osc3",
    [M_LFO] = "lfo", [M_REVERB] = "reverb", [M_OUT] = "output",
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
        sel[i] = false;
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
    sel[i] = false;
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
// Preset rack — any selection of modules (knob values, step data and the
// cables between them) is stored in a numbered slot, e.g. #1-warm-drone.
// One text file per slot: <dir>/0001-warm-drone.raya. On the web the
// directory lives in IndexedDB (IDBFS), so presets survive reloads.
// ---------------------------------------------------------------------------

#define PRESET_MAX      2048
#define PRESET_NAME_MAX 40
#if defined(PLATFORM_WEB)
#define PRESET_ROOT "/raya"
#else
#define PRESET_ROOT "raya-data"
#endif
#define PRESET_DIR PRESET_ROOT "/presets"

typedef struct {
    bool  used;
    char  name[PRESET_NAME_MAX + 1];   // as typed by the user
    char  slug[PRESET_NAME_MAX + 1];   // my-pad-oscillator
    char  file[PRESET_NAME_MAX + 16];  // 0001-my-pad-oscillator.raya
    char  summary[96];                 // modules it contains
    char *text;
} Preset;

static Preset rack[PRESET_MAX];   // rack[i] is slot #i+1
static int    rack_count;
static bool   rack_ready;

static const char *preset_label(int slot) { return TextFormat("#%d-%s", slot + 1, rack[slot].slug); }

static void make_slug(const char *name, char *out)
{
    int n = 0;
    bool dash = false;
    for (const char *c = name; *c && n < PRESET_NAME_MAX; c++) {
        unsigned char ch = (unsigned char)*c;
        if (!isalnum(ch)) { dash = true; continue; }
        if (dash && n && n < PRESET_NAME_MAX - 1) out[n++] = '-';
        dash = false;
        out[n++] = (char)tolower(ch);
    }
    out[n] = 0;
    if (!n) strcpy(out, "preset");
}

typedef struct { char *s; int len, cap; } Buf;

static void bput(Buf *b, const char *fmt, ...)
{
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (n < b->cap - b->len) { b->len += n; return; }
        b->cap = b->cap * 2 + n;
        b->s = realloc(b->s, b->cap);
    }
}

// "CHORD LEN" <-> "CHORD_LEN" so every line splits on spaces
static void knob_key(const char *name, char *out)
{
    int i = 0;
    for (; name[i] && i < 31; i++) out[i] = name[i] == ' ' ? '_' : name[i];
    out[i] = 0;
}

static int knob_index(const ModDef *d, const char *key)
{
    char k[32];
    for (int i = 0; i < d->nknob; i++) {
        knob_key(d->knobs[i].name, k);
        if (!strcmp(k, key)) return i;
    }
    return -1;
}

static int mod_type_from_id(const char *id)
{
    for (int t = 0; t < M_COUNT; t++) if (!strcmp(MOD_ID[t], id)) return t;
    return -1;
}

static int selection_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_MODULES; i++) n += mods[i].alive && sel[i];
    return n;
}

static void select_none(void) { memset(sel, 0, sizeof sel); }

// Serialize the selected modules (positions relative to their top-left corner)
// and the cables running between them. Caller frees.
static char *preset_serialize(const char *name)
{
    Buf b = { malloc(1024), 0, 1024 };
    int map[MAX_MODULES], n = 0;
    float x0 = 1e9f, y0 = 1e9f;
    for (int i = 0; i < MAX_MODULES; i++) {
        map[i] = -1;
        if (!mods[i].alive || !sel[i]) continue;
        map[i] = n++;
        x0 = fminf(x0, mods[i].pos.x);
        y0 = fminf(y0, mods[i].pos.y);
    }
    bput(&b, "raya-preset 1\nname %s\n", name);
    for (int i = 0; i < MAX_MODULES; i++) {
        if (map[i] < 0) continue;
        const Module *m = &mods[i];
        const ModDef *d = &DEFS[m->type];
        bput(&b, "module %s %.0f %.0f\n", MOD_ID[m->type], m->pos.x - x0, m->pos.y - y0);
        for (int k = 0; k < d->nknob; k++) {
            char key[32];
            knob_key(d->knobs[k].name, key);
            bput(&b, "knob %s %.6g\n", key, m->p[k]);
        }
        if (d->save) {
            char data[512] = { 0 };
            d->save(m, data, sizeof data);
            bput(&b, "data %s\n", data);
        }
    }
    for (int c = 0; c < ncables; c++) {
        const Cable *k = &cables[c];
        if (map[k->fm] >= 0 && map[k->tm] >= 0)
            bput(&b, "cable %d %d %d %d %d\n", map[k->fm], k->fo, map[k->tm], k->ti, k->color);
    }
    return b.s;
}

// Iterate over the lines of a preset text.
static const char *next_line(const char *p, char *line, int cap)
{
    int len = (int)strcspn(p, "\n");
    int n = len < cap - 1 ? len : cap - 1;
    memcpy(line, p, n);
    line[n] = 0;
    if (n && line[n - 1] == '\r') line[n - 1] = 0;
    p += len;
    return *p ? p + 1 : p;
}

// Add the preset's modules with their top-left corner at world position `at`.
// The new modules become the selection. Returns how many modules were added.
static int preset_instantiate(const char *text, Vector2 at)
{
    int map[MAX_MODULES], n = 0, added = 0;
    Module *cur = NULL;
    char line[1024];
    select_none();
    for (const char *p = text; *p; ) {
        p = next_line(p, line, sizeof line);
        char kw[16];
        if (sscanf(line, "%15s", kw) != 1) continue;
        if (!strcmp(kw, "module")) {
            char id[32]; float x, y;
            cur = NULL;
            if (n >= MAX_MODULES) continue;
            int idx = -1;
            if (sscanf(line, "module %31s %f %f", id, &x, &y) == 3) {
                int t = mod_type_from_id(id);
                if (t >= 0) idx = add_module(t, at.x + x, at.y + y);
            }
            map[n++] = idx;
            if (idx >= 0) { cur = &mods[idx]; sel[idx] = true; added++; }
        } else if (!strcmp(kw, "knob") && cur) {
            char key[32]; float v;
            const ModDef *d = &DEFS[cur->type];
            int k;
            if (sscanf(line, "knob %31s %f", key, &v) == 2 && (k = knob_index(d, key)) >= 0)
                cur->p[k] = clampf(v, d->knobs[k].min, d->knobs[k].max);
        } else if (!strcmp(kw, "data") && cur) {
            if (DEFS[cur->type].load) DEFS[cur->type].load(cur, line + 4);
        } else if (!strcmp(kw, "cable")) {
            int a, o, b, i, c;
            if (sscanf(line, "cable %d %d %d %d %d", &a, &o, &b, &i, &c) == 5 &&
                a >= 0 && a < n && b >= 0 && b < n && map[a] >= 0 && map[b] >= 0 &&
                o >= 0 && o < DEFS[mods[map[a]].type].nout && i >= 0 && i < DEFS[mods[map[b]].type].nin)
                add_cable(map[a], o, map[b], i, abs(c) % N_CABLE_COLORS);
        }
    }
    rebuild_routing();
    return added;
}

// Name and module summary for the rack listing.
static void preset_describe(Preset *ps)
{
    int cnt[M_COUNT] = { 0 };
    char line[256];
    ps->name[0] = 0;
    for (const char *p = ps->text; *p; ) {
        p = next_line(p, line, sizeof line);
        char id[32];
        if (!strncmp(line, "name ", 5)) snprintf(ps->name, sizeof ps->name, "%s", line + 5);
        else if (sscanf(line, "module %31s", id) == 1) {
            int t = mod_type_from_id(id);
            if (t >= 0) cnt[t]++;
        }
    }
    if (!ps->name[0]) snprintf(ps->name, sizeof ps->name, "%s", ps->slug);
    int n = 0;
    ps->summary[0] = 0;
    for (int t = 0; t < M_COUNT; t++) {
        if (!cnt[t]) continue;
        n += snprintf(ps->summary + n, sizeof ps->summary - n, "%s%s", n ? ", " : "", DEFS[t].name);
        if (cnt[t] > 1) n += snprintf(ps->summary + n, sizeof ps->summary - n, " X%d", cnt[t]);
        if (n >= (int)sizeof ps->summary) break;
    }
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    s[fread(s, 1, n, f)] = 0;
    fclose(f);
    return s;
}

static bool write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    return fclose(f) == 0 && ok;
}

static bool fs_hold;   // batch several writes into one sync

static void fs_sync(void)
{
    if (fs_hold) return;
#if defined(PLATFORM_WEB)
    emscripten_run_script("FS.syncfs(false, function (e) { if (e) console.error('raya: preset sync failed', e); });");
#endif
}

static bool fs_ready(void)
{
#if defined(PLATFORM_WEB)
    return emscripten_run_script_int("Module.rayaFsReady | 0") != 0;
#else
    return true;
#endif
}

// Put a preset into its slot; takes ownership of text.
static bool rack_put(int slot, const char *file, char *text)
{
    if (slot < 0 || slot >= PRESET_MAX || rack[slot].used || strncmp(text, "raya-preset", 11)) { free(text); return false; }
    Preset *ps = &rack[slot];
    memset(ps, 0, sizeof *ps);
    ps->used = true;
    ps->text = text;
    snprintf(ps->file, sizeof ps->file, "%s", file);
    const char *s = strchr(file, '-');               // slug = file name between "0001-" and ".raya"
    int len = s ? (int)strlen(s + 1) - 5 : 0;
    if (len > PRESET_NAME_MAX) len = PRESET_NAME_MAX;
    if (len > 0) { memcpy(ps->slug, s + 1, len); ps->slug[len] = 0; } else strcpy(ps->slug, "preset");
    preset_describe(ps);
    rack_count++;
    return true;
}

// Store a new preset in the lowest free slot; takes ownership of text.
// Returns the slot, or -1 when the rack is full or the write failed.
static int rack_store(const char *name, char *text)
{
    int slot = 0;
    while (slot < PRESET_MAX && rack[slot].used) slot++;
    char slug[PRESET_NAME_MAX + 1], file[PRESET_NAME_MAX + 16];
    make_slug(name, slug);
    snprintf(file, sizeof file, "%04d-%s.raya", slot + 1, slug);
    if (slot >= PRESET_MAX || !write_file(TextFormat("%s/%s", PRESET_DIR, file), text)) { free(text); return -1; }
    rack_put(slot, file, text);
    fs_sync();
    return slot;
}

static void rack_delete(int slot)
{
    if (!rack[slot].used) return;
    remove(TextFormat("%s/%s", PRESET_DIR, rack[slot].file));
    free(rack[slot].text);
    memset(&rack[slot], 0, sizeof rack[slot]);
    rack_count--;
    fs_sync();
}

// Factory presets: seeded into an empty rack on first start. Positions are
// relative; module order defines the indices used by the cable lines.
static const struct { const char *name, *body; } FACTORY[] = {
    { "ambient starter",
      "module clock 0 0\n"
      "module lfo 0 200\n"
      "module seq 252 0\n"
      "module chords 660 0\n"
      "module osc3 252 290\n"
      "module reverb 684 290\n"
      "module output 940 290\n"
      "cable 0 0 2 0 3\ncable 0 2 3 0 0\n"
      "cable 3 0 4 0 1\ncable 3 1 4 1 1\ncable 3 2 4 2 1\n"
      "cable 2 1 4 5 3\ncable 1 0 4 4 2\n"
      "cable 4 0 5 0 4\ncable 4 1 5 1 4\ncable 5 0 6 0 0\ncable 5 1 6 1 0\n" },
    { "warm drone",
      "module osc3 0 0\n"
      "knob OCTAVE -1\nknob WAVE 0.2\nknob DETUNE 0.5\nknob CUTOFF 600\nknob ATTACK 6\n"
      "module reverb 436 0\n"
      "knob SIZE 1.4\nknob DECAY 18\nknob MIX 0.55\n"
      "module output 696 0\n"
      "cable 0 0 1 0 4\ncable 0 1 1 1 4\ncable 1 0 2 0 0\ncable 1 1 2 1 0\n" },
    { "glass pad",
      "module lfo 0 0\n"
      "knob RATE 0.03\n"
      "module osc3 256 0\n"
      "knob OCTAVE 1\nknob WAVE 0.05\nknob DETUNE 0.15\nknob CUTOFF 3500\nknob RESO 0.35\n"
      "knob ATTACK 5\nknob RELEASE 10\nknob MOD_AMT 0.6\nknob LEVEL 0.5\n"
      "module reverb 692 0\n"
      "knob SIZE 1.8\nknob DECAY 30\nknob DAMP 0.2\nknob MIX 0.7\n"
      "module output 952 0\n"
      "cable 0 2 1 4 2\ncable 1 0 2 0 4\ncable 1 1 2 1 4\ncable 2 0 3 0 0\ncable 2 1 3 1 0\n" },
    { "breathing chords",
      "module clock 0 0\n"
      "knob BPM 60\n"
      "module chords 256 0\n"
      "knob PROG 5\nknob CHORD_LEN 3\n"
      "module osc3 0 300\n"
      "knob WAVE 0.3\nknob CUTOFF 1100\nknob ATTACK 3\nknob GLIDE 1.2\n"
      "module reverb 436 300\n"
      "module output 696 300\n"
      "cable 0 2 1 0 0\ncable 1 0 2 0 1\ncable 1 1 2 1 1\ncable 1 2 2 2 1\n"
      "cable 2 0 3 0 4\ncable 2 1 3 1 4\ncable 3 0 4 0 0\ncable 3 1 4 1 0\n" },
    { "sequenced swells",
      "module clock 0 0\n"
      "knob BPM 66\n"
      "module seq 256 0\n"
      "knob CHANCE 0.7\nknob GLIDE 0.4\n"
      "data 1:0.200 0:0.500 1:0.650 0:0.300 1:0.900 0:0.400 0:0.700 1:0.350 "
      "1:0.550 0:0.250 1:0.800 0:0.600 0:0.150 1:0.450 0:0.950 1:0.300\n"
      "module osc3 0 300\n"
      "knob WAVE 0.45\nknob CUTOFF 500\nknob ATTACK 0.6\nknob RELEASE 4\nknob MOD_AMT 0.7\n"
      "module reverb 436 300\n"
      "knob DECAY 16\nknob MIX 0.6\n"
      "module output 696 300\n"
      "cable 0 1 1 0 3\ncable 1 0 2 3 1\ncable 1 1 2 4 2\n"
      "cable 2 0 3 0 4\ncable 2 1 3 1 4\ncable 3 0 4 0 0\ncable 3 1 4 1 0\n" },
    { "arp shimmer",
      "module clock 0 0\n"
      "module seq 256 0\n"
      "knob CHANCE 0.9\nknob GLIDE 0\n"
      "module chords 668 0\n"
      "knob PROG 12\n"
      "module osc3 256 300\n"
      "knob WAVE 0.15\nknob DETUNE 0.2\nknob CUTOFF 2400\nknob ATTACK 0.02\nknob RELEASE 2.5\n"
      "knob GLIDE 0.05\nknob LEVEL 0.5\n"
      "module reverb 692 300\n"
      "knob SIZE 1.7\nknob DECAY 25\nknob MIX 0.65\n"
      "module output 952 300\n"
      "cable 0 0 1 0 3\ncable 0 2 2 0 0\ncable 1 1 2 1 2\ncable 2 5 3 0 1\ncable 1 0 3 3 3\n"
      "cable 3 0 4 0 4\ncable 3 1 4 1 4\ncable 4 0 5 0 0\ncable 4 1 5 1 0\n" },
    { "deep drone",
      "module lfo 0 0\n"
      "knob RATE 0.02\n"
      "module osc3 256 0\n"
      "knob OCTAVE -2\nknob WAVE 0.55\nknob DETUNE 0.6\nknob CUTOFF 280\nknob RESO 0.4\n"
      "knob ATTACK 8\nknob MOD_AMT 0.4\n"
      "module reverb 692 0\n"
      "knob DECAY 20\nknob DAMP 0.6\n"
      "module output 952 0\n"
      "cable 0 0 1 4 2\ncable 1 0 2 0 4\ncable 1 1 2 1 4\ncable 2 0 3 0 0\ncable 2 1 3 1 0\n" },
    { "chord brain",
      "module clock 0 0\n"
      "knob BPM 64\n"
      "module chords 256 0\n"
      "cable 0 2 1 0 0\n" },
    { "slow drift",
      "module lfo 0 0\n"
      "knob RATE 0.015\nknob DEPTH 0.8\n" },
    { "endless hall",
      "module reverb 0 0\n"
      "knob SIZE 1.9\nknob DECAY 40\nknob DAMP 0.55\nknob MIX 0.75\n"
      "module output 260 0\n"
      "cable 0 0 1 0 0\ncable 0 1 1 1 0\n" },
};
#define N_FACTORY ((int)(sizeof FACTORY / sizeof FACTORY[0]))

static char *factory_text(int i)
{
    Buf b = { malloc(1024), 0, 1024 };
    bput(&b, "raya-preset 1\nname %s\n%s", FACTORY[i].name, FACTORY[i].body);
    return b.s;
}

static void rack_load(void)
{
    MakeDirectory(PRESET_DIR);
    FilePathList fl = LoadDirectoryFiles(PRESET_DIR);
    for (unsigned int i = 0; i < fl.count; i++) {
        const char *fn = GetFileName(fl.paths[i]);
        int slot;
        if (sscanf(fn, "%d-", &slot) != 1 || !IsFileExtension(fn, ".raya")) continue;
        char *text = read_file(fl.paths[i]);
        if (text) rack_put(slot - 1, fn, text);
    }
    UnloadDirectoryFiles(fl);
    const char *seeded = PRESET_DIR "/.seeded";
    if (!FileExists(seeded)) {
        fs_hold = true;
        for (int i = 0; i < N_FACTORY; i++) rack_store(FACTORY[i].name, factory_text(i));
        write_file(seeded, "factory presets installed\n");
        fs_hold = false;
        fs_sync();
    }
    rack_ready = true;
}

// Case-insensitive: every space-separated word of the query must appear in
// the slot label, the name or the module summary.
static bool preset_matches(int slot, const char *query)
{
    char hay[256], q[PRESET_NAME_MAX + 1];
    const Preset *ps = &rack[slot];
    snprintf(hay, sizeof hay, "%s %s %s", preset_label(slot), ps->name, ps->summary);
    snprintf(q, sizeof q, "%s", query);
    for (char *c = hay; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (char *c = q; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (char *w = strtok(q, " "); w; w = strtok(NULL, " "))
        if (!strstr(hay, w)) return false;
    return true;
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

enum { D_NONE, D_MODULE, D_KNOB, D_CABLE, D_PAN, D_CUSTOM, D_BOX, D_MAP };
enum { MODAL_NONE, MODAL_RACK, MODAL_SAVE };

static struct {
    int     mode;
    int     mod, idx;
    bool    fromOut;
    int     color;
    Vector2 last, press;
    float   t;
    bool    menu;
    Vector2 menuPos;
    double  lastClick;
    int     lastKnobMod, lastKnobIdx;
    // preset rack + save dialog
    int     modal;
    char    text[PRESET_NAME_MAX + 1];   // search query or preset name
    int     hl, scroll, confirmDel;      // confirmDel: slot + 1 armed for deletion
    bool    placeAt;                     // load at placePos (opened from the context menu)
    Vector2 placePos;
    char    toast[96];
    double  toastT;
} ui = { .lastKnobMod = -1, .toastT = -10 };

static const int MENU_ORDER[M_COUNT] = { M_CLOCK, M_LFO, M_SEQ, M_CHORD, M_OSC3, M_REVERB, M_OUT };
#define MENU_W 160
#define MENU_ITEM_H 24
#define MENU_N (M_COUNT + 1)   // modules + "PRESET RACK..."

static Rectangle menu_item(int i)
{
    float gap = i == M_COUNT ? 8 : 0;
    return (Rectangle){ ui.menuPos.x, ui.menuPos.y + 26 + i * MENU_ITEM_H + gap, MENU_W, MENU_ITEM_H };
}

static void toast(const char *s) { snprintf(ui.toast, sizeof ui.toast, "%s", s); ui.toastT = GetTime(); }

// --- top bar buttons --------------------------------------------------------

static Rectangle btn_rack(void) { return (Rectangle){ GetScreenWidth() - 18 - 150.0f, 14, 150, 26 }; }
static Rectangle btn_save(void) { Rectangle r = btn_rack(); return (Rectangle){ r.x - 10 - 190, r.y, 190, 26 }; }

// --- patch overview (minimap) ---------------------------------------------

#define MAP_W 200
#define MAP_H 124

typedef struct { bool show; Rectangle box; float s; Vector2 o; } Minimap;  // map = o + world * s

static Minimap minimap(void)
{
    Minimap m = { .box = { GetScreenWidth() - 18.0f - MAP_W, 52, MAP_W, MAP_H } };
    // fit the patch itself; the view frame is clipped to the box
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = 0; i < MAX_MODULES; i++) {
        if (!mods[i].alive) continue;
        Rectangle r = module_rect(&mods[i]);
        float wx = r.x - pan.x, wy = r.y - pan.y;
        if (!m.show) { x0 = wx; y0 = wy; x1 = wx + r.width; y1 = wy + r.height; }
        x0 = fminf(x0, wx); y0 = fminf(y0, wy);
        x1 = fmaxf(x1, wx + r.width); y1 = fmaxf(y1, wy + r.height);
        m.show = true;
    }
    if (!m.show) return m;
    float pad = 8;
    m.s = fminf((MAP_W - 2 * pad) / (x1 - x0), (MAP_H - 2 * pad) / (y1 - y0));
    m.o.x = m.box.x + (MAP_W - (x1 - x0) * m.s) / 2 - x0 * m.s;
    m.o.y = m.box.y + (MAP_H - (y1 - y0) * m.s) / 2 - y0 * m.s;
    return m;
}

static Minimap mapDrag;   // layout frozen while dragging inside the overview

static Vector2 map_pt(const Minimap *m, Vector2 screen)   // screen position -> minimap
{
    return (Vector2){ m->o.x + (screen.x - pan.x) * m->s, m->o.y + (screen.y - pan.y) * m->s };
}

static void map_center_view(const Minimap *m, Vector2 mp)  // pan so the clicked spot is centred
{
    Vector2 w = { (mp.x - m->o.x) / m->s, (mp.y - m->o.y) / m->s };
    pan.x = GetScreenWidth() / 2.0f - w.x;
    pan.y = GetScreenHeight() / 2.0f - w.y;
}

static void draw_minimap(Vector2 mp)
{
    Minimap m = minimap();
    if (!m.show) return;
    bool hover = CheckCollisionPointRec(mp, m.box) || ui.mode == D_MAP;
    DrawRectangleRec(m.box, fade(INSET, hover ? 0.55f : 0.35f));
    DrawRectangleLinesEx(m.box, 1, fade(PANEL_HI, hover ? 0.6f : 0.35f));
    BeginScissorMode((int)m.box.x, (int)m.box.y, (int)m.box.width, (int)m.box.height);
    for (int i = 0; i < MAX_MODULES; i++) {
        if (!mods[i].alive) continue;
        Rectangle r = module_rect(&mods[i]);
        Vector2 a = map_pt(&m, (Vector2){ r.x, r.y });
        Rectangle mr = { a.x, a.y, fmaxf(r.width * m.s, 2), fmaxf(r.height * m.s, 2) };
        DrawRectangleRec(mr, sel[i] ? fade(ACCENT, 0.28f) : fade(PANEL_HI, 0.45f));
        DrawRectangle((int)mr.x, (int)mr.y, 1, (int)mr.height, fade(WOOD, 0.5f));
        DrawRectangle((int)(mr.x + mr.width - 1), (int)mr.y, 1, (int)mr.height, fade(WOOD, 0.5f));
    }
    for (int c = 0; c < ncables; c++) {
        Cable *k = &cables[c];
        Vector2 a = map_pt(&m, port_pos(&mods[k->fm], true, k->fo));
        Vector2 b = map_pt(&m, port_pos(&mods[k->tm], false, k->ti));
        DrawLineEx(a, b, 1, fade(CABLE_COLORS[k->color], 0.45f));
    }
    Vector2 v = map_pt(&m, (Vector2){ 0, 0 });
    DrawRectangleLinesEx((Rectangle){ v.x, v.y, GetScreenWidth() * m.s, GetScreenHeight() * m.s }, 1, fade(TEXT, hover ? 0.5f : 0.3f));
    EndScissorMode();
}

// --- rack picker geometry ---------------------------------------------------

#define RACK_W 620
#define ROW_H  30

static int filt[PRESET_MAX], nfilt;

static void rack_filter(void)
{
    nfilt = 0;
    for (int i = 0; i < PRESET_MAX; i++)
        if (rack[i].used && preset_matches(i, ui.text)) filt[nfilt++] = i;
    ui.hl = clampi(ui.hl, 0, nfilt ? nfilt - 1 : 0);
}

static Rectangle rack_box(void)
{
    float w = fminf(RACK_W, GetScreenWidth() - 32.0f), h = fminf(640, GetScreenHeight() - 60.0f);
    return (Rectangle){ (GetScreenWidth() - w) / 2, (GetScreenHeight() - h) / 2, w, h };
}
static Rectangle rack_search(void) { Rectangle b = rack_box(); return (Rectangle){ b.x + 16, b.y + 42, b.width - 32, 30 }; }
static Rectangle rack_list(void)   { Rectangle b = rack_box(); return (Rectangle){ b.x + 16, b.y + 84, b.width - 32, b.height - 84 - 36 }; }
static int       rack_visible(void) { int n = (int)(rack_list().height / ROW_H); return n < 1 ? 1 : n; }
static Rectangle rack_row(int v)   { Rectangle l = rack_list(); return (Rectangle){ l.x, l.y + v * ROW_H, l.width, ROW_H }; }
static Rectangle rack_del(Rectangle row) { return (Rectangle){ row.x + row.width - 28, row.y + 7, 16, 16 }; }

static void rack_scroll_to_hl(void)
{
    int vis = rack_visible();
    if (ui.hl < ui.scroll) ui.scroll = ui.hl;
    if (ui.hl >= ui.scroll + vis) ui.scroll = ui.hl - vis + 1;
    ui.scroll = clampi(ui.scroll, 0, nfilt > vis ? nfilt - vis : 0);
}

// --- save dialog geometry ---------------------------------------------------

static Rectangle save_box(void)   { float w = fminf(460, GetScreenWidth() - 32.0f); return (Rectangle){ (GetScreenWidth() - w) / 2, GetScreenHeight() / 2 - 100.0f, w, 200 }; }
static Rectangle save_field(void) { Rectangle b = save_box(); return (Rectangle){ b.x + 16, b.y + 58, b.width - 32, 30 }; }
static Rectangle save_ok(void)    { Rectangle b = save_box(); return (Rectangle){ b.x + b.width - 16 - 100, b.y + b.height - 44, 100, 28 }; }
static Rectangle save_cancel(void){ Rectangle r = save_ok(); return (Rectangle){ r.x - 10 - 100, r.y, 100, 28 }; }

static int next_free_slot(void) { int s = 0; while (s < PRESET_MAX && rack[s].used) s++; return s; }

// --- actions ------------------------------------------------------------------

static void drain_chars(void) { while (GetCharPressed()) {} }

static void open_rack(bool at, Vector2 pos)
{
    ui.modal = MODAL_RACK;
    ui.text[0] = 0;
    ui.hl = ui.scroll = ui.confirmDel = 0;
    ui.placeAt = at; ui.placePos = pos;
    ui.menu = false;
    drain_chars();
}

static void open_save(void)
{
    if (!selection_count()) { toast("SELECT MODULES FIRST: CLICK A HEADER OR SHIFT-DRAG"); return; }
    ui.modal = MODAL_SAVE;
    ui.text[0] = 0;
    ui.menu = false;
    drain_chars();
}

static void load_preset(int slot)
{
    Vector2 at;
    if (ui.placeAt) at = (Vector2){ ui.placePos.x - pan.x, ui.placePos.y - pan.y };
    else {  // to the right of everything already in the rack
        bool any = false;
        float maxX = 0, minY = 0;
        for (int i = 0; i < MAX_MODULES; i++) {
            if (!mods[i].alive) continue;
            float r = mods[i].pos.x + DEFS[mods[i].type].w;
            if (!any || r > maxX) maxX = r;
            if (!any || mods[i].pos.y < minY) minY = mods[i].pos.y;
            any = true;
        }
        at = any ? (Vector2){ maxX + 40, minY } : (Vector2){ 24 - pan.x, 70 - pan.y };
    }
    int n = preset_instantiate(rack[slot].text, at);
    if (!ui.placeAt && n) {   // bring the new modules into view
        float sx = at.x + pan.x, sy = at.y + pan.y;
        if (sx < 0 || sx > GetScreenWidth() - 240 || sy < 0 || sy > GetScreenHeight() - 120) {
            pan.x = 24 - at.x; pan.y = 70 - at.y;
        }
    }
    toast(n ? TextFormat("LOADED %s", preset_label(slot)) : "NO ROOM FOR MORE MODULES");
    ui.modal = MODAL_NONE;
}

static void do_save(void)
{
    if (!ui.text[0]) return;
    int slot = rack_store(ui.text, preset_serialize(ui.text));
    if (slot >= 0) toast(TextFormat("SAVED AS %s", preset_label(slot)));
    else toast(rack_count >= PRESET_MAX ? TextFormat("PRESET RACK IS FULL (%d)", PRESET_MAX) : "COULD NOT WRITE PRESET");
    ui.modal = MODAL_NONE;
}

static void text_input(void)
{
    int c, len = (int)strlen(ui.text);
    while ((c = GetCharPressed()))
        if (c >= 32 && c < 127 && len < PRESET_NAME_MAX) { ui.text[len++] = (char)c; ui.text[len] = 0; }
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && len) ui.text[--len] = 0;
}

static void modal_input(Vector2 mp)
{
    bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (ui.modal == MODAL_SAVE) {
        text_input();
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || (click && CheckCollisionPointRec(mp, save_ok()))) do_save();
        else if (IsKeyPressed(KEY_ESCAPE) || (click && (CheckCollisionPointRec(mp, save_cancel()) || !CheckCollisionPointRec(mp, save_box()))))
            ui.modal = MODAL_NONE;
        return;
    }
    // MODAL_RACK
    char before[sizeof ui.text];
    strcpy(before, ui.text);
    text_input();
    if (strcmp(before, ui.text)) { ui.hl = 0; ui.scroll = 0; ui.confirmDel = 0; }
    rack_filter();
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) { ui.hl = clampi(ui.hl + 1, 0, nfilt ? nfilt - 1 : 0); rack_scroll_to_hl(); }
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP))     { ui.hl = clampi(ui.hl - 1, 0, nfilt ? nfilt - 1 : 0); rack_scroll_to_hl(); }
    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        int vis = rack_visible();
        ui.scroll = clampi(ui.scroll - (int)(wheel > 0 ? 1 : -1) * 2, 0, nfilt > vis ? nfilt - vis : 0);
    }
    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && nfilt) { load_preset(filt[ui.hl]); return; }
    if (IsKeyPressed(KEY_ESCAPE)) { ui.modal = MODAL_NONE; return; }
    if (!click) return;
    if (!CheckCollisionPointRec(mp, rack_box())) { ui.modal = MODAL_NONE; return; }
    int vis = rack_visible();
    for (int v = 0; v < vis && ui.scroll + v < nfilt; v++) {
        Rectangle row = rack_row(v);
        if (!CheckCollisionPointRec(mp, row)) continue;
        int slot = filt[ui.scroll + v];
        if (CheckCollisionPointRec(mp, rack_del(row))) {
            if (ui.confirmDel == slot + 1) {
                toast(TextFormat("DELETED %s", preset_label(slot)));
                rack_delete(slot);
                ui.confirmDel = 0;
                rack_filter();
            } else ui.confirmDel = slot + 1;
        } else load_preset(slot);
        return;
    }
    ui.confirmDel = 0;
}

static Rectangle box_rect(Vector2 a, Vector2 b)
{
    return (Rectangle){ fminf(a.x, b.x), fminf(a.y, b.y), fabsf(a.x - b.x), fabsf(a.y - b.y) };
}

static void handle_input(void)
{
    Vector2 mp = GetMousePosition();
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool cmd = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);

    if (ui.modal) { modal_input(mp); ui.last = mp; return; }

    if (ui.menu) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            bool left = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
            ui.menu = false;
            for (int i = 0; i < M_COUNT; i++)
                if (left && CheckCollisionPointRec(mp, menu_item(i)))
                    add_module(MENU_ORDER[i], ui.menuPos.x - pan.x, ui.menuPos.y - pan.y);
            if (left && CheckCollisionPointRec(mp, menu_item(M_COUNT))) open_rack(true, ui.menuPos);
        }
        if (IsKeyPressed(KEY_ESCAPE)) ui.menu = false;
        return;
    }

    // keyboard
    if (IsKeyPressed(KEY_P) && !cmd) { open_rack(false, mp); return; }
    if (IsKeyPressed(KEY_S) && !cmd) { open_save(); return; }
    if (cmd && IsKeyPressed(KEY_A)) for (int i = 0; i < MAX_MODULES; i++) sel[i] = mods[i].alive;
    if (IsKeyPressed(KEY_ESCAPE)) select_none();

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        int pm, pi; bool po;
        int ki, kk;
        int mi = module_at(mp);
        ui.press = mp;
        if (CheckCollisionPointRec(mp, btn_rack())) { open_rack(false, mp); return; }
        if (selection_count() && CheckCollisionPointRec(mp, btn_save())) { open_save(); return; }
        Minimap mm = minimap();
        if (mm.show && CheckCollisionPointRec(mp, mm.box)) { mapDrag = mm; map_center_view(&mm, mp); ui.mode = D_MAP; ui.last = mp; return; }
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
            else if (mp.y < mods[mi].pos.y + pan.y + HEADER_H) {
                if (shift) { sel[mi] = !sel[mi]; ui.mode = D_NONE; }
                else {
                    if (!sel[mi]) { select_none(); sel[mi] = true; }
                    ui.mode = D_MODULE; ui.mod = mi;
                }
            }
            else ui.mode = D_NONE;
        } else ui.mode = shift ? D_BOX : D_PAN;
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
            float maxY = GetScreenHeight() - 40 - MENU_N * MENU_ITEM_H;
            if (ui.menuPos.y > maxY) ui.menuPos.y = maxY;
            if (ui.menuPos.x > GetScreenWidth() - MENU_W - 4) ui.menuPos.x = GetScreenWidth() - MENU_W - 4;
        }
    }

    Vector2 delta = { mp.x - ui.last.x, mp.y - ui.last.y };
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        switch (ui.mode) {
        case D_MODULE:
            for (int i = 0; i < MAX_MODULES; i++)
                if (mods[i].alive && sel[i]) { mods[i].pos.x += delta.x; mods[i].pos.y += delta.y; }
            break;
        case D_PAN:    pan.x += delta.x; pan.y += delta.y; break;
        case D_MAP: map_center_view(&mapDrag, mp); break;
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
        } else if (ui.mode == D_BOX) {
            Rectangle b = box_rect(ui.press, mp);
            for (int i = 0; i < MAX_MODULES; i++)
                if (mods[i].alive && CheckCollisionRecs(b, module_rect(&mods[i]))) sel[i] = true;
        } else if (ui.mode == D_PAN && hypotf(mp.x - ui.press.x, mp.y - ui.press.y) < 4) {
            select_none();   // a plain click on empty space
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
// Overlays: buttons, rack picker, save dialog, toast
// ---------------------------------------------------------------------------

static void draw_button(Rectangle r, const char *label, Vector2 mp, bool accent)
{
    bool h = CheckCollisionPointRec(mp, r);
    DrawRectangleRec(r, h ? PANEL_HI : PANEL);
    DrawRectangleLinesEx(r, 1, h || accent ? ACCENT_DIM : PANEL_HI);
    txt_c(label, r.x + r.width / 2, r.y + r.height / 2 - 5, 10, h || accent ? ACCENT : TEXT);
}

static void draw_field(Rectangle r, const char *text, const char *placeholder)
{
    DrawRectangleRec(r, INSET);
    DrawRectangleLinesEx(r, 1, ACCENT_DIM);
    float x = r.x + 10, y = r.y + r.height / 2 - 6;
    if (text[0]) txt(text, x, y, 12, TEXT);
    else txt(placeholder, x, y, 12, fade(TEXT_DIM, 0.7f));
    if (fmodf((float)GetTime(), 1.0f) < 0.6f) {
        float cx = x + (text[0] ? txt_w(text, 12) + 2 : 0);
        DrawRectangle((int)cx, (int)y - 2, 2, 16, ACCENT);
    }
}

static void draw_dialog_frame(Rectangle b, const char *title)
{
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){ 2, 6, 6, 150 });
    DrawRectangleRec((Rectangle){ b.x + 8, b.y + 10, b.width, b.height }, (Color){ 0, 0, 0, 120 });
    DrawRectangleGradientH((int)b.x - CHEEK, (int)b.y, CHEEK, (int)b.height, WOOD_DARK, WOOD);
    DrawRectangleGradientH((int)(b.x + b.width), (int)b.y, CHEEK, (int)b.height, WOOD, WOOD_DARK);
    DrawRectangleRec(b, PANEL);
    DrawRectangleRec((Rectangle){ b.x, b.y, b.width, 30 }, (Color){ 33, 35, 37, 255 });
    txt(title, b.x + 16, b.y + 10, 10, TEXT);
}

static void draw_rack(Vector2 mp)
{
    Rectangle b = rack_box();
    draw_dialog_frame(b, "PRESET RACK");
    const char *count = TextFormat("%d / %d", rack_count, PRESET_MAX);
    txt(count, b.x + b.width - 16 - txt_w(count, 10), b.y + 10, 10, TEXT_DIM);
    draw_field(rack_search(), ui.text, "SEARCH BY NAME, NUMBER OR MODULE");

    Rectangle l = rack_list();
    DrawRectangleRec(l, INSET);
    rack_filter();
    if (!rack_ready) txt("LOADING PRESETS...", l.x + 12, l.y + 10, 10, TEXT_DIM);
    else if (!nfilt) txt(rack_count ? "NO PRESET MATCHES" : "THE RACK IS EMPTY: SELECT MODULES AND PRESS S TO SAVE ONE", l.x + 12, l.y + 10, 10, TEXT_DIM);
    int vis = rack_visible();
    ui.scroll = clampi(ui.scroll, 0, nfilt > vis ? nfilt - vis : 0);
    for (int v = 0; v < vis && ui.scroll + v < nfilt; v++) {
        int fi = ui.scroll + v, slot = filt[fi];
        Rectangle row = rack_row(v);
        bool hover = CheckCollisionPointRec(mp, row);
        if (fi == ui.hl || hover) DrawRectangleRec(row, fade(ACCENT, fi == ui.hl ? 0.12f : 0.06f));
        if (fi == ui.hl) DrawRectangle((int)row.x, (int)row.y, 2, (int)row.height, ACCENT);
        txt(preset_label(slot), row.x + 12, row.y + 9, 12, fi == ui.hl ? ACCENT : TEXT);
        Rectangle del = rack_del(row);
        bool armed = ui.confirmDel == slot + 1;
        char sum[100];
        snprintf(sum, sizeof sum, "%s", armed ? "CLICK X AGAIN TO DELETE" : rack[slot].summary);
        float maxw = del.x - 12 - (row.x + 24 + txt_w(preset_label(slot), 12));
        for (int n = (int)strlen(sum); n > 3 && txt_w(sum, 10) > maxw; n--) strcpy(sum + n - 4, "...");
        float sw = txt_w(sum, 10);
        if (sw <= maxw) txt(sum, del.x - 12 - sw, row.y + 10, 10, armed ? CABLE_COLORS[1] : TEXT_DIM);
        if (hover || armed) {
            bool hd = CheckCollisionPointRec(mp, del);
            txt_c("x", del.x + del.width / 2, del.y + 3, 10, hd || armed ? CABLE_COLORS[1] : TEXT_DIM);
        }
    }
    if (nfilt > vis) {   // scrollbar
        float th = l.height * vis / nfilt, ty = l.y + (l.height - th) * ui.scroll / (float)(nfilt - vis);
        DrawRectangle((int)(l.x + l.width - 3), (int)ty, 2, (int)th, ACCENT_DIM);
    }
    txt(ui.placeAt ? "ENTER / CLICK: LOAD HERE    UP/DOWN: SELECT    ESC: CLOSE"
                   : "ENTER / CLICK: LOAD    UP/DOWN: SELECT    ESC: CLOSE",
        b.x + 16, b.y + b.height - 24, 10, TEXT_DIM);
}

static void draw_save(Vector2 mp)
{
    Rectangle b = save_box();
    draw_dialog_frame(b, "SAVE PRESET");
    int nm = selection_count(), nc = 0;
    for (int c = 0; c < ncables; c++) nc += sel[cables[c].fm] && sel[cables[c].tm];
    txt(TextFormat("%d MODULE%s, %d CABLE%s", nm, nm == 1 ? "" : "S", nc, nc == 1 ? "" : "S"), b.x + 16, b.y + 40, 10, TEXT_DIM);
    draw_field(save_field(), ui.text, "NAME, E.G. MY PAD OSCILLATOR");
    int slot = next_free_slot();
    if (slot >= PRESET_MAX) txt(TextFormat("THE RACK IS FULL (%d PRESETS)", PRESET_MAX), b.x + 16, b.y + 100, 10, CABLE_COLORS[1]);
    else if (ui.text[0]) {
        char slug[PRESET_NAME_MAX + 1];
        make_slug(ui.text, slug);
        txt("STORED AS", b.x + 16, b.y + 102, 10, TEXT_DIM);
        txt(TextFormat("#%d-%s", slot + 1, slug), b.x + 16 + txt_w("STORED AS", 10) + 10, b.y + 100, 12, ACCENT);
    }
    draw_button(save_cancel(), "CANCEL", mp, false);
    draw_button(save_ok(), "SAVE", mp, ui.text[0] != 0);
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
    if (!rack_ready && fs_ready()) rack_load();
    handle_input();
    Vector2 mp = GetMousePosition();
    Minimap mm = minimap();
    bool overlay = ui.modal || ui.menu || ui.mode == D_MAP || (mm.show && CheckCollisionPointRec(mp, mm.box));

    int hoverMod = overlay ? -1 : module_at(mp), hoverKnob = -1, km, kk;
    if (!overlay && knob_at(mp, &km, &kk) && km == hoverMod) hoverKnob = kk;
    int hpMod = -1, hpIdx = -1; bool hpOut = false;
    if (!overlay) find_port(mp, &hpMod, &hpIdx, &hpOut);

    BeginDrawing();
    draw_background((float)GetTime());

    for (int i = 0; i < MAX_MODULES; i++)
        if (mods[i].alive) {
            draw_module(i, hoverMod, hoverKnob, (ui.mode == D_KNOB && ui.mod == i) ? ui.idx : -1, hpMod, hpIdx, hpOut);
            if (sel[i]) {
                Rectangle r = module_rect(&mods[i]);
                DrawRectangleLinesEx((Rectangle){ r.x - 3, r.y - 3, r.width + 6, r.height + 6 }, 1.5f, ACCENT);
            }
        }

    for (int c = 0; c < ncables; c++) {
        Cable *k = &cables[c];
        draw_cable(port_pos(&mods[k->fm], true, k->fo), port_pos(&mods[k->tm], false, k->ti), CABLE_COLORS[k->color], 0.92f);
    }
    if (ui.mode == D_CABLE) {
        Vector2 a = port_pos(&mods[ui.mod], ui.fromOut, ui.idx);
        draw_cable(a, mp, CABLE_COLORS[ui.color], 0.75f);
    }
    if (ui.mode == D_BOX) {
        Rectangle b = box_rect(ui.press, mp);
        DrawRectangleRec(b, fade(ACCENT, 0.06f));
        DrawRectangleLinesEx(b, 1, fade(ACCENT, 0.7f));
    }

    // brand, buttons, hints
    txt("R A Y A", 18, 14, 20, TEXT);
    txt("AMBIENT MODULAR", 18, 38, 10, TEXT_DIM);
    Vector2 bmp = overlay ? (Vector2){ -1, -1 } : mp;
    draw_button(btn_rack(), TextFormat("PRESET RACK  %d", rack_count), bmp, false);
    draw_minimap(ui.modal || ui.menu ? (Vector2){ -1, -1 } : mp);
    int ns = selection_count();
    if (ns) draw_button(btn_save(), TextFormat("SAVE %d MODULE%s AS PRESET", ns, ns == 1 ? "" : "S"), bmp, true);
    txt("DRAG JACK TO JACK: PATCH   RIGHT-CLICK JACK: UNPLUG   RIGHT-CLICK SPACE: ADD   DRAG SPACE: PAN   SHIFT-DRAG SPACE: SELECT   P: PRESETS   S: SAVE SELECTION   DOUBLE-CLICK KNOB: RESET",
        18, GetScreenHeight() - 22, 10, fade(TEXT_DIM, 0.8f));

    if (ui.menu) {
        Rectangle box = { ui.menuPos.x, ui.menuPos.y, MENU_W, 38 + MENU_N * MENU_ITEM_H };
        DrawRectangleRec((Rectangle){ box.x + 5, box.y + 6, box.width, box.height }, (Color){ 0, 0, 0, 110 });
        DrawRectangleRec(box, PANEL);
        DrawRectangleLinesEx(box, 1, PANEL_HI);
        txt("ADD MODULE", box.x + 10, box.y + 9, 10, TEXT_DIM);
        for (int i = 0; i < MENU_N; i++) {
            Rectangle it = menu_item(i);
            bool h = CheckCollisionPointRec(mp, it);
            if (i == M_COUNT) DrawLineEx((Vector2){ it.x + 8, it.y - 4 }, (Vector2){ it.x + it.width - 8, it.y - 4 }, 1, PANEL_HI);
            if (h) DrawRectangleRec(it, fade(ACCENT, 0.12f));
            txt(i < M_COUNT ? DEFS[MENU_ORDER[i]].name : "PRESET RACK...", it.x + 12, it.y + 7, 10, h ? ACCENT : TEXT);
        }
    }
    if (ui.modal == MODAL_RACK) draw_rack(mp);
    if (ui.modal == MODAL_SAVE) draw_save(mp);

    float ta = 1.0f - (float)(GetTime() - ui.toastT - 2.0) / 0.6f;
    if (ta > 0) {
        float w = txt_w(ui.toast, 10) + 32, x = (GetScreenWidth() - w) / 2, y = GetScreenHeight() - 62.0f;
        DrawRectangleRec((Rectangle){ x, y, w, 26 }, fade(PANEL, ta));
        DrawRectangleLinesEx((Rectangle){ x, y, w, 26 }, 1, fade(ACCENT_DIM, ta));
        txt(ui.toast, x + 16, y + 8, 10, fade(ACCENT, ta));
    }
    EndDrawing();
}

// The startup patch is factory preset #1.
static void default_layout(void)
{
    char *text = factory_text(0);
    preset_instantiate(text, (Vector2){ 24, 70 });
    free(text);
    select_none();
}

int main(void)
{
    parse_progressions();

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 760, "raya");
    SetExitKey(KEY_NULL);

#if defined(PLATFORM_WEB)
    // Presets live in IndexedDB; mount it and pull its contents in before use.
    emscripten_run_script(
        "try { FS.mkdir('" PRESET_ROOT "'); FS.mount(IDBFS, {}, '" PRESET_ROOT "');"
        "  FS.syncfs(true, function (e) { if (e) console.error('raya: preset load failed', e); Module.rayaFsReady = 1; }); }"
        "catch (e) { console.error('raya: no preset storage', e); Module.rayaFsReady = 1; }");
#endif

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
