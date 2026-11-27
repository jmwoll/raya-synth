// raya — ambient modular synth environment
// Single file, C99, raylib. Build for the web with `make`, see Makefile.
//
// Copyright (C) 2026 Jan Wollschlaeger
// SPDX-License-Identifier: LGPL-2.1-or-later
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License as published by the Free
// Software Foundation, either version 2.1 of the License, or (at your option)
// any later version. See LICENSE and THIRD-PARTY-NOTICES.md.
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
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <dirent.h>
#include <sys/stat.h>
#include "layer3.h"   // shine MP3 encoder

#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif

#define SR           48000.0f
#define TAU          6.28318530718f
#define MAX_MODULES  64
#define MAX_IO       20
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
enum { F_NUM, F_INT, F_HZ, F_SEC, F_BPM, F_SEMI, F_PCT, F_PROG, F_OCT, F_LABEL, F_DB, F_RATIO, F_OCTS, F_SEMIS };

typedef struct {
    const char  *name;
    float        min, max, def;
    int          kind;
    int          fmt;
    const char **labels;   // K_SEG, F_LABEL
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
    void (*fini)(Module *);   // optional: free what init allocated besides st
    // optional: audio stored with the project (tape, sampler)
    unsigned *(*audio_id)(Module *);
    bool (*audio_save)(Module *, const char *path, int how);
    void (*audio_load)(Module *, const char *path);
    // optional: mouse wheel and right-click inside the custom area
    bool (*wheel)(Module *, Vector2 mouse, Rectangle area, float amount);
    bool (*rpress)(Module *, Vector2 mouse, Rectangle area);
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
static volatile bool g_running = true;   // transport: STOP pauses all processing

// In the browser the audio callback runs on the main thread, so the UI may
// hand buffers to a module directly; natively the audio thread takes them.
#if defined(PLATFORM_WEB)
#define UI_OWNS_AUDIO 1
#else
#define UI_OWNS_AUDIO (!g_running)
#endif

enum { SAVE_LATER, SAVE_NOW, SAVE_ALL };   // how urgently a module stores its audio
static volatile int g_route;  // routing copy the audio thread should use
static int    g_cur;          // routing copy used during the current audio block

enum { M_CLOCK, M_SEQ, M_CHORD, M_OSC3, M_LFO, M_REVERB, M_OUT, M_TAPE, M_ARP, M_PERF, M_DRUMS, M_SAMPLER, M_FILTER, M_CHORUS, M_ARR, M_WAVOR, M_COUNT };
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
    "Emaj7 C#m7 G#m7 C#m7",  // from the electro 128 reference track
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
// TAPE — a tape loop that plays, records and overdubs. TIME is the loop
// length and so also the echo time: playback reads one loop behind the
// record head, recording writes input + FEEDBACK x old tape. Loads .wav,
// .mp3, .ogg, .flac and .qoa; exports the loop as .wav or .mp3.
// ---------------------------------------------------------------------------

#define TAPE_INIT_SEC 16     // initial tape, grows with TIME
#define TAPE_MAX_SEC  300    // longest tape; longer files are cut here
#define TAPE_BTN_N    6

static void toast(const char *s);
static int  g_pickTarget = -1;   // tape module waiting for the file picker

typedef struct {
    short *buf; int cap;                     // stereo frames, int16
    short *next; int nextCap; bool nextKeep; // replacement buffer from the UI...
    volatile int swap;                       // ...swapped in by the audio thread
    short *old;                              // retired buffer, freed by the UI
    double h;                                // head position in frames
    float  wph, fph, pplay;
    bool   rec, play, recNow, scrub;
    char   name[48];
    unsigned id;           // names this tape's audio file inside a project
    volatile bool dirty;   // audio changed since it was last stored
    double savedT;
} TapeSt;

static unsigned new_id(void)
{
    static unsigned n;
    return (unsigned)time(NULL) * 2654435761u + ++n * 40503u;
}

static const char *TAPE_BTN[TAPE_BTN_N] = { "REC", "PLAY", "LOAD", "WAV", "MP3", "CLEAR" };

static void tape_init(Module *m)
{
    TapeSt *s = calloc(1, sizeof(TapeSt));
    s->cap = TAPE_INIT_SEC * (int)SR;
    s->buf = calloc((size_t)s->cap * 2, sizeof(short));
    s->rec = s->play = true;   // armed: patch something in and it echoes
    strcpy(s->name, "EMPTY TAPE");
    s->id = new_id();
    m->st = s;
}

static void tape_fini(Module *m)
{
    TapeSt *s = m->st;
    if (!s) return;
    free(s->buf); free(s->next); free(s->old);
    s->buf = s->next = s->old = NULL;
    s->cap = 0;
}

static int tape_len(const Module *m)
{
    const TapeSt *s = m->st;
    return clampi((int)(m->p[0] * SR), 64, s->cap > 64 ? s->cap : 64);
}

static void tape_take(TapeSt *s)   // take the buffer the UI handed over
{
    s->old = s->buf;
    s->buf = s->next; s->cap = s->nextCap; s->next = NULL;
    if (!s->nextKeep) s->h = 0;
    s->swap = 0;
}

static void tape_process(Module *m)
{
    TapeSt *s = m->st;
    if (s->swap) tape_take(s);
    float inL = IN(m, 0), inR = HAS(m, 1) ? IN(m, 1) : inL;
    int L = tape_len(m);
    float speed = m->p[1] * exp2f(IN(m, 4));
    if (HAS(m, 3) && rising(IN(m, 3), &s->pplay)) { s->h = speed < 0 ? L - 1 : 0; s->play = true; }
    bool rec = HAS(m, 2) ? IN(m, 2) > 0.5f : s->rec;
    s->recNow = rec && s->play;
    float mix = m->p[4], lvl = m->p[5];
    if (!s->play || !s->buf) {
        m->out[0] = inL * (1 - mix) * lvl;
        m->out[1] = inR * (1 - mix) * lvl;
        return;
    }
    if (s->h >= L) s->h = fmod(s->h, L);

    // wow and flutter: read slightly ahead of the head by a wobbling amount
    s->wph = fractf(s->wph + 0.43f / SR);
    s->fph = fractf(s->fph + 6.1f / SR);
    float off = 1 + m->p[3] * (60.0f * (1 + sinf(TAU * s->wph)) + 1.5f * (1 + sinf(TAU * s->fph)));
    double rp = fmod(s->h + off, L);
    int   i0 = (int)rp, i1 = (i0 + 1) % L;
    float fr = (float)(rp - i0);
    float wetL = (s->buf[2 * i0]     * (1 - fr) + s->buf[2 * i1]     * fr) / 32768.0f;
    float wetR = (s->buf[2 * i0 + 1] * (1 - fr) + s->buf[2 * i1 + 1] * fr) / 32768.0f;

    if (s->recNow) {   // tape saturation keeps endless overdubs bounded
        int w = (int)s->h;
        if (fabsf(inL) + fabsf(inR) > 1e-4f) s->dirty = true;
        float fb = m->p[2];
        s->buf[2 * w]     = (short)(tanhf(inL + fb * s->buf[2 * w]     / 32768.0f) * 32767.0f);
        s->buf[2 * w + 1] = (short)(tanhf(inR + fb * s->buf[2 * w + 1] / 32768.0f) * 32767.0f);
    }
    s->h += speed;
    if (s->h >= L) s->h -= L;
    if (s->h < 0) s->h += L;
    if (s->h < 0 || s->h >= L) s->h = 0;

    m->out[0] = (inL * (1 - mix) + wetL * mix) * lvl;
    m->out[1] = (inR * (1 - mix) + wetR * mix) * lvl;
}

// Hand a new buffer to the audio thread (UI thread only).
static bool tape_handover(TapeSt *s, short *buf, int cap, bool keepHead)
{
    if (s->swap) { free(buf); return false; }
    s->next = buf; s->nextCap = cap; s->nextKeep = keepHead;
    s->swap = 1;
    return true;
}

// Put an audio file on the tape; returns its length in frames (0 on failure).
static int tape_read_audio(Module *m, const char *path)
{
    TapeSt *s = m->st;
    Wave w = LoadWave(path);
    if (!w.data || !w.frameCount) return 0;
    WaveFormat(&w, (int)SR, 16, 2);
    int n = (int)w.frameCount;
    if (n > TAPE_MAX_SEC * (int)SR) n = TAPE_MAX_SEC * (int)SR;
    int cap = n > TAPE_INIT_SEC * (int)SR ? n : TAPE_INIT_SEC * (int)SR;
    short *b = calloc((size_t)cap * 2, sizeof(short));
    memcpy(b, w.data, (size_t)n * 2 * sizeof(short));
    UnloadWave(w);
    return tape_handover(s, b, cap, false) ? n : 0;
}

static bool tape_load(Module *m, const char *path)
{
    TapeSt *s = m->st;
    int n = tape_read_audio(m, path);
    if (!n) { toast(TextFormat("CANNOT READ %s", GetFileName(path))); return false; }
    m->p[0] = clampf(n / SR, DEFS[m->type].knobs[0].min, DEFS[m->type].knobs[0].max);
    s->rec = false; s->play = true; s->dirty = true;
    snprintf(s->name, sizeof s->name, "%s", GetFileName(path));
    toast(TextFormat("LOADED %s (%.1f S)", GetFileName(path), n / SR));
    return true;
}

static bool write_mp3(const char *path, const short *pcm, int frames)
{
    shine_config_t cfg;
    shine_set_config_mpeg_defaults(&cfg.mpeg);
    cfg.wave.channels = PCM_STEREO;
    cfg.wave.samplerate = (int)SR;
    cfg.mpeg.mode = JOINT_STEREO;
    cfg.mpeg.bitr = 192;
    if (shine_check_config(cfg.wave.samplerate, cfg.mpeg.bitr) < 0) return false;
    shine_t enc = shine_initialise(&cfg);
    if (!enc) return false;
    FILE *f = fopen(path, "wb");
    if (!f) { shine_close(enc); return false; }
    int spp = shine_samples_per_pass(enc), written;
    int16_t *chunk = calloc((size_t)spp * 2, sizeof(int16_t));
    for (int pos = 0; pos < frames; pos += spp) {
        int n = frames - pos < spp ? frames - pos : spp;
        memset(chunk, 0, (size_t)spp * 2 * sizeof(int16_t));
        memcpy(chunk, pcm + 2 * (size_t)pos, (size_t)n * 2 * sizeof(int16_t));
        unsigned char *data = shine_encode_buffer_interleaved(enc, chunk, &written);
        fwrite(data, 1, written, f);
    }
    unsigned char *data = shine_flush(enc, &written);
    fwrite(data, 1, written, f);
    shine_close(enc);
    free(chunk);
    return fclose(f) == 0;
}

#if defined(PLATFORM_WEB)
#define EXPORT_DIR "/tmp"
#else
#define EXPORT_DIR "raya-data/recordings"
#endif

static bool write_wav(const char *path, const short *pcm, int frames, int ch)
{
    Wave w = { .frameCount = (unsigned int)frames, .sampleRate = (unsigned int)SR, .sampleSize = 16, .channels = (unsigned int)ch, .data = (void *)pcm };
    return ExportWave(w, path);
}

// Write 48 kHz stereo audio as raya-<what>-<time>.wav/.mp3 and hand it to the
// user (a download in the browser, raya-data/recordings natively).
static void export_audio(const short *pcm, int frames, bool mp3, const char *what)
{
    char stamp[32], path[256];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    MakeDirectory(EXPORT_DIR);
    snprintf(path, sizeof path, "%s/raya-%s-%s.%s", EXPORT_DIR, what, stamp, mp3 ? "mp3" : "wav");
    if (!(mp3 ? write_mp3(path, pcm, frames) : write_wav(path, pcm, frames, 2))) { toast("COULD NOT WRITE THE AUDIO FILE"); return; }
#if defined(PLATFORM_WEB)
    emscripten_run_script(TextFormat("rayaDownload('%s')", path));
#endif
    toast(TextFormat("SAVED %s (%.1f S)", GetFileName(path), frames / SR));
}

static void tape_export(Module *m, bool mp3)
{
    TapeSt *s = m->st;
    export_audio(s->buf, tape_len(m), mp3, "tape");
}

static Rectangle tape_wave_rect(Rectangle a) { return (Rectangle){ a.x, a.y, a.width, a.height - 28 }; }
static Rectangle tape_btn(Rectangle a, int i)
{
    float bw = (a.width - (TAPE_BTN_N - 1) * 4) / TAPE_BTN_N;
    return (Rectangle){ a.x + i * (bw + 4), a.y + a.height - 22, bw, 22 };
}

static void tape_draw(Module *m, Rectangle a)
{
    TapeSt *s = m->st;
    // housekeeping on the UI thread: free retired buffers, grow the tape for TIME
    if (s->swap && UI_OWNS_AUDIO) tape_take(s);
    if (!s->swap && s->old) { free(s->old); s->old = NULL; }
    int want = clampi((int)(m->p[0] * SR), 64, TAPE_MAX_SEC * (int)SR);
    if (!s->swap && want > s->cap) {
        int cap = want + 8 * (int)SR < TAPE_MAX_SEC * (int)SR ? want + 8 * (int)SR : TAPE_MAX_SEC * (int)SR;
        short *b = calloc((size_t)cap * 2, sizeof(short));
        memcpy(b, s->buf, (size_t)s->cap * 2 * sizeof(short));
        tape_handover(s, b, cap, true);
    }

    Rectangle wr = tape_wave_rect(a);
    DrawRectangleRec(wr, INSET);
    int L = tape_len(m), cols = (int)wr.width - 4;
    float mid = wr.y + wr.height / 2 + 5, amp = (wr.height - 16) / 2;
    float headX = wr.x + 2 + (float)(s->h / L) * cols;
    for (int c = 0; c < cols && s->buf; c++) {
        int f0 = (int)((long long)c * L / cols), f1 = (int)((long long)(c + 1) * L / cols);
        int step = (f1 - f0) / 24 + 1;
        float pk = 0;
        for (int f = f0; f < f1; f += step) {
            float v = fmaxf(fabsf(s->buf[2 * f] / 32768.0f), fabsf(s->buf[2 * f + 1] / 32768.0f));
            if (v > pk) pk = v;
        }
        float h = fmaxf(1, sqrtf(pk) * amp);
        float x = wr.x + 2 + c;
        DrawLineV((Vector2){ x, mid - h }, (Vector2){ x, mid + h }, fade(PHOSPHOR, x < headX ? 0.55f : 0.3f));
    }
    Color hc = s->recNow ? CABLE_COLORS[1] : ACCENT;
    DrawLineEx((Vector2){ headX, wr.y + 14 }, (Vector2){ headX, wr.y + wr.height - 2 }, 1.5f, hc);
    char nm[48];
    snprintf(nm, sizeof nm, "%s", s->name);
    for (int n = (int)strlen(nm); n > 3 && txt_w(nm, 10) > wr.width * 0.6f; n--) strcpy(nm + n - 4, "...");
    txt(nm, wr.x + 6, wr.y + 4, 10, TEXT_DIM);
    const char *t = TextFormat("%.1f / %.1f S", s->h / SR, L / SR);
    txt(t, wr.x + wr.width - 6 - txt_w(t, 10), wr.y + 4, 10, TEXT_DIM);
    if (s->recNow && fmodf((float)GetTime(), 1.0f) < 0.6f) DrawCircleV((Vector2){ wr.x + wr.width - 14 - txt_w(t, 10), wr.y + 9 }, 3, CABLE_COLORS[1]);

    for (int i = 0; i < TAPE_BTN_N; i++) {
        Rectangle b = tape_btn(a, i);
        bool on = (i == 0 && s->rec) || (i == 1 && s->play);
        Color c = i == 0 ? CABLE_COLORS[1] : ACCENT;
        bool hov = CheckCollisionPointRec(GetMousePosition(), b);
        DrawRectangleRec(b, on ? fade(c, 0.18f) : (hov ? PANEL_HI : INSET));
        DrawRectangleLinesEx(b, 1, on ? c : PANEL_HI);
        txt_c(TAPE_BTN[i], b.x + b.width / 2, b.y + 6, 10, on ? c : (hov ? TEXT : TEXT_DIM));
    }
}

static bool tape_press(Module *m, Vector2 mp, Rectangle a)
{
    TapeSt *s = m->st;
    if (!CheckCollisionPointRec(mp, a)) return false;
    s->scrub = false;
    for (int i = 0; i < TAPE_BTN_N; i++) {
        if (!CheckCollisionPointRec(mp, tape_btn(a, i))) continue;
        switch (i) {
        case 0: s->rec = !s->rec; break;
        case 1: s->play = !s->play; break;
        case 2:
#if defined(PLATFORM_WEB)
            g_pickTarget = (int)(m - mods);
            emscripten_run_script("rayaPickFile()");
#else
            toast("DROP AN AUDIO FILE ONTO THE TAPE TO LOAD IT");
#endif
            break;
        case 3: tape_export(m, false); break;
        case 4: tape_export(m, true); break;
        case 5:
            if (s->buf) memset(s->buf, 0, (size_t)s->cap * 2 * sizeof(short));
            strcpy(s->name, "EMPTY TAPE");
            s->dirty = true;
            break;
        }
        return true;
    }
    Rectangle wr = tape_wave_rect(a);
    if (CheckCollisionPointRec(mp, wr)) {   // click the tape to move the head
        s->scrub = true;
        s->h = clampf((mp.x - wr.x - 2) / (wr.width - 4), 0, 0.999f) * tape_len(m);
        return true;
    }
    return false;
}

static void tape_drag(Module *m, Vector2 mp, Rectangle a)
{
    TapeSt *s = m->st;
    Rectangle wr = tape_wave_rect(a);
    if (s->scrub) s->h = clampf((mp.x - wr.x - 2) / (wr.width - 4), 0, 0.999f) * tape_len(m);
}

// data line: rec play id name
static void tape_save(const Module *m, char *out, int cap)
{
    const TapeSt *s = m->st;
    snprintf(out, cap, "%d %d %u %s", s->rec, s->play, s->id, s->name);
}

static unsigned *tape_audio_id(Module *m) { return &((TapeSt *)m->st)->id; }

// Big tapes are written less often while they keep recording.
static bool tape_audio_save(Module *m, const char *path, int how)
{
    TapeSt *s = m->st;
    int L = tape_len(m);
    double now = GetTime();
    if (!s->buf || (how != SAVE_ALL && !s->dirty) || (how == SAVE_LATER && now - s->savedT < 20 + L / SR * 0.5)) return false;
    s->dirty = false;
    s->savedT = now;
    return write_wav(path, s->buf, L, 2);
}

static void tape_audio_load(Module *m, const char *path)
{
    TapeSt *s = m->st;
    tape_read_audio(m, path);
    s->dirty = false;
    s->savedT = GetTime();
}

static void tape_load_data(Module *m, const char *str)
{
    TapeSt *s = m->st;
    int rec, play, used = 0;
    unsigned id;
    int n = sscanf(str, "%d %d %u %n", &rec, &play, &id, &used);
    if (n >= 2) { s->rec = rec != 0; s->play = play != 0; }
    if (n == 3) {
        s->id = id;
        if (used && str[used]) snprintf(s->name, sizeof s->name, "%s", str + used);
    }
}

// ---------------------------------------------------------------------------
// ARP — arpeggiates the chord tones patched into N1..N4 (e.g. from CHORDS)
// ---------------------------------------------------------------------------

enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_DOWNUP, ARP_RANDOM, ARP_WALK, ARP_CONVERGE, ARP_DIVERGE, ARP_PLAYED, ARP_NMODES };
static const char *ARP_MODES[ARP_NMODES] = { "UP", "DOWN", "UP-DOWN", "DOWN-UP", "RANDOM", "WALK", "CONVERGE", "DIVERGE", "AS PLAYED" };

#define ARP_POOL 16

typedef struct {
    int   step, cnt, walk, cur;
    float pclk, prst, iph;
    int   since, period, gateLeft, gap;
    float note;
    float pool[ARP_POOL];
    int   npool;
} ArpSt;

static void arp_init(Module *m)
{
    ArpSt *s = calloc(1, sizeof(ArpSt));
    s->period = (int)(SR * 0.4f);
    s->note = -0.25f;   // A3
    s->cur = -1;
    m->st = s;
}

// Chord tones (sorted unless AS PLAYED), spread over OCTAVES.
static int arp_pool(Module *m, float *pool)
{
    float base[4];
    int k = 0;
    for (int i = 0; i < 4; i++) if (HAS(m, i)) base[k++] = IN(m, i);
    if (!k) {   // nothing patched: A minor 7
        static const int def[4] = { 57, 60, 64, 67 };
        for (int i = 0; i < 4; i++) base[k++] = (def[i] - 60) / 12.0f;
    }
    if ((int)roundf(m->p[0]) != ARP_PLAYED)
        for (int i = 1; i < k; i++)
            for (int j = i; j > 0 && base[j] < base[j - 1]; j--) { float t = base[j]; base[j] = base[j - 1]; base[j - 1] = t; }
    int oct = clampi((int)roundf(m->p[1]), 1, 4), n = 0;
    for (int o = 0; o < oct; o++)
        for (int i = 0; i < k; i++) pool[n++] = base[i] + o;
    return n;
}

static int arp_index(int mode, int i, int n, int *walk)
{
    if (n <= 1) return 0;
    int p = i % n;
    switch (mode) {
    case ARP_DOWN:     return n - 1 - p;
    case ARP_UPDOWN:   { int q = i % (2 * n - 2); return q < n ? q : 2 * n - 2 - q; }
    case ARP_DOWNUP:   { int q = i % (2 * n - 2); return n - 1 - (q < n ? q : 2 * n - 2 - q); }
    case ARP_RANDOM:   return clampi((int)(frand() * n), 0, n - 1);
    case ARP_WALK:     *walk += frand() < 0.5f ? -1 : 1;
                       if (*walk < 0) *walk = 1;
                       if (*walk >= n) *walk = n - 2;
                       return *walk;
    case ARP_CONVERGE: return p % 2 == 0 ? p / 2 : n - 1 - p / 2;
    case ARP_DIVERGE:  { int q = n - 1 - p; return q % 2 == 0 ? q / 2 : n - 1 - q / 2; }
    default:           return p;   // UP, AS PLAYED
    }
}

static void arp_process(Module *m)
{
    ArpSt *s = m->st;
    float clk;
    if (HAS(m, 4)) clk = IN(m, 4);
    else {   // free-running eighths at 72 BPM
        s->iph += 72.0f * 2.0f / 60.0f / SR;
        if (s->iph >= 1) s->iph -= 1;
        clk = s->iph < 0.5f;
    }
    if (rising(IN(m, 5), &s->prst)) { s->step = 0; s->cnt = 0; s->walk = 0; }
    s->since++;
    if (rising(clk, &s->pclk)) {
        if (s->since > 16) s->period = s->since;
        s->since = 0;
        int div = clampi((int)roundf(m->p[2]), 1, 8);
        if (s->cnt++ % div == 0) {
            s->npool = arp_pool(m, s->pool);
            int idx = arp_index((int)roundf(m->p[0]), s->step++, s->npool, &s->walk);
            if (frand() < m->p[4]) {
                s->note = s->pool[idx] + roundf(m->p[5]);
                s->cur = idx;
                s->gateLeft = (int)(s->period * div * m->p[3]);
                s->gap = 48;   // a short low so every note retriggers
            } else s->cur = -1;
        }
    }
    m->out[0] = s->note;
    m->out[1] = s->gap > 0 ? 0.0f : (s->gateLeft > 0 ? 1.0f : 0.0f);
    if (s->gap > 0) s->gap--;
    else if (s->gateLeft > 0) s->gateLeft--;
}

static void arp_draw(Module *m, Rectangle a)
{
    ArpSt *s = m->st;
    DrawRectangleRec(a, INSET);
    int mode = clampi((int)roundf(m->p[0]), 0, ARP_NMODES - 1);
    txt(ARP_MODES[mode], a.x + 8, a.y + 6, 10, ACCENT);
    const char *info = TextFormat("%d NOTES", s->npool);
    txt(info, a.x + a.width - 8 - txt_w(info, 10), a.y + 6, 10, TEXT_DIM);
    if (!s->npool) return;
    float lo = s->pool[0], hi = s->pool[0];
    for (int i = 1; i < s->npool; i++) { lo = fminf(lo, s->pool[i]); hi = fmaxf(hi, s->pool[i]); }
    Rectangle r = { a.x + 8, a.y + 22, a.width - 16, a.height - 28 };
    float bw = r.width / s->npool;
    for (int i = 0; i < s->npool; i++) {
        float t = hi > lo ? (s->pool[i] - lo) / (hi - lo) : 0.5f;
        float h = 4 + t * (r.height - 4);
        bool on = i == s->cur && s->gateLeft > 0;
        DrawRectangleRec((Rectangle){ r.x + i * bw + 2, r.y + r.height - h, bw - 4, h }, on ? ACCENT : (i == s->cur ? ACCENT_DIM : (Color){ 60, 72, 68, 255 }));
    }
}

// ---------------------------------------------------------------------------
// PERFORMER — plays solo phrases in a scale and key. Phrases are written in
// scale degrees, so each one works in every key and scale:
//   3      degree 3 of the scale for one clock pulse (1 = root, 6 = root an
//          octave up in a pentatonic scale, 8 in a seven-note mode)
//   3--    the same note held for three pulses       .   a one-pulse rest
//   ~3     slide up into the note                    3,  3'  octave down/up
//   3b     blue note: a semitone up, only in the blues scales
// ---------------------------------------------------------------------------

enum { SC_AUTO, SC_MINP, SC_MAJP, SC_MINB, SC_MAJB, SC_ION, SC_DOR, SC_PHR, SC_LYD, SC_MIX, SC_AEO, SC_LOC, SC_COUNT };
static const char *SCALE_LABELS[SC_COUNT] = {
    "AUTO", "MIN PENTA", "MAJ PENTA", "MIN BLUES", "MAJ BLUES", "IONIAN", "DORIAN",
    "PHRYGIAN", "LYDIAN", "MIXOLYDIAN", "AEOLIAN", "LOCRIAN" };
static const struct { int n, st[7]; } SCALES[SC_COUNT] = {
    [SC_MINP] = { 5, { 0, 3, 5, 7, 10 } },       [SC_MAJP] = { 5, { 0, 2, 4, 7, 9 } },
    [SC_MINB] = { 5, { 0, 3, 5, 7, 10 } },       [SC_MAJB] = { 5, { 0, 2, 4, 7, 9 } },
    [SC_ION]  = { 7, { 0, 2, 4, 5, 7, 9, 11 } }, [SC_DOR]  = { 7, { 0, 2, 3, 5, 7, 9, 10 } },
    [SC_PHR]  = { 7, { 0, 1, 3, 5, 7, 8, 10 } }, [SC_LYD]  = { 7, { 0, 2, 4, 6, 7, 9, 11 } },
    [SC_MIX]  = { 7, { 0, 2, 4, 5, 7, 9, 10 } }, [SC_AEO]  = { 7, { 0, 2, 3, 5, 7, 8, 10 } },
    [SC_LOC]  = { 7, { 0, 1, 3, 5, 6, 8, 10 } },
};

static const char *PHRASES[] = {
    // 1-32: minor pentatonic — calls, answers and sighs
    "1--- 2- 3--- . 4----- . . .",          "4--- 3- 2- 1----- . . 5, 1---",
    "6--- 5- 4--- . 3- 4------ . .",        "1- 2- 4--- . . 5- 4- 2------ .",
    "4- 5- 6---- . 5- 4--- 2- 1----",       "~2--- 1- 5,----- . . 1------",
    "3- 4- 5- 4- 2--- . . 1------",         "6-- 5- 6- 7---- . 6- 4------",
    "1--- . 4--- . 5--- . 6-------",        "~4----- 3- 2- 3- 1------- .",
    "2- 1- 5,- 1--- . . 2- 3- 4------",     "7--- 6- 5--- 4- 5------- . .",
    "4- . 4- 5- 6--- 5- 4- 2-----",         "1-- 2-- 3-- 4------ . . 3- 2----",
    "~6------- 5- 4- 5------- . .",         "4,- 5,- 1--- 2- 1- 5,------",
    "3--- 2- 1--- . 2- 3- 4- 3-----",       "8--- 7- 6--- . 5- 6------- .",
    "1- 4- 2- 5- 3- 6----- . . .",          "5- 4- 2- 1- 2- 4--- . 1------",
    "~4--- . ~4--- . 5- 4- 2------",        "6- 4- 6- 7--- 6- 5- 4------",
    "2--- 3--- 4--- 5--- 6-------",         "6--- 5--- 4--- 2--- 1-------",
    "1- 2- 1- 5,--- . 4,- 5,- 1------",     "4-- 6-- 5-- 4-- 2-- 1------",
    "~7----- 6- 4--- . 6--- 5------",       "3- 2- 3- 4---- . 2- 1- 2------",
    "1------- . . 4------- . . 2-------",   "6- 7- 9----- . 8- 7- 6-------",
    "4- 2- 4- 5- 4- 2- 1- 5,-------",       "2--- . 1- 2- 3--- . 2------ .",
    // 33-64: major pentatonic — open, hopeful lines
    "3--- 2- 1--- . 5,- 1-------",          "1- 2- 3- 4--- . 5- 4------ .",
    "6--- 5- 4- 3--- . 2- 3-------",        "4- 5- 6- 7- 8----- . 7- 6-----",
    "3-- 4-- 5-- 4----- . 3- 2- 1----",     "~3----- 2- 1- 2------- . .",
    "5- 4- 3- 4- 5----- . 6-------",        "1--- 4--- 3--- 2- 1-------",
    "8--- 6--- 7--- 5--- 6-------",         "2- 3- 5- 3- 2- 1--- . 5,------",
    "4,- 5,- 1- 2- 3------ . 2- 1----",     "3- . 3- 4- 5--- 4- 3- 2------",
    "6- 5- 4--- . 5- 4- 3--- . 1------",    "~5------- 4- 3- 4------- . .",
    "1-- 3-- 4-- 6------ . 5- 4-----",      "7- 6- 5- 6--- . 4- 3- 4------",
    "3--- . 5--- . 4- 3- 2- 3------",       "1- 2- 3- 2- 1- 5,- 4,------- .",
    "4--- 6--- 8------- . 7- 6-----",       "2-- 1-- 5,-- 1------- . . .",
    "5- 6- 5- 4- 3- 4--- . 3------",        "8- 7- 6- 5- 4--- . 5- 6-------",
    "3- 4- 3- 2--- . 1- 2- 3-------",       "~4----- 5- 6----- . 5- 4------",
    "1------ . 3------ . 4------- .",       "6- 4- 3- 1- 2--- 3- 4-------",
    "4,- 1- 2- 3- 4- 5- 6------- .",        "7--- 8--- 9------- . 8- 7------",
    "3- 2- 3- 5- 4--- . 3- 2- 1-----",      "5--- 3--- 4--- 2--- 3-------",
    "1- 3- 2- 4- 3- 5- 4------- .",         "6------- 5- 4- 3------- . .",
    // 65-96: minor blues — the blue note (b5) as a passing tone
    "1- 2- 3- 3b- 4------ . 2- 1----",      "4- 3b- 3- 2--- . 1------- .",
    "~2b- 2- 1--- . 5,- 1------",           "6- 5- 4- 3b- 3- 2- 1------",
    "3- 3b- 4--- . 3- 3b- 4--- . 6-----",   "~4--- 3b- 3- 2- 1- 5,------",
    "1- . 2- 2b- 3--- . 2- 1------",        "8b- 8- 7- 6--- . 5- 6-------",
    "4--- 3b--- 3--- 2------- .",           "~6--- 5- 4- 3b- 4------- . .",
    "2- 3- 3b- 3- 2- 1- 5,- 1-----",        "5- 6- 5- 4- 3b- 4------ . .",
    "1--- ~2b- 3--- . 4- 3b- 3------",      "6- 4- 3b- 3- 4- 2- 1-------",
    "~7--- 6- 5- 4--- . 3b- 4------",       "3- 3- 3b- 4- . 3- 2- 1------",
    "1- 2- 3- 3b- 4- 5- 6------- .",        "6- 5- 4- 3b- 3- 2- 1- 5,------",
    "~4------- 3b- 3- 2------- .",          "2- 1- 2- 2b- 3--- . 2- 1------",
    "4- 6- 5- 4- 3b--- 3- 2------",         "8- 7- 6- 5- 6--- 4- 3b- 4------",
    "1--- 3b--- 4--- 5- 6-------",          "~3b--- 4- 3b- 3- 2- 1-------",
    "5- 4- 3b- 4- 5- 6--- . 5------",       "1- 5,- 1- 2- 2b- 3- 4-------",
    "6--- . 6- 5- 4--- . 3b- 3- 2-----",    "4- 4- 3b- 3- 2- 3- 1-------",
    "~6- 7- 8b- 8--- . 7- 6------",         "3--- 3b- 4- 3- 2- 1--- . 5,-----",
    "2-- 3-- 3b-- 4------ . 2- 1----",      "~1- 2b- 3- 2- 1- 5,- 4,-------",
    // 97-112: major blues — the minor third (2b) and flat seventh (5b)
    "1- 2- 2b- 3--- . 5- 3- 1------",       "3- 2b- 2- 1--- . 5,- 1-------",
    "5- 5b- 5- 4- 3--- . 2- 3------",       "~2b- 3--- 4- 3- 2- 1-------",
    "6- 5b- 5- 4--- . 3- 2b- 3------",      "1--- 2b- 3- 4- 5- 4------- .",
    "4- 3- 2b- 2- 1- 2- 3-------",          "~5b--- 5- 4- 3- 4------- .",
    "3- 4- 5- 6- 5b- 5- 4------",           "2- 2b- 3- 1--- . 5,- 1------",
    "8- 7b- 7- 6--- . 5- 6-------",         "1- 3- 2b- 3- 4--- . 3- 1------",
    "~3------ 2b- 2- 1------- .",           "6--- 5b- 5- 3--- . 4- 3-----",
    "4- 5- 5b- 5- 6- 8------- .",           "3- 2b- 1- 5,- 1- 2b- 3-------",
    // 113-128: minor pentatonic — long tones and wide space
    "1------- . . . . 4------- . . . .",    "4------- 2------- . . 1----------",
    "6------- . . 5--- 4----------- . .",   "~2------- 1------- . . . 5,--------",
    "3--- 4------- . . . 2-----------",     "5------ 4------ 2------ 1------",
    "4----------- . . . . 6-----------",    "1--- . . . 2--- . . . 4----------",
    "~4------- . . 3--- 2------- . .",      "7------- 6------- 4-----------",
    "2----- 4----- 6------- . . . .",       "6--- 4--- 2--- 1-----------",
    "~5--------- 4--- 2-------- . .",       "1------ 4,------ 5,------ 1-------",
    "3-------- . . 4-------- . . 2--------", "6----------- . . . ~7--- 6-----------",
    // 129-137: ionian
    "3--- 2- 1--- . 5- 4- 3-------",        "1- 2- 3- 5--- . 6- 5------- .",
    "5--- 4- 3- 2--- 1- 7,- 1------",       "8--- 7- 6- 5--- . 3- 4- 3------",
    "1--- 3--- 5--- 7- 8-------",           "~4--- 3- 2- 3------- . 5----",
    "6- 5- 4- 3- 2- 3--- . 1------",        "3- 4- 5- 7- 8----- . 7- 5-----",
    "2------ . 3------ . 5--------",
    // 138-148: dorian — the bright sixth in a minor mode
    "1- 2- 3- 4- 5- 6--- . 5- 3------",     "6--- 5- 4- 3--- . 1- 3-------",
    "~3--- 4- 5- 6------ . 5- 4----",       "8- 7- 6- 5--- 6- 4- 5-------",
    "1--- 6,--- 7,- 1- 2- 3-------",        "5- 6- 8- 6- 5--- . 3- 4-------",
    "4- 3- 1- 6,--- . 7,- 1-------",        "~6------- 5- 6- 7- 6------ .",
    "3- 5- 6- 5- 3--- 2- 1-------",         "2--- 3--- 6------- . 5- 4-----",
    "1- 3- 5- 6- 7- 6- 5------- .",
    // 149-157: phrygian — the dark flat second
    "1- 2--- 1--- . 7,- 1-------",          "~2------- 1- 7,- 1------- .",
    "5- 6- 5- 4- 3- 2- 1-------",           "1--- 2--- 3--- 2- 1-------",
    "8- 7- 6- 5--- . 6- 5------",           "3- 2- 3- 4--- . 2- 1------",
    "5--- 6- 5- 2--- . 1-------",           "~8----- 9- 8- 7------ . .",
    "1- 2- 1- 7,- 6,--- 7,- 1------",
    // 158-168: lydian — the floating raised fourth
    "3- 4- 5--- . 4- 3- 2------",           "1- 2- 3- 4------- . 3- 2- 1----",
    "~4------- 5- 7- 8------- .",           "5--- 4- 3- 4--- . 7,- 1-------",
    "8- 7- 5- 4--- . 5- 3-------",          "2- 3- 4- 6- 7- 8------- . .",
    "4--- . 4- 5- 6- 5- 4------",           "1- 5- 4- 3- 2------- . 1----",
    "~7------ 8- 9- 11------- .",           "3--- 4--- 5------- . 4- 2-----",
    "6- 5- 4- 2- 3------- . . 1------",
    // 169-177: mixolydian — major with a flat seventh
    "5- 6- 7--- . 6- 5- 4- 3------",        "1- 3- 5- 7------- . 6- 5----",
    "~7--- 8- 7- 5--- . 4- 3------",        "3- 4- 5- 6- 7- 6- 5------- .",
    "8--- 7--- 5--- 4- 3-------",           "1--- 7,--- 1- 2- 3------ .",
    "5- 7- 8- 9--- . 8- 7- 5-----",         "~3----- 4- 5- 7------- . .",
    "2- 3- 1- 7,--- 6,- 7,- 1------",
    // 178-186: aeolian — natural minor, the flat sixth
    "1- 2- 3- 5--- . 6- 5------",           "~6--- 5- 4- 3--- 2- 1------",
    "5--- 6--- 5- 3- 4-------",             "8- 7- 6- 5- 4- 3- 1-------",
    "3--- . 2- 3- 4- 5- 6-------",          "1--- 5--- 6------- . 5- 3----",
    "~3------- 2- 1- 7,------- .",          "6- 5- 6- 8--- . 7- 6- 5-----",
    "4- 3- 2- 3- 1--- . 6,- 7,- 1-----",
    // 187-192: locrian — the unsettled flat fifth
    "1- 2- 3--- . 2- 1- 7,-------",         "5--- 4- 3- 2--- 1-------",
    "~2----- 1- 7,- 6,------- .",           "3- 5- 6- 5- 3--- . 2- 1-----",
    "8- 7- 5--- . 4- 3- 2------",           "1------ . 2------ . 5---------",
};
#define NPHRASE ((int)(sizeof PHRASES / sizeof PHRASES[0]))

// The scale each block of phrases is written for (AUTO plays it there).
static const struct { int end, scale; } PH_FAMILY[] = {
    { 32, SC_MINP }, { 64, SC_MAJP }, { 96, SC_MINB }, { 112, SC_MAJB }, { 128, SC_MINP },
    { 137, SC_ION }, { 148, SC_DOR }, { 157, SC_PHR }, { 168, SC_LYD }, { 177, SC_MIX },
    { 186, SC_AEO }, { 192, SC_LOC },
};

static int phrase_scale(int ph)
{
    for (int i = 0; i < (int)(sizeof PH_FAMILY / sizeof PH_FAMILY[0]); i++)
        if (ph < PH_FAMILY[i].end) return PH_FAMILY[i].scale;
    return SC_MINP;
}

#define PH_MAX_EV 48

typedef struct { signed char deg, oct; unsigned char len; bool rest, blue, slide; } PhEv;
typedef struct { int n, units; PhEv ev[PH_MAX_EV]; } Phrase;
static Phrase phrases[NPHRASE];

static void parse_phrases(void)
{
    for (int p = 0; p < NPHRASE; p++) {
        Phrase *ph = &phrases[p];
        for (const char *s = PHRASES[p]; *s && ph->n < PH_MAX_EV; ) {
            while (*s == ' ') s++;
            if (!*s) break;
            PhEv e = { 0 };
            if (*s == '.') { e.rest = true; e.len = 1; s++; }
            else {
                if (*s == '~') { e.slide = true; s++; }
                while (isdigit((unsigned char)*s)) e.deg = (signed char)(e.deg * 10 + (*s++ - '0'));
                for (;; s++) {
                    if (*s == ',') e.oct--;
                    else if (*s == '\'') e.oct++;
                    else if (*s == 'b') e.blue = true;
                    else break;
                }
                e.len = 1;
                while (*s == '-') { e.len++; s++; }
                if (e.deg < 1) e.deg = 1;
            }
            ph->ev[ph->n++] = e;
            ph->units += e.len;
            while (*s && *s != ' ') s++;
        }
    }
}

static int scale_semi(int sc, int idx)
{
    int n = SCALES[sc].n;
    int o = idx >= 0 ? idx / n : -((-idx + n - 1) / n);
    return SCALES[sc].st[idx - o * n] + 12 * o;
}

static int scale_quantize(int sc, int semi)
{
    int pc = ((semi % 12) + 12) % 12, best = 0, bd = 99;
    for (int k = 0; k < SCALES[sc].n; k++)
        for (int w = -12; w <= 12; w += 12) {
            int c = SCALES[sc].st[k] + w, d = abs(c - pc);
            if (d < bd) { bd = d; best = c; }
        }
    return semi - pc + best;
}

// Semitones above the root for an event, in the selected scale.
static int phrase_semi(int ph, int sel, const PhEv *e, int degOff)
{
    int home = phrase_scale(ph), sc = sel == SC_AUTO ? home : sel;
    int semi = scale_semi(home, e->deg - 1 + degOff) + 12 * e->oct;
    if (sc != home) semi = scale_quantize(sc, semi);
    if (e->blue && (sc == SC_MINB || sc == SC_MAJB)) semi += 1;
    return semi;
}

typedef struct {
    int   ph, ev, units, cur;
    bool  resting;
    float pclk, prst, iph;
    int   since, period, gateLeft, gap;
    float target;
    int   slideLeft, slideLen;
} PerfSt;

static void perf_init(Module *m)
{
    PerfSt *s = calloc(1, sizeof(PerfSt));
    s->ph = -1; s->ev = -1; s->cur = -1;
    s->period = (int)(SR * 0.4f);
    m->st = s;
}

static int perf_root(Module *m)   // MIDI note of the key's root, F#3..F4
{
    int pc = HAS(m, 2) ? (((int)roundf(IN(m, 2) * 12)) % 12 + 12) % 12 : clampi((int)roundf(m->p[0]), 0, 11);
    return pc <= 6 ? 60 + pc : 48 + pc;
}

static void perf_tick(Module *m)
{
    PerfSt *s = m->st;
    int ph = clampi((int)roundf(m->p[2]), 0, NPHRASE - 1);
    if (ph != s->ph) { s->ph = ph; s->ev = -1; s->units = 0; s->resting = false; }
    if (s->units > 0 && --s->units > 0) return;
    const Phrase *P = &phrases[ph];
    if (s->resting) { s->resting = false; s->ev = -1; }
    if (++s->ev >= P->n) {   // phrase done: breathe for SPACE pulses, then again
        int space = (int)roundf(m->p[3]);
        s->ev = -1; s->cur = -1;
        if (space > 0) { s->resting = true; s->units = space; return; }
        s->ev = 0;
    }
    const PhEv *e = &P->ev[s->ev];
    s->units = e->len;
    s->cur = s->ev;
    if (e->rest) return;
    int degOff = 0, octOff = 0;
    float vary = m->p[4];
    if (frand() < vary * 0.5f) {   // variation: leave a gap, step aside, or jump an octave
        float r = frand();
        if (r < 0.3f) { s->cur = -1; return; }
        if (r < 0.7f) degOff = frand() < 0.5f ? -1 : 1;
        else octOff = frand() < 0.5f ? -1 : 1;
    }
    int midi = perf_root(m) + phrase_semi(ph, clampi((int)roundf(m->p[1]), 0, SC_COUNT - 1), e, degOff) + 12 * octOff;
    s->target = (midi - 60) / 12.0f + roundf(m->p[6]);
    int len = s->period * e->len;
    s->slideLen = s->slideLeft = e->slide ? (int)(len * 0.35f) : 0;
    s->gateLeft = (int)(len * m->p[5]);
    s->gap = 48;
}

static void perf_process(Module *m)
{
    PerfSt *s = m->st;
    float clk;
    if (HAS(m, 0)) clk = IN(m, 0);
    else {   // free-running eighths at 72 BPM
        s->iph += 72.0f * 2.0f / 60.0f / SR;
        if (s->iph >= 1) s->iph -= 1;
        clk = s->iph < 0.5f;
    }
    if (rising(IN(m, 1), &s->prst)) { s->ev = -1; s->units = 0; s->resting = false; }
    s->since++;
    if (rising(clk, &s->pclk)) {
        if (s->since > 16) s->period = s->since;
        s->since = 0;
        perf_tick(m);
    }
    float p = s->target;
    if (s->slideLeft > 0) { p -= (1.0f / 12.0f) * s->slideLeft / s->slideLen; s->slideLeft--; }
    m->out[0] = p;
    m->out[1] = s->gap > 0 ? 0.0f : (s->gateLeft > 0 ? 1.0f : 0.0f);
    if (s->gap > 0) s->gap--;
    else if (s->gateLeft > 0) s->gateLeft--;
}

static void perf_draw(Module *m, Rectangle a)
{
    PerfSt *s = m->st;
    DrawRectangleRec(a, INSET);
    int ph = clampi((int)roundf(m->p[2]), 0, NPHRASE - 1), sel = clampi((int)roundf(m->p[1]), 0, SC_COUNT - 1);
    int sc = sel == SC_AUTO ? phrase_scale(ph) : sel;
    int root = perf_root(m);
    txt(TextFormat("PHRASE %03d  %s", ph + 1, SCALE_LABELS[phrase_scale(ph)]), a.x + 8, a.y + 6, 10, TEXT_DIM);
    const char *key = TextFormat("%s %s", NOTE_NAMES[root % 12], SCALE_LABELS[sc]);
    txt(key, a.x + a.width - 8 - txt_w(key, 10), a.y + 6, 10, ACCENT);

    // the phrase as a small piano roll
    const Phrase *P = &phrases[ph];
    int lo = 99, hi = -99;
    for (int i = 0; i < P->n; i++)
        if (!P->ev[i].rest) { int v = phrase_semi(ph, sel, &P->ev[i], 0); lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
    if (lo > hi) return;
    Rectangle r = { a.x + 8, a.y + 22, a.width - 16, a.height - 28 };
    float uw = r.width / (P->units ? P->units : 1), x = r.x;
    for (int i = 0; i < P->n; i++) {
        const PhEv *e = &P->ev[i];
        float w = e->len * uw;
        if (!e->rest) {
            int v = phrase_semi(ph, sel, e, 0);
            float t = hi > lo ? (float)(v - lo) / (hi - lo) : 0.5f;
            float y = r.y + (1 - t) * (r.height - 4);
            bool on = i == s->cur && s->ph == ph;
            Color c = on ? ACCENT : (e->blue && (sc == SC_MINB || sc == SC_MAJB) ? fade(CABLE_COLORS[0], 0.8f) : (Color){ 70, 96, 86, 255 });
            DrawRectangleRec((Rectangle){ x + 1, y, fmaxf(w - 2, 2), 4 }, c);
        }
        x += w;
    }
}

// ---------------------------------------------------------------------------
// Samples — shared by SAMPLER and DRUMS: 16-bit audio at SR with a display
// summary, a voice that plays a region of it, and a waveform view coloured
// by frequency content (warm = low, cool = high).
// ---------------------------------------------------------------------------

#define SUM_BLOCK 256

typedef struct {
    short *data;              // interleaved, ch channels
    int    frames, ch;
    unsigned char *pk, *br;   // per SUM_BLOCK frames: peak and brightness, 0..255
    int    nsum;
} Sample;

static void sample_free(Sample *s)
{
    free(s->data); free(s->pk); free(s->br);
    memset(s, 0, sizeof *s);
}

// Peak and brightness per block. Brightness estimates the dominant frequency
// from how fast the signal moves: for a sine, rms(slope) / rms = 2 sin(pi f / SR).
static void sample_summarize(Sample *s)
{
    free(s->pk); free(s->br);
    s->nsum = (s->frames + SUM_BLOCK - 1) / SUM_BLOCK;
    s->pk = calloc(s->nsum ? s->nsum : 1, 1);
    s->br = calloc(s->nsum ? s->nsum : 1, 1);
    float prev = 0;
    for (int b = 0; b < s->nsum; b++) {
        int f0 = b * SUM_BLOCK, f1 = f0 + SUM_BLOCK < s->frames ? f0 + SUM_BLOCK : s->frames;
        float pk = 0;
        double e = 0, d = 0;
        for (int f = f0; f < f1; f++) {
            float x = 0;
            for (int c = 0; c < s->ch; c++) x += s->data[(size_t)f * s->ch + c];
            x /= 32768.0f * s->ch;
            pk = fmaxf(pk, fabsf(x));
            e += x * x;
            d += (x - prev) * (x - prev);
            prev = x;
        }
        s->pk[b] = (unsigned char)clampf(pk * 255, 0, 255);
        if (e > 1e-9) {
            float r = clampf((float)sqrt(d / e) / 2, 0, 1);
            float hz = asinf(r) * SR / 3.14159265f;
            s->br[b] = (unsigned char)(clampf(log2f(fmaxf(hz, 1) / 40.0f) / log2f(12000.0f / 40.0f), 0, 1) * 255);
        }
    }
}

// float [-1, 1] -> Sample (mono)
static void sample_from_float(Sample *s, const float *y, int n)
{
    sample_free(s);
    s->ch = 1;
    s->frames = n;
    s->data = malloc((size_t)(n ? n : 1) * sizeof(short));
    for (int i = 0; i < n; i++) s->data[i] = (short)(clampf(y[i], -1, 1) * 32767.0f);
    sample_summarize(s);
}

static Color spectrum_color(float t)   // 0 = low, 1 = high
{
    static const Color stops[4] = { { 206, 104, 112, 255 }, { 224, 164, 76, 255 }, { 104, 228, 150, 255 }, { 140, 188, 238, 255 } };
    t = clampf(t, 0, 1) * 3;
    int i = t >= 3 ? 2 : (int)t;
    float f = t - i;
    Color a = stops[i], b = stops[i + 1];
    return (Color){ (unsigned char)(a.r + (b.r - a.r) * f), (unsigned char)(a.g + (b.g - a.g) * f), (unsigned char)(a.b + (b.b - a.b) * f), 255 };
}

// Waveform of frames [start, start + span) in r: height = level, colour = brightness.
static void wave_view(const Sample *s, Rectangle r, double start, double span, float alpha)
{
    if (!s->data || s->frames <= 0 || span <= 0) return;
    int cols = (int)r.width;
    double fpc = span / cols;
    float mid = r.y + r.height / 2, h = r.height / 2 - 1;
    for (int c = 0; c < cols; c++) {
        double a = start + c * fpc, b = a + fpc;
        if (b <= 0 || a >= s->frames) continue;
        if (a < 0) a = 0;
        if (b > s->frames) b = s->frames;
        float pk = 0, br = 0, lo = 0, hi = 0;
        if (fpc >= SUM_BLOCK) {
            int b0 = (int)(a / SUM_BLOCK), b1 = (int)ceil(b / SUM_BLOCK);
            float wsum = 0;
            for (int k = b0; k < b1 && k < s->nsum; k++) {
                float p = s->pk[k] / 255.0f;
                pk = fmaxf(pk, p);
                br += s->br[k] * (p + 0.01f);
                wsum += p + 0.01f;
            }
            br = wsum > 0 ? br / wsum / 255.0f : 0;
            lo = -pk; hi = pk;
        } else {
            int f0 = (int)a, f1 = (int)b;
            if (f1 <= f0) f1 = f0 + 1;
            lo = 1; hi = -1;
            for (int f = f0; f < f1 && f < s->frames; f++) {
                float x = 0;
                for (int ch = 0; ch < s->ch; ch++) x += s->data[(size_t)f * s->ch + ch];
                x /= 32768.0f * s->ch;
                lo = fminf(lo, x); hi = fmaxf(hi, x);
            }
            if (fpc > 4) { pk = fmaxf(fabsf(lo), fabsf(hi)); lo = -pk; hi = pk; }   // envelope
            int k = f0 / SUM_BLOCK;
            br = k < s->nsum ? s->br[k] / 255.0f : 0;
        }
        float x = r.x + c;
        float y0 = mid - fmaxf(hi * h, 0.5f), y1 = mid - fminf(lo * h, -0.5f);
        DrawLineV((Vector2){ x, y0 }, (Vector2){ x, y1 }, fade(spectrum_color(br), alpha));
    }
}

// A voice plays frames [pos, end) of a sample.
typedef struct {
    const Sample *s;
    double pos, end, rate, t;
    float  gain, gl, gr;      // level and constant-power pan gains
    float  amp, fadeIn, fadeOut;
    int    state;             // 0 idle, 1 playing, 2 fading out
} Voice;

static void voice_start(Voice *v, const Sample *s, double start, double end, double rate, float gain, float pan, float fadeSec)
{
    float a = (clampf(pan, -1, 1) + 1) * 0.785398f;
    v->s = s; v->pos = start; v->end = end; v->rate = rate; v->t = 0;
    v->gain = gain; v->gl = cosf(a) * 1.41421f; v->gr = sinf(a) * 1.41421f;
    v->fadeIn = fadeSec > 0 ? 1.0f / (fadeSec * SR) : 1.0f;
    v->fadeOut = fadeSec > 0 ? 1.0f / (fadeSec * SR) : 1.0f / (0.004f * SR);
    v->amp = fadeSec > 0 ? 0 : 1;
    v->state = 1;
}

static void voice_stop(Voice *v, float fadeSec)
{
    if (v->state == 1) { v->state = 2; v->fadeOut = 1.0f / (fmaxf(fadeSec, 0.001f) * SR); }
}

// Add the voice's next frame to L/R. decayTau > 0 shortens the sound.
static void voice_run(Voice *v, float *L, float *R, float decayTau)
{
    if (!v->state) return;
    const Sample *s = v->s;
    int i0 = (int)v->pos;
    if (!s || !s->data || i0 < 0 || i0 + 1 >= s->frames || v->pos >= v->end) { v->state = 0; return; }
    if (v->state == 1) {
        v->amp = fminf(v->amp + v->fadeIn, 1);
        if ((v->end - v->pos) / v->rate <= 1.0f / v->fadeOut) v->state = 2;   // fade into the region's end
    } else if ((v->amp -= v->fadeOut) <= 0) { v->state = 0; v->amp = 0; return; }
    float fr = (float)(v->pos - i0), l, r;
    const short *d = s->data;
    if (s->ch == 2) {
        l = (d[2 * i0] * (1 - fr) + d[2 * i0 + 2] * fr) / 32768.0f;
        r = (d[2 * i0 + 1] * (1 - fr) + d[2 * i0 + 3] * fr) / 32768.0f;
    } else l = r = (d[i0] * (1 - fr) + d[i0 + 1] * fr) / 32768.0f;
    float g = v->amp * v->gain * (decayTau > 0 ? expf(-(float)v->t / decayTau) : 1);
    *L += l * g * v->gl;
    *R += r * g * v->gr;
    v->pos += v->rate;
    v->t += 1.0 / SR;
}

// ---------------------------------------------------------------------------
// Drum synthesis — every kit is rendered from the same small set of circuit-
// style generators (pitch-swept sines, tuned noise, six detuned square
// oscillators, inharmonic partials, a chip LFSR), then given its character:
// sample rate, bit depth, 8-bit companding, saturation, tape and vinyl.
//
// These sounds were created by Claude Opus 5.5 (Anthropic) from rough, general
// descriptions of each machine's hardware characteristics. They are clean-room
// reimplementations: nothing was sampled from real hardware or taken from any
// recording or sample set; they only approximate how such drum kits sound.
// Kit names are raya's own; raya is not affiliated with any manufacturer.
// ---------------------------------------------------------------------------

enum { BQ_LP, BQ_HP, BQ_BP };
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } Biquad;

static Biquad bq_make(int type, float f, float q)
{
    float w = TAU * clampf(f, 10, SR * 0.45f) / SR, c = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
    Biquad b = { 0 };
    if (type == BQ_LP)      { b.b0 = (1 - c) / 2; b.b1 = 1 - c; b.b2 = (1 - c) / 2; }
    else if (type == BQ_HP) { b.b0 = (1 + c) / 2; b.b1 = -(1 + c); b.b2 = (1 + c) / 2; }
    else                    { b.b0 = al; b.b1 = 0; b.b2 = -al; }
    b.b0 /= a0; b.b1 /= a0; b.b2 /= a0;
    b.a1 = -2 * c / a0; b.a2 = (1 - al) / a0;
    return b;
}

static float bq(Biquad *b, float x)
{
    float y = b->b0 * x + b->z1;
    b->z1 = b->b1 * x - b->a1 * y + b->z2;
    b->z2 = b->b2 * x - b->a2 * y;
    return y;
}

static unsigned dr_seed;
static float dr_noise(void)
{
    dr_seed ^= dr_seed << 13; dr_seed ^= dr_seed >> 17; dr_seed ^= dr_seed << 5;
    return (int)dr_seed * (1.0f / 2147483648.0f);
}
static float dr_rand01(void) { return dr_noise() * 0.5f + 0.5f; }

// NES-style noise: a 15-bit LFSR clocked every `period` samples.
typedef struct { unsigned reg; int cnt; float out; } Lfsr;
static float lfsr(Lfsr *l, int period, bool shortMode)
{
    if (!l->reg) l->reg = 1;
    if (--l->cnt <= 0) {
        l->cnt = period;
        unsigned b = (l->reg ^ (l->reg >> (shortMode ? 6 : 1))) & 1;
        l->reg = (l->reg >> 1) | (b << 14);
        l->out = (l->reg & 1) ? 1.0f : -1.0f;
    }
    return l->out;
}

enum { MT_SQUARE, MT_NOISE, MT_CHIP };                                   // hats and cymbals
enum { TM_ANALOG, TM_ACOUSTIC, TM_SIMMONS, TM_CHIP, TM_MALLET };       // toms
enum { BL_COWBELL, BL_CLAVE, BL_BLOCK, BL_TRIANGLE, BL_CHIP, BL_BAR }; // pad 14
enum { PC_CONGA, PC_TAMB, PC_GUIRO, PC_BONGO, PC_ZAP, PC_BOWL };       // pad 16

static const char *BELL_NAMES[] = { "COWBELL", "CLAVE", "BLOCK", "TRIANGLE", "BLEEP", "BAR" };
static const char *PERC_NAMES[] = { "CONGA", "TAMB", "GUIRO", "BONGO", "ZAP", "BOWL" };

typedef struct {
    const char *name;
    float kick[2][5];    // start Hz, end Hz, pitch decay, amp decay, click
    float snare[2][7];   // tone Hz, tone Hz, tone decay, noise decay, noise mix, noise colour Hz, attack
    float clap[2];       // band Hz, tail
    float rim[2];        // high Hz, low Hz
    int   metal;
    float hat[3];        // closed decay, open decay, high-pass Hz
    float cym[2];        // crash decay, ride decay
    int   tomType;
    float tom[3];        // low tom Hz, pitch drop ratio, decay
    int   bell, perc;
    float shaker[3];     // high-pass Hz, attack, decay
    float lp, rate, drive, crackle, hiss;   // character, 0 = off
    int   bits, metalBits;
    bool  mulaw, chip;
} KitDef;

static const KitDef KITS[] = {
    { "BOOMER",
      { { 98, 49, 0.035f, 0.42f, 0.25f }, { 125, 56, 0.022f, 0.12f, 0.35f } },
      { { 185, 330, 0.056f, 0.072f, 0.55f, 9000, 0 }, { 200, 350, 0.04f, 0.11f, 0.72f, 11000, 0 } },
      { 1050, 0.08f }, { 1700, 455 }, MT_SQUARE, { 0.029f, 0.171f, 6300 }, { 0.44f, 0.36f },
      TM_ANALOG, { 95, 1.18f, 0.16f }, BL_COWBELL, PC_CONGA, { 5000, 0.001f, 0.028f } },
    { "PUNCHY",
      { { 260, 53, 0.032f, 0.18f, 0.7f }, { 300, 60, 0.025f, 0.098f, 0.9f } },
      { { 190, 345, 0.048f, 0.094f, 0.62f, 12000, 0 }, { 205, 360, 0.036f, 0.132f, 0.78f, 14000, 0 } },
      { 1200, 0.09f }, { 1850, 500 }, MT_NOISE, { 0.033f, 0.189f, 7500 }, { 0.64f, 0.84f },
      TM_ANALOG, { 105, 1.35f, 0.14f }, BL_COWBELL, PC_TAMB, { 5500, 0.001f, 0.03f },
      .drive = 0.15f, .metalBits = 6 },
    { "CRISP",
      { { 170, 58, 0.02f, 0.098f, 0.5f }, { 200, 62, 0.015f, 0.068f, 0.6f } },
      { { 210, 360, 0.04f, 0.077f, 0.65f, 8000, 0 }, { 230, 380, 0.032f, 0.055f, 0.75f, 9000, 0 } },
      { 1300, 0.06f }, { 1600, 520 }, MT_NOISE, { 0.026f, 0.135f, 7000 }, { 0.4f, 0.72f },
      TM_ACOUSTIC, { 110, 1.1f, 0.125f }, BL_COWBELL, PC_TAMB, { 6000, 0.002f, 0.035f },
      .lp = 10000, .rate = 25000, .bits = 8 },
    { "TINY",
      { { 150, 62, 0.015f, 0.083f, 0.2f }, { 175, 68, 0.012f, 0.053f, 0.3f } },
      { { 220, 400, 0.032f, 0.055f, 0.78f, 10000, 0 }, { 240, 420, 0.028f, 0.077f, 0.85f, 12000, 0 } },
      { 1150, 0.065f }, { 1900, 600 }, MT_SQUARE, { 0.023f, 0.113f, 7500 }, { 0.32f, 0.3f },
      TM_ANALOG, { 120, 1.12f, 0.1f }, BL_CLAVE, PC_CONGA, { 6500, 0.001f, 0.02f } },
    { "PARLOR",
      { { 80, 68, 0.02f, 0.064f, 0.05f }, { 95, 75, 0.015f, 0.045f, 0.08f } },
      { { 240, 450, 0.024f, 0.039f, 0.8f, 7000, 0 }, { 260, 470, 0.02f, 0.055f, 0.85f, 8000, 0 } },
      { 900, 0.05f }, { 1500, 420 }, MT_SQUARE, { 0.019f, 0.09f, 8000 }, { 0.36f, 0.3f },
      TM_ANALOG, { 140, 1.05f, 0.075f }, BL_CLAVE, PC_GUIRO, { 4500, 0.001f, 0.03f },
      .lp = 9000, .hiss = 0.15f },
    { "GROOVEBOX",
      { { 200, 52, 0.03f, 0.225f, 0.6f }, { 140, 48, 0.03f, 0.375f, 0.3f } },
      { { 195, 340, 0.048f, 0.088f, 0.66f, 11000, 0 }, { 180, 330, 0.056f, 0.066f, 0.55f, 9000, 0 } },
      { 1150, 0.1f }, { 1750, 470 }, MT_SQUARE, { 0.029f, 0.158f, 6800 }, { 0.52f, 0.48f },
      TM_ANALOG, { 100, 1.25f, 0.15f }, BL_COWBELL, PC_CONGA, { 5500, 0.001f, 0.03f },
      .lp = 14000, .drive = 0.35f },
    { "DUSTY",
      { { 120, 50, 0.03f, 0.188f, 0.4f }, { 160, 56, 0.02f, 0.112f, 0.5f } },
      { { 200, 330, 0.048f, 0.11f, 0.6f, 6500, 0 }, { 180, 300, 0.056f, 0.099f, 0.5f, 5000, 0.003f } },
      { 950, 0.11f }, { 1400, 400 }, MT_NOISE, { 0.033f, 0.158f, 6000 }, { 0.64f, 0.72f },
      TM_ACOUSTIC, { 95, 1.1f, 0.15f }, BL_BLOCK, PC_BONGO, { 4500, 0.008f, 0.06f },
      .lp = 7000, .rate = 24000, .drive = 0.45f, .hiss = 0.3f, .bits = 12 },
    { "POP 80",
      { { 110, 58, 0.025f, 0.12f, 0.45f }, { 130, 62, 0.02f, 0.083f, 0.55f } },
      { { 195, 320, 0.056f, 0.105f, 0.62f, 8500, 0 }, { 215, 350, 0.04f, 0.077f, 0.7f, 9500, 0 } },
      { 1100, 0.09f }, { 1650, 380 }, MT_NOISE, { 0.039f, 0.225f, 7000 }, { 0.72f, 0.96f },
      TM_ACOUSTIC, { 90, 1.12f, 0.175f }, BL_COWBELL, PC_CONGA, { 5000, 0.004f, 0.05f },
      .lp = 11000, .rate = 28000, .bits = 8, .mulaw = true },
    { "BREAKER",
      { { 130, 55, 0.03f, 0.143f, 0.6f }, { 150, 60, 0.02f, 0.09f, 0.7f } },
      { { 185, 300, 0.064f, 0.11f, 0.58f, 9000, 0 }, { 200, 320, 0.048f, 0.083f, 0.66f, 10000, 0 } },
      { 1250, 0.085f }, { 1700, 480 }, MT_NOISE, { 0.033f, 0.203f, 7500 }, { 0.8f, 0.9f },
      TM_ACOUSTIC, { 85, 1.15f, 0.2f }, BL_COWBELL, PC_TAMB, { 5500, 0.003f, 0.045f },
      .rate = 26000, .drive = 0.2f, .bits = 8, .mulaw = true },
    { "HEXAGON",
      { { 160, 50, 0.06f, 0.225f, 0.3f }, { 220, 55, 0.08f, 0.165f, 0.4f } },
      { { 210, 330, 0.08f, 0.121f, 0.55f, 8000, 0 }, { 250, 380, 0.064f, 0.165f, 0.62f, 10000, 0 } },
      { 1000, 0.1f }, { 1200, 400 }, MT_NOISE, { 0.033f, 0.18f, 7000 }, { 0.72f, 0.78f },
      TM_SIMMONS, { 75, 2.6f, 0.275f }, BL_BLOCK, PC_ZAP, { 6000, 0.002f, 0.04f },
      .drive = 0.2f },
    { "STUDIO",
      { { 110, 55, 0.02f, 0.15f, 0.55f }, { 95, 50, 0.03f, 0.225f, 0.35f } },
      { { 200, 340, 0.064f, 0.143f, 0.6f, 9500, 0 }, { 185, 310, 0.08f, 0.176f, 0.55f, 7500, 0 } },
      { 1100, 0.1f }, { 1250, 420 }, MT_NOISE, { 0.046f, 0.315f, 6500 }, { 1.04f, 1.32f },
      TM_ACOUSTIC, { 82, 1.08f, 0.225f }, BL_COWBELL, PC_TAMB, { 4000, 0.015f, 0.09f } },
    { "BRUSHES",
      { { 90, 52, 0.03f, 0.165f, 0.15f }, { 80, 48, 0.04f, 0.225f, 0.1f } },
      { { 220, 360, 0.032f, 0.193f, 0.9f, 6000, 0.02f }, { 210, 340, 0.04f, 0.099f, 0.8f, 7000, 0.003f } },
      { 1500, 0.04f }, { 1300, 450 }, MT_NOISE, { 0.052f, 0.405f, 5500 }, { 1.12f, 1.56f },
      TM_ACOUSTIC, { 80, 1.05f, 0.25f }, BL_TRIANGLE, PC_BONGO, { 3500, 0.03f, 0.14f },
      .lp = 9000 },
    { "TAPE LOFI",
      { { 115, 52, 0.025f, 0.165f, 0.4f }, { 140, 58, 0.02f, 0.098f, 0.5f } },
      { { 195, 330, 0.056f, 0.11f, 0.6f, 8000, 0 }, { 205, 345, 0.04f, 0.083f, 0.7f, 9000, 0 } },
      { 1050, 0.1f }, { 1600, 450 }, MT_NOISE, { 0.033f, 0.203f, 6500 }, { 0.8f, 0.96f },
      TM_ACOUSTIC, { 88, 1.1f, 0.2f }, BL_BLOCK, PC_CONGA, { 4500, 0.008f, 0.06f },
      .lp = 5200, .drive = 0.55f, .hiss = 0.5f },
    { "VINYL",
      { { 105, 50, 0.03f, 0.18f, 0.35f }, { 130, 55, 0.02f, 0.112f, 0.45f } },
      { { 190, 320, 0.056f, 0.121f, 0.6f, 7000, 0 }, { 175, 300, 0.064f, 0.099f, 0.55f, 6000, 0.002f } },
      { 1000, 0.1f }, { 1500, 430 }, MT_NOISE, { 0.033f, 0.18f, 6000 }, { 0.72f, 0.9f },
      TM_ACOUSTIC, { 85, 1.1f, 0.2f }, BL_COWBELL, PC_TAMB, { 4500, 0.006f, 0.05f },
      .lp = 6000, .rate = 22050, .drive = 0.3f, .crackle = 1.0f, .hiss = 0.35f, .bits = 12 },
    { "8-BIT",
      { { 300, 45, 0.03f, 0.105f, 0 }, { 500, 60, 0.02f, 0.06f, 0 } },
      { { 220, 440, 0.024f, 0.066f, 0.85f, 12000, 0 }, { 260, 520, 0.016f, 0.044f, 0.9f, 14000, 0 } },
      { 1500, 0.05f }, { 2000, 700 }, MT_CHIP, { 0.019f, 0.09f, 4000 }, { 0.36f, 0.36f },
      TM_CHIP, { 110, 2, 0.1f }, BL_CHIP, PC_ZAP, { 6000, 0.001f, 0.03f },
      .rate = 22050, .bits = 5, .chip = true },
    { "FELT",
      { { 70, 46, 0.04f, 0.262f, 0 }, { 85, 52, 0.03f, 0.165f, 0 } },
      { { 240, 410, 0.04f, 0.165f, 0.55f, 3500, 0.006f }, { 200, 320, 0.064f, 0.22f, 0.45f, 2800, 0.015f } },
      { 800, 0.15f }, { 900, 300 }, MT_NOISE, { 0.039f, 0.27f, 5000 }, { 1.4f, 1.8f },
      TM_MALLET, { 110, 1, 0.45f }, BL_BAR, PC_BOWL, { 3000, 0.03f, 0.15f },
      .lp = 4500 },
};
#define N_KITS ((int)(sizeof KITS / sizeof KITS[0]))

static const char *KIT_NAMES[N_KITS];
static const char *DRUM_PAD[16]  = { "KICK 1", "KICK 2", "SNARE 1", "SNARE 2", "CLAP", "RIM", "CL HAT", "OP HAT",
                                     "CRASH", "RIDE", "LO TOM", "MID TOM", "HI TOM", NULL, "SHAKER", NULL };
static const float DRUM_LEN[16]  = { 1.6f, 1.2f, 0.7f, 0.7f, 0.8f, 0.25f, 0.3f, 1.4f, 4.0f, 3.5f, 1.3f, 1.2f, 1.1f, 1.5f, 0.45f, 2.5f };
static const float DRUM_GAIN[16] = { 1.0f, 0.95f, 0.8f, 0.8f, 0.7f, 0.55f, 0.45f, 0.45f, 0.5f, 0.42f, 0.8f, 0.78f, 0.75f, 0.5f, 0.4f, 0.6f };
static const float DRUM_PAN[16]  = { 0, 0, 0, 0, 0.05f, -0.1f, 0.25f, 0.3f, -0.3f, 0.35f, -0.35f, 0, 0.35f, -0.2f, 0.4f, -0.4f };

static float env_ad(float t, float atk, float dec) { return (atk > 0 ? fminf(t / atk, 1) : 1) * expf(-t / dec); }

static void dr_kick(const float *k, bool chip, float *y, int n)
{
    Biquad hp = bq_make(BQ_HP, 1500, 0.7f);
    double ph = 0;
    for (int i = 0; i < n; i++) {
        float t = i / SR, f = k[1] + (k[0] - k[1]) * expf(-t / k[2]);
        ph += f / SR;
        float s = chip ? (fractf((float)ph) < 0.5f ? 0.8f : -0.8f) : sinf(TAU * (float)ph);
        float c = t < 0.008f ? k[4] * bq(&hp, dr_noise()) * (1 - t / 0.008f) : 0;
        y[i] = s * env_ad(t, 0.001f, k[3]) + c;
    }
}

static void dr_snare(const float *s, bool chip, float *y, int n)
{
    Biquad hp = bq_make(BQ_HP, s[5] * 0.15f, 0.7f), lp = bq_make(BQ_LP, s[5], 0.7f);
    Lfsr lf = { 0 };
    double p1 = 0, p2 = 0;
    for (int i = 0; i < n; i++) {
        float t = i / SR, bend = 1 + 0.2f * expf(-t / 0.008f);
        p1 += s[0] * bend / SR; p2 += s[1] * bend / SR;
        float tone = (0.65f * sinf(TAU * (float)p1) + 0.35f * sinf(TAU * (float)p2)) * env_ad(t, 0.0005f, s[2]);
        float nz = bq(&lp, bq(&hp, chip ? lfsr(&lf, 3, false) : dr_noise()));
        y[i] = (1 - s[4]) * tone + s[4] * 1.6f * nz * env_ad(t, s[6] > 0 ? s[6] : 0.0005f, s[3]);
    }
}

// Several quick noise bursts, then a short room tail.
static void dr_clap(float band, float tail, float *y, int n)
{
    static const float at[4] = { 0, 0.0105f, 0.0195f, 0.031f };
    Biquad bp = bq_make(BQ_BP, band, 1.6f), bp2 = bq_make(BQ_BP, band * 1.15f, 1.6f);
    for (int i = 0; i < n; i++) {
        float t = i / SR, e = 0;
        for (int k = 0; k < 4; k++) if (t >= at[k]) e = fmaxf(e, expf(-(t - at[k]) / 0.0045f));
        if (t >= at[3]) e = fmaxf(e, 0.55f * expf(-(t - at[3]) / tail));
        y[i] = bq(&bp2, bq(&bp, dr_noise())) * e * 4.0f;
    }
}

static void dr_rim(float hi, float lo, float *y, int n)
{
    Biquad hp = bq_make(BQ_HP, 2000, 0.7f);
    for (int i = 0; i < n; i++) {
        float t = i / SR;
        float x = 0.7f * sinf(TAU * hi * t) * expf(-t / 0.007f) + 0.6f * sinf(TAU * lo * t) * expf(-t / 0.012f)
                + 0.3f * bq(&hp, dr_noise()) * expf(-t / 0.003f);
        y[i] = tanhf(3 * x) * 0.7f;
    }
}

// Classic analog metal: six detuned square oscillators, band-passed.
static const float SQ_METAL[6] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };

static void dr_metal_square(float dec, float hp, bool cymbal, float *y, int n)
{
    double ph[6] = { 0 };
    Biquad hiBand = bq_make(BQ_BP, 7100, 1.0f), loBand = bq_make(BQ_BP, 3440, 1.2f);
    Biquad h1 = bq_make(BQ_HP, hp, 0.7f), h2 = bq_make(BQ_HP, hp, 0.7f);
    for (int i = 0; i < n; i++) {
        float t = i / SR, x = 0;
        for (int k = 0; k < 6; k++) { ph[k] += SQ_METAL[k] / SR; x += fractf((float)ph[k]) < 0.5f ? 1.0f : -1.0f; }
        x /= 6;
        float hi = bq(&h2, bq(&h1, bq(&hiBand, x))) * env_ad(t, 0.0005f, dec);
        float lo = cymbal ? bq(&loBand, x) * 0.6f * env_ad(t, 0.0005f, dec * 0.3f) : 0;
        y[i] = (hi + lo + (cymbal ? 0.08f * dr_noise() * env_ad(t, 0.001f, dec * 0.7f) : 0)) * 3;
    }
}

// Real cymbals: many inharmonic partials plus noise.
static void dr_metal_noise(float dec, float hp, int np, float lo, float hi, float noiseMix, float atk, float *y, int n)
{
    float am[40], cr[40], ci[40], wr[40], wi[40];   // partials as rotating phasors: cheap sines
    if (np > 40) np = 40;
    for (int k = 0; k < np; k++) {
        float f = lo * powf(hi / lo, dr_rand01());
        am[k] = 0.4f + 0.6f * dr_rand01();
        cr[k] = 1; ci[k] = 0; wr[k] = cosf(TAU * f / SR); wi[k] = sinf(TAU * f / SR);
    }
    Biquad h1 = bq_make(BQ_HP, hp, 0.7f), h2 = bq_make(BQ_HP, hp, 0.7f), lp = bq_make(BQ_LP, hi * 1.1f, 0.7f);
    for (int i = 0; i < n; i++) {
        float t = i / SR, x = 0;
        for (int k = 0; k < np; k++) {
            x += am[k] * ci[k];
            float r = cr[k] * wr[k] - ci[k] * wi[k];
            ci[k] = cr[k] * wi[k] + ci[k] * wr[k];
            cr[k] = r;
        }
        if ((i & 1023) == 1023)
            for (int k = 0; k < np; k++) { float g = 1 / sqrtf(cr[k] * cr[k] + ci[k] * ci[k]); cr[k] *= g; ci[k] *= g; }
        x = x * 2.0f / np + noiseMix * bq(&lp, dr_noise());
        y[i] = bq(&h2, bq(&h1, x)) * env_ad(t, atk, dec) * 1.6f;
    }
}

// period: samples per LFSR step; the short mode loops quickly and rings like metal
static void dr_metal_chip(float dec, int period, bool shortMode, float *y, int n)
{
    Lfsr lf = { 0 };
    for (int i = 0; i < n; i++) {
        float t = i / SR;
        y[i] = lfsr(&lf, period, shortMode) * env_ad(t, 0.0005f, dec) * 0.6f;
    }
}

static void dr_tom(int type, float f, float drop, float dec, float *y, int n)
{
    Biquad hp = bq_make(BQ_HP, 2000, 0.7f), lp = bq_make(BQ_LP, 1200, 0.7f);
    double ph = 0, ph2 = 0;
    for (int i = 0; i < n; i++) {
        float t = i / SR, ft, x;
        switch (type) {
        case TM_SIMMONS:   // the electronic "pew": a big pitch sweep plus noise
            ft = f * (1 + (drop - 1) * expf(-t / 0.12f));
            ph += ft / SR;
            x = sinf(TAU * (float)ph) * env_ad(t, 0.001f, dec) + 0.25f * bq(&lp, dr_noise()) * env_ad(t, 0.001f, dec * 0.6f);
            break;
        case TM_CHIP:      // a triangle channel sweeping down
            ft = f * (1 + (drop - 1) * expf(-t / 0.05f));
            ph += ft / SR;
            x = (fabsf(4 * fractf((float)ph) - 2) - 1) * env_ad(t, 0.0005f, dec);
            break;
        case TM_MALLET:    // a soft mallet on a tuned membrane
            ph += f / SR; ph2 += f * 2.76f / SR;
            x = (sinf(TAU * (float)ph) * env_ad(t, 0.004f, dec) + 0.3f * sinf(TAU * (float)ph2) * env_ad(t, 0.004f, dec * 0.25f));
            break;
        case TM_ACOUSTIC:  // drum head: fundamental, an inharmonic mode and stick noise
            ft = f * (1 + (drop - 1) * expf(-t / 0.05f));
            ph += ft / SR; ph2 += ft * 1.58f / SR;
            x = sinf(TAU * (float)ph) * env_ad(t, 0.0008f, dec) + 0.35f * sinf(TAU * (float)ph2) * env_ad(t, 0.0008f, dec * 0.4f)
              + 0.25f * bq(&hp, dr_noise()) * expf(-t / 0.012f);
            break;
        default:           // analog: a resonator with a little pitch drop and skin noise
            ft = f * (1 + (drop - 1) * expf(-t / 0.03f));
            ph += ft / SR;
            x = sinf(TAU * (float)ph) * env_ad(t, 0.0008f, dec) + 0.08f * bq(&lp, dr_noise()) * expf(-t / (dec * 0.5f));
        }
        y[i] = x;
    }
}

static void dr_bell(int type, float *y, int n)
{
    double p[5] = { 0 };
    Biquad bp = bq_make(BQ_BP, 1000, 0.8f), hp = bq_make(BQ_HP, 3000, 0.7f);
    for (int i = 0; i < n; i++) {
        float t = i / SR, x = 0;
        switch (type) {
        case BL_COWBELL:   // analog cowbell: squares at 540 and 800 Hz
            p[0] += 540 / SR; p[1] += 800 / SR;
            x = (fractf((float)p[0]) < 0.5f ? 1.0f : -1.0f) + (fractf((float)p[1]) < 0.5f ? 1.0f : -1.0f);
            x = bq(&bp, x * 0.5f) * (0.7f * expf(-t / 0.015f) + 0.3f * expf(-t / 0.25f)) * 2.5f;
            break;
        case BL_CLAVE: x = sinf(TAU * 2500 * t) * env_ad(t, 0.0003f, 0.025f); break;
        case BL_BLOCK:
            x = (sinf(TAU * 820 * t) * expf(-t / 0.05f) + 0.5f * sinf(TAU * 2100 * t) * expf(-t / 0.02f))
              + 0.3f * bq(&hp, dr_noise()) * expf(-t / 0.002f);
            break;
        case BL_TRIANGLE: {
            static const float fr[5] = { 2200, 3570, 5100, 6780, 8400 };
            for (int k = 0; k < 5; k++) x += sinf(TAU * fr[k] * t) / (k + 1.5f);
            x *= env_ad(t, 0.002f, 1.0f);
        } break;
        case BL_CHIP:      // two quick squares, an arpeggio blip
            p[0] += (t < 0.05f ? 880 : 1320) / SR;
            x = (fractf((float)p[0]) < 0.5f ? 0.6f : -0.6f) * env_ad(t, 0.0005f, 0.12f);
            break;
        default:           // BL_BAR: a marimba-like bar, partials 1 : 3.93
            x = sinf(TAU * 660 * t) * env_ad(t, 0.002f, 0.6f) + 0.3f * sinf(TAU * 660 * 3.93f * t) * env_ad(t, 0.002f, 0.12f);
        }
        y[i] = x;
    }
}

static void dr_shaker(const float *s, float *y, int n)
{
    Biquad h = bq_make(BQ_HP, s[0], 0.7f), b = bq_make(BQ_BP, s[0] * 1.6f, 0.9f);
    for (int i = 0; i < n; i++) {
        float t = i / SR;
        y[i] = (bq(&h, dr_noise()) + 0.5f * bq(&b, dr_noise())) * env_ad(t, s[1], s[2]);
    }
}

static void dr_perc(int type, float *y, int n)
{
    Biquad bp = bq_make(BQ_BP, 3200, 2.0f), hp = bq_make(BQ_HP, 5000, 0.7f), slap = bq_make(BQ_HP, 1500, 0.7f);
    double ph = 0, p2 = 0, p3 = 0;
    for (int i = 0; i < n; i++) {
        float t = i / SR, x = 0;
        switch (type) {
        case PC_CONGA:     // analog conga: the tom circuit, tuned high
            ph += 330 * (1 + 0.06f * expf(-t / 0.02f)) / SR;
            x = sinf(TAU * (float)ph) * env_ad(t, 0.0008f, 0.16f);
            break;
        case PC_BONGO:
            ph += 420 * (1 + 0.08f * expf(-t / 0.015f)) / SR;
            x = sinf(TAU * (float)ph) * env_ad(t, 0.0006f, 0.09f) + 0.3f * bq(&slap, dr_noise()) * expf(-t / 0.006f);
            break;
        case PC_TAMB: {    // jingles shaken three times
            static const float at[3] = { 0, 0.028f, 0.065f };
            float e = 0;
            for (int k = 0; k < 3; k++) if (t >= at[k]) e = fmaxf(e, (k ? 0.6f : 1.0f) * expf(-(t - at[k]) / 0.07f));
            x = bq(&hp, dr_noise()) * e * 1.4f;
        } break;
        case PC_GUIRO: {   // a scrape: a run of short ticks
            float k = t / 0.016f;
            int tick = (int)k;
            float e = tick < 14 ? expf(-(k - tick) * 6) * (0.6f + 0.4f * sinf(3.14159f * tick / 14)) : 0;
            x = bq(&bp, dr_noise()) * e * 2.0f;
        } break;
        case PC_ZAP:       // a falling square sweep
            ph += (200 + 1400 * expf(-t / 0.03f)) / SR;
            x = (fractf((float)ph) < 0.5f ? 0.6f : -0.6f) * env_ad(t, 0.0005f, 0.09f);
            break;
        default:           // PC_BOWL: a singing bowl with slow beating
            ph += 280 / SR; p2 += 280 * 2.71f / SR; p3 += 280.7f / SR;
            x = (0.5f * sinf(TAU * (float)ph) + 0.5f * sinf(TAU * (float)p3)) * env_ad(t, 0.004f, 2.5f)
              + 0.4f * sinf(TAU * (float)p2) * env_ad(t, 0.004f, 1.4f);
        }
        y[i] = x;
    }
}

// The kit's character: saturation, filter, sample rate, bit depth, noise.
static void dr_character(const KitDef *k, int slot, float *y, int n)
{
    float pk = 1e-6f;
    for (int i = 0; i < n; i++) pk = fmaxf(pk, fabsf(y[i]));
    for (int i = 0; i < n; i++) y[i] /= pk;
    if (k->drive > 0) { float g = 1 + k->drive * 5, nm = tanhf(g); for (int i = 0; i < n; i++) y[i] = tanhf(y[i] * g) / nm; }
    if (k->lp > 0) {
        Biquad a = bq_make(BQ_LP, k->lp, 0.54f), b = bq_make(BQ_LP, k->lp, 1.31f);   // 4th-order Butterworth
        for (int i = 0; i < n; i++) y[i] = bq(&b, bq(&a, y[i]));
    }
    if (k->rate > 0) {   // sample and hold at the machine's rate
        float step = k->rate / SR, acc = 1, held = 0;
        for (int i = 0; i < n; i++) { if ((acc += step) >= 1) { acc -= 1; held = y[i]; } y[i] = held; }
    }
    int bits = (slot >= 6 && slot <= 9 && k->metalBits) ? k->metalBits : k->bits;
    if (bits > 0) {
        float q = (float)(1 << (bits - 1));
        for (int i = 0; i < n; i++) {
            float x = y[i];
            if (k->mulaw) {   // companded like the 8-bit sample chips of the early 80s
                float c = copysignf(logf(1 + 255 * fabsf(x)) / logf(256), x);
                c = roundf(c * q) / q;
                x = copysignf((powf(256, fabsf(c)) - 1) / 255, c);
            } else x = roundf(x * q) / q;
            y[i] = x;
        }
    }
    if (k->hiss > 0 || k->crackle > 0) {
        float pop = 0;
        for (int i = 0; i < n; i++) {
            float fadeOut = 1 - (float)i / n;
            if (k->crackle > 0 && dr_rand01() < k->crackle * 0.0004f) pop = dr_noise() * 0.6f;
            y[i] += (k->hiss * 0.04f * dr_noise() + pop) * fadeOut;
            pop *= 0.82f;
        }
    }
}

typedef struct { Sample s[16]; bool done[16]; volatile bool ready; } Kit;
static Kit kits[N_KITS];

static void kit_render_slot(int ki, int slot)
{
    const KitDef *k = &KITS[ki];
    {
        int n = (int)(DRUM_LEN[slot] * SR);
        float *y = calloc(n, sizeof(float));
        dr_seed = 0x9e3779b9u * (unsigned)(ki * 16 + slot + 1);
        switch (slot) {
        case 0: case 1:  dr_kick(k->kick[slot], k->chip, y, n); break;
        case 2: case 3:  dr_snare(k->snare[slot - 2], k->chip, y, n); break;
        case 4:          dr_clap(k->clap[0], k->clap[1], y, n); break;
        case 5:          dr_rim(k->rim[0], k->rim[1], y, n); break;
        case 6: case 7: {
            float dec = k->hat[slot - 6];
            if (k->metal == MT_SQUARE) dr_metal_square(dec, k->hat[2], false, y, n);
            else if (k->metal == MT_CHIP) dr_metal_chip(dec, slot == 7 ? 2 : 1, slot == 6, y, n);
            else dr_metal_noise(dec, k->hat[2], 24, 3000, 14000, 0.5f, 0.0005f, y, n);
        } break;
        case 8: case 9: {
            float dec = k->cym[slot - 8];
            if (k->metal == MT_SQUARE) dr_metal_square(dec, k->hat[2] * (slot == 9 ? 1.1f : 0.8f), slot == 8, y, n);
            else if (k->metal == MT_CHIP) dr_metal_chip(dec, slot == 8 ? 3 : 2, slot == 9, y, n);
            else if (slot == 8) dr_metal_noise(dec, 2500, 40, 400, 11000, 0.45f, 0.004f, y, n);
            else {   // ride: a quieter wash plus the bell's ping
                dr_metal_noise(dec, 3500, 30, 2500, 12000, 0.25f, 0.001f, y, n);
                for (int i = 0; i < n; i++) {
                    float t = i / SR;
                    y[i] = 0.55f * y[i] + 0.35f * (sinf(TAU * 720 * t) + 0.6f * sinf(TAU * 1730 * t) + 0.4f * sinf(TAU * 2810 * t)) * env_ad(t, 0.0005f, 0.9f);
                }
            }
        } break;
        case 10: case 11: case 12: {
            static const float ratio[3] = { 1.0f, 1.42f, 2.0f };
            dr_tom(k->tomType, k->tom[0] * ratio[slot - 10], k->tom[1], k->tom[2] * (1 - 0.15f * (slot - 10)), y, n);
        } break;
        case 13: dr_bell(k->bell, y, n); break;
        case 14: dr_shaker(k->shaker, y, n); break;
        default: dr_perc(k->perc, y, n);
        }
        dr_character(k, slot, y, n);
        // level, then trim the silent tail and fade the last few milliseconds
        float pk = 1e-6f;
        for (int i = 0; i < n; i++) pk = fmaxf(pk, fabsf(y[i]));
        int end = n;
        while (end > 1 && fabsf(y[end - 1]) < pk * 0.0015f) end--;
        end = end + 480 < n ? end + 480 : n;
        int fade = end < 480 ? end : 480;
        for (int i = 0; i < end; i++) {
            float g = DRUM_GAIN[slot] / pk;
            if (i > end - fade) g *= (float)(end - i) / fade;
            y[i] *= g;
        }
        sample_from_float(&kits[ki].s[slot], y, end);
        free(y);
    }
    kits[ki].done[slot] = true;
    bool all = true;
    for (int i = 0; i < 16; i++) all = all && kits[ki].done[i];
    kits[ki].ready = all;
}

// Render pads of kit `ki` for at most `budget` seconds; true once it is complete.
// A frame never stalls long enough to interrupt the audio.
static bool kit_work(int ki, double budget)
{
    double t0 = GetTime();
    for (int slot = 0; slot < 16 && !kits[ki].ready; slot++) {
        if (kits[ki].done[slot]) continue;
        kit_render_slot(ki, slot);
        if (GetTime() - t0 > budget) break;
    }
    return kits[ki].ready;
}

static const char *drum_pad_name(int kit, int slot)
{
    if (slot == 13) return BELL_NAMES[KITS[kit].bell];
    if (slot == 15) return PERC_NAMES[KITS[kit].perc];
    return DRUM_PAD[slot];
}

// ---------------------------------------------------------------------------
// DRUMS — 16 pads, each with its own trigger input, playing one of 16 kits
// ---------------------------------------------------------------------------

typedef struct {
    int   kit;          // kit the voices play, -1 until rendered
    Voice v[16];
    float ptrig[16], glow[16];
    int   sel;          // pad shown in the waveform
    float lpL, lpR, hpL, hpR;
} DrumSt;

static void drums_init(Module *m)
{
    DrumSt *s = calloc(1, sizeof(DrumSt));
    s->kit = -1;
    m->st = s;
}

static void drums_hit(Module *m, int pad)
{
    DrumSt *s = m->st;
    if (s->kit < 0) return;
    const Sample *smp = &kits[s->kit].s[pad];
    voice_start(&s->v[pad], smp, 0, smp->frames, exp2f(m->p[1] / 12.0f), 1, DRUM_PAN[pad], 0);
    if (pad == 6) voice_stop(&s->v[7], 0.006f);   // the closed hat chokes the open one
    s->glow[pad] = 1;
}

static void drums_process(Module *m)
{
    DrumSt *s = m->st;
    for (int k = 0; k < 16; k++) if (rising(IN(m, k), &s->ptrig[k])) drums_hit(m, k);
    float L = 0, R = 0, dec = m->p[2];
    float tau = dec >= 0.999f ? 0 : 0.02f + 2.0f * dec * dec;
    for (int k = 0; k < 16; k++) voice_run(&s->v[k], &L, &R, tau);
    float tone = m->p[3];
    if (tone < -0.01f) {   // darker: low-pass from 18 kHz down to ~700 Hz
        float a = 1 - expf(-TAU * 18000 * powf(0.04f, -tone) / SR);
        s->lpL += a * (L - s->lpL); s->lpR += a * (R - s->lpR);
        L = s->lpL; R = s->lpR;
    } else if (tone > 0.01f) {   // thinner: high-pass up to ~600 Hz
        float a = 1 - expf(-TAU * 20 * powf(30, tone) / SR);
        s->hpL += a * (L - s->hpL); s->hpR += a * (R - s->hpR);
        L -= s->hpL; R -= s->hpR;
    }
    if (m->p[4] > 0.01f) { float g = 1 + m->p[4] * 4, nm = tanhf(g); L = tanhf(L * g) / nm; R = tanhf(R * g) / nm; }
    m->out[0] = L * m->p[5];
    m->out[1] = R * m->p[5];
}

static Color pad_color(int pad)
{
    if (pad < 2)  return CABLE_COLORS[1];   // kicks
    if (pad < 6)  return CABLE_COLORS[0];   // snares, clap, rim
    if (pad < 8)  return CABLE_COLORS[2];   // hats
    if (pad < 10) return CABLE_COLORS[3];   // cymbals
    if (pad < 13) return CABLE_COLORS[4];   // toms
    return (Color){ 120, 186, 160, 255 };   // percussion
}

static Rectangle drum_pad(Rectangle a, int i)
{
    float w = (a.width - 7 * 4) / 8;
    return (Rectangle){ a.x + (i % 8) * (w + 4), a.y + 16 + (i / 8) * 34, w, 30 };
}
static Rectangle drum_wave(Rectangle a) { return (Rectangle){ a.x, a.y + 86, a.width, a.height - 86 }; }

static void drums_draw(Module *m, Rectangle a)
{
    DrumSt *s = m->st;
    int want = clampi((int)roundf(m->p[0]), 0, N_KITS - 1);
    if (want != s->kit && kit_work(want, 0.006)) s->kit = want;   // the old kit plays until the new one is ready
    int kit = s->kit >= 0 ? s->kit : want;
    txt(want == s->kit ? KITS[want].name : TextFormat("%s  ...", KITS[want].name), a.x, a.y + 2, 10, ACCENT);
    const Sample *smp = &kits[kit].s[s->sel];
    const char *info = TextFormat("%s  %.2f S", drum_pad_name(kit, s->sel), smp->frames / SR);
    txt(info, a.x + a.width - txt_w(info, 10), a.y + 2, 10, TEXT_DIM);
    for (int i = 0; i < 16; i++) {
        Rectangle r = drum_pad(a, i);
        Color c = pad_color(i);
        float g = s->glow[i];
        DrawRectangleRec(r, INSET);
        DrawRectangleRec(r, fade(c, 0.12f + 0.55f * g));
        DrawRectangleLinesEx(r, 1, i == s->sel ? c : fade(c, 0.35f));
        txt_c(drum_pad_name(kit, i), r.x + r.width / 2, r.y + 10, 10, g > 0.3f ? TEXT : fade(TEXT, 0.75f));
        s->glow[i] *= 0.86f;
    }
    Rectangle w = drum_wave(a);
    DrawRectangleRec(w, INSET);
    Rectangle wi = { w.x + 2, w.y + 2, w.width - 4, w.height - 4 };
    wave_view(smp, wi, 0, smp->frames, 0.95f);
    const Voice *v = &s->v[s->sel];
    if (v->state && v->s == smp) {
        float x = wi.x + (float)(v->pos / smp->frames) * wi.width;
        DrawLineEx((Vector2){ x, wi.y }, (Vector2){ x, wi.y + wi.height }, 1, fade(TEXT, 0.7f));
    }
}

static bool drums_press(Module *m, Vector2 mp, Rectangle a)
{
    DrumSt *s = m->st;
    for (int i = 0; i < 16; i++)
        if (CheckCollisionPointRec(mp, drum_pad(a, i))) { s->sel = i; drums_hit(m, i); return true; }
    if (CheckCollisionPointRec(mp, drum_wave(a))) { drums_hit(m, s->sel); return true; }
    return false;
}

// ---------------------------------------------------------------------------
// SAMPLER — an audio file with up to 8 markers. PLAY n plays from marker n to
// the next marker (or the end), STOP n stops it. Wheel zooms, drag scrolls.
// ---------------------------------------------------------------------------

#define SMP_MARKS       8
#define SAMPLER_MAX_SEC 600
#define DRAG_VIEW       100
#define DRAG_OVER       101

typedef struct {
    Sample smp, next, old;             // next / old: hand-over with the audio thread
    volatile int swap;
    int    nmark, mark[SMP_MARKS];     // frame positions, ascending
    double vs, vw;                     // visible frames [vs, vs + vw)
    Voice  v[SMP_MARKS];
    float  pplay[SMP_MARKS], pstop[SMP_MARKS];
    int    drag;
    float  dragX;
    double dragVs, lastClick;
    char   name[48];
    unsigned id;
    volatile bool dirty;
    double savedT;
} SamplerSt;

static const char *SMP_BTN[2] = { "LOAD", "AUTO" };
static const char *VOICE_LABELS[2] = { "POLY", "MONO" };

static void sampler_init(Module *m)
{
    SamplerSt *s = calloc(1, sizeof(SamplerSt));
    s->id = new_id();
    s->drag = -1;
    m->st = s;
}

static void sampler_fini(Module *m)
{
    SamplerSt *s = m->st;
    if (!s) return;
    sample_free(&s->smp); sample_free(&s->next); sample_free(&s->old);
}

static int sampler_end(const SamplerSt *s, int i) { return i + 1 < s->nmark ? s->mark[i + 1] : s->smp.frames; }

static void sampler_swap(SamplerSt *s)
{
    for (int i = 0; i < SMP_MARKS; i++) s->v[i].state = 0;
    s->old = s->smp;
    s->smp = s->next;
    memset(&s->next, 0, sizeof s->next);
    s->swap = 0;
}

static void sampler_play(Module *m, int i)
{
    SamplerSt *s = m->st;
    if (i >= s->nmark || !s->smp.data) return;
    float fade = m->p[1];
    if ((int)roundf(m->p[2]) == 1)   // MONO: one region at a time
        for (int k = 0; k < SMP_MARKS; k++) if (k != i) voice_stop(&s->v[k], fade);
    voice_start(&s->v[i], &s->smp, s->mark[i], sampler_end(s, i), exp2f(m->p[0] / 12.0f), 1, 0, fade);
}

static void sampler_process(Module *m)
{
    SamplerSt *s = m->st;
    if (s->swap) sampler_swap(s);
    for (int i = 0; i < SMP_MARKS; i++) {
        if (rising(IN(m, i), &s->pplay[i])) sampler_play(m, i);
        if (rising(IN(m, SMP_MARKS + i), &s->pstop[i])) voice_stop(&s->v[i], m->p[1]);
    }
    float rate = exp2f(m->p[0] / 12.0f + IN(m, 16)), L = 0, R = 0;
    for (int i = 0; i < SMP_MARKS; i++) { s->v[i].rate = rate; voice_run(&s->v[i], &L, &R, 0); }
    m->out[0] = L * m->p[3];
    m->out[1] = R * m->p[3];
}

// Read an audio file (UI thread). keep: restore a project, leaving markers and view alone.
static bool sampler_read(Module *m, const char *path, bool keep)
{
    SamplerSt *s = m->st;
    if (s->swap) return false;
    Wave w = LoadWave(path);
    if (!w.data || !w.frameCount) return false;
    int ch = w.channels >= 2 ? 2 : 1;
    WaveFormat(&w, (int)SR, 16, ch);
    int n = (int)w.frameCount;
    if (n > SAMPLER_MAX_SEC * (int)SR) n = SAMPLER_MAX_SEC * (int)SR;
    Sample nx = { .ch = ch, .frames = n, .data = malloc((size_t)n * ch * sizeof(short)) };
    memcpy(nx.data, w.data, (size_t)n * ch * sizeof(short));
    UnloadWave(w);
    sample_summarize(&nx);
    s->next = nx;
    s->swap = 1;
    if (UI_OWNS_AUDIO) sampler_swap(s);
    if (!keep) {
        s->nmark = 1; s->mark[0] = 0;
        s->vs = 0; s->vw = n;
    }
    for (int i = 0; i < s->nmark; i++) s->mark[i] = clampi(s->mark[i], 0, n - 1);
    if (s->vw <= 0 || s->vs + s->vw > n) { s->vs = 0; s->vw = n; }
    return true;
}

static bool sampler_load(Module *m, const char *path)
{
    SamplerSt *s = m->st;
    if (!sampler_read(m, path, false)) { toast(TextFormat("CANNOT READ %s", GetFileName(path))); return false; }
    snprintf(s->name, sizeof s->name, "%s", GetFileName(path));
    s->dirty = true;
    toast(TextFormat("LOADED %s (%.1f S)", GetFileName(path), s->next.data ? s->next.frames / SR : s->smp.frames / SR));
    return true;
}

// Markers at the 8 strongest onsets (sudden rises in level).
static void sampler_auto(SamplerSt *s)
{
    const Sample *p = &s->smp;
    if (!p->nsum) return;
    float *o = calloc(p->nsum, sizeof(float));
    for (int b = 1; b < p->nsum; b++) {
        float prev = 0;
        for (int k = b - 4 < 0 ? 0 : b - 4; k < b; k++) prev = fmaxf(prev, p->pk[k]);
        o[b] = fmaxf(0, p->pk[b] - prev * 1.05f);
    }
    int gap = (int)(0.12f * SR / SUM_BLOCK), first = 0;
    while (first < p->nsum && p->pk[first] < 8) first++;
    s->nmark = 1;
    s->mark[0] = (first < p->nsum ? first : 0) * SUM_BLOCK;
    for (int b = first - gap; b < first + gap; b++) if (b >= 0 && b < p->nsum) o[b] = 0;
    while (s->nmark < SMP_MARKS) {
        int best = -1;
        for (int b = 0; b < p->nsum; b++) if (o[b] > 2 && (best < 0 || o[b] > o[best])) best = b;
        if (best < 0) break;
        s->mark[s->nmark++] = best * SUM_BLOCK;
        for (int b = best - gap; b < best + gap; b++) if (b >= 0 && b < p->nsum) o[b] = 0;
    }
    free(o);
    for (int i = 1; i < s->nmark; i++)
        for (int j = i; j > 0 && s->mark[j] < s->mark[j - 1]; j--) { int t = s->mark[j]; s->mark[j] = s->mark[j - 1]; s->mark[j - 1] = t; }
}

static Rectangle smp_lane(Rectangle a) { return (Rectangle){ a.x, a.y, a.width, 16 }; }
static Rectangle smp_wave(Rectangle a) { return (Rectangle){ a.x, a.y + 16, a.width, a.height - 64 }; }
static Rectangle smp_over(Rectangle a) { return (Rectangle){ a.x, a.y + a.height - 44, a.width, 12 }; }
static Rectangle smp_btn(Rectangle a, int i)   // LOAD, AUTO, then pads 1..8
{
    float w = (a.width - 9 * 4) / 10;
    return (Rectangle){ a.x + i * (w + 4), a.y + a.height - 24, w, 24 };
}

static double smp_frame_at(const SamplerSt *s, Rectangle r, float x) { return s->vs + (x - r.x) / r.width * s->vw; }
static float  smp_x_of(const SamplerSt *s, Rectangle r, double f)    { return r.x + (float)((f - s->vs) / s->vw) * r.width; }

static void smp_clamp_view(SamplerSt *s)
{
    if (!s->smp.data || s->swap) return;   // a new file is on its way: keep its view
    double n = s->smp.frames;
    if (s->vw > n) s->vw = n;
    if (s->vw < 64) s->vw = 64 < n ? 64 : n;
    if (s->vs < 0) s->vs = 0;
    if (s->vs + s->vw > n) s->vs = n - s->vw;
}

static int smp_marker_at(const SamplerSt *s, Rectangle lane, float x)
{
    for (int i = 0; i < s->nmark; i++) if (fabsf(smp_x_of(s, lane, s->mark[i]) - x) < 7) return i;
    return -1;
}

static void sampler_draw(Module *m, Rectangle a)
{
    SamplerSt *s = m->st;
    if (s->swap && UI_OWNS_AUDIO) sampler_swap(s);
    if (!s->swap && s->old.data) sample_free(&s->old);
    smp_clamp_view(s);
    Rectangle lane = smp_lane(a), wv = smp_wave(a), ov = smp_over(a);
    DrawRectangleRec((Rectangle){ a.x, a.y, a.width, wv.y + wv.height - a.y }, INSET);
    DrawRectangleRec(ov, INSET);
    const Sample *p = &s->smp;
    if (!p->data) {
        txt_c("LOAD OR DROP AN AUDIO FILE  (WAV, MP3, OGG, FLAC)", wv.x + wv.width / 2, wv.y + wv.height / 2 - 5, 10, TEXT_DIM);
    } else {
        BeginScissorMode((int)wv.x, (int)lane.y, (int)wv.width, (int)(wv.y + wv.height - lane.y));
        for (int i = 0; i < s->nmark; i++) {   // regions, tinted by marker
            float x0 = smp_x_of(s, wv, s->mark[i]), x1 = smp_x_of(s, wv, sampler_end(s, i));
            DrawRectangleRec((Rectangle){ x0, wv.y, x1 - x0, wv.height }, fade(CABLE_COLORS[i % N_CABLE_COLORS], 0.07f));
        }
        wave_view(p, (Rectangle){ wv.x, wv.y + 2, wv.width, wv.height - 4 }, s->vs, s->vw, 0.95f);
        for (int i = 0; i < s->nmark; i++) {
            float x = smp_x_of(s, wv, s->mark[i]);
            Color c = CABLE_COLORS[i % N_CABLE_COLORS];
            DrawLineEx((Vector2){ x, lane.y }, (Vector2){ x, wv.y + wv.height }, 1, fade(c, 0.8f));
            DrawRectangleRec((Rectangle){ x, lane.y + 1, 14, 13 }, c);
            txt(TextFormat("%d", i + 1), x + 4, lane.y + 3, 10, INSET);
        }
        for (int i = 0; i < SMP_MARKS; i++)
            if (s->v[i].state) {
                float x = smp_x_of(s, wv, s->v[i].pos);
                DrawLineEx((Vector2){ x, wv.y }, (Vector2){ x, wv.y + wv.height }, 1.5f, TEXT);
            }
        EndScissorMode();
        char nm[48];
        snprintf(nm, sizeof nm, "%s", s->name);
        for (int n = (int)strlen(nm); n > 3 && txt_w(nm, 10) > wv.width * 0.5f; n--) strcpy(nm + n - 4, "...");
        txt(nm, wv.x + 6, wv.y + 4, 10, fade(TEXT_DIM, 0.9f));
        const char *t = TextFormat("%.2f - %.2f / %.1f S", s->vs / SR, (s->vs + s->vw) / SR, p->frames / SR);
        txt(t, wv.x + wv.width - 6 - txt_w(t, 10), wv.y + 4, 10, fade(TEXT_DIM, 0.9f));
        // overview of the whole file with the visible window
        wave_view(p, ov, 0, p->frames, 0.5f);
        float x0 = ov.x + (float)(s->vs / p->frames) * ov.width, x1 = ov.x + (float)((s->vs + s->vw) / p->frames) * ov.width;
        DrawRectangleLinesEx((Rectangle){ x0, ov.y, fmaxf(x1 - x0, 2), ov.height }, 1, fade(TEXT, 0.6f));
    }
    for (int i = 0; i < 10; i++) {
        Rectangle b = smp_btn(a, i);
        bool hov = CheckCollisionPointRec(GetMousePosition(), b);
        if (i < 2) {
            DrawRectangleRec(b, hov ? PANEL_HI : INSET);
            DrawRectangleLinesEx(b, 1, PANEL_HI);
            txt_c(SMP_BTN[i], b.x + b.width / 2, b.y + 7, 10, hov ? TEXT : TEXT_DIM);
            continue;
        }
        int k = i - 2;
        bool has = k < s->nmark, on = s->v[k].state != 0;
        Color c = CABLE_COLORS[k % N_CABLE_COLORS];
        DrawRectangleRec(b, on ? fade(c, 0.5f) : (has ? fade(c, 0.12f) : INSET));
        DrawRectangleLinesEx(b, 1, has ? fade(c, 0.6f) : PANEL_HI);
        txt_c(TextFormat("%d", k + 1), b.x + b.width / 2, b.y + 7, 10, has ? TEXT : fade(TEXT_DIM, 0.5f));
    }
}

static bool sampler_press(Module *m, Vector2 mp, Rectangle a)
{
    SamplerSt *s = m->st;
    Rectangle lane = smp_lane(a), wv = smp_wave(a), ov = smp_over(a);
    s->drag = -1;
    for (int i = 0; i < 10; i++) {
        if (!CheckCollisionPointRec(mp, smp_btn(a, i))) continue;
        if (i == 0) {
#if defined(PLATFORM_WEB)
            g_pickTarget = (int)(m - mods);
            emscripten_run_script("rayaPickFile()");
#else
            toast("DROP AN AUDIO FILE ONTO THE SAMPLER TO LOAD IT");
#endif
        } else if (i == 1) sampler_auto(s);
        else sampler_play(m, i - 2);
        return true;
    }
    if (!s->smp.data) return CheckCollisionPointRec(mp, a);
    if (CheckCollisionPointRec(mp, lane)) {
        int k = smp_marker_at(s, lane, mp.x);
        if (k < 0 && s->nmark < SMP_MARKS) {   // a new marker
            s->mark[s->nmark] = clampi((int)smp_frame_at(s, lane, mp.x), 0, s->smp.frames - 1);
            k = s->nmark++;
            for (; k > 0 && s->mark[k] < s->mark[k - 1]; k--) { int t = s->mark[k]; s->mark[k] = s->mark[k - 1]; s->mark[k - 1] = t; }
        }
        s->drag = k;
        return true;
    }
    if (CheckCollisionPointRec(mp, wv)) {
        double now = GetTime();
        if (now - s->lastClick < 0.3) { s->vs = 0; s->vw = s->smp.frames; }   // double-click: whole file
        s->lastClick = now;
        s->drag = DRAG_VIEW; s->dragX = mp.x; s->dragVs = s->vs;
        return true;
    }
    if (CheckCollisionPointRec(mp, ov)) {
        s->drag = DRAG_OVER;
        s->vs = (mp.x - ov.x) / ov.width * s->smp.frames - s->vw / 2;
        smp_clamp_view(s);
        return true;
    }
    return CheckCollisionPointRec(mp, a);
}

static void sampler_drag(Module *m, Vector2 mp, Rectangle a)
{
    SamplerSt *s = m->st;
    Rectangle wv = smp_wave(a), ov = smp_over(a);
    if (s->drag >= 0 && s->drag < s->nmark) {   // move a marker, keeping them in order
        int k = s->drag;
        s->mark[k] = clampi((int)smp_frame_at(s, wv, mp.x), 0, s->smp.frames - 1);
        for (; k > 0 && s->mark[k] < s->mark[k - 1]; k--) { int t = s->mark[k]; s->mark[k] = s->mark[k - 1]; s->mark[k - 1] = t; }
        for (; k + 1 < s->nmark && s->mark[k] > s->mark[k + 1]; k++) { int t = s->mark[k]; s->mark[k] = s->mark[k + 1]; s->mark[k + 1] = t; }
        s->drag = k;
    } else if (s->drag == DRAG_VIEW) {
        s->vs = s->dragVs - (mp.x - s->dragX) / wv.width * s->vw;
    } else if (s->drag == DRAG_OVER) {
        s->vs = (mp.x - ov.x) / ov.width * s->smp.frames - s->vw / 2;
    }
    smp_clamp_view(s);
}

static bool sampler_wheel(Module *m, Vector2 mp, Rectangle a, float wheel)
{
    SamplerSt *s = m->st;
    Rectangle wv = smp_wave(a);
    if (!s->smp.data || mp.y < a.y || mp.y > wv.y + wv.height) return false;
    double f = smp_frame_at(s, wv, mp.x);
    s->vw *= pow(0.8, wheel);
    smp_clamp_view(s);
    s->vs = f - (mp.x - wv.x) / wv.width * s->vw;
    smp_clamp_view(s);
    return true;
}

static bool sampler_rpress(Module *m, Vector2 mp, Rectangle a)   // right-click a marker: remove it
{
    SamplerSt *s = m->st;
    Rectangle lane = smp_lane(a);
    if (!CheckCollisionPointRec(mp, (Rectangle){ lane.x, lane.y, lane.width, smp_wave(a).y + smp_wave(a).height - lane.y })) return false;
    int k = smp_marker_at(s, lane, mp.x);
    if (k < 0) return false;
    for (int i = 0; i < SMP_MARKS; i++) s->v[i].state = 0;
    for (int i = k; i + 1 < s->nmark; i++) s->mark[i] = s->mark[i + 1];
    s->nmark--;
    return true;
}

// data line: id view-start view-width markers... name
static void sampler_save(const Module *m, char *out, int cap)
{
    const SamplerSt *s = m->st;
    int n = snprintf(out, cap, "%u %.0f %.0f %d", s->id, s->vs, s->vw, s->nmark);
    for (int i = 0; i < s->nmark && n < cap; i++) n += snprintf(out + n, cap - n, " %d", s->mark[i]);
    if (n < cap) snprintf(out + n, cap - n, " %s", s->name);
}

static void sampler_load_data(Module *m, const char *str)
{
    SamplerSt *s = m->st;
    int used = 0, nm = 0;
    if (sscanf(str, "%u %lf %lf %d%n", &s->id, &s->vs, &s->vw, &nm, &used) < 4) return;
    str += used;
    s->nmark = 0;
    for (int i = 0; i < nm && i < SMP_MARKS; i++) {
        if (sscanf(str, " %d%n", &s->mark[i], &used) != 1) break;
        str += used;
        s->nmark++;
    }
    while (*str == ' ') str++;
    snprintf(s->name, sizeof s->name, "%s", str);
}

static unsigned *sampler_audio_id(Module *m) { return &((SamplerSt *)m->st)->id; }

static bool sampler_audio_save(Module *m, const char *path, int how)
{
    SamplerSt *s = m->st;
    if (!s->smp.data || (how != SAVE_ALL && !s->dirty)) return false;
    s->dirty = false;
    return write_wav(path, s->smp.data, s->smp.frames, s->smp.ch);
}

static void sampler_audio_load(Module *m, const char *path)
{
    sampler_read(m, path, true);
    ((SamplerSt *)m->st)->dirty = false;
}

// ---------------------------------------------------------------------------
// FILTER — a low cut (high-pass) and a high cut (low-pass) in series, stereo.
// The CV inputs open each cutoff by up to six octaves.
// ---------------------------------------------------------------------------

typedef struct { float ic1[4], ic2[4]; } FiltSt;

static void filt_init(Module *m) { m->st = calloc(1, sizeof(FiltSt)); }

// One step of a state-variable filter (Simper); returns low-pass, writes high-pass.
static float svf(float x, float fc, float k, float *ic1, float *ic2, float *hp)
{
    float g = tanf(3.14159265f * clampf(fc, 10, SR * 0.45f) / SR);
    float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float v3 = x - *ic2, v1 = a1 * *ic1 + a2 * v3, v2 = *ic2 + a2 * *ic1 + a3 * v3;
    *ic1 = 2 * v1 - *ic1; *ic2 = 2 * v2 - *ic2;
    *hp = x - k * v1 - v2;
    return v2;
}

static void filt_process(Module *m)
{
    FiltSt *s = m->st;
    float in[2] = { IN(m, 0), HAS(m, 1) ? IN(m, 1) : IN(m, 0) };
    float lo = m->p[0] * exp2f(IN(m, 3) * m->p[3] * 6), hi = m->p[1] * exp2f(IN(m, 2) * m->p[3] * 6);
    float k = 2 - 2 * m->p[2], hp;
    for (int c = 0; c < 2; c++) {
        float x = in[c];
        svf(x, lo, 1.414f, &s->ic1[c], &s->ic2[c], &hp);              // low cut
        m->out[c] = svf(hp, hi, k, &s->ic1[c + 2], &s->ic2[c + 2], &hp);   // high cut, resonant
    }
}

static void filt_draw(Module *m, Rectangle a)   // the pass band on a log frequency axis
{
    DrawRectangleRec(a, INSET);
    float lo = m->p[0] * exp2f(IN(m, 3) * m->p[3] * 6), hi = m->p[1] * exp2f(IN(m, 2) * m->p[3] * 6);
    float span = log2f(20000.0f / 20), x0 = a.x + clampf(log2f(lo / 20) / span, 0, 1) * a.width, x1 = a.x + clampf(log2f(hi / 20) / span, 0, 1) * a.width;
    if (x1 > x0) DrawRectangleRec((Rectangle){ x0, a.y + 6, x1 - x0, a.height - 12 }, fade(ACCENT, 0.18f));
    DrawLineEx((Vector2){ x0, a.y + 4 }, (Vector2){ x0, a.y + a.height - 4 }, 1.5f, ACCENT);
    DrawLineEx((Vector2){ x1, a.y + 4 }, (Vector2){ x1, a.y + a.height - 4 }, 1.5f, ACCENT);
    txt(TextFormat("%.0f HZ", lo), a.x + 4, a.y + a.height - 13, 10, TEXT_DIM);
    const char *h = hi < 1000 ? TextFormat("%.0f HZ", hi) : TextFormat("%.1f KHZ", hi / 1000);
    txt(h, a.x + a.width - 4 - txt_w(h, 10), a.y + 3, 10, TEXT_DIM);
}

// ---------------------------------------------------------------------------
// CHORUS — three slowly swept delay taps per side, spread across the stereo field
// ---------------------------------------------------------------------------

#define CH_LEN 4096

typedef struct { float buf[2][CH_LEN]; int w; float ph; } ChorusSt;

static void chorus_init(Module *m) { m->st = calloc(1, sizeof(ChorusSt)); }

static void chorus_process(Module *m)
{
    ChorusSt *s = m->st;
    float in[2] = { IN(m, 0), HAS(m, 1) ? IN(m, 1) : IN(m, 0) };
    s->ph = fractf(s->ph + m->p[0] / SR);
    s->w = (s->w + 1) & (CH_LEN - 1);
    for (int c = 0; c < 2; c++) {
        s->buf[c][s->w] = in[c];
        float wet = 0;
        for (int v = 0; v < 3; v++) {
            float d = (0.010f + m->p[1] * 0.007f * (1 + sinf(TAU * (s->ph + v / 3.0f + c / 6.0f)))) * SR;
            float rp = s->w - d;
            if (rp < 0) rp += CH_LEN;
            int i0 = (int)rp;
            float fr = rp - i0;
            i0 &= CH_LEN - 1;
            int i1 = (i0 + 1) & (CH_LEN - 1);
            wet += s->buf[c][i0] * (1 - fr) + s->buf[c][i1] * fr;
        }
        m->out[c] = in[c] * (1 - m->p[2] * 0.5f) + wet / 3 * m->p[2];
    }
}

// ---------------------------------------------------------------------------
// ARRANGER — the song: up to 12 sections, each so many bars long, played in
// order from a 1/16 clock. Each section lets lanes 1-8 through (IN n -> OUT n)
// or mutes them, and sets a LEVEL that glides there slowly (filter sweeps).
// RAMP rises from 0 to 1 across every section.
// ---------------------------------------------------------------------------

#define AR_SEC   12
#define AR_LANES 8
static const int AR_LENS[] = { 0, 1, 2, 4, 8, 12, 16, 24, 32, 48, 64 };
#define AR_NLENS ((int)(sizeof AR_LENS / sizeof AR_LENS[0]))

typedef struct {
    bool  on[AR_SEC][AR_LANES];
    int   len[AR_SEC];            // index into AR_LENS; 0 = section skipped
    float level[AR_SEC];
    int   sec, bar, step;
    float pclk, prst, lvl;
    int   dragLevel;              // section whose level is being dragged, or -1
} ArrSt;

static void arr_init(Module *m)
{
    ArrSt *s = calloc(1, sizeof(ArrSt));
    for (int i = 0; i < 4; i++) {
        s->len[i] = 4;   // 8 bars
        s->level[i] = 0.25f + 0.25f * i;
        for (int l = 0; l < AR_LANES; l++) s->on[i][l] = l <= i + 1;
    }
    s->step = -1;
    s->dragLevel = -1;
    m->st = s;
}

static int arr_next(const ArrSt *s, int from)   // next section that is not skipped
{
    for (int k = 1; k <= AR_SEC; k++) { int i = (from + k) % AR_SEC; if (s->len[i]) return i; }
    return from;
}

static void arr_process(Module *m)
{
    ArrSt *s = m->st;
    if (rising(IN(m, 1), &s->prst)) { s->sec = s->len[0] ? 0 : arr_next(s, 0); s->bar = 0; s->step = -1; }
    if (rising(IN(m, 0), &s->pclk)) {
        if (!s->len[s->sec]) s->sec = arr_next(s, s->sec);
        if (++s->step >= 16) {
            s->step = 0;
            if (++s->bar >= AR_LENS[s->len[s->sec]]) { s->bar = 0; s->sec = arr_next(s, s->sec); }
        }
    }
    for (int l = 0; l < AR_LANES; l++) m->out[l] = s->on[s->sec][l] ? IN(m, 2 + l) : 0;
    float g = m->p[0];
    s->lvl += (s->level[s->sec] - s->lvl) * (g < 0.01f ? 1 : 1 - expf(-1 / (g * SR)));
    m->out[AR_LANES] = s->lvl;
    int bars = AR_LENS[s->len[s->sec]];
    m->out[AR_LANES + 1] = bars ? (s->bar * 16 + (s->step < 0 ? 0 : s->step) + 1) / (float)(bars * 16) : 0;
}

#define AR_LABEL_W 26
static float arr_colw(Rectangle a) { return (a.width - AR_LABEL_W) / AR_SEC; }
static Rectangle arr_head(Rectangle a, int i)  { float w = arr_colw(a); return (Rectangle){ a.x + AR_LABEL_W + i * w, a.y, w - 2, 26 }; }
static Rectangle arr_cell(Rectangle a, int i, int l) { float w = arr_colw(a); return (Rectangle){ a.x + AR_LABEL_W + i * w, a.y + 30 + l * 17, w - 2, 15 }; }
static Rectangle arr_lvl(Rectangle a, int i)   { float w = arr_colw(a); return (Rectangle){ a.x + AR_LABEL_W + i * w, a.y + 30 + AR_LANES * 17 + 2, w - 2, a.height - (30 + AR_LANES * 17 + 2) }; }

static void arr_draw(Module *m, Rectangle a)
{
    ArrSt *s = m->st;
    txt("LEN", a.x, a.y + 14, 10, TEXT_DIM);
    for (int l = 0; l < AR_LANES; l++) txt(TextFormat("%d", l + 1), a.x + 8, arr_cell(a, 0, l).y + 3, 10, TEXT_DIM);
    txt("LVL", a.x, arr_lvl(a, 0).y + 8, 10, TEXT_DIM);
    for (int i = 0; i < AR_SEC; i++) {
        bool cur = i == s->sec, used = s->len[i] > 0;
        Rectangle h = arr_head(a, i);
        DrawRectangleRec(h, cur ? fade(ACCENT, 0.25f) : INSET);
        txt_c(TextFormat("%d", i + 1), h.x + h.width / 2, h.y + 2, 10, cur ? ACCENT : (used ? TEXT : fade(TEXT_DIM, 0.5f)));
        txt_c(used ? TextFormat("%d", AR_LENS[s->len[i]]) : "-", h.x + h.width / 2, h.y + 14, 10, used ? TEXT_DIM : fade(TEXT_DIM, 0.4f));
        if (cur && used) {   // progress through the section
            float p = (s->bar * 16 + (s->step < 0 ? 0 : s->step) + 1) / (float)(AR_LENS[s->len[i]] * 16);
            DrawRectangle((int)h.x, (int)(h.y + h.height - 2), (int)(h.width * p), 2, ACCENT);
        }
        for (int l = 0; l < AR_LANES; l++) {
            Rectangle c = arr_cell(a, i, l);
            Color col = CABLE_COLORS[l % N_CABLE_COLORS];
            DrawRectangleRec(c, s->on[i][l] ? fade(col, used ? (cur ? 0.85f : 0.5f) : 0.15f) : INSET);
        }
        Rectangle lv = arr_lvl(a, i);
        DrawRectangleRec(lv, INSET);
        float hgt = (lv.height - 2) * s->level[i];
        DrawRectangleRec((Rectangle){ lv.x + 1, lv.y + lv.height - 1 - hgt, lv.width - 2, hgt }, fade(ACCENT, used ? (cur ? 0.8f : 0.45f) : 0.15f));
    }
}

static void arr_set_level(ArrSt *s, Rectangle a, int i, float y)
{
    Rectangle lv = arr_lvl(a, i);
    s->level[i] = clampf((lv.y + lv.height - y) / lv.height, 0, 1);
}

static bool arr_press(Module *m, Vector2 mp, Rectangle a)
{
    ArrSt *s = m->st;
    s->dragLevel = -1;
    for (int i = 0; i < AR_SEC; i++) {
        if (CheckCollisionPointRec(mp, arr_head(a, i))) {   // jump to this section
            if (!s->len[i]) s->len[i] = 4;
            s->sec = i; s->bar = 0; s->step = -1;
            return true;
        }
        for (int l = 0; l < AR_LANES; l++)
            if (CheckCollisionPointRec(mp, arr_cell(a, i, l))) { s->on[i][l] = !s->on[i][l]; return true; }
        if (CheckCollisionPointRec(mp, arr_lvl(a, i))) { s->dragLevel = i; arr_set_level(s, a, i, mp.y); return true; }
    }
    return CheckCollisionPointRec(mp, a);
}

static void arr_drag(Module *m, Vector2 mp, Rectangle a)
{
    ArrSt *s = m->st;
    if (s->dragLevel >= 0) arr_set_level(s, a, s->dragLevel, mp.y);
}

static bool arr_wheel(Module *m, Vector2 mp, Rectangle a, float w)   // wheel on a header: section length
{
    ArrSt *s = m->st;
    for (int i = 0; i < AR_SEC; i++)
        if (CheckCollisionPointRec(mp, arr_head(a, i))) { s->len[i] = clampi(s->len[i] + (w > 0 ? 1 : -1), 0, AR_NLENS - 1); return true; }
    return false;
}

static bool arr_rpress(Module *m, Vector2 mp, Rectangle a)   // right-click a header: skip the section
{
    ArrSt *s = m->st;
    for (int i = 0; i < AR_SEC; i++)
        if (CheckCollisionPointRec(mp, arr_head(a, i))) { s->len[i] = 0; return true; }
    return false;
}

// data line: per section "bars:level:lanes" (lanes as 1/0 per lane)
static void arr_save(const Module *m, char *out, int cap)
{
    const ArrSt *s = m->st;
    int n = 0;
    for (int i = 0; i < AR_SEC && n < cap; i++) {
        n += snprintf(out + n, cap - n, "%s%d:%.3f:", i ? " " : "", AR_LENS[s->len[i]], s->level[i]);
        for (int l = 0; l < AR_LANES && n < cap; l++) n += snprintf(out + n, cap - n, "%d", s->on[i][l]);
    }
}

static void arr_load(Module *m, const char *str)
{
    ArrSt *s = m->st;
    for (int i = 0; i < AR_SEC; i++) {
        int bars, used; float lv; char lanes[AR_LANES + 1] = { 0 };
        if (sscanf(str, " %d:%f:%8[01]%n", &bars, &lv, lanes, &used) != 3) break;
        str += used;
        s->len[i] = 0;
        for (int k = 0; k < AR_NLENS; k++) if (AR_LENS[k] == bars) s->len[i] = k;
        s->level[i] = clampf(lv, 0, 1);
        for (int l = 0; l < AR_LANES; l++) s->on[i][l] = lanes[l] == '1';
    }
    s->sec = s->len[0] ? 0 : arr_next(s, 0);
    s->lvl = s->level[s->sec];
}

// ---------------------------------------------------------------------------
// WAVOR — wavetable / additive / sampling hybrid synth.
//
// Left pane: an ADSR, the WAVETABLE (one of 64 waves, a drawn shape, drawn
// partials, or one cycle sampled from a dropped audio file), four MORPHERS
// (64 kinds) that reshape it, and the VOICES stage: up to 64 unison copies
// of all of that, spread across the stereo image and shaped by curves
// (detune, aggressiveness, entropizer, position, level). Right pane: a chain
// of ten effects.
//
// The UI turns wave + morphers into WV_F frames (frame 0 = the plain wave,
// the last = fully morphed; POSITION picks between them) and band-limits each
// frame into mip levels by FFT, a few frames per video frame. Every frame
// also gets a brighter, saturated twin that AGGRESSIVENESS fades towards. The
// audio side only reads finished tables, handed over like the tape buffers.
// ---------------------------------------------------------------------------

#define WV_N      2048                 // samples per cycle at full resolution
#define WV_HALF   (WV_N / 2)
#define WV_F      32                   // frames from plain wave to fully morphed
#define WV_LV     9                    // mip levels: 512, 256, ... 2 harmonics
#define WV_T      4233                 // floats per frame and variant: all levels plus a guard sample each
#define WV_MAXV   64                   // unison voices per note
#define WV_NOTES  4
#define WV_SLOTS  4
#define WV_NWAVE  64
#define WV_USER   WV_NWAVE             // wave index of the user table
#define WV_UPTS   256                  // points of a drawn shape
#define WV_HARM   32                   // partials in the additive editor
#define WV_BLOCK  32                   // the engine renders in blocks (one block of latency)
#define WV_NCURVE 5
#define WV_DISP   96                   // points per frame in the wavetable view
#define WV_DLY    131072               // delay line: 2.7 s
#define WV_CHO    4096
#define WV_FLG    2048
#define WV_W      1220                 // module width
#define WV_H      878                  // custom area height
#define WV_BAR    36                   // the preset bar above the panes

static float knob_to_t(const KnobDef *k, float v);
typedef struct WavorSt WavorSt;
static unsigned wv_sound_sig(const WavorSt *s);
static float knob_from_t(const KnobDef *k, float t);
static const char *knob_text(const KnobDef *k, float v);

static int   g_kbd_owner = -1;   // module typing text: keyboard shortcuts are off
static float wv_cs[WV_HALF], wv_sn[WV_HALF];
static int   wv_lsize[WV_LV], wv_loff[WV_LV];
static float wv_thumb[WV_NWAVE][48];
static bool  wv_inited;

static const char *WV_WAVES[WV_NWAVE + 1] = {
    "SINE", "TRIANGLE", "SQUARE", "8-BIT", "SAW", "PULSE 25", "PULSE 12", "HALF SINE",
    "ABS SINE", "SOFT SAW", "SOFT SQR", "TRAPEZOID", "STAIRS", "NES TRI", "PARABOLA", "DBL SAW",
    "SINE+OCT", "SINE+12", "ORGAN 1", "ORGAN 2", "ORGAN 3", "FLUTE", "CLARINET", "OBOE",
    "BRASS", "STRINGS", "VOCAL A", "VOCAL E", "VOCAL I", "VOCAL O", "VOCAL U", "CHOIR",
    "BELL", "GLASS", "HOLLOW", "NASAL", "PRIMES", "FIBONACCI", "OCTAVES", "ODD HARM",
    "EVEN HARM", "BUZZ 16", "BUZZ 64", "FORMANT 1", "FORMANT 2", "FORMANT 3", "RANDOM 1", "RANDOM 2",
    "NOISE", "RESO SAW", "RESO PLS", "PHASE DST", "FM 1:1", "FM 2:1", "FM 3:1", "FM BELL",
    "SYNC SAW", "SYNC SINE", "FOLDED", "CHEBYSHEV", "TANH SAW", "SPIKE", "BIPULSE", "SAW+SQR",
    "USER",
};

// 64 morphers in 8 families of 8 (the picker shows one family per column).
static const char *WV_MCAT[8] = { "WARP", "FREQUENCY", "FM + MOD", "SHAPE", "DIGITAL", "FILTER", "HARMONICS", "TEXTURE" };
static const char *WV_MORPHS[64] = {
    "SKEW L", "SKEW R", "PINCH", "BEND", "MIRROR", "SQUASH", "STRETCH", "FLIP",
    "SYNC", "SOFT SYNC", "MULTIPLY", "SHIFT UP", "SHIFT DN", "SPREAD", "SQUEEZE", "+OCTAVE",
    "FM 1X", "FM 2X", "FM 3X", "FM 7X", "SELF FM", "RING 3X", "AM", "DISPERSE",
    "SATURATE", "HARDCLIP", "FOLD", "ASYM", "RECTIFY", "FATTEN", "EXPAND", "WRAP",
    "BITCRUSH", "DOWNSMPL", "CHEBY 2", "CHEBY 3", "SLEW", "NOISE", "RND HARM", "STUTTER",
    "LOWPASS", "HIGHPASS", "BANDPASS", "FORMANT", "COMB", "NOTCH", "DARKEN", "BRIGHTEN",
    "ODD ONLY", "EVEN ONLY", "DECIMATE", "PRIMES", "OCTAVES", "BLUR", "PEAKS", "INVERT",
    "VOWEL", "WINDOW", "RESONATE", "SINE SHP", "SCRAMBLE", "SPEC GATE", "TO SINE", "TO SAW",
};

// ---- parameters -----------------------------------------------------------

enum { WC_DET, WC_AGG, WC_ENT, WC_POS, WC_LVL };
static const char *WV_CURVES[WV_NCURVE] = { "DETUNE", "AGGRESSION", "ENTROPIZER", "POSITION", "LEVEL" };
static const int   WV_CURVE_COL[WV_NCURVE] = { 0, 1, 4, 3, 2 };   // CABLE_COLORS

enum {
    WQ_A, WQ_D, WQ_S, WQ_R,
    WQ_OCT, WQ_GLIDE, WQ_PHASE, WQ_LEVEL, WQ_POS, WQ_ENVPOS, WQ_LFORATE, WQ_LFOPOS,
    WQ_WAVE,
    WQ_M1T, WQ_M1A, WQ_M2T, WQ_M2A, WQ_M3T, WQ_M3A, WQ_M4T, WQ_M4A,
    WQ_VOICES, WQ_SPREAD, WQ_ENTRATE,
    WQ_CURVE,                                   // WV_NCURVE x (left, centre, right)
    WQ_FLT_ON = WQ_CURVE + WV_NCURVE * 3, WQ_FLT_MODE, WQ_FLT_CUT, WQ_FLT_RES, WQ_FLT_ENV,
    WQ_EQ_ON, WQ_EQ_LOW, WQ_EQ_MID, WQ_EQ_FREQ, WQ_EQ_HIGH,
    WQ_SAT_ON, WQ_SAT_DRIVE, WQ_SAT_BIAS, WQ_SAT_TONE, WQ_SAT_MIX,
    WQ_DST_ON, WQ_DST_TYPE, WQ_DST_DRIVE, WQ_DST_TONE, WQ_DST_MIX,
    WQ_CMP_ON, WQ_CMP_THR, WQ_CMP_RATIO, WQ_CMP_ATK, WQ_CMP_REL, WQ_CMP_GAIN,
    WQ_CHO_ON, WQ_CHO_RATE, WQ_CHO_DEPTH, WQ_CHO_MIX,
    WQ_FLG_ON, WQ_FLG_RATE, WQ_FLG_DEPTH, WQ_FLG_FB, WQ_FLG_MIX,
    WQ_PHS_ON, WQ_PHS_RATE, WQ_PHS_DEPTH, WQ_PHS_FB, WQ_PHS_MIX,
    WQ_DLY_ON, WQ_DLY_TIME, WQ_DLY_FB, WQ_DLY_PING, WQ_DLY_TONE, WQ_DLY_MIX,
    WQ_PLT_ON, WQ_PLT_PRE, WQ_PLT_DECAY, WQ_PLT_DAMP, WQ_PLT_MIX,
    WQ_FLT_KEY, WQ_MA, WQ_MD, WQ_MCUT, WQ_MPOS, WQ_MPITCH, WQ_TREM,
    WQ_VIBRATE, WQ_VIBDEPTH, WQ_VIBDELAY, WQ_NOISE, WQ_NTONE, WQ_NDECAY,
    WQ_N
};

typedef struct { const char *key; KnobDef k; } WvParam;

static const char *WV_FLT_MODES[] = { "LP", "BP", "HP", "NOTCH" };
static const char *WV_DST_TYPES[] = { "CLIP", "FOLD", "CRUSH", "FUZZ" };

#define WV_ONOFF(key, name, on) { key, { name, 0, 1, on, K_STEP, F_INT } }
#define WV_CURVE_P(c, key, l, m, r) \
    [WQ_CURVE + (c) * 3]     = { key "l", { "LEFT",   0, 1, l, K_KNOB, F_PCT } }, \
    [WQ_CURVE + (c) * 3 + 1] = { key "c", { "CENTRE", 0, 1, m, K_KNOB, F_PCT } }, \
    [WQ_CURVE + (c) * 3 + 2] = { key "r", { "RIGHT",  0, 1, r, K_KNOB, F_PCT } }

static const WvParam WVP[WQ_N] = {
    [WQ_A]       = { "a",       { "ATTACK", 0.002f, 20, 1.2f, K_EXP, F_SEC } },
    [WQ_D]       = { "d",       { "DECAY", 0.01f, 20, 2.5f, K_EXP, F_SEC } },
    [WQ_S]       = { "s",       { "SUSTAIN", 0, 1, 0.75f, K_KNOB, F_PCT } },
    [WQ_R]       = { "r",       { "RELEASE", 0.01f, 30, 5, K_EXP, F_SEC } },
    [WQ_OCT]     = { "oct",     { "OCTAVE", -3, 3, 0, K_STEP, F_OCT } },
    [WQ_GLIDE]   = { "glide",   { "GLIDE", 0, 2, 0.35f, K_KNOB, F_SEC } },
    [WQ_PHASE]   = { "phase",   { "PHASE RND", 0, 1, 1, K_KNOB, F_PCT } },
    [WQ_LEVEL]   = { "level",   { "LEVEL", 0, 1, 0.7f, K_KNOB, F_PCT } },
    [WQ_POS]     = { "pos",     { "POSITION", 0, 1, 0.3f, K_KNOB, F_PCT } },
    [WQ_ENVPOS]  = { "envpos",  { "ENV>POS", -1, 1, 0.15f, K_KNOB, F_NUM } },
    [WQ_LFORATE] = { "lforate", { "LFO RATE", 0.01f, 8, 0.07f, K_EXP, F_HZ } },
    [WQ_LFOPOS]  = { "lfopos",  { "LFO>POS", 0, 1, 0.25f, K_KNOB, F_PCT } },
    [WQ_WAVE]    = { "wave",    { "WAVE", 0, WV_USER, 25, K_STEP, F_INT } },
    [WQ_M1T]     = { "m1",      { "MORPH 1", 0, 63, 43, K_STEP, F_INT } },
    [WQ_M1A]     = { "m1a",     { "AMOUNT", 0, 1, 0.7f, K_KNOB, F_PCT } },
    [WQ_M2T]     = { "m2",      { "MORPH 2", 0, 63, 9, K_STEP, F_INT } },
    [WQ_M2A]     = { "m2a",     { "AMOUNT", 0, 1, 0.45f, K_KNOB, F_PCT } },
    [WQ_M3T]     = { "m3",      { "MORPH 3", 0, 63, 46, K_STEP, F_INT } },
    [WQ_M3A]     = { "m3a",     { "AMOUNT", 0, 1, 0, K_KNOB, F_PCT } },
    [WQ_M4T]     = { "m4",      { "MORPH 4", 0, 63, 24, K_STEP, F_INT } },
    [WQ_M4A]     = { "m4a",     { "AMOUNT", 0, 1, 0, K_KNOB, F_PCT } },
    [WQ_VOICES]  = { "voices",  { "VOICES", 1, WV_MAXV, 8, K_STEP, F_INT } },
    [WQ_SPREAD]  = { "spread",  { "SPREAD", 0, 1, 0.85f, K_KNOB, F_PCT } },
    [WQ_ENTRATE] = { "entrate", { "ENT RATE", 0.01f, 4, 0.15f, K_EXP, F_HZ } },
    WV_CURVE_P(WC_DET, "cdet", 0.55f, 0.2f, 0.55f),
    WV_CURVE_P(WC_AGG, "cagg", 0.15f, 0, 0.15f),
    WV_CURVE_P(WC_ENT, "cent", 0.35f, 0.25f, 0.35f),
    WV_CURVE_P(WC_POS, "cpos", 0.38f, 0.5f, 0.62f),
    WV_CURVE_P(WC_LVL, "clvl", 0.8f, 1, 0.8f),
    [WQ_FLT_ON]    = WV_ONOFF("flt", "FILTER", 1),
    [WQ_FLT_MODE]  = { "fltmode", { "MODE", 0, 3, 0, K_STEP, F_LABEL, WV_FLT_MODES } },
    [WQ_FLT_CUT]   = { "fltcut",  { "CUTOFF", 30, 18000, 5000, K_EXP, F_HZ } },
    [WQ_FLT_RES]   = { "fltres",  { "RESO", 0, 1, 0.15f, K_KNOB, F_PCT } },
    [WQ_FLT_ENV]   = { "fltenv",  { "ENV AMT", -1, 1, 0.2f, K_KNOB, F_NUM } },
    [WQ_EQ_ON]     = WV_ONOFF("eq", "EQ", 0),
    [WQ_EQ_LOW]    = { "eqlow",   { "LOW", -15, 15, 0, K_KNOB, F_DB } },
    [WQ_EQ_MID]    = { "eqmid",   { "MID", -15, 15, 0, K_KNOB, F_DB } },
    [WQ_EQ_FREQ]   = { "eqfreq",  { "MID FREQ", 150, 8000, 1000, K_EXP, F_HZ } },
    [WQ_EQ_HIGH]   = { "eqhigh",  { "HIGH", -15, 15, 0, K_KNOB, F_DB } },
    [WQ_SAT_ON]    = WV_ONOFF("sat", "SATURATION", 0),
    [WQ_SAT_DRIVE] = { "satdrive", { "DRIVE", 0, 1, 0.35f, K_KNOB, F_PCT } },
    [WQ_SAT_BIAS]  = { "satbias",  { "BIAS", 0, 1, 0.2f, K_KNOB, F_PCT } },
    [WQ_SAT_TONE]  = { "sattone",  { "TONE", 0, 1, 0.7f, K_KNOB, F_PCT } },
    [WQ_SAT_MIX]   = { "satmix",   { "MIX", 0, 1, 1, K_KNOB, F_PCT } },
    [WQ_DST_ON]    = WV_ONOFF("dst", "DISTORTION", 0),
    [WQ_DST_TYPE]  = { "dsttype",  { "TYPE", 0, 3, 0, K_STEP, F_LABEL, WV_DST_TYPES } },
    [WQ_DST_DRIVE] = { "dstdrive", { "DRIVE", 0, 1, 0.4f, K_KNOB, F_PCT } },
    [WQ_DST_TONE]  = { "dsttone",  { "TONE", 0, 1, 0.6f, K_KNOB, F_PCT } },
    [WQ_DST_MIX]   = { "dstmix",   { "MIX", 0, 1, 0.5f, K_KNOB, F_PCT } },
    [WQ_CMP_ON]    = WV_ONOFF("cmp", "COMPRESSOR", 0),
    [WQ_CMP_THR]   = { "cmpthr",   { "THRESH", -40, 0, -18, K_KNOB, F_DB } },
    [WQ_CMP_RATIO] = { "cmpratio", { "RATIO", 1, 20, 4, K_EXP, F_RATIO } },
    [WQ_CMP_ATK]   = { "cmpatk",   { "ATTACK", 0.0005f, 0.2f, 0.01f, K_EXP, F_SEC } },
    [WQ_CMP_REL]   = { "cmprel",   { "RELEASE", 0.02f, 2, 0.25f, K_EXP, F_SEC } },
    [WQ_CMP_GAIN]  = { "cmpgain",  { "MAKEUP", 0, 24, 4, K_KNOB, F_DB } },
    [WQ_CHO_ON]    = WV_ONOFF("cho", "CHORUS", 1),
    [WQ_CHO_RATE]  = { "chorate",  { "RATE", 0.05f, 5, 0.35f, K_EXP, F_HZ } },
    [WQ_CHO_DEPTH] = { "chodepth", { "DEPTH", 0, 1, 0.5f, K_KNOB, F_PCT } },
    [WQ_CHO_MIX]   = { "chomix",   { "MIX", 0, 1, 0.35f, K_KNOB, F_PCT } },
    [WQ_FLG_ON]    = WV_ONOFF("flg", "FLANGER", 0),
    [WQ_FLG_RATE]  = { "flgrate",  { "RATE", 0.02f, 5, 0.15f, K_EXP, F_HZ } },
    [WQ_FLG_DEPTH] = { "flgdepth", { "DEPTH", 0, 1, 0.6f, K_KNOB, F_PCT } },
    [WQ_FLG_FB]    = { "flgfb",    { "FEEDBACK", -0.95f, 0.95f, 0.5f, K_KNOB, F_NUM } },
    [WQ_FLG_MIX]   = { "flgmix",   { "MIX", 0, 1, 0.5f, K_KNOB, F_PCT } },
    [WQ_PHS_ON]    = WV_ONOFF("phs", "PHASER", 0),
    [WQ_PHS_RATE]  = { "phsrate",  { "RATE", 0.02f, 5, 0.2f, K_EXP, F_HZ } },
    [WQ_PHS_DEPTH] = { "phsdepth", { "DEPTH", 0, 1, 0.7f, K_KNOB, F_PCT } },
    [WQ_PHS_FB]    = { "phsfb",    { "FEEDBACK", 0, 0.9f, 0.4f, K_KNOB, F_PCT } },
    [WQ_PHS_MIX]   = { "phsmix",   { "MIX", 0, 1, 0.5f, K_KNOB, F_PCT } },
    [WQ_DLY_ON]    = WV_ONOFF("dly", "DELAY", 1),
    [WQ_DLY_TIME]  = { "dlytime",  { "TIME", 0.02f, 2.5f, 0.42f, K_EXP, F_SEC } },
    [WQ_DLY_FB]    = { "dlyfb",    { "FEEDBACK", 0, 0.95f, 0.45f, K_KNOB, F_PCT } },
    [WQ_DLY_PING]  = { "dlyping",  { "PING-PONG", 0, 1, 0.6f, K_KNOB, F_PCT } },
    [WQ_DLY_TONE]  = { "dlytone",  { "TONE", 0, 1, 0.45f, K_KNOB, F_PCT } },
    [WQ_DLY_MIX]   = { "dlymix",   { "MIX", 0, 1, 0.25f, K_KNOB, F_PCT } },
    [WQ_PLT_ON]    = WV_ONOFF("plt", "PLATE REVERB", 1),
    [WQ_PLT_PRE]   = { "pltpre",   { "PREDELAY", 0, 0.2f, 0.02f, K_KNOB, F_SEC } },
    [WQ_PLT_DECAY] = { "pltdecay", { "DECAY", 0.05f, 0.97f, 0.7f, K_KNOB, F_PCT } },
    [WQ_PLT_DAMP]  = { "pltdamp",  { "DAMP", 0, 1, 0.35f, K_KNOB, F_PCT } },
    [WQ_PLT_MIX]   = { "pltmix",   { "MIX", 0, 1, 0.35f, K_KNOB, F_PCT } },
    [WQ_FLT_KEY]   = { "fltkey",   { "KEYTRACK", 0, 1, 0, K_KNOB, F_PCT } },
    [WQ_MA]        = { "ma",       { "M ATTACK", 0.001f, 5, 0.001f, K_EXP, F_SEC } },
    [WQ_MD]        = { "md",       { "M DECAY", 0.01f, 10, 0.5f, K_EXP, F_SEC } },
    [WQ_MCUT]      = { "mcut",     { ">CUTOFF", -8, 8, 0, K_KNOB, F_OCTS } },
    [WQ_MPOS]      = { "mpos",     { ">POSITION", -1, 1, 0, K_KNOB, F_NUM } },
    [WQ_MPITCH]    = { "mpitch",   { ">PITCH", -24, 24, 0, K_KNOB, F_SEMIS } },
    [WQ_TREM]      = { "trem",     { "TREMOLO", 0, 1, 0, K_KNOB, F_PCT } },
    [WQ_VIBRATE]   = { "vibrate",  { "VIB RATE", 0.5f, 12, 5.5f, K_EXP, F_HZ } },
    [WQ_VIBDEPTH]  = { "vibdepth", { "VIB DEPTH", 0, 1, 0, K_KNOB, F_SEMIS } },
    [WQ_VIBDELAY]  = { "vibdelay", { "VIB DELAY", 0, 4, 0, K_KNOB, F_SEC } },
    [WQ_NOISE]     = { "noise",    { "NOISE", 0, 1, 0, K_KNOB, F_PCT } },
    [WQ_NTONE]     = { "ntone",    { "N TONE", 0.5f, 32, 4, K_EXP, F_NUM } },
    [WQ_NDECAY]    = { "ndecay",   { "N DECAY", 0, 3, 0, K_KNOB, F_SEC } },
};

typedef struct { const char *name; int on, n, k[5]; } WvFx;
#define WV_NFX 10
static const WvFx WV_FX[WV_NFX] = {
    { "FILTER",       WQ_FLT_ON, 5, { WQ_FLT_MODE, WQ_FLT_CUT, WQ_FLT_RES, WQ_FLT_ENV, WQ_FLT_KEY } },
    { "EQ",           WQ_EQ_ON,  4, { WQ_EQ_LOW, WQ_EQ_MID, WQ_EQ_FREQ, WQ_EQ_HIGH } },
    { "SATURATION",   WQ_SAT_ON, 4, { WQ_SAT_DRIVE, WQ_SAT_BIAS, WQ_SAT_TONE, WQ_SAT_MIX } },
    { "DISTORTION",   WQ_DST_ON, 4, { WQ_DST_TYPE, WQ_DST_DRIVE, WQ_DST_TONE, WQ_DST_MIX } },
    { "COMPRESSOR",   WQ_CMP_ON, 5, { WQ_CMP_THR, WQ_CMP_RATIO, WQ_CMP_ATK, WQ_CMP_REL, WQ_CMP_GAIN } },
    { "CHORUS",       WQ_CHO_ON, 3, { WQ_CHO_RATE, WQ_CHO_DEPTH, WQ_CHO_MIX } },
    { "FLANGER",      WQ_FLG_ON, 4, { WQ_FLG_RATE, WQ_FLG_DEPTH, WQ_FLG_FB, WQ_FLG_MIX } },
    { "PHASER",       WQ_PHS_ON, 4, { WQ_PHS_RATE, WQ_PHS_DEPTH, WQ_PHS_FB, WQ_PHS_MIX } },
    { "DELAY",        WQ_DLY_ON, 5, { WQ_DLY_TIME, WQ_DLY_FB, WQ_DLY_PING, WQ_DLY_TONE, WQ_DLY_MIX } },
    { "PLATE REVERB", WQ_PLT_ON, 4, { WQ_PLT_PRE, WQ_PLT_DECAY, WQ_PLT_DAMP, WQ_PLT_MIX } },
};

// ---- state ----------------------------------------------------------------

typedef struct {
    float fic1[2], fic2[2];                               // filter
    Biquad eq[3][2]; float eqKey[4];                      // eq
    float satLp[2], dstLp[2][2], hold[2]; int holdN;     // saturation, distortion
    float gr;                                             // compressor gain reduction (dB)
    float cho[2][WV_CHO], choPh; int cw;                  // chorus
    float flg[2][WV_FLG], flgPh; int fw;                  // flanger
    float apz[2][6], phsPh, phsFb[2];                     // phaser
    float dly[2][WV_DLY], dt, dlp[2]; int dw;             // delay
    float pre[16384], ap[4][1024], mapL[2048], mapR[2048];   // plate (Dattorro)
    float dL1[8192], dL2[8192], dR1[8192], dR2[8192], apL2[4096], apR2[8192];
    float dampL, dampR, lastL, lastR, pph; int pw;
} WvFxSt;

typedef struct {   // what a table build is made from, copied when it starts
    int   wave, userMode, mt[WV_SLOTS];
    float ma[WV_SLOTS], user[WV_UPTS], part[WV_HARM];
} WvSnap;

enum { WD_NONE, WD_KNOB, WD_CURVE, WD_EDIT };

struct WavorSt {
    float q[WQ_N];
    // wavetable: built by the UI into tab[1 - cur], taken by the audio via pending
    float *tab[2];
    volatile int cur, pending;
    unsigned sig;
    bool  building; int bf, dst;
    WvSnap snap;
    float base[WV_N];
    float disp[WV_F][WV_DISP]; bool dispOk;
    // user table: a drawn shape or drawn partials
    float user[WV_UPTS], part[WV_HARM];
    int   userMode; bool userSet; unsigned userVer;
    // envelope, rendered one block ahead
    int   stage; bool gate, trig; float env, cA, cD, cR;
    float envBuf[WV_BLOCK], outL[WV_BLOCK], outR[WV_BLOCK]; int bi;
    // notes and voices
    float pitch[WV_NOTES]; bool was[WV_NOTES];
    float ph[WV_NOTES][WV_MAXV];
    float ent[WV_MAXV][5], entT[WV_MAXV][5];
    float lph;
    float vx[WV_MAXV], vl[WV_MAXV], va[WV_MAXV], vp[WV_MAXV]; int vn, dnotes; float dpos;   // for the UI
    WvFxSt fx;
    float menv, tOn, vph, nenv, nic1, nic2; int mstage;   // mod envelope, vibrato, noise
    int   pidx; char pname[33]; unsigned psig;              // preset: 0..127 factory, 128..255 user, -1 none
    bool  browse, saveMode; int browseTab, naming, armDel; char nameBuf[33];
    // UI
    int   pick;            // -1, 0 = wave picker, 1..4 = morpher picker for that slot
    bool  drawMode;
    int   curve, drag, dragQ, dragH, lastKnob, lastJ;
    float dragT, lastY, lastV;
    double lastClick;
};

// ---- tables: FFT, waves, morphers ----------------------------------------

static void wv_fft(float *re, float *im, int n, bool inv)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, step = WV_N / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; k++) {
                float wr = wv_cs[k * step], wi = inv ? wv_sn[k * step] : -wv_sn[k * step];
                int a = i + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
    }
}

static float wv_hash(unsigned x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return (x & 0xffffff) / 16777216.0f;
}

static bool wv_isprime(int n)
{
    if (n < 2) return false;
    for (int d = 2; d * d <= n; d++) if (n % d == 0) return false;
    return true;
}

// Remove DC and scale to peak 1, boosting a near-silent cycle at most 20x so
// a filter that removes (almost) everything stays (almost) silent.
static void wv_normalize(float *x, int n)
{
    float mean = 0, pk = 0;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= n;
    for (int i = 0; i < n; i++) { x[i] -= mean; pk = fmaxf(pk, fabsf(x[i])); }
    float g = 1 / fmaxf(pk, 0.05f);
    for (int i = 0; i < n; i++) x[i] *= g;
}

static float wv_at(const float *x, float ph)   // cyclic, linear interpolation
{
    float p = (ph - floorf(ph)) * WV_N;
    int i = (int)p;
    float f = p - i;
    int i0 = i & (WV_N - 1), i1 = (i + 1) & (WV_N - 1);
    return x[i0] + f * (x[i1] - x[i0]);
}

static float wv_fold(float v) { return 1 - 4 * fabsf(fractf((v + 1) * 0.25f) - 0.5f); }

static void wv_from_harm(float *y, const float *amp, int nh)   // sum of sines, amp[h] for h = 1..nh
{
    static float re[WV_N], im[WV_N];
    memset(re, 0, sizeof re); memset(im, 0, sizeof im);
    for (int h = 1; h <= nh && h < WV_HALF; h++) { im[h] = -amp[h] * WV_HALF; im[WV_N - h] = amp[h] * WV_HALF; }
    wv_fft(re, im, WV_N, true);
    for (int i = 0; i < WV_N; i++) y[i] = re[i] / WV_N;
}

// Magnitudes of partials 1..WV_HARM of a cycle, scaled to a maximum of 1.
static void wv_partials_of(const float *x, int len, float *part)
{
    float mx = 0;
    for (int h = 1; h <= WV_HARM; h++) {
        double c = 0, s = 0;
        for (int i = 0; i < len; i++) { double a = TAU * h * i / len; c += x[i] * cos(a); s += x[i] * sin(a); }
        part[h - 1] = (float)sqrt(c * c + s * s);
        mx = fmaxf(mx, part[h - 1]);
    }
    for (int h = 0; h < WV_HARM; h++) part[h] = mx > 0 ? part[h] / mx : 0;
}

// Spectral envelope of a vowel (formants for a voice around 130 Hz).
static float wv_vowel(int v, float hz)
{
    static const float F[5][3] = { { 800, 1150, 2900 }, { 400, 1600, 2700 }, { 300, 2300, 3000 }, { 450, 800, 2830 }, { 325, 700, 2530 } };
    static const float G[3] = { 1, 0.5f, 0.25f }, B[3] = { 110, 140, 200 };
    float e = 0.02f;
    for (int k = 0; k < 3; k++) { float d = (hz - F[v][k]) / B[k]; e += G[k] * expf(-0.5f * d * d); }
    return e;
}

// One of the 64 waves into y (WV_N samples), peak 1.
static void wv_wave(int w, float *y)
{
    static float amp[WV_HALF];
    memset(amp, 0, sizeof amp);
    int nh = 0;
#define WH(h, a) do { amp[h] = (a); if ((h) > nh) nh = (h); } while (0)
    switch (w) {
    case 0:  WH(1, 1); break;
    case 9:  for (int h = 1; h <= 256; h++) WH(h, powf((float)h, -1.6f)); break;
    case 14: for (int h = 1; h <= 256; h++) WH(h, 1.0f / (h * h)); break;
    case 16: WH(1, 1); WH(2, 0.5f); break;
    case 17: WH(1, 1); WH(3, 0.45f); break;
    case 18: WH(1, 1); WH(2, 0.7f); WH(4, 0.5f); WH(8, 0.25f); break;
    case 19: { static const float a[] = { 0, 1, .8f, .6f, .6f, 0, .4f, 0, .35f, 0, .2f, 0, .15f, 0, 0, 0, .1f }; for (int h = 1; h <= 16; h++) WH(h, a[h]); } break;
    case 20: { static const float a[] = { 0, .6f, 1, .9f, .3f, .15f, .4f, 0, .3f }; for (int h = 1; h <= 8; h++) WH(h, a[h]); } break;
    case 21: WH(1, 1); WH(2, 0.3f); WH(3, 0.12f); WH(4, 0.06f); WH(5, 0.03f); break;
    case 22: for (int h = 1; h <= 64; h++) WH(h, (h & 1) ? 1.0f / h : 0.03f / h); break;
    case 23: for (int h = 1; h <= 64; h++) WH(h, h * expf(-h / 3.5f)); break;
    case 24: for (int h = 1; h <= 96; h++) { float d = (h - 7) / 3.0f; WH(h, powf((float)h, -0.6f) * (1 + 2 * expf(-0.5f * d * d)) * expf(-h / 40.0f)); } break;
    case 25: for (int h = 1; h <= 256; h++) WH(h, expf(-h / 48.0f) / h); break;
    case 26: case 27: case 28: case 29: case 30:
        for (int h = 1; h <= 96; h++) WH(h, wv_vowel(w - 26, h * 130.0f) * powf((float)h, -0.3f));
        break;
    case 31: for (int h = 1; h <= 96; h++) WH(h, (wv_vowel(0, h * 130.0f) + wv_vowel(3, h * 130.0f)) * powf((float)h, -0.4f)); break;
    case 32: WH(1, 1); WH(4, .6f); WH(7, .45f); WH(11, .35f); WH(15, .25f); WH(20, .15f); break;
    case 33: WH(1, 1); WH(5, .5f); WH(9, .35f); WH(13, .3f); WH(17, .2f); WH(21, .15f); break;
    case 34: WH(1, 1); WH(3, .9f); WH(5, .35f); WH(7, .1f); break;
    case 35: for (int h = 1; h <= 128; h++) WH(h, h % 3 == 0 ? 0 : (h >= 5 && h <= 9 ? 2.0f : 1.0f) / h); break;
    case 36: for (int h = 1; h <= 128; h++) WH(h, (h == 1 || wv_isprime(h)) ? powf((float)h, -0.7f) : 0); break;
    case 37: { static const int f[] = { 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144 }; for (int i = 0; i < 11; i++) WH(f[i], 1 / sqrtf((float)f[i])); } break;
    case 38: for (int h = 1; h <= 256; h <<= 1) WH(h, 1 / sqrtf((float)h)); break;
    case 39: for (int h = 1; h <= 128; h += 2) WH(h, 1 / sqrtf((float)h)); break;
    case 40: WH(1, 1); for (int h = 2; h <= 128; h += 2) WH(h, powf((float)h, -0.7f)); break;
    case 41: for (int h = 1; h <= 16; h++) WH(h, 1); break;
    case 42: for (int h = 1; h <= 64; h++) WH(h, 1); break;
    case 43: case 44: case 45: {
        float c = w == 43 ? 3 : w == 44 ? 8 : 18, bw = w == 43 ? 1 : w == 44 ? 2 : 3;
        for (int h = 1; h <= 128; h++) { float d = (h - c) / bw; WH(h, (0.12f + expf(-0.5f * d * d)) / sqrtf((float)h)); }
    } break;
    case 46: for (int h = 1; h <= 64; h++) WH(h, (0.2f + wv_hash(h * 31 + 7)) * powf((float)h, -0.8f)); break;
    case 47: WH(1, 1); for (int h = 2; h <= 128; h++) { float r = wv_hash(h * 97 + 3); WH(h, r > 0.75f ? r / sqrtf((float)h) : 0); } break;
    }
#undef WH
    if (nh) { wv_from_harm(y, amp, nh); wv_normalize(y, WV_N); return; }
    for (int i = 0; i < WV_N; i++) {
        float t = i / (float)WV_N, s1 = sinf(TAU * t), v = 0;
        float saw = 2 * fractf(t + 0.5f) - 1, tri = 1 - 4 * fabsf(fractf(t + 0.25f) - 0.5f), sq = t < 0.5f ? 1.0f : -1.0f;
        switch (w) {
        case 1:  v = tri; break;
        case 2:  v = sq; break;
        case 3:  { float ts = floorf(t * 16) / 16; v = roundf((2 * fractf(ts + 0.5f) - 1) * 4) / 4; } break;
        case 4:  v = saw; break;
        case 5:  v = t < 0.25f ? 1 : -1; break;
        case 6:  v = t < 0.125f ? 1 : -1; break;
        case 7:  v = fmaxf(s1, 0); break;
        case 8:  v = fabsf(s1); break;
        case 10: v = tanhf(3 * s1); break;
        case 11: v = clampf(2.2f * tri, -1, 1); break;
        case 12: v = roundf(s1 * 2) / 2; break;
        case 13: v = floorf((tri * 0.5f + 0.5f) * 15.999f) / 7.5f - 1; break;
        case 15: v = saw + 0.7f * (2 * fractf(2 * t + 0.75f) - 1); break;
        case 48: { float u = t * 128; int k = (int)u; float f = u - k, a = wv_hash(k & 127) * 2 - 1, b = wv_hash((k + 1) & 127) * 2 - 1;
                   v = a + (b - a) * (f * f * (3 - 2 * f)); } break;
        case 49: v = (1 - t) * cosf(TAU * 6 * t); break;
        case 50: v = (1 - 2 * fabsf(t - 0.5f)) * cosf(TAU * 8 * t); break;
        case 51: { float d = 0.08f, p = t < d ? 0.5f * t / d : 0.5f + 0.5f * (t - d) / (1 - d); v = -cosf(TAU * p); } break;
        case 52: v = sinf(TAU * t + 2.0f * s1); break;
        case 53: v = sinf(TAU * t + 1.6f * sinf(TAU * 2 * t)); break;
        case 54: v = sinf(TAU * t + 1.3f * sinf(TAU * 3 * t)); break;
        case 55: v = sinf(TAU * t + 2.2f * sinf(TAU * 7 * t)); break;
        case 56: v = 2 * fractf(t * 2.6f) - 1; break;
        case 57: v = sinf(TAU * 3.4f * t); break;
        case 58: v = sinf(3.14159265f * 1.8f * s1); break;
        case 59: { float s3 = s1 * s1 * s1, s5 = s3 * s1 * s1; v = 0.5f * (4 * s3 - 3 * s1) + 0.5f * (16 * s5 - 20 * s3 + 5 * s1); } break;
        case 60: v = tanhf(3 * saw); break;
        case 61: { float d = (t - 0.5f) / 0.025f; v = expf(-d * d); } break;
        case 62: { float d1 = (t - 0.25f) / 0.02f, d2 = (t - 0.75f) / 0.02f; v = expf(-d1 * d1) - expf(-d2 * d2); } break;
        case 63: v = 0.6f * saw + 0.5f * sq + 0.4f * s1; break;
        }
        y[i] = v;
    }
    wv_normalize(y, WV_N);
}

// Gain of partial h for the spectral-filter morphers.
static float wv_gain(int type, float a, int h)
{
    float lh = log2f((float)h);
    switch (type) {
    case 38: return 1 + a * (2 * wv_hash(h * 7919u) - 1);
    case 40: { float c = 512 * exp2f(-8.5f * a), r = h / c; return 1 / sqrtf(1 + r * r * r * r); }
    case 41: { float c = 0.3f * exp2f(8 * a), r = c / h; return 1 / sqrtf(1 + r * r * r * r); }
    case 42: { float bw = 8 * (1 - a) + 0.35f, d = (lh - 3) / bw; return expf(-0.5f * d * d); }
    case 43: { float d = (lh - (1 + 5 * a)) / 0.3f; return 1 + 6 * fminf(1, 4 * a) * expf(-0.5f * d * d); }
    case 44: { float P = 2 + 6 * a; return 1 - fminf(1, 2 * a) * (0.5f + 0.5f * cosf(TAU * h / P)); }
    case 45: { float d = (lh - (1 + 6 * a)) / 0.2f; return 1 - fminf(1, 4 * a) * expf(-0.5f * d * d); }
    case 46: return powf((float)h, -1.5f * a);
    case 47: return powf((float)h, 1.2f * a);
    case 48: return (h & 1) ? 1 : 1 - a;
    case 49: return (h & 1) && h > 1 ? 1 - a : 1;
    case 50: { int n = 1 + (int)(a * 7.99f); return (h - 1) % n == 0 ? 1 : 0; }
    case 51: return h == 1 || wv_isprime(h) ? 1 : 1 - a;
    case 52: return (h & (h - 1)) == 0 ? 1 : 1 - a;
    case 56: {   // vowels A > E > I > O > U across the amount
        float v = a * 4, hz = h * 130.0f;
        int v0 = v >= 4 ? 3 : (int)v;
        float f = v - v0, e = (1 - f) * wv_vowel(v0, hz) + f * wv_vowel(v0 + 1, hz);
        return 1 + fminf(1, 4 * a) * (e - 1);
    }
    }
    return 1;
}

// Apply morpher `type` with amount a (0 = no change) to a cycle with peak 1.
static void wv_morph(int type, float a, float *x)
{
    static float src[WV_N], re[WV_N], im[WV_N], mag[WV_HALF], pha[WV_HALF], nre[WV_HALF], nim[WV_HALF];
    memcpy(src, x, sizeof src);
    bool spec = type == 11 || type == 12 || type == 13 || type == 14 || type == 23 || type == 38 ||
                (type >= 40 && type <= 56) || type == 60 || type == 61;
    if (spec) {
        memcpy(re, x, sizeof re); memset(im, 0, sizeof im);
        wv_fft(re, im, WV_N, false);
        float mx = 0;
        for (int h = 1; h < WV_HALF; h++) { mag[h] = hypotf(re[h], im[h]); pha[h] = atan2f(im[h], re[h]); mx = fmaxf(mx, mag[h]); }
        mag[0] = 0;
        switch (type) {
        case 11: case 12: {   // shift every partial up / down by up to 24 places
            float sft = 24 * a; int s0 = (int)sft; float f = sft - s0;
            for (int h = 1; h < WV_HALF; h++) {
                int k0 = type == 11 ? h - s0 : h + s0, k1 = type == 11 ? k0 - 1 : k0 + 1;
                float m0 = k0 >= 1 && k0 < WV_HALF ? mag[k0] : 0, m1 = k1 >= 1 && k1 < WV_HALF ? mag[k1] : 0;
                float p = k0 >= 1 && k0 < WV_HALF ? pha[k0] : 0, m = m0 * (1 - f) + m1 * f;
                nre[h] = m * cosf(p); nim[h] = m * sinf(p);
            }
            for (int h = 1; h < WV_HALF; h++) { re[h] = nre[h]; im[h] = nim[h]; }
        } break;
        case 13: case 14: {   // move partial h to h * r
            float r = type == 13 ? 1 + 3 * a : 1 / (1 + 3 * a);
            memset(nre, 0, sizeof nre); memset(nim, 0, sizeof nim);
            for (int h = 1; h < WV_HALF; h++) {
                float t = h * r; int k = (int)t; float f = t - k;
                if (k >= 1 && k < WV_HALF)         { nre[k] += re[h] * (1 - f); nim[k] += im[h] * (1 - f); }
                if (k + 1 >= 1 && k + 1 < WV_HALF) { nre[k + 1] += re[h] * f;  nim[k + 1] += im[h] * f; }
            }
            for (int h = 1; h < WV_HALF; h++) { re[h] = nre[h]; im[h] = nim[h]; }
        } break;
        case 23: case 60:     // rotate phases: a chirp, or at random
            for (int h = 1; h < WV_HALF; h++) {
                float p = pha[h] + (type == 23 ? a * 3.14159265f * h * h / 64 : a * TAU * wv_hash(h * 2971u));
                re[h] = mag[h] * cosf(p); im[h] = mag[h] * sinf(p);
            }
            break;
        case 53: {   // blur the magnitudes across neighbours
            float sg = 0.3f + 4 * a;
            for (int h = 1; h < WV_HALF; h++) {
                float s = 0, ws = 0;
                for (int k = -12; k <= 12; k++) {
                    int j = h + k;
                    if (j < 1 || j >= WV_HALF) continue;
                    float w = expf(-0.5f * k * k / (sg * sg));
                    s += mag[j] * w; ws += w;
                }
                float m = s / ws;
                re[h] = m * cosf(pha[h]); im[h] = m * sinf(pha[h]);
            }
        } break;
        case 54: case 55: case 61:
            for (int h = 1; h < WV_HALF; h++) {
                float m = mag[h];
                if (type == 54 && mx > 0) m = mx * powf(m / mx, 1 + 3 * a);
                if (type == 55) m = h <= 32 ? m + a * (mag[33 - h] - m) : m * (1 - a);
                if (type == 61) { float th = a * 0.5f * mx; if (m < th) m *= (m / th) * (m / th); }
                re[h] = m * cosf(pha[h]); im[h] = m * sinf(pha[h]);
            }
            break;
        default:
            for (int h = 1; h < WV_HALF; h++) { float g = wv_gain(type, a, h); re[h] *= g; im[h] *= g; }
        }
        re[0] = im[0] = re[WV_HALF] = im[WV_HALF] = 0;
        for (int h = 1; h < WV_HALF; h++) { re[WV_N - h] = re[h]; im[WV_N - h] = -im[h]; }
        wv_fft(re, im, WV_N, true);
        for (int i = 0; i < WV_N; i++) x[i] = re[i] / WV_N;
        return;
    }
    if (type == 36) {   // slew limit, twice round the cycle
        float mx = 2 / (1 + a * a * 400), y = src[0];
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < WV_N; i++) { y += clampf(src[i] - y, -mx, mx); x[i] = y; }
        return;
    }
    for (int i = 0; i < WV_N; i++) {
        float p = i / (float)WV_N, s = src[i], v = s;
        switch (type) {
        case 0: case 1: {
            float m = type == 0 ? 0.5f - 0.45f * a : 0.5f + 0.45f * a;
            v = wv_at(src, p < m ? 0.5f * p / m : 0.5f + 0.5f * (p - m) / (1 - m));
        } break;
        case 2:  v = wv_at(src, p - 0.9f * a * sinf(2 * TAU * p) / (2 * TAU)); break;
        case 3:  v = wv_at(src, p + 0.9f * a * sinf(2 * TAU * p) / (2 * TAU)); break;
        case 4:  v = s + a * (wv_at(src, p < 0.5f ? 2 * p : 2 - 2 * p) - s); break;
        case 5:  { float w = 1 - 0.85f * a; v = p < w ? wv_at(src, p / w) : src[0]; } break;
        case 6:  { float u = 2 * p - 1; v = wv_at(src, 0.5f + 0.5f * copysignf(powf(fabsf(u), 1 / (1 + 3 * a)), u)); } break;
        case 7:  v = p < 0.5f ? s : s * (1 - 2 * a); break;
        case 8:  v = wv_at(src, p * (1 + 7 * a)); break;
        case 9:  { float w = 0.5f - 0.5f * cosf(TAU * p); v = wv_at(src, p * (1 + 7 * a)) * (1 + fminf(1, 4 * a) * (w - 1)); } break;
        case 10: { float r = 1 + 7 * a; int n0 = (int)r; float f = r - n0; v = wv_at(src, p * n0) * (1 - f) + wv_at(src, p * (n0 + 1)) * f; } break;
        case 15: v = s + a * wv_at(src, 2 * p); break;
        case 16: case 17: case 18: case 19: {
            static const float K[4] = { 1, 2, 3, 7 };
            v = wv_at(src, p + a * 0.25f * sinf(TAU * K[type - 16] * p));
        } break;
        case 20: v = wv_at(src, p + a * 0.2f * s); break;
        case 21: v = s * (1 + a * (1.5f * sinf(TAU * 3 * p) - 1)); break;
        case 22: v = s * (1 + a * (cosf(TAU * 2 * p) - 1)); break;
        case 24: { float g = 1 + 9 * a; v = tanhf(g * s) / tanhf(g); } break;
        case 25: v = clampf((1 + 9 * a) * s, -1, 1); break;
        case 26: v = wv_fold(s * (1 + 5 * a)); break;
        case 27: { float b = 0.6f * a; v = tanhf((1 + 3 * a) * s + b) - tanhf(b); } break;
        case 28: v = s + a * (fabsf(s) - s); break;
        case 29: v = copysignf(powf(fabsf(s), 1 / (1 + 3 * a)), s); break;
        case 30: v = copysignf(powf(fabsf(s), 1 + 4 * a), s); break;
        case 31: v = 2 * fractf((s * (1 + 4 * a) + 1) * 0.5f) - 1; break;
        case 32: { float L = exp2f(10 - 8.5f * a); v = roundf(s * L) / L; } break;
        case 33: { int S = 1 + (int)(a * 127); v = src[(i / S) * S]; } break;
        case 34: v = s + a * (2 * s * s - 1 - s); break;
        case 35: v = s + a * (4 * s * s * s - 3 * s - s); break;
        case 37: v = s + a * 0.7f * (2 * wv_hash(i * 2654435761u) - 1); break;
        case 39: { int j = ((i / 128) & 1) ? i - 128 : i; v = s + a * (src[j] - s); } break;
        case 57: v = s * (1 + a * (0.5f - 0.5f * cosf(TAU * p) - 1)); break;
        case 58: v = s + fminf(1, 3 * a) * (1 - p) * (1 - p) * sinf(TAU * (2 + 14 * a) * p); break;
        case 59: v = s + fminf(1, 4 * a) * (sinf(1.5707963f * s * (1 + 3 * a)) - s); break;
        case 62: v = s + a * (sinf(TAU * p) - s); break;
        case 63: v = s + a * (2 * fractf(p + 0.5f) - 1 - s); break;
        }
        x[i] = v;
    }
}

static void wv_static_init(void)
{
    if (wv_inited) return;
    wv_inited = true;
    for (int k = 0; k < WV_HALF; k++) { wv_cs[k] = cosf(TAU * k / WV_N); wv_sn[k] = sinf(TAU * k / WV_N); }
    int off = 0;
    for (int k = 0; k < WV_LV; k++) {
        wv_lsize[k] = 4 * (512 >> k) > 64 ? 4 * (512 >> k) : 64;
        wv_loff[k] = off;
        off += wv_lsize[k] + 1;
    }
    static float y[WV_N];
    for (int w = 0; w < WV_NWAVE; w++) {
        wv_wave(w, y);
        for (int j = 0; j < 48; j++) wv_thumb[w][j] = y[j * WV_N / 48];
    }
}

// ---- building the table (UI side) -----------------------------------------

static void wv_snapshot(const WavorSt *s, WvSnap *sn)
{
    memset(sn, 0, sizeof *sn);
    sn->wave = clampi((int)s->q[WQ_WAVE], 0, WV_USER);
    sn->userMode = s->userMode;
    for (int k = 0; k < WV_SLOTS; k++) { sn->mt[k] = clampi((int)s->q[WQ_M1T + 2 * k], 0, 63); sn->ma[k] = s->q[WQ_M1A + 2 * k]; }
    memcpy(sn->user, s->user, sizeof sn->user);
    memcpy(sn->part, s->part, sizeof sn->part);
}

static unsigned wv_signature(const WavorSt *s)
{
    WvSnap sn;
    wv_snapshot(s, &sn);
    unsigned h = 2166136261u;
    const unsigned char *b = (const unsigned char *)&sn;
    for (size_t i = 0; i < offsetof(WvSnap, user); i++) h = (h ^ b[i]) * 16777619u;
    if (sn.wave == WV_USER) h = (h ^ s->userVer) * 16777619u;
    return h;
}

static void wv_make_base(WavorSt *s)
{
    const WvSnap *sn = &s->snap;
    if (sn->wave < WV_NWAVE) { wv_wave(sn->wave, s->base); return; }
    if (sn->userMode == 1) {
        static float amp[WV_HARM + 1];
        for (int h = 1; h <= WV_HARM; h++) amp[h] = sn->part[h - 1];
        wv_from_harm(s->base, amp, WV_HARM);
    } else
        for (int i = 0; i < WV_N; i++) {
            float p = i * (float)WV_UPTS / WV_N;
            int j = (int)p;
            float f = p - j;
            s->base[i] = sn->user[j] + f * (sn->user[(j + 1) % WV_UPTS] - sn->user[j]);
        }
    wv_normalize(s->base, WV_N);
}

// Frame f: morph the base by t = f / (WV_F - 1) of every slot's amount, then
// band-limit it into every mip level, plus the brighter twin.
static void wv_build_frame(WavorSt *s, float *dst, int f)
{
    static float x[WV_N], re[WV_N], im[WV_N], R[WV_N], I[WV_N];
    float t = f / (float)(WV_F - 1);
    memcpy(x, s->base, sizeof x);
    for (int k = 0; k < WV_SLOTS; k++) {
        float a = s->snap.ma[k] * t;
        if (a > 1e-4f) { wv_morph(s->snap.mt[k], clampf(a, 0, 1), x); wv_normalize(x, WV_N); }
    }
    float rmsA = 0;
    for (int v = 0; v < 2; v++) {
        for (int i = 0; i < WV_N; i++) re[i] = v ? tanhf(2.5f * x[i]) : x[i];
        memset(im, 0, sizeof im);
        wv_fft(re, im, WV_N, false);
        if (v) for (int h = 1; h < WV_HALF; h++) { float g = powf((float)h, 0.45f); re[h] *= g; im[h] *= g; }
        float gain = 1;
        for (int k = 0; k < WV_LV; k++) {
            int n = wv_lsize[k], H = 512 >> k;
            float *o = dst + (size_t)(f * 2 + v) * WV_T + wv_loff[k];
            memset(R, 0, n * sizeof *R); memset(I, 0, n * sizeof *I);
            for (int h = 1; h <= H; h++) { R[h] = re[h]; I[h] = im[h]; R[n - h] = re[h]; I[n - h] = -im[h]; }
            wv_fft(R, I, n, true);
            if (k == 0) {   // one gain for all levels, from the full-band cycle
                float pk = 0, rms = 0;
                for (int i = 0; i < n; i++) { float y = R[i] / WV_N; pk = fmaxf(pk, fabsf(y)); rms += y * y; }
                rms = sqrtf(rms / n);
                if (!v) { gain = 1 / fmaxf(pk, 0.05f); rmsA = rms * gain; }
                else gain = fminf(rms > 1e-9f ? rmsA / rms : 1, 1.6f / fmaxf(pk, 1e-6f));
            }
            for (int i = 0; i < n; i++) o[i] = R[i] / WV_N * gain;
            o[n] = o[0];
        }
    }
}

// Called every video frame: (re)build the table when the wave or the
// morphers changed, within `budget` seconds (a huge budget builds it all now).
static void wv_work(Module *m, double budget)
{
    WavorSt *s = m->st;
    if (!s || !s->tab[0] || !s->tab[1]) return;
    if (s->pending >= 0 && UI_OWNS_AUDIO) { s->cur = s->pending; s->pending = -1; }
    if (!s->building) {
        unsigned sig = wv_signature(s);
        if (sig == s->sig && s->dispOk) return;
        if (s->pending >= 0) return;   // the audio has not taken the last table yet
        wv_snapshot(s, &s->snap);
        s->sig = sig; s->building = true; s->bf = 0; s->dst = 1 - s->cur;
        wv_make_base(s);
    }
    bool all = budget > 1e6;
    double t0 = all ? 0 : GetTime();
    while (s->bf < WV_F && (all || GetTime() - t0 < budget)) wv_build_frame(s, s->tab[s->dst], s->bf++);
    if (s->bf < WV_F) return;
    s->building = false;
    const float *lv = s->tab[s->dst] + wv_loff[2];
    for (int f = 0; f < WV_F; f++)
        for (int j = 0; j < WV_DISP; j++) s->disp[f][j] = lv[(size_t)f * 2 * WV_T + j * wv_lsize[2] / WV_DISP];
    s->dispOk = true;
    s->pending = s->dst;
    if (UI_OWNS_AUDIO) { s->cur = s->pending; s->pending = -1; }
}

// ---- audio ----------------------------------------------------------------

static float wv_curve(const float *q, int c, float x)   // x: stereo position -1..1
{
    const float *h = q + WQ_CURVE + c * 3;
    float v = h[1] + x * (h[2] - h[0]) * 0.5f + x * x * ((h[2] + h[0]) * 0.5f - h[1]);
    return clampf(v, 0, 1);
}

typedef struct { const float *t; float w; } WvTap;

static void wv_voice_run(const WvTap *tp, int nt, int size, float *ph, float inc, float gl, float gr, float *L, float *R)
{
    float p = *ph, fs = (float)size;
    for (int i = 0; i < WV_BLOCK; i++) {
        float x = p * fs;
        int i0 = (int)x;
        float fr = x - i0, v = 0;
        for (int k = 0; k < nt; k++) { const float *t = tp[k].t; v += tp[k].w * (t[i0] + fr * (t[i0 + 1] - t[i0])); }
        L[i] += v * gl; R[i] += v * gr;
        p += inc;
        if (p >= 1) p -= 1;
    }
    *ph = p;
}

#define WV_PS(n) ((int)((n) * (SR / 29761.0f)))   // Dattorro's plate lengths at our rate

static float wv_apd(float *b, int mask, int w, int D, float g, float x)   // allpass, integer delay
{
    float d = b[(w - D) & mask], v = x + g * d;
    b[w & mask] = v;
    return d - g * v;
}

static float wv_apf(float *b, int mask, int w, float D, float g, float x)   // allpass, fractional delay
{
    float rp = w - D;
    int i0 = (int)floorf(rp);
    float fr = rp - i0, d = b[i0 & mask] + fr * (b[(i0 + 1) & mask] - b[i0 & mask]), v = x + g * d;
    b[w & mask] = v;
    return d - g * v;
}

static void wv_plate(WvFxSt *f, const float *q, float *L, float *R)
{
    float decay = q[WQ_PLT_DECAY], damp = q[WQ_PLT_DAMP] * 0.9f, mix = q[WQ_PLT_MIX];
    float dd2 = clampf(decay + 0.15f, 0.25f, 0.5f), ex = 8 * (SR / 29761.0f);
    int pd = clampi((int)(q[WQ_PLT_PRE] * SR), 0, 16000);
    for (int t = 0; t < WV_BLOCK; t++) {
        int w = f->pw;
        f->pre[w & 16383] = (L[t] + R[t]) * 0.5f + 1e-18f;
        float x = f->pre[(w - pd) & 16383];
        x = wv_apd(f->ap[0], 1023, w, WV_PS(142), 0.75f, x);
        x = wv_apd(f->ap[1], 1023, w, WV_PS(107), 0.75f, x);
        x = wv_apd(f->ap[2], 1023, w, WV_PS(379), 0.625f, x);
        x = wv_apd(f->ap[3], 1023, w, WV_PS(277), 0.625f, x);
        f->pph = fractf(f->pph + 0.9f / SR);
        float mL = WV_PS(672) + ex * (1 + sinf(TAU * f->pph)), mR = WV_PS(908) + ex * (1 + cosf(TAU * f->pph));
        // left half of the tank
        f->dL1[w & 8191] = wv_apf(f->mapL, 2047, w, mL, -0.7f, x + decay * f->lastR);
        f->dampL += (f->dL1[(w - WV_PS(4453)) & 8191] - f->dampL) * (1 - damp);
        f->dL2[w & 8191] = wv_apd(f->apL2, 4095, w, WV_PS(1800), dd2, f->dampL * decay);
        float nl = f->dL2[(w - WV_PS(3720)) & 8191];
        // right half
        f->dR1[w & 8191] = wv_apf(f->mapR, 2047, w, mR, -0.7f, x + decay * f->lastL);
        f->dampR += (f->dR1[(w - WV_PS(4217)) & 8191] - f->dampR) * (1 - damp);
        f->dR2[w & 8191] = wv_apd(f->apR2, 8191, w, WV_PS(2656), dd2, f->dampR * decay);
        float nr = f->dR2[(w - WV_PS(3163)) & 8191];
        f->lastL = nl; f->lastR = nr;
#define WV_TAP(buf, mask, d) (buf)[(w - WV_PS(d)) & (mask)]
        float yl = 0.6f * (WV_TAP(f->dR1, 8191, 266) + WV_TAP(f->dR1, 8191, 2974) - WV_TAP(f->apR2, 8191, 1913) + WV_TAP(f->dR2, 8191, 1996)
                         - WV_TAP(f->dL1, 8191, 1990) - WV_TAP(f->apL2, 4095, 187) - WV_TAP(f->dL2, 8191, 1066));
        float yr = 0.6f * (WV_TAP(f->dL1, 8191, 353) + WV_TAP(f->dL1, 8191, 3627) - WV_TAP(f->apL2, 4095, 1228) + WV_TAP(f->dL2, 8191, 2673)
                         - WV_TAP(f->dR1, 8191, 2111) - WV_TAP(f->apR2, 8191, 335) - WV_TAP(f->dR2, 8191, 121));
#undef WV_TAP
        f->pw = (w + 1) & 0xffffff;
        L[t] = L[t] * (1 - 0.5f * mix) + yl * mix;
        R[t] = R[t] * (1 - 0.5f * mix) + yr * mix;
    }
}

static void wv_biquad_set(Biquad *b, int type, float f, float db, float qv)   // 0 low shelf, 1 peak, 2 high shelf
{
    float A = powf(10, db / 40), w = TAU * f / SR, c = cosf(w), sn = sinf(w);
    float b0, b1, b2, a0, a1, a2;
    if (type == 1) {
        float al = sn / (2 * qv);
        b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A;
    } else {
        float al = sn / 2 * 1.41421356f, sa = 2 * sqrtf(A) * al, s = type == 0 ? 1 : -1;
        b0 = A * ((A + 1) - s * (A - 1) * c + sa);
        b1 = 2 * s * A * ((A - 1) - s * (A + 1) * c);
        b2 = A * ((A + 1) - s * (A - 1) * c - sa);
        a0 = (A + 1) + s * (A - 1) * c + sa;
        a1 = -2 * s * ((A - 1) + s * (A + 1) * c);
        a2 = (A + 1) + s * (A - 1) * c - sa;
    }
    b->b0 = b0 / a0; b->b1 = b1 / a0; b->b2 = b2 / a0; b->a1 = a1 / a0; b->a2 = a2 / a0;
}

static void wv_fx(Module *m, float *L, float *R, float env)
{
    WavorSt *s = m->st;
    WvFxSt *f = &s->fx;
    const float *q = s->q;
    float *ch[2] = { L, R };
    const int n = WV_BLOCK;

    if (q[WQ_FLT_ON] > 0.5f) {   // state-variable filter, cutoff follows the envelope and CUTOFF
        float key = q[WQ_FLT_KEY] * (s->pitch[0] + roundf(q[WQ_OCT]));
        float fc = clampf(q[WQ_FLT_CUT] * exp2f(q[WQ_FLT_ENV] * env * 5 + q[WQ_MCUT] * s->menv + key + IN(m, 6) * 5), 20, SR * 0.45f);
        float g = tanf(3.14159265f * fc / SR), k = 2 - 1.95f * q[WQ_FLT_RES];
        float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        int mode = (int)q[WQ_FLT_MODE];
        for (int c = 0; c < 2; c++)
            for (int t = 0; t < n; t++) {
                float x = ch[c][t], v3 = x - f->fic2[c];
                float v1 = a1 * f->fic1[c] + a2 * v3, v2 = f->fic2[c] + a2 * f->fic1[c] + a3 * v3;
                f->fic1[c] = 2 * v1 - f->fic1[c]; f->fic2[c] = 2 * v2 - f->fic2[c];
                ch[c][t] = mode == 0 ? v2 : mode == 1 ? k * v1 : mode == 2 ? x - k * v1 - v2 : x - k * v1;
            }
    }
    if (q[WQ_EQ_ON] > 0.5f) {    // low shelf 180 Hz, a bell, high shelf 4.5 kHz
        float key[4] = { q[WQ_EQ_LOW], q[WQ_EQ_MID], q[WQ_EQ_FREQ], q[WQ_EQ_HIGH] };
        if (memcmp(key, f->eqKey, sizeof key)) {
            memcpy(f->eqKey, key, sizeof key);
            for (int c = 0; c < 2; c++) {
                wv_biquad_set(&f->eq[0][c], 0, 180, key[0], 0.7f);
                wv_biquad_set(&f->eq[1][c], 1, key[2], key[1], 0.8f);
                wv_biquad_set(&f->eq[2][c], 2, 4500, key[3], 0.7f);
            }
        }
        for (int c = 0; c < 2; c++)
            for (int t = 0; t < n; t++) ch[c][t] = bq(&f->eq[2][c], bq(&f->eq[1][c], bq(&f->eq[0][c], ch[c][t])));
    }
    if (q[WQ_SAT_ON] > 0.5f) {   // warm tanh saturation; BIAS adds even harmonics
        float g = exp2f(q[WQ_SAT_DRIVE] * 4.5f), b = q[WQ_SAT_BIAS] * 0.5f, tb = tanhf(b), comp = 1 / sqrtf(g), mix = q[WQ_SAT_MIX];
        float lc = 1 - expf(-TAU * 1500 * exp2f(q[WQ_SAT_TONE] * 3.7f) / SR);
        for (int c = 0; c < 2; c++)
            for (int t = 0; t < n; t++) {
                float x = ch[c][t], y = (tanhf(g * x + b) - tb) * comp;
                f->satLp[c] += (y - f->satLp[c]) * lc;
                ch[c][t] = x + (f->satLp[c] - x) * mix;
            }
    }
    if (q[WQ_DST_ON] > 0.5f) {   // clip, fold, bit/rate crush or fuzz, then a tone filter
        int type = (int)q[WQ_DST_TYPE];
        float dr = q[WQ_DST_DRIVE], g = exp2f(dr * 6), mix = q[WQ_DST_MIX];
        float lc = 1 - expf(-TAU * 600 * exp2f(q[WQ_DST_TONE] * 5) / SR);
        float lev = exp2f(12 - dr * 9.5f);
        int hold = 1 + (int)(dr * dr * 24);
        for (int t = 0; t < n; t++) {
            bool take = type != 2 || ++f->holdN >= hold;
            if (take) f->holdN = 0;
            for (int c = 0; c < 2; c++) {
                float x = ch[c][t], y, v = g * x;
                switch (type) {
                case 0:  y = clampf(v, -1, 1) * 0.5f; break;
                case 1:  y = wv_fold(v) * 0.5f; break;
                case 2:  if (take) f->hold[c] = roundf(x * lev) / lev; y = f->hold[c]; break;
                default: y = (v > 0 ? 1 - expf(-v) : -(1 - expf(v * 0.7f)) * 0.85f) * 0.5f; break;
                }
                f->dstLp[c][0] += (y - f->dstLp[c][0]) * lc;
                f->dstLp[c][1] += (f->dstLp[c][0] - f->dstLp[c][1]) * lc;
                ch[c][t] = x + (f->dstLp[c][1] - x) * mix;
            }
        }
    }
    if (q[WQ_CMP_ON] > 0.5f) {   // stereo-linked peak compressor
        float thr = q[WQ_CMP_THR], sl = 1 - 1 / q[WQ_CMP_RATIO], mk = q[WQ_CMP_GAIN];
        float ca = 1 - expf(-1 / (q[WQ_CMP_ATK] * SR)), cr = 1 - expf(-1 / (q[WQ_CMP_REL] * SR));
        for (int t = 0; t < n; t++) {
            float lv = fmaxf(fabsf(L[t]), fabsf(R[t])), over = 20 * log10f(lv + 1e-9f) - thr;
            float target = over > 0 ? over * sl : 0;
            f->gr += (target - f->gr) * (target > f->gr ? ca : cr);
            float gain = exp2f((mk - f->gr) * 0.16609640f);
            L[t] *= gain; R[t] *= gain;
        }
    }
    if (q[WQ_CHO_ON] > 0.5f) {   // three swept taps per side
        float rate = q[WQ_CHO_RATE], dep = q[WQ_CHO_DEPTH], mix = q[WQ_CHO_MIX];
        for (int t = 0; t < n; t++) {
            f->choPh = fractf(f->choPh + rate / SR);
            f->cw = (f->cw + 1) & (WV_CHO - 1);
            for (int c = 0; c < 2; c++) {
                float x = ch[c][t], wet = 0;
                f->cho[c][f->cw] = x;
                for (int v = 0; v < 3; v++) {
                    float d = (0.010f + dep * 0.007f * (1 + sinf(TAU * (f->choPh + v / 3.0f + c / 6.0f)))) * SR;
                    float rp = f->cw - d;
                    if (rp < 0) rp += WV_CHO;
                    int i0 = (int)rp;
                    float fr = rp - i0;
                    i0 &= WV_CHO - 1;
                    wet += f->cho[c][i0] + fr * (f->cho[c][(i0 + 1) & (WV_CHO - 1)] - f->cho[c][i0]);
                }
                ch[c][t] = x * (1 - mix * 0.5f) + wet / 3 * mix;
            }
        }
    }
    if (q[WQ_FLG_ON] > 0.5f) {   // short swept delay with feedback
        float rate = q[WQ_FLG_RATE], dep = q[WQ_FLG_DEPTH], fb = q[WQ_FLG_FB], mix = q[WQ_FLG_MIX];
        for (int t = 0; t < n; t++) {
            f->flgPh = fractf(f->flgPh + rate / SR);
            f->fw = (f->fw + 1) & (WV_FLG - 1);
            for (int c = 0; c < 2; c++) {
                float x = ch[c][t];
                float d = (0.0006f + dep * 0.0055f * (0.5f + 0.5f * sinf(TAU * (f->flgPh + c * 0.25f)))) * SR;
                float rp = f->fw - d;
                if (rp < 0) rp += WV_FLG;
                int i0 = (int)rp;
                float fr = rp - i0;
                i0 &= WV_FLG - 1;
                float tap = f->flg[c][i0] + fr * (f->flg[c][(i0 + 1) & (WV_FLG - 1)] - f->flg[c][i0]);
                f->flg[c][f->fw] = clampf(x + fb * tap, -2, 2);
                ch[c][t] = x * (1 - 0.5f * mix) + tap * 0.5f * mix;
            }
        }
    }
    if (q[WQ_PHS_ON] > 0.5f) {   // six swept allpass stages
        f->phsPh = fractf(f->phsPh + q[WQ_PHS_RATE] * n / SR);
        float fb = q[WQ_PHS_FB], mix = q[WQ_PHS_MIX];
        for (int c = 0; c < 2; c++) {
            float lfo = 0.5f + 0.5f * sinf(TAU * (f->phsPh + c * 0.25f));
            float tn = tanf(3.14159265f * fminf(180 * exp2f(q[WQ_PHS_DEPTH] * 4.5f * lfo), SR * 0.4f) / SR), a = (tn - 1) / (tn + 1);
            for (int t = 0; t < n; t++) {
                float x = ch[c][t], v = x + fb * f->phsFb[c];
                for (int st = 0; st < 6; st++) { float y = a * v + f->apz[c][st]; f->apz[c][st] = v - a * y; v = y; }
                f->phsFb[c] = v + 1e-18f;
                ch[c][t] = x * (1 - 0.5f * mix) + v * 0.5f * mix;
            }
        }
    }
    if (q[WQ_DLY_ON] > 0.5f) {   // stereo delay, ping-pong, darkening repeats
        float target = clampf(q[WQ_DLY_TIME] * SR, 16, WV_DLY - 4), fb = q[WQ_DLY_FB], p = q[WQ_DLY_PING], mix = q[WQ_DLY_MIX];
        float lc = 1 - expf(-TAU * 800 * exp2f(q[WQ_DLY_TONE] * 4.4f) / SR);
        for (int t = 0; t < n; t++) {
            f->dt += (target - f->dt) * 0.0003f;
            float rp = f->dw - f->dt;
            if (rp < 0) rp += WV_DLY;
            int i0 = (int)rp;
            float fr = rp - i0;
            i0 &= WV_DLY - 1;
            int i1 = (i0 + 1) & (WV_DLY - 1);
            float tl = f->dly[0][i0] + fr * (f->dly[0][i1] - f->dly[0][i0]);
            float tr = f->dly[1][i0] + fr * (f->dly[1][i1] - f->dly[1][i0]);
            f->dlp[0] += (tl - f->dlp[0]) * lc;
            f->dlp[1] += (tr - f->dlp[1]) * lc;
            float fl = f->dlp[0] * fb, fr2 = f->dlp[1] * fb, x = L[t], y = R[t], mono = (x + y) * 0.5f;
            f->dly[0][f->dw] = clampf(x + p * (mono - x) + (1 - p) * fl + p * fr2, -4, 4) + 1e-18f;
            f->dly[1][f->dw] = clampf(y * (1 - p) + (1 - p) * fr2 + p * fl, -4, 4) + 1e-18f;
            f->dw = (f->dw + 1) & (WV_DLY - 1);
            L[t] = x * (1 - 0.5f * mix) + tl * mix;
            R[t] = y * (1 - 0.5f * mix) + tr * mix;
        }
    }
    if (q[WQ_PLT_ON] > 0.5f) wv_plate(f, q, L, R);
}

// Render the next block: notes, unison voices and the effects.
static void wv_render(Module *m)
{
    WavorSt *s = m->st;
    const float *q = s->q;
    float L[WV_BLOCK] = { 0 }, R[WV_BLOCK] = { 0 };
    if (s->pending >= 0) { s->cur = s->pending; s->pending = -1; }
    const float *tab = s->tab[s->cur];
    s->cA = 1 / (q[WQ_A] * SR);
    s->cD = 1 - expf(-4 / (q[WQ_D] * SR));
    s->cR = 1 - expf(-6.9f / (q[WQ_R] * SR));
    float env0 = s->envBuf[0], dtb = WV_BLOCK / SR;
    bool trig = s->trig;
    s->trig = false;

    // mod envelope (attack, decay to zero), vibrato with its delay, noise burst
    if (trig) { s->mstage = 1; s->tOn = 0; s->nenv = 1; if (q[WQ_MA] <= 0.0015f) { s->menv = 1; s->mstage = 2; } }
    s->tOn += dtb;
    if (s->mstage == 1) { s->menv += dtb / q[WQ_MA]; if (s->menv >= 1) { s->menv = 1; s->mstage = 2; } }
    else if (s->mstage == 2) s->menv *= expf(-4.6f * dtb / q[WQ_MD]);
    s->nenv = q[WQ_NDECAY] > 0 ? s->nenv * expf(-4.6f * dtb / q[WQ_NDECAY]) : 1;
    s->vph = fractf(s->vph + q[WQ_VIBRATE] * dtb);
    float vfade = q[WQ_VIBDELAY] > 0 ? clampf(s->tOn / q[WQ_VIBDELAY], 0, 1) : 1;
    float pmod = (q[WQ_VIBDEPTH] * vfade * vfade * sinf(TAU * s->vph) + q[WQ_MPITCH] * s->menv) / 12;
    float trem = 1 - q[WQ_TREM] * vfade * (0.5f - 0.5f * cosf(TAU * s->vph));

    // notes: P1 plays C3 when unpatched, P2..P4 only when patched
    bool act[WV_NOTES];
    int nact = 0;
    float gc = q[WQ_GLIDE] < 0.001f ? 1 : 1 - expf(-WV_BLOCK / (q[WQ_GLIDE] * 0.3f * SR));
    for (int j = 0; j < WV_NOTES; j++) {
        act[j] = j == 0 || HAS(m, j);
        if (!act[j]) { s->was[j] = false; continue; }
        float t = HAS(m, j) ? IN(m, j) : -1.0f;
        if (!s->was[j]) s->pitch[j] = t;
        s->was[j] = true;
        s->pitch[j] += (t - s->pitch[j]) * gc;
        nact++;
    }

    // wavetable position: knob, envelope, internal LFO and the POS input
    s->lph = fractf(s->lph + q[WQ_LFORATE] * WV_BLOCK / SR);
    float pos = q[WQ_POS] + q[WQ_ENVPOS] * env0 + q[WQ_LFOPOS] * 0.5f * sinf(TAU * s->lph) + q[WQ_MPOS] * s->menv + IN(m, 5);
    s->dpos = clampf(pos, 0, 1);

    // voices: placed across the stereo image, shaped by the curves; the
    // entropizer gives each its own slowly wandering offsets
    int n = clampi((int)q[WQ_VOICES], 1, WV_MAXV);
    float er = q[WQ_ENTRATE] * WV_BLOCK / SR, eg = fminf(1, er * 3);
    float vg[WV_MAXV][2], vdet[WV_MAXV], vag[WV_MAXV], vpos[WV_MAXV];
    for (int i = 0; i < n; i++) {
        float *e = s->ent[i], *et = s->entT[i];
        for (int k = 0; k < 5; k++) {
            if (frand() < er) et[k] = frand() * 2 - 1;
            e[k] += (et[k] - e[k]) * eg;
        }
        float u = n > 1 ? i / (float)(n - 1) * 2 - 1 : 0, x0 = u * q[WQ_SPREAD];
        float E = wv_curve(q, WC_ENT, x0), x = clampf(x0 + e[0] * E * 0.35f, -1, 1);
        vdet[i] = u * wv_curve(q, WC_DET, x) * 0.5f + e[1] * E * 0.25f;   // semitones
        vag[i]  = clampf(wv_curve(q, WC_AGG, x) + e[2] * E * 0.5f, 0, 1);
        float lv = wv_curve(q, WC_LVL, x) * clampf(1 + e[3] * E * 0.6f, 0, 1.6f);
        vpos[i] = clampf(pos + wv_curve(q, WC_POS, x) - 0.5f + e[4] * E * 0.25f, 0, 1);
        float pa = (x + 1) * 0.78539816f;
        vg[i][0] = cosf(pa) * lv; vg[i][1] = sinf(pa) * lv;
        s->vx[i] = x; s->vl[i] = lv; s->va[i] = vag[i]; s->vp[i] = vpos[i];
    }
    s->vn = n; s->dnotes = nact;

    // a note starting from silence restarts the phases (PHASE RND 0: all together)
    if (trig) {
        if (env0 < 0.02f)
            for (int j = 0; j < WV_NOTES; j++)
                for (int i = 0; i < WV_MAXV; i++) s->ph[j][i] = q[WQ_PHASE] * frand();
    }

    if (tab) {
        float oct = roundf(q[WQ_OCT]);
        for (int j = 0; j < WV_NOTES; j++) {
            if (!act[j]) continue;
            float f = 261.6256f * exp2f(s->pitch[j] + oct + pmod);
            for (int i = 0; i < n; i++) {
                float fi = f * exp2f(vdet[i] / 12);
                if (fi > 20000 || fi < 1) continue;
                int k = clampi((int)ceilf(log2f(fi * 512 / 19000)), 0, WV_LV - 1);
                float fp = vpos[i] * (WV_F - 1);
                int f0 = (int)fp < WV_F - 2 ? (int)fp : WV_F - 2;
                float fr = fp - f0, ag = vag[i];
                const float *base = tab + wv_loff[k];
                float w[4] = { (1 - fr) * (1 - ag), fr * (1 - ag), (1 - fr) * ag, fr * ag };
                int fo[4] = { f0, f0 + 1, f0, f0 + 1 }, va[4] = { 0, 0, 1, 1 };
                WvTap tp[4];
                int nt = 0;
                for (int c = 0; c < 4; c++)
                    if (w[c] > 1e-4f) { tp[nt].t = base + (size_t)(fo[c] * 2 + va[c]) * WV_T; tp[nt++].w = w[c]; }
                wv_voice_run(tp, nt, wv_lsize[k], &s->ph[j][i], fi / SR, vg[i][0], vg[i][1], L, R);
            }
        }
    }
    float amp = q[WQ_LEVEL] * 1.9f / sqrtf((float)(n * (nact ? nact : 1)));
    if (q[WQ_NOISE] > 0) {   // breath, bow or pick noise: band-pass around a multiple of the pitch
        float f0 = 261.6256f * exp2f(s->pitch[0] + roundf(q[WQ_OCT]) + pmod);
        float fc = clampf(f0 * q[WQ_NTONE], 60, 16000), g = tanf(3.14159265f * fc / SR), k = 0.7f;
        float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        float lvl = q[WQ_NOISE] * s->nenv * 1.2f * sqrtf((float)(n * (nact ? nact : 1))) * sqrtf(SR * 0.5f / fc);
        for (int t = 0; t < WV_BLOCK; t++) {
            float x = frand() * 2 - 1, v3 = x - s->nic2;
            float v1 = a1 * s->nic1 + a2 * v3, v2 = s->nic2 + a2 * s->nic1 + a3 * v3;
            s->nic1 = 2 * v1 - s->nic1; s->nic2 = 2 * v2 - s->nic2;
            L[t] += k * v1 * lvl; R[t] += k * v1 * lvl;
        }
    }
    amp *= trem;
    for (int t = 0; t < WV_BLOCK; t++) { float g = s->envBuf[t] * amp; L[t] *= g; R[t] *= g; }
    wv_fx(m, L, R, env0);
    for (int t = 0; t < WV_BLOCK; t++) {
        s->outL[t] = isfinite(L[t]) ? L[t] : 0;
        s->outR[t] = isfinite(R[t]) ? R[t] : 0;
    }
}

static void wv_process(Module *m)
{
    WavorSt *s = m->st;
    m->out[0] = s->outL[s->bi];
    m->out[1] = s->outR[s->bi];
    // ADSR on GATE; with GATE unpatched it holds, so the voices drone
    bool gated = HAS(m, 4), on = gated ? IN(m, 4) > 0.5f : true;
    if (on && !s->gate) { s->stage = 1; s->trig = true; }
    if (!on && s->gate) s->stage = 3;
    s->gate = on;
    switch (s->stage) {
    case 1: s->env += s->cA; if (s->env >= 1) { s->env = 1; s->stage = 2; } break;
    case 2: s->env += (s->q[WQ_S] - s->env) * s->cD; break;
    case 3: s->env -= s->env * s->cR; if (s->env < 1e-5f) { s->env = 0; s->stage = 0; } break;
    }
    s->envBuf[s->bi] = s->env;
    if (++s->bi >= WV_BLOCK) { s->bi = 0; wv_render(m); }
}

static void wv_init(Module *m)
{
    wv_static_init();
    WavorSt *s = calloc(1, sizeof(WavorSt));
    m->st = s;
    for (int i = 0; i < WQ_N; i++) s->q[i] = WVP[i].k.def;
    s->tab[0] = calloc((size_t)WV_F * 2 * WV_T, sizeof(float));
    s->tab[1] = calloc((size_t)WV_F * 2 * WV_T, sizeof(float));
    s->pending = -1; s->pick = -1; s->lastKnob = -1; s->lastJ = -1; s->pidx = -1; s->naming = -1; s->armDel = -1;
    s->fx.dt = s->q[WQ_DLY_TIME] * SR;
    s->cA = 1 / (s->q[WQ_A] * SR);
    for (int j = 0; j < WV_NOTES; j++) for (int i = 0; i < WV_MAXV; i++) s->ph[j][i] = frand();
    for (int i = 0; i < WV_MAXV; i++) for (int k = 0; k < 5; k++) s->ent[i][k] = s->entT[i][k] = frand() * 2 - 1;
    for (int j = 0; j < WV_UPTS; j++) s->user[j] = sinf(TAU * j / WV_UPTS);
    s->part[0] = 1;
    wv_work(m, 1e9);
    s->psig = wv_sound_sig(s);
}

static void wv_fini(Module *m)
{
    WavorSt *s = m->st;
    if (!s) return;
    if (g_kbd_owner == (int)(m - mods)) g_kbd_owner = -1;
    free(s->tab[0]); free(s->tab[1]);
    s->tab[0] = s->tab[1] = NULL;
}

// ---- layout (relative to the custom area) ---------------------------------

static Rectangle wv_rc(Rectangle a, float x, float y, float w, float h) { return (Rectangle){ a.x + x, a.y + y, w, h }; }
static Rectangle wv_env_box(Rectangle a)  { return wv_rc(a, 0, 0, 290, 138); }
static Rectangle wv_mst_box(Rectangle a)  { return wv_rc(a, 300, 0, 300, 138); }
static Rectangle wv_wt_box(Rectangle a)   { return wv_rc(a, 0, 148, 396, 330); }
static Rectangle wv_mor_box(Rectangle a)  { return wv_rc(a, 406, 148, 194, 330); }
static Rectangle wv_voi_box(Rectangle a)  { return wv_rc(a, 0, 488, 600, 216); }
static Rectangle wv_fx_box(Rectangle a)   { return wv_rc(a, 620, 0, a.width - 620, a.height); }
static Rectangle wv_mod_box(Rectangle a)  { return wv_rc(a, 0, 714, 600, a.height - 714); }
static Rectangle wv_pick_box(Rectangle a) { return wv_rc(a, 0, 148, 600, 330); }
static Rectangle wv_view(Rectangle a)     { Rectangle b = wv_wt_box(a); return (Rectangle){ b.x + 6, b.y + 26, b.width - 12, b.height - 32 }; }
static Rectangle wv_wave_btn(Rectangle a) { Rectangle b = wv_wt_box(a); return (Rectangle){ b.x + 86, b.y + 4, 186, 18 }; }
static Rectangle wv_draw_btn(Rectangle a) { Rectangle b = wv_wt_box(a); return (Rectangle){ b.x + 278, b.y + 4, 52, 18 }; }
static Rectangle wv_tab_btn(Rectangle a, int i) { Rectangle v = wv_view(a); return (Rectangle){ v.x + v.width - 150 + i * 74, v.y + 4, 70, 16 }; }
static Rectangle wv_slot(Rectangle a, int k)     { Rectangle b = wv_mor_box(a); return (Rectangle){ b.x + 6, b.y + 24 + k * 76, b.width - 12, 70 }; }
static Rectangle wv_slot_btn(Rectangle a, int k) { Rectangle r = wv_slot(a, k); return (Rectangle){ r.x + 44, r.y + 8, r.width - 50, 20 }; }
static Rectangle wv_chip(Rectangle a, int c)     { Rectangle b = wv_voi_box(a); return (Rectangle){ b.x + 70 + c * 104, b.y + 4, 98, 18 }; }
static Rectangle wv_stage(Rectangle a)    { Rectangle b = wv_voi_box(a); return (Rectangle){ b.x + 6, b.y + 26, 466, b.height - 32 }; }
static float wv_fx_h(Rectangle a) { return (wv_fx_box(a).height - 26) / WV_NFX; }
static Rectangle wv_fx_row(Rectangle a, int r) { Rectangle b = wv_fx_box(a); float h = wv_fx_h(a); return (Rectangle){ b.x + 6, b.y + 22 + r * h, b.width - 12, h - 4 }; }

// the curve plot inside the stage: x is the stereo position -1..1
static float wv_stage_x(Rectangle st, float x) { return st.x + 18 + (x + 1) * 0.5f * (st.width - 36); }
static float wv_stage_top(Rectangle st)  { return st.y + 18; }
static float wv_stage_base(Rectangle st) { return st.y + st.height - 34; }

typedef struct { int q; Vector2 c; } WvKnob;

static int wv_knobs(const WavorSt *s, Rectangle a, WvKnob *k)
{
    int n = 0;
    Rectangle e = wv_env_box(a), mb = wv_mst_box(a), vb = wv_voi_box(a);
    for (int i = 0; i < 4; i++) k[n++] = (WvKnob){ WQ_A + i, { e.x + 8 + (e.width - 16) * (i + 0.5f) / 4, e.y + 106 } };
    static const int MST[8] = { WQ_OCT, WQ_GLIDE, WQ_PHASE, WQ_LEVEL, WQ_POS, WQ_ENVPOS, WQ_LFORATE, WQ_LFOPOS };
    for (int i = 0; i < 8; i++) k[n++] = (WvKnob){ MST[i], { mb.x + mb.width * ((i % 4) + 0.5f) / 4, mb.y + (i < 4 ? 44 : 102) } };
    if (s->pick < 0)
        for (int i = 0; i < WV_SLOTS; i++) { Rectangle r = wv_slot(a, i); k[n++] = (WvKnob){ WQ_M1A + 2 * i, { r.x + 22, r.y + 30 } }; }
    static const int VOI[3] = { WQ_VOICES, WQ_SPREAD, WQ_ENTRATE };
    for (int i = 0; i < 3; i++) k[n++] = (WvKnob){ VOI[i], { vb.x + vb.width - 64, vb.y + 50 + i * 58 } };
    for (int r = 0; r < WV_NFX; r++) {
        Rectangle row = wv_fx_row(a, r);
        for (int i = 0; i < WV_FX[r].n; i++) k[n++] = (WvKnob){ WV_FX[r].k[i], { row.x + 166 + i * 76, row.y + row.height / 2 - 6 } };
    }
    static const int MOD[12] = { WQ_MA, WQ_MD, WQ_MCUT, WQ_MPOS, WQ_MPITCH, WQ_TREM, WQ_VIBRATE, WQ_VIBDEPTH, WQ_VIBDELAY, WQ_NOISE, WQ_NTONE, WQ_NDECAY };
    Rectangle md = wv_mod_box(a);
    for (int i = 0; i < 12; i++) k[n++] = (WvKnob){ MOD[i], { md.x + md.width * ((i % 6) + 0.5f) / 6, md.y + (i < 6 ? 42 : 94) } };
    return n;
}

static void wv_set_q(WavorSt *s, int i, float v)
{
    const KnobDef *kd = &WVP[i].k;
    v = clampf(v, kd->min, kd->max);
    s->q[i] = kd->kind == K_STEP ? roundf(v) : v;
}

// ---- drawing --------------------------------------------------------------

static void wv_box(Rectangle r, const char *title)
{
    DrawRectangleRec(r, (Color){ 31, 33, 35, 255 });
    DrawRectangleLinesEx(r, 1, PANEL_LO);
    txt(title, r.x + 8, r.y + 8, 10, TEXT_DIM);
}

static void wv_button(Rectangle r, const char *label, Vector2 mp, bool on)
{
    bool h = CheckCollisionPointRec(mp, r);
    DrawRectangleRec(r, on ? fade(ACCENT, 0.16f) : h ? PANEL_HI : PANEL);
    DrawRectangleLinesEx(r, 1, on || h ? ACCENT_DIM : PANEL_HI);
    txt_c(label, r.x + r.width / 2, r.y + r.height / 2 - 5, 10, on || h ? ACCENT : TEXT);
}

static void wv_draw_knob(const WavorSt *s, const WvKnob *k, Vector2 mp, bool dim)
{
    const KnobDef *kd = &WVP[k->q].k;
    float v = s->q[k->q], t = clampf(knob_to_t(kd, v), 0, 1), a1 = 135 + 270 * t;
    bool hover = CheckCollisionPointCircle(mp, k->c, 15);
    bool active = s->drag == WD_KNOB && s->dragQ == k->q && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    Color arc = dim ? fade(ACCENT_DIM, 0.45f) : ACCENT_DIM;
    DrawRing(k->c, 12, 14, 135, 405, 30, PANEL_LO);
    if (kd->min < 0 && kd->max > 0) {
        float az = 135 + 270 * knob_to_t(kd, 0);
        DrawRing(k->c, 12, 14, fminf(az, a1), fmaxf(az, a1), 20, arc);
    } else DrawRing(k->c, 12, 14, 135, a1, 30, arc);
    DrawCircleV((Vector2){ k->c.x, k->c.y + 1.5f }, 10, (Color){ 0, 0, 0, 80 });
    DrawCircleV(k->c, 10, (Color){ 30, 31, 33, 255 });
    DrawCircleV(k->c, 8.5f, (Color){ 46, 48, 50, 255 });
    float ar = a1 * DEG2RAD;
    DrawLineEx((Vector2){ k->c.x + cosf(ar) * 3, k->c.y + sinf(ar) * 3 }, (Vector2){ k->c.x + cosf(ar) * 8.5f, k->c.y + sinf(ar) * 8.5f }, 2, dim ? TEXT_DIM : TEXT);
    const char *label = kd->name;
    if (k->q == WQ_VOICES) label = TextFormat("%d VOICE%s", (int)v, v > 1.5f ? "S" : "");
    else if (k->q >= WQ_M1A && k->q <= WQ_M4A && (k->q - WQ_M1A) % 2 == 0) label = TextFormat("%.0f%%", v * 100);
    else if (hover || active || kd->fmt == F_LABEL) label = knob_text(kd, v);
    if (hover || active) DrawRing(k->c, 14.5f, 15.5f, 0, 360, 30, fade(ACCENT, 0.5f));
    txt_c(label, k->c.x, k->c.y + 17, 10, hover || active ? ACCENT : dim ? fade(TEXT_DIM, 0.55f) : TEXT_DIM);
}

static void wv_draw_env(WavorSt *s, Rectangle e)
{
    const float *q = s->q;
    Rectangle g = { e.x + 8, e.y + 24, e.width - 16, 58 };
    DrawRectangleRec(g, INSET);
    float wA = 0.1f + knob_to_t(&WVP[WQ_A].k, q[WQ_A]), wD = 0.1f + knob_to_t(&WVP[WQ_D].k, q[WQ_D]);
    float wR = 0.1f + knob_to_t(&WVP[WQ_R].k, q[WQ_R]), wS = 0.45f, sc = (g.width - 12) / (wA + wD + wS + wR);
    float x0 = g.x + 6, top = g.y + 6, bot = g.y + g.height - 4, hh = bot - top, S = q[WQ_S];
    float x1 = x0 + wA * sc, x2 = x1 + wD * sc, x3 = x2 + wS * sc, x4 = x3 + wR * sc;
    Vector2 pts[48];
    int n = 0;
    pts[n++] = (Vector2){ x0, bot };
    pts[n++] = (Vector2){ x1, top };
    for (int i = 1; i <= 16; i++) { float u = i / 16.0f; pts[n++] = (Vector2){ x1 + u * (x2 - x1), bot - hh * (S + (1 - S) * expf(-4 * u)) }; }
    pts[n++] = (Vector2){ x3, bot - hh * S };
    for (int i = 1; i <= 16; i++) { float u = i / 16.0f; pts[n++] = (Vector2){ x3 + u * (x4 - x3), bot - hh * S * expf(-5 * u) }; }
    for (int i = 0; i + 1 < n; i++) {
        DrawTriangle(pts[i], (Vector2){ pts[i].x, bot }, pts[i + 1], fade(ACCENT, 0.06f));
        DrawTriangle(pts[i + 1], (Vector2){ pts[i].x, bot }, (Vector2){ pts[i + 1].x, bot }, fade(ACCENT, 0.06f));
    }
    for (int i = 0; i + 1 < n; i++) DrawLineEx(pts[i], pts[i + 1], 1.6f, ACCENT);
    // where the envelope is now
    float ex = s->stage == 1 ? x0 + s->env * (x1 - x0) : s->stage == 2 ? x2 + (x3 - x2) * 0.5f :
               s->stage == 3 ? x3 + (x4 - x3) * clampf(1 - s->env / fmaxf(S, 0.001f), 0, 1) : x4;
    Vector2 d = { ex, bot - hh * s->env };
    DrawCircleV(d, 6, fade(PHOSPHOR, 0.18f));
    DrawCircleV(d, 2.5f, PHOSPHOR);
}

static void wv_draw_view(WavorSt *s, Rectangle v, Vector2 mp)
{
    DrawRectangleRec(v, INSET);
    if (!s->dispOk) { txt_c("BUILDING...", v.x + v.width / 2, v.y + v.height / 2, 10, TEXT_DIM); return; }
    const float dx = 3.6f, dy = 3.4f;
    float w = v.width - 24 - dx * (WV_F - 1), amp = (v.height - 34 - dy * (WV_F - 1)) * 0.5f;
    Vector2 pts[WV_DISP];
    for (int f = WV_F - 1; f >= 0; f--) {   // back to front
        float ox = v.x + 12 + f * dx, oy = v.y + v.height - 16 - amp - f * dy, t = f / (float)(WV_F - 1);
        for (int j = 0; j < WV_DISP; j++) pts[j] = (Vector2){ ox + j * w / (WV_DISP - 1), oy - s->disp[f][j] * amp * 0.92f };
        Color c = { (unsigned char)(52 + t * 60), (unsigned char)(110 + t * 32), (unsigned char)(82 + t * 100), 255 };
        DrawLineStrip(pts, WV_DISP, fade(c, 0.22f + 0.4f * (1 - t)));
    }
    // the frame being played (POSITION with its modulation), and each voice's own position
    float fp = s->dpos * (WV_F - 1);
    int f0 = (int)fp < WV_F - 2 ? (int)fp : WV_F - 2;
    float fr = fp - f0, ox = v.x + 12 + fp * dx, oy = v.y + v.height - 16 - amp - fp * dy;
    for (int j = 0; j < WV_DISP; j++) {
        float y = s->disp[f0][j] + fr * (s->disp[f0 + 1][j] - s->disp[f0][j]);
        pts[j] = (Vector2){ ox + j * w / (WV_DISP - 1), oy - y * amp * 0.92f };
    }
    for (int j = 0; j + 1 < WV_DISP; j++) DrawLineEx(pts[j], pts[j + 1], 5, fade(PHOSPHOR, 0.10f));
    for (int j = 0; j + 1 < WV_DISP; j++) DrawLineEx(pts[j], pts[j + 1], 1.6f, PHOSPHOR);
    for (int i = 0; i < s->vn; i++) {
        float vp = s->vp[i] * (WV_F - 1);
        DrawCircleV((Vector2){ v.x + 7 + vp * dx, v.y + v.height - 16 - vp * dy }, 1.6f, fade(CABLE_COLORS[WV_CURVE_COL[WC_POS]], 0.8f));
    }
    txt("WAVE", v.x + 8, v.y + v.height - 12, 10, fade(TEXT_DIM, 0.7f));
    txt("FULLY MORPHED", v.x + v.width - 8 - txt_w("FULLY MORPHED", 10), v.y + 6, 10, fade(TEXT_DIM, 0.7f));
    (void)mp;
}

static void wv_draw_editor(WavorSt *s, Rectangle a, Vector2 mp)
{
    Rectangle v = wv_view(a);
    DrawRectangleRec(v, INSET);
    for (int g = 1; g < 8; g++) DrawLineEx((Vector2){ v.x + g * v.width / 8, v.y }, (Vector2){ v.x + g * v.width / 8, v.y + v.height }, 1, fade(ACCENT_DIM, 0.18f));
    wv_button(wv_tab_btn(a, 0), "SHAPE", mp, s->userMode == 0);
    wv_button(wv_tab_btn(a, 1), "PARTIALS", mp, s->userMode == 1);
    if (s->userMode == 0) {
        float cy = v.y + v.height / 2, hh = v.height / 2 - 26;
        DrawLineEx((Vector2){ v.x, cy }, (Vector2){ v.x + v.width, cy }, 1, fade(ACCENT_DIM, 0.4f));
        Vector2 prev = { 0 };
        for (int j = 0; j <= WV_UPTS; j++) {
            Vector2 p = { v.x + j * v.width / WV_UPTS, cy - s->user[j % WV_UPTS] * hh };
            if (j) { DrawLineEx(prev, p, 4, fade(PHOSPHOR, 0.10f)); DrawLineEx(prev, p, 1.6f, PHOSPHOR); }
            prev = p;
        }
        txt("DRAG TO DRAW ONE CYCLE", v.x + 8, v.y + 8, 10, TEXT_DIM);
    } else {
        float bw = v.width / WV_HARM, bot = v.y + v.height - 16, hh = bot - (v.y + 28);
        for (int h = 0; h < WV_HARM; h++) {
            float bh = s->part[h] * hh;
            DrawRectangleRec((Rectangle){ v.x + h * bw + 2, bot - bh, bw - 4, bh }, fade(ACCENT, 0.55f));
            DrawRectangleRec((Rectangle){ v.x + h * bw + 2, bot - bh, bw - 4, 2 }, PHOSPHOR);
            if (h % 4 == 0) txt(TextFormat("%d", h + 1), v.x + h * bw + 3, bot + 3, 10, fade(TEXT_DIM, 0.7f));
        }
        txt("DRAG TO SET PARTIALS 1-32", v.x + 8, v.y + 8, 10, TEXT_DIM);
    }
}

static void wv_draw_stage(WavorSt *s, Rectangle st, Vector2 mp)
{
    const float *q = s->q;
    DrawRectangleRec(st, INSET);
    float top = wv_stage_top(st), base = wv_stage_base(st), hh = base - top;
    Vector2 me = { st.x + st.width / 2, st.y + st.height - 13 };
    DrawLineEx((Vector2){ me.x, top }, (Vector2){ me.x, base }, 1, fade(ACCENT_DIM, 0.25f));
    txt("L", st.x + 6, base + 4, 10, TEXT_DIM);
    txt("R", st.x + st.width - 12, base + 4, 10, TEXT_DIM);
    // the voices, as bars where they sit in the stereo image, and their rays to the listener
    int n = s->vn;
    float bw = clampf((st.width - 36) / (n ? n : 1) * 0.55f, 2, 9);
    for (int i = 0; i < n; i++) DrawLineEx(me, (Vector2){ wv_stage_x(st, s->vx[i]), base }, 1, fade(ACCENT_DIM, 0.22f));
    for (int i = 0; i < n; i++) {
        float x = wv_stage_x(st, s->vx[i]), h = clampf(s->vl[i], 0, 1) * hh, ag = s->va[i];
        Color c0 = ACCENT, c1 = CABLE_COLORS[1];
        Color c = { (unsigned char)(c0.r + (c1.r - c0.r) * ag), (unsigned char)(c0.g + (c1.g - c0.g) * ag), (unsigned char)(c0.b + (c1.b - c0.b) * ag), 255 };
        DrawRectangleRec((Rectangle){ x - bw / 2, base - h, bw, h }, fade(c, 0.35f));
        DrawRectangleRec((Rectangle){ x - bw / 2, base - h, bw, 2 }, c);
    }
    DrawCircleV(me, 11, fade(PHOSPHOR, 0.12f));
    DrawCircleV(me, 5, PHOSPHOR);
    DrawTriangle((Vector2){ me.x, me.y - 10 }, (Vector2){ me.x - 3, me.y - 5 }, (Vector2){ me.x + 3, me.y - 5 }, PHOSPHOR);
    // the curves; the selected one has handles
    for (int pass = 0; pass < 2; pass++)
        for (int c = 0; c < WV_NCURVE; c++) {
            bool sel = c == s->curve;
            if (sel != (pass == 1)) continue;
            Color col = CABLE_COLORS[WV_CURVE_COL[c]];
            Vector2 prev = { 0 };
            for (int i = 0; i <= 40; i++) {
                float x = i / 20.0f - 1;
                Vector2 p = { wv_stage_x(st, x), base - wv_curve(q, c, x) * hh };
                if (i) DrawLineEx(prev, p, sel ? 2 : 1, fade(col, sel ? 1 : 0.35f));
                prev = p;
            }
            if (sel)
                for (int h = 0; h < 3; h++) {
                    Vector2 p = { wv_stage_x(st, h - 1.0f), base - q[WQ_CURVE + c * 3 + h] * hh };
                    bool hv = s->drag == WD_CURVE && s->dragH == h && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
                    DrawCircleV(p, hv ? 6 : 4.5f, col);
                    DrawCircleV(p, 2, INSET);
                }
        }
    const float *h = q + WQ_CURVE + s->curve * 3;
    txt(TextFormat("%s   L %.0f%%   C %.0f%%   R %.0f%%", WV_CURVES[s->curve], h[0] * 100, h[1] * 100, h[2] * 100), st.x + 8, st.y + 5, 10, CABLE_COLORS[WV_CURVE_COL[s->curve]]);
    const char *info = TextFormat("%d x %d NOTE%s", n, s->dnotes, s->dnotes == 1 ? "" : "S");
    txt(info, st.x + st.width - 8 - txt_w(info, 10), st.y + 5, 10, TEXT_DIM);
    (void)mp;
}

// Picker: 8 x 8 cells; the morpher picker has one family per column.
static Rectangle wv_pick_cell(Rectangle a, int pick, int i)
{
    Rectangle pb = wv_pick_box(a);
    float gy = pb.y + 26 + (pick ? 14 : 0), cw = (pb.width - 12) / 8, ch = (pb.y + pb.height - 6 - gy) / 8;
    int c = pick ? i / 8 : i % 8, r = pick ? i % 8 : i / 8;
    return (Rectangle){ pb.x + 6 + c * cw, gy + r * ch, cw - 3, ch - 3 };
}
static Rectangle wv_pick_user(Rectangle a)  { Rectangle pb = wv_pick_box(a); return (Rectangle){ pb.x + pb.width - 150, pb.y + 4, 110, 18 }; }
static Rectangle wv_pick_close(Rectangle a) { Rectangle pb = wv_pick_box(a); return (Rectangle){ pb.x + pb.width - 30, pb.y + 4, 24, 18 }; }

static void wv_draw_picker(WavorSt *s, Rectangle a, Vector2 mp)
{
    Rectangle pb = wv_pick_box(a);
    DrawRectangleRec((Rectangle){ pb.x + 5, pb.y + 6, pb.width, pb.height }, (Color){ 0, 0, 0, 110 });
    DrawRectangleRec(pb, (Color){ 26, 28, 30, 255 });
    DrawRectangleLinesEx(pb, 1, ACCENT_DIM);
    bool wave = s->pick == 0;
    int cur = wave ? (int)s->q[WQ_WAVE] : (int)s->q[WQ_M1T + 2 * (s->pick - 1)];
    txt(wave ? "CHOOSE A WAVE" : TextFormat("CHOOSE MORPHER %d", s->pick), pb.x + 10, pb.y + 9, 10, TEXT);
    if (wave) wv_button(wv_pick_user(a), "USER / DRAW", mp, cur == WV_USER);
    wv_button(wv_pick_close(a), "X", mp, false);
    if (!wave)
        for (int c = 0; c < 8; c++) { Rectangle r = wv_pick_cell(a, 1, c * 8); txt(WV_MCAT[c], r.x + 2, r.y - 13, 10, TEXT_DIM); }
    for (int i = 0; i < 64; i++) {
        Rectangle r = wv_pick_cell(a, s->pick, i);
        bool h = CheckCollisionPointRec(mp, r), on = i == cur;
        DrawRectangleRec(r, on ? fade(ACCENT, 0.18f) : h ? PANEL_HI : (Color){ 36, 38, 40, 255 });
        if (on || h) DrawRectangleLinesEx(r, 1, on ? ACCENT : ACCENT_DIM);
        if (wave) {
            Vector2 prev = { 0 };
            for (int j = 0; j <= 48; j++) {
                Vector2 p = { r.x + 4 + j * (r.width - 8) / 48, r.y + 11 - wv_thumb[i][j % 48] * 7 };
                if (j) DrawLineEx(prev, p, 1, on ? PHOSPHOR : fade(ACCENT, 0.7f));
                prev = p;
            }
            txt(WV_WAVES[i], r.x + 4, r.y + r.height - 12, 10, on || h ? ACCENT : TEXT);
        } else
            txt(WV_MORPHS[i], r.x + 5, r.y + r.height / 2 - 5, 10, on || h ? ACCENT : TEXT);
    }
}

static void wv_draw_body(Module *m, Rectangle a)
{
    WavorSt *s = m->st;
    const float *q = s->q;
    Vector2 mp = GetMousePosition();
    Vector2 kmp = s->pick >= 0 && CheckCollisionPointRec(mp, wv_pick_box(a)) ? (Vector2){ -1e4f, -1e4f } : mp;

    // envelope and master
    Rectangle e = wv_env_box(a), mb = wv_mst_box(a);
    wv_box(e, "ENVELOPE");
    wv_draw_env(s, e);
    wv_box(mb, "MASTER");
    DrawLineEx((Vector2){ mb.x + 10, mb.y + 78 }, (Vector2){ mb.x + mb.width - 10, mb.y + 78 }, 1, PANEL_LO);

    // wavetable
    Rectangle wb = wv_wt_box(a);
    wv_box(wb, "WAVETABLE");
    int wave = (int)q[WQ_WAVE];
    const char *wl = wave == WV_USER ? (s->userMode ? "USER  PARTIALS" : "USER  SHAPE") : TextFormat("%02d  %s", wave + 1, WV_WAVES[wave]);
    wv_button(wv_wave_btn(a), wl, kmp, false);
    wv_button(wv_draw_btn(a), "DRAW", kmp, s->drawMode);
    const char *pl = TextFormat("POS %.0f%%", s->dpos * 100);
    txt(pl, wb.x + wb.width - 8 - txt_w(pl, 10), wb.y + 8, 10, ACCENT);
    if (s->drawMode) wv_draw_editor(s, a, kmp);
    else wv_draw_view(s, wv_view(a), kmp);

    // morphers
    Rectangle mo = wv_mor_box(a);
    wv_box(mo, "MORPHERS");
    for (int k = 0; k < WV_SLOTS; k++) {
        Rectangle r = wv_slot(a, k), b = wv_slot_btn(a, k);
        int t = clampi((int)q[WQ_M1T + 2 * k], 0, 63);
        float am = q[WQ_M1A + 2 * k];
        DrawRectangleRec(r, (Color){ 25, 27, 29, 255 });
        wv_button(b, TextFormat("%d  %s", k + 1, WV_MORPHS[t]), kmp, false);
        txt(WV_MCAT[t / 8], b.x + 2, b.y + 27, 10, fade(TEXT_DIM, 0.8f));
        DrawRectangleRec((Rectangle){ b.x, b.y + 42, b.width, 3 }, PANEL_LO);
        DrawRectangleRec((Rectangle){ b.x, b.y + 42, b.width * am, 3 }, am > 0 ? ACCENT : ACCENT_DIM);
    }

    // voices
    Rectangle vb = wv_voi_box(a);
    wv_box(vb, "VOICES");
    for (int c = 0; c < WV_NCURVE; c++) {
        Rectangle r = wv_chip(a, c);
        Color col = CABLE_COLORS[WV_CURVE_COL[c]];
        bool on = c == s->curve, h = CheckCollisionPointRec(kmp, r);
        DrawRectangleRec(r, on ? fade(col, 0.22f) : h ? PANEL_HI : PANEL);
        DrawRectangleLinesEx(r, 1, on ? col : fade(col, 0.45f));
        txt_c(WV_CURVES[c], r.x + r.width / 2, r.y + 4, 10, on ? TEXT : fade(col, 0.9f));
    }
    wv_draw_stage(s, wv_stage(a), kmp);

    // effects
    Rectangle fb = wv_fx_box(a);
    wv_box(fb, "FX CHAIN");
    txt("CLICK A NAME TO SWITCH IT ON OR OFF", fb.x + fb.width - 8 - txt_w("CLICK A NAME TO SWITCH IT ON OR OFF", 10), fb.y + 8, 10, fade(TEXT_DIM, 0.55f));
    for (int r = 0; r < WV_NFX; r++) {
        Rectangle row = wv_fx_row(a, r);
        bool on = q[WV_FX[r].on] > 0.5f, h = CheckCollisionPointRec(kmp, (Rectangle){ row.x, row.y, 130, row.height });
        DrawRectangleRec(row, on ? (Color){ 35, 39, 39, 255 } : (Color){ 27, 28, 30, 255 });
        if (r < WV_NFX - 1) DrawLineEx((Vector2){ row.x + 14, row.y + 24 }, (Vector2){ row.x + 14, row.y + wv_fx_h(a) + 13 }, 1, PANEL_HI);
        Vector2 led = { row.x + 14, row.y + 18 };
        if (on) { DrawCircleV(led, 8, fade(ACCENT, 0.2f)); DrawCircleV(led, 4.5f, ACCENT); }
        else { DrawCircleV(led, 4.5f, PANEL_LO); DrawRing(led, 4, 5, 0, 360, 16, h ? ACCENT_DIM : PANEL_HI); }
        txt(WV_FX[r].name, row.x + 28, row.y + 13, 10, on ? TEXT : h ? ACCENT : TEXT_DIM);
        txt(TextFormat("%02d", r + 1), row.x + 28, row.y + 30, 10, fade(TEXT_DIM, 0.45f));
        if (r == 4) {   // compressor: gain reduction
            Rectangle gm = { row.x + row.width - 18, row.y + 8, 6, row.height - 16 };
            DrawRectangleRec(gm, INSET);
            float g = on ? clampf(s->fx.gr / 24, 0, 1) : 0;
            DrawRectangleRec((Rectangle){ gm.x, gm.y, gm.width, gm.height * g }, CABLE_COLORS[1]);
            txt("GR", gm.x - 16, gm.y + gm.height - 10, 10, fade(TEXT_DIM, 0.6f));
        }
    }

    Rectangle mdb = wv_mod_box(a);
    wv_box(mdb, "MODULATION");
    txt("MOD ENVELOPE, TREMOLO, VIBRATO, NOISE", mdb.x + mdb.width - 8 - txt_w("MOD ENVELOPE, TREMOLO, VIBRATO, NOISE", 10), mdb.y + 8, 10, fade(TEXT_DIM, 0.55f));
    WvKnob ks[80];
    int nk = wv_knobs(s, a, ks);
    for (int i = 0; i < nk; i++) {
        bool dim = false;
        for (int r = 0; r < WV_NFX; r++)
            for (int j = 0; j < WV_FX[r].n; j++) if (WV_FX[r].k[j] == ks[i].q) dim = q[WV_FX[r].on] < 0.5f;
        if (ks[i].q >= WQ_M1A && ks[i].q <= WQ_M4A) dim = q[ks[i].q] <= 0;
        wv_draw_knob(s, &ks[i], kmp, dim);
    }
    if (s->pick >= 0) wv_draw_picker(s, a, mp);
}

// ---- interaction ----------------------------------------------------------

static void wv_toggle_draw(WavorSt *s)
{
    if (s->drawMode) { s->drawMode = false; return; }
    int w = (int)s->q[WQ_WAVE];
    if (w != WV_USER) {   // start drawing from the wave on screen
        static float tmp[WV_N];
        wv_wave(w, tmp);
        for (int j = 0; j < WV_UPTS; j++) s->user[j] = tmp[j * (WV_N / WV_UPTS)];
        wv_partials_of(tmp, WV_N, s->part);
        s->userMode = 0; s->userSet = true; s->q[WQ_WAVE] = WV_USER; s->userVer++;
    }
    s->drawMode = true;
}

static void wv_edit(WavorSt *s, Rectangle a, Vector2 mp)
{
    Rectangle v = wv_view(a);
    float fx = clampf((mp.x - v.x) / v.width, 0, 0.9999f);
    if (s->userMode == 0) {
        float hh = v.height / 2 - 26, y = clampf((v.y + v.height / 2 - mp.y) / hh, -1, 1);
        int j = (int)(fx * WV_UPTS);
        int j0 = s->lastJ < 0 ? j : s->lastJ;
        float y0 = s->lastJ < 0 ? y : s->lastV;
        int step = j >= j0 ? 1 : -1;
        for (int k = j0; ; k += step) {
            s->user[k] = j == j0 ? y : y0 + (y - y0) * (k - j0) / (float)(j - j0);
            if (k == j) break;
        }
        s->lastJ = j; s->lastV = y;
    } else {
        float bot = v.y + v.height - 16, y = clampf((bot - mp.y) / (bot - (v.y + 28)), 0, 1);
        int h = (int)(fx * WV_HARM);
        int h0 = s->lastJ < 0 ? h : s->lastJ;
        float y0 = s->lastJ < 0 ? y : s->lastV;
        int step = h >= h0 ? 1 : -1;
        for (int k = h0; ; k += step) {
            s->part[k] = h == h0 ? y : y0 + (y - y0) * (k - h0) / (float)(h - h0);
            if (k == h) break;
        }
        s->lastJ = h; s->lastV = y;
    }
    s->userSet = true;
    s->userVer++;
}

static void wv_set_curve(WavorSt *s, Rectangle a, Vector2 mp)
{
    Rectangle st = wv_stage(a);
    float top = wv_stage_top(st), base = wv_stage_base(st);
    wv_set_q(s, WQ_CURVE + s->curve * 3 + s->dragH, (base - mp.y) / (base - top));
}

static int wv_pick_hit(const WavorSt *s, Rectangle a, Vector2 mp)   // 0..63, 100 user, 101 close, -1
{
    if (CheckCollisionPointRec(mp, wv_pick_close(a))) return 101;
    if (s->pick == 0 && CheckCollisionPointRec(mp, wv_pick_user(a))) return 100;
    for (int i = 0; i < 64; i++) if (CheckCollisionPointRec(mp, wv_pick_cell(a, s->pick, i))) return i;
    return -1;
}

static bool wv_press_body(Module *m, Vector2 mp, Rectangle a)
{
    WavorSt *s = m->st;
    s->drag = WD_NONE;
    if (s->pick >= 0) {
        int hit = wv_pick_hit(s, a, mp);
        if (hit >= 0 && hit < 64) {
            if (s->pick == 0) { s->q[WQ_WAVE] = (float)hit; s->drawMode = false; }
            else s->q[WQ_M1T + 2 * (s->pick - 1)] = (float)hit;
            s->pick = -1;
        } else if (hit == 100) {
            s->pick = -1;
            if (s->userSet) s->q[WQ_WAVE] = WV_USER;
            else { s->drawMode = false; wv_toggle_draw(s); }
        } else if (hit == 101 || !CheckCollisionPointRec(mp, wv_pick_box(a))) s->pick = -1;
        return true;
    }
    WvKnob ks[80];
    int nk = wv_knobs(s, a, ks);
    for (int i = 0; i < nk; i++)
        if (CheckCollisionPointCircle(mp, ks[i].c, 15)) {
            double now = GetTime();
            const KnobDef *kd = &WVP[ks[i].q].k;
            if (s->lastKnob == ks[i].q && now - s->lastClick < 0.3) { s->q[ks[i].q] = kd->def; s->lastKnob = -1; return true; }
            s->drag = WD_KNOB; s->dragQ = ks[i].q; s->dragT = knob_to_t(kd, s->q[ks[i].q]); s->lastY = mp.y;
            s->lastKnob = ks[i].q; s->lastClick = now;
            return true;
        }
    if (CheckCollisionPointRec(mp, wv_wave_btn(a))) { s->pick = 0; return true; }
    if (CheckCollisionPointRec(mp, wv_draw_btn(a))) { wv_toggle_draw(s); return true; }
    for (int k = 0; k < WV_SLOTS; k++) if (CheckCollisionPointRec(mp, wv_slot_btn(a, k))) { s->pick = 1 + k; return true; }
    for (int r = 0; r < WV_NFX; r++) {
        Rectangle row = wv_fx_row(a, r);
        if (CheckCollisionPointRec(mp, (Rectangle){ row.x, row.y, 130, row.height })) { s->q[WV_FX[r].on] = s->q[WV_FX[r].on] > 0.5f ? 0 : 1; return true; }
    }
    for (int c = 0; c < WV_NCURVE; c++) if (CheckCollisionPointRec(mp, wv_chip(a, c))) { s->curve = c; return true; }
    Rectangle st = wv_stage(a);
    if (CheckCollisionPointRec(mp, st)) {   // grab the nearest handle of the selected curve
        float x = (mp.x - wv_stage_x(st, -1)) / (wv_stage_x(st, 1) - wv_stage_x(st, -1));
        s->dragH = x < 1 / 3.0f ? 0 : x < 2 / 3.0f ? 1 : 2;
        s->drag = WD_CURVE;
        wv_set_curve(s, a, mp);
        return true;
    }
    if (s->drawMode) {
        for (int i = 0; i < 2; i++) if (CheckCollisionPointRec(mp, wv_tab_btn(a, i))) { s->userMode = i; s->userVer++; return true; }
        if (CheckCollisionPointRec(mp, wv_view(a))) { s->drag = WD_EDIT; s->lastJ = -1; wv_edit(s, a, mp); return true; }
    }
    return false;
}

static void wv_drag(Module *m, Vector2 mp, Rectangle a)
{
    WavorSt *s = m->st;
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    switch (s->drag) {
    case WD_KNOB: {
        const KnobDef *kd = &WVP[s->dragQ].k;
        float range = kd->kind == K_STEP ? fmaxf(200.0f, (kd->max - kd->min) * 6.0f) : 200.0f;
        s->dragT = clampf(s->dragT - (mp.y - s->lastY) / range * (shift ? 0.1f : 1.0f), 0, 1);
        s->lastY = mp.y;
        wv_set_q(s, s->dragQ, knob_from_t(kd, s->dragT));
    } break;
    case WD_CURVE: wv_set_curve(s, a, mp); break;
    case WD_EDIT:  wv_edit(s, a, mp); break;
    }
}

static bool wv_wheel_body(Module *m, Vector2 mp, Rectangle a, float w)
{
    WavorSt *s = m->st;
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int d = w > 0 ? 1 : -1;
    if (s->pick >= 0) return CheckCollisionPointRec(mp, wv_pick_box(a));
    WvKnob ks[80];
    int nk = wv_knobs(s, a, ks);
    for (int i = 0; i < nk; i++)
        if (CheckCollisionPointCircle(mp, ks[i].c, 15)) {
            const KnobDef *kd = &WVP[ks[i].q].k;
            if (kd->kind == K_STEP) wv_set_q(s, ks[i].q, s->q[ks[i].q] + d);
            else wv_set_q(s, ks[i].q, knob_from_t(kd, knob_to_t(kd, s->q[ks[i].q]) + w * (shift ? 0.005f : 0.03f)));
            return true;
        }
    if (CheckCollisionPointRec(mp, wv_wave_btn(a))) {   // step through the waves
        int wv = (int)s->q[WQ_WAVE];
        s->q[WQ_WAVE] = (float)(wv == WV_USER ? (d > 0 ? 0 : WV_NWAVE - 1) : (wv + d + WV_NWAVE) % WV_NWAVE);
        s->drawMode = false;
        return true;
    }
    for (int k = 0; k < WV_SLOTS; k++)
        if (CheckCollisionPointRec(mp, wv_slot_btn(a, k))) { s->q[WQ_M1T + 2 * k] = (float)(((int)s->q[WQ_M1T + 2 * k] + d + 64) % 64); return true; }
    Rectangle st = wv_stage(a);
    if (CheckCollisionPointRec(mp, st)) {
        float x = (mp.x - wv_stage_x(st, -1)) / (wv_stage_x(st, 1) - wv_stage_x(st, -1));
        int h = x < 1 / 3.0f ? 0 : x < 2 / 3.0f ? 1 : 2, qi = WQ_CURVE + s->curve * 3 + h;
        wv_set_q(s, qi, s->q[qi] + w * (shift ? 0.005f : 0.03f));
        return true;
    }
    return false;
}

static bool wv_rpress_body(Module *m, Vector2 mp, Rectangle a)   // right-click: bypass a morpher, reset a curve
{
    WavorSt *s = m->st;
    if (s->pick >= 0) { s->pick = -1; return true; }
    for (int k = 0; k < WV_SLOTS; k++) if (CheckCollisionPointRec(mp, wv_slot_btn(a, k))) { s->q[WQ_M1A + 2 * k] = 0; return true; }
    if (CheckCollisionPointRec(mp, wv_stage(a))) {
        for (int h = 0; h < 3; h++) s->q[WQ_CURVE + s->curve * 3 + h] = WVP[WQ_CURVE + s->curve * 3 + h].k.def;
        return true;
    }
    return false;
}

// ---- presets and sampling --------------------------------------------------

static void wv_save(const Module *m, char *out, int cap)
{
    const WavorSt *s = m->st;
    int n = 0;
#define WV_PUT(...) do { if (n < cap) n += snprintf(out + n, cap - n, __VA_ARGS__); } while (0)
    for (int i = 0; i < WQ_N; i++) if (WVP[i].key) WV_PUT("%s=%.5g ", WVP[i].key, s->q[i]);
    WV_PUT("um=%d us=%d u=", s->userMode, s->userSet ? 1 : 0);
    for (int j = 0; j < WV_UPTS; j++) WV_PUT("%02x", (int)roundf(clampf(s->user[j], -1, 1) * 127.5f + 127.5f));
    WV_PUT(" h=");
    for (int j = 0; j < WV_HARM; j++) WV_PUT("%02x", (int)roundf(clampf(s->part[j], 0, 1) * 255));
    char pn[sizeof s->pname];
    snprintf(pn, sizeof pn, "%s", s->pname[0] ? s->pname : "INIT");
    for (char *c = pn; *c; c++) if (*c == ' ') *c = '_';
    WV_PUT(" pi=%d pn=%s", s->pidx, pn);
#undef WV_PUT
}

static void wv_parse(WavorSt *s, const char *str)
{
    char tok[640];
    for (const char *p = str; *p; ) {
        while (*p == ' ') p++;
        int len = (int)strcspn(p, " ");
        if (!len) break;
        if (len < (int)sizeof tok) {
            memcpy(tok, p, len);
            tok[len] = 0;
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = 0;
                const char *k = tok, *v = eq + 1;
                unsigned b;
                if (!strcmp(k, "u")) { for (int j = 0; j < WV_UPTS && v[2 * j] && v[2 * j + 1] && sscanf(v + 2 * j, "%2x", &b) == 1; j++) s->user[j] = (b - 127.5f) / 127.5f; }
                else if (!strcmp(k, "h")) { for (int j = 0; j < WV_HARM && v[2 * j] && v[2 * j + 1] && sscanf(v + 2 * j, "%2x", &b) == 1; j++) s->part[j] = b / 255.0f; }
                else if (!strcmp(k, "um")) s->userMode = atoi(v) ? 1 : 0;
                else if (!strcmp(k, "us")) s->userSet = atoi(v) != 0;
                else if (!strcmp(k, "pi")) s->pidx = atoi(v);
                else if (!strcmp(k, "pn")) { snprintf(s->pname, sizeof s->pname, "%s", v); for (char *c = s->pname; *c; c++) if (*c == '_') *c = ' '; }
                else for (int i = 0; i < WQ_N; i++) if (WVP[i].key && !strcmp(k, WVP[i].key)) { wv_set_q(s, i, (float)atof(v)); break; }
            }
        }
        p += len;
    }
    s->userVer++;
    s->fx.dt = s->q[WQ_DLY_TIME] * SR;
}

static void wv_load(Module *m, const char *str)   // a project or preset rack line
{
    WavorSt *s = m->st;
    wv_parse(s, str);
    s->psig = wv_sound_sig(s);
    wv_work(m, 1e9);
}

// Drop an audio file on WAVOR: find the pitch where it is loudest and take
// one cycle of it as the user shape.
static void wv_load_audio(Module *m, const char *path)
{
    WavorSt *s = m->st;
    Wave w = LoadWave(path);
    if (!w.data || w.frameCount < 64) { if (w.data) UnloadWave(w); toast(TextFormat("CANNOT READ %s", GetFileName(path))); return; }
    WaveFormat(&w, (int)SR, 32, 1);
    const float *x = w.data;
    int n = (int)w.frameCount < 60 * (int)SR ? (int)w.frameCount : 60 * (int)SR;
    enum { WIN = 2048, MINLAG = 24, MAXLAG = 1200 };
    int st = 0;
    float best = -1, period = 0;
    for (int i = 0; i + WIN + MAXLAG < n; i += WIN / 2) {
        float e = 0;
        for (int k = 0; k < WIN; k++) e += x[i + k] * x[i + k];
        if (e > best) { best = e; st = i; }
    }
    if (n > WIN + MAXLAG) {   // normalised autocorrelation; the shortest strong peak avoids octave errors
        static float r[MAXLAG + 1];
        float rmax = 0;
        for (int l = MINLAG; l <= MAXLAG; l++) {
            double xy = 0, xx = 0, yy = 0;
            for (int k = 0; k < WIN; k++) { double p = x[st + k], q = x[st + k + l]; xy += p * q; xx += p * p; yy += q * q; }
            r[l] = xx > 0 && yy > 0 ? (float)(xy / sqrt(xx * yy)) : 0;
            rmax = fmaxf(rmax, r[l]);
        }
        if (rmax > 0.5f)
            for (int l = MINLAG + 1; l < MAXLAG; l++)
                if (r[l] >= 0.9f * rmax && r[l] >= r[l - 1] && r[l] >= r[l + 1]) {
                    float d = r[l - 1] - 2 * r[l] + r[l + 1];
                    period = l + (fabsf(d) > 1e-9f ? 0.5f * (r[l - 1] - r[l + 1]) / d : 0);
                    break;
                }
    }
    int z = st;
    for (int k = 0; k < WIN && st + k + 1 < n; k++) if (x[st + k] <= 0 && x[st + k + 1] > 0) { z = st + k; break; }
    float len = period > 0 ? period : fminf(WIN, (float)(n - z - 2));
    for (int j = 0; j < WV_UPTS; j++) {
        float p = z + j * len / WV_UPTS;
        int i0 = clampi((int)p, 0, n - 2);
        s->user[j] = x[i0] + (p - i0) * (x[i0 + 1] - x[i0]);
    }
    UnloadWave(w);
    float mean = 0, pk = 0;
    for (int j = 0; j < WV_UPTS; j++) mean += s->user[j] / WV_UPTS;
    for (int j = 0; j < WV_UPTS; j++) { s->user[j] -= mean; pk = fmaxf(pk, fabsf(s->user[j])); }
    for (int j = 0; j < WV_UPTS; j++) s->user[j] = pk > 1e-6f ? s->user[j] / pk : 0;
    wv_partials_of(s->user, WV_UPTS, s->part);
    s->userMode = 0; s->userSet = true; s->q[WQ_WAVE] = WV_USER; s->userVer++;
    if (period > 0) toast(TextFormat("WAVOR: ONE CYCLE OF %s (%.1f HZ)", GetFileName(path), SR / period));
    else toast(TextFormat("WAVOR: NO CLEAR PITCH IN %s, TOOK A SLICE", GetFileName(path)));
}


// ---- presets ----------------------------------------------------------------
// 128 factory presets (8 groups of 16) and 128 user slots. A preset is a
// list of key=value tokens applied on top of WV_INIT; user slots hold a full
// copy of the sound and live in their own files, shared by all projects.

#define WV_NPRESET 128
#define WV_GROUPS  8
static const char *WV_GROUP[WV_GROUPS] = { "PLUCK", "SYNTH", "BASS", "PAD", "WIND", "STRING", "KEYS", "MALLET" };

// A plain, dry, single-voice saw: every preset starts from this.
static const char *WV_INIT =
    "wave=4 m1a=0 m2a=0 m3a=0 m4a=0 pos=0 envpos=0 lfopos=0 lforate=0.1 a=0.003 d=1 s=1 r=0.3 glide=0 phase=0 "
    "level=0.7 oct=0 voices=1 spread=0 entrate=0.15 cdetl=0 cdetc=0 cdetr=0 caggl=0 caggc=0 caggr=0 centl=0 centc=0 centr=0 "
    "cposl=0.5 cposc=0.5 cposr=0.5 clvll=1 clvlc=1 clvlr=1 flt=0 fltmode=0 fltcut=18000 fltres=0 fltenv=0 fltkey=0 "
    "eq=0 eqlow=0 eqmid=0 eqfreq=1000 eqhigh=0 sat=0 dst=0 cmp=0 cho=0 flg=0 phs=0 dly=0 plt=0 "
    "ma=0.001 md=0.5 mcut=0 mpos=0 mpitch=0 trem=0 vibrate=5.5 vibdepth=0 vibdelay=0 noise=0 ntone=4 ndecay=0";

typedef struct { const char *name, *data; } WvPreset;
// WV_FACTORY_BEGIN (generated from the preset list with its reference checks)
static const WvPreset WV_FACTORY[WV_NPRESET] = {
    { "DIGI PLUCK", "wave=4 a=0.001 d=0.4 s=0 r=0.25 flt=1 fltcut=500 fltres=0.25 fltkey=1 mcut=4.5 md=0.22 voices=2 spread=0.5 cdetl=0.08 cdetr=0.08 phase=1 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.573" },
    { "HOUSE PLUCK", "wave=63 a=0.001 d=0.3 s=0 r=0.2 flt=1 fltcut=700 fltres=0.35 fltkey=1 mcut=4 md=0.15 cho=1 chomix=0.3 dly=1 dlytime=0.375 dlyfb=0.25 dlymix=0.15 level=0.715" },
    { "TRANCE PLUCK", "wave=4 voices=7 spread=1 cdetl=0.5 cdetc=0.2 cdetr=0.5 phase=1 a=0.001 d=0.5 s=0 r=0.3 flt=1 fltcut=600 fltkey=1 fltres=0.2 mcut=4.5 md=0.3 plt=1 pltmix=0.2 level=0.555" },
    { "FUTURE PLUCK", "wave=4 m1=8 m1a=0.7 pos=0 mpos=1 md=0.15 a=0.001 d=0.35 s=0 r=0.25 flt=1 fltcut=900 fltkey=1 mcut=3 voices=3 spread=0.6 cdetl=0.15 cdetr=0.15 phase=1 cho=1 chomix=0.25 level=0.635" },
    { "SQUARE PLUCK", "wave=2 a=0.001 d=0.3 s=0 r=0.2 flt=1 fltcut=500 fltkey=1 fltres=0.3 mcut=4 md=0.18 level=0.581" },
    { "CHIP PLUCK", "wave=3 a=0.001 d=0.25 s=0 r=0.1 dly=1 dlytime=0.2 dlyfb=0.2 dlymix=0.12 level=0.661" },
    { "SOFT PLUCK", "wave=1 a=0.002 d=0.6 s=0 r=0.3 flt=1 fltcut=800 fltkey=1 mcut=2 md=0.3 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.652" },
    { "FM PLUCK", "wave=0 m1=17 m1a=0.9 pos=0 mpos=1 md=0.12 a=0.001 d=0.5 s=0 r=0.25 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.615" },
    { "LOFI PLUCK", "wave=9 m1=32 m1a=0.6 m2=33 m2a=0.3 pos=1 a=0.001 d=0.4 s=0 r=0.2 flt=1 fltcut=1500 fltkey=0.5 mcut=2 md=0.2 sat=1 satdrive=0.4 level=1" },
    { "DUB PLUCK", "wave=5 a=0.001 d=0.3 s=0 r=0.2 flt=1 fltcut=600 fltres=0.4 fltkey=1 mcut=3.5 md=0.12 dly=1 dlytime=0.5 dlyfb=0.55 dlyping=1 dlymix=0.35 dlytone=0.3 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.731" },
    { "RESO PLUCK", "wave=4 a=0.001 d=0.4 s=0 r=0.2 flt=1 fltcut=350 fltres=0.75 fltkey=1 mcut=5 md=0.2 level=0.384" },
    { "GLASS PLUCK", "wave=33 a=0.001 d=0.7 s=0 r=0.4 flt=1 fltcut=2500 fltkey=1 mcut=1.5 md=0.3 plt=1 pltmix=0.25 pltdecay=0.7 level=0.709" },
    { "WOOD PLUCK", "wave=17 a=0.001 d=0.15 s=0 r=0.08 noise=0.15 ntone=3 ndecay=0.02 level=0.533" },
    { "OCTAVE PLUCK", "wave=4 m1=15 m1a=0.7 pos=1 a=0.001 d=0.35 s=0 r=0.2 flt=1 fltcut=700 fltkey=1 mcut=3.5 md=0.2 dly=1 dlytime=0.25 dlymix=0.15 level=0.719" },
    { "BUBBLE PLUCK", "wave=0 mpitch=12 md=0.03 a=0.001 d=0.3 s=0 r=0.15 dly=1 dlytime=0.3 dlyping=1 dlyfb=0.35 dlymix=0.25 level=0.639" },
    { "ZAP PLUCK", "wave=4 mpitch=24 md=0.05 a=0.001 d=0.35 s=0 r=0.15 flt=1 fltcut=600 fltkey=1 mcut=3 fltres=0.3 level=0.736" },
    { "ACID 303", "wave=4 a=0.002 d=0.6 s=0.5 r=0.08 glide=0.08 flt=1 fltcut=650 fltres=0.95 fltkey=0.5 mcut=3.5 md=0.18 sat=1 satdrive=0.4 satmix=0.6 level=0.394" },
    { "ACID SQUARE", "wave=2 a=0.002 d=0.6 s=0.5 r=0.08 glide=0.08 flt=1 fltcut=700 fltres=0.93 fltkey=0.5 mcut=3.5 md=0.2 dst=1 dstdrive=0.25 dstmix=0.3 level=0.316" },
    { "SUPERSAW LEAD", "wave=4 voices=9 spread=1 cdetl=0.55 cdetc=0.25 cdetr=0.55 phase=1 a=0.005 r=0.3 flt=1 fltcut=9000 dly=1 dlytime=0.375 dlymix=0.18 plt=1 pltmix=0.2 level=0.411" },
    { "SAW LEAD", "wave=4 voices=2 spread=0.3 cdetl=0.1 cdetr=0.1 glide=0.05 vibdepth=0.15 vibdelay=0.4 vibrate=5.5 flt=1 fltcut=6000 dly=1 dlytime=0.3 dlymix=0.15 level=0.516" },
    { "SQUARE LEAD", "wave=2 glide=0.05 vibdepth=0.12 vibdelay=0.4 flt=1 fltcut=5000 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.53" },
    { "SYNC LEAD", "wave=4 m1=8 m1a=0.8 pos=0.5 lfopos=0.3 lforate=0.3 flt=1 fltcut=8000 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.605" },
    { "CHIP LEAD", "wave=5 vibdepth=0.25 vibrate=7 vibdelay=0.2 a=0.001 r=0.05 level=0.543" },
    { "SINE LEAD", "wave=0 glide=0.06 vibdepth=0.15 vibdelay=0.35 vibrate=5.5 dly=1 dlytime=0.33 dlymix=0.2 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.681" },
    { "FM LEAD", "wave=0 m1=17 m1a=0.6 m2=16 m2a=0.3 pos=1 vibdepth=0.1 vibdelay=0.4 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.537" },
    { "BRASS SYNTH", "wave=4 voices=3 spread=0.6 cdetl=0.2 cdetr=0.2 phase=1 a=0.06 d=0.5 s=0.7 r=0.25 flt=1 fltcut=700 fltkey=0.5 mcut=2.5 ma=0.05 md=0.4 cho=1 chomix=0.25 level=0.725" },
    { "POLY SYNTH", "wave=63 voices=4 spread=0.7 cdetl=0.2 cdetr=0.2 phase=1 a=0.01 d=0.8 s=0.6 r=0.4 flt=1 fltcut=2500 fltres=0.2 fltkey=0.5 mcut=1.5 md=0.5 cho=1 chomix=0.3 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.646" },
    { "HOUSE STAB", "wave=19 a=0.002 d=0.3 s=0 r=0.1 flt=1 fltcut=3500 fltkey=0.5 plt=1 pltmix=0.2 pltdecay=0.5 level=0.663" },
    { "RAVE HOOVER", "wave=4 m1=5 m1a=0.5 pos=1 voices=5 spread=1 cdetl=0.6 cdetc=0.3 cdetr=0.6 phase=1 mpitch=-4 md=0.25 a=0.002 d=0.6 s=0.6 r=0.3 cho=1 chomix=0.5 chodepth=0.8 level=0.697" },
    { "VOCAL SYNTH", "wave=26 m1=56 m1a=0.9 pos=0.5 lfopos=0.9 lforate=0.35 vibdepth=0.1 vibdelay=0.4 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.478" },
    { "PWM LEAD", "wave=2 m1=5 m1a=0.7 pos=0.4 lfopos=0.7 lforate=0.6 flt=1 fltcut=6000 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.55" },
    { "HARD LEAD", "wave=4 dst=1 dsttype=0 dstdrive=0.55 dstmix=0.7 dsttone=0.85 flt=1 fltcut=10000 vibdepth=0.12 vibdelay=0.5 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.846" },
    { "SUB BASS", "wave=0 oct=-2 a=0.005 r=0.12 level=0.558" },
    { "DEEP SUB", "wave=0 m1=24 m1a=0.25 pos=1 oct=-3 a=0.005 r=0.15 level=0.558" },
    { "808 BASS", "wave=0 oct=-2 mpitch=12 md=0.06 a=0.001 d=2.5 s=0 r=0.3 sat=1 satdrive=0.3 level=1" },
    { "REESE BASS", "wave=4 oct=-2 voices=2 spread=0.3 cdetl=0.3 cdetr=0.3 phase=0 flt=1 fltcut=900 fltres=0.1 fltkey=0.3 level=0.493" },
    { "ACID BASS", "wave=4 oct=-1 a=0.002 d=0.6 s=0.5 r=0.06 glide=0.06 flt=1 fltcut=420 fltres=0.93 fltkey=0.5 mcut=3.5 md=0.15 level=0.272" },
    { "SAW BASS", "wave=4 oct=-2 flt=1 fltcut=600 fltkey=0.5 fltres=0.15 a=0.003 r=0.08 level=0.718" },
    { "SQUARE BASS", "wave=2 oct=-2 flt=1 fltcut=800 fltkey=0.5 a=0.003 r=0.08 level=0.634" },
    { "FM BASS", "wave=0 m1=16 m1a=0.7 pos=0 mpos=1 md=0.25 oct=-2 a=0.002 d=0.8 s=0.7 r=0.1 level=0.567" },
    { "PLUCK BASS", "wave=4 oct=-2 a=0.001 d=0.5 s=0.15 r=0.08 flt=1 fltcut=200 fltkey=0.5 mcut=4 md=0.12 level=0.692" },
    { "HOUSE BASS", "wave=16 oct=-2 a=0.002 d=0.35 s=0.4 r=0.06 flt=1 fltcut=1200 fltkey=0.5 level=0.604" },
    { "WOBBLE BASS", "wave=4 m1=40 m1a=0.9 pos=0.5 lfopos=1 lforate=2.67 oct=-2 a=0.003 r=0.1 dst=1 dstdrive=0.2 dstmix=0.3 level=0.606" },
    { "FINGER BASS", "wave=14 m1=44 m1a=0.55 pos=1 oct=-2 a=0.003 d=2 s=0 r=0.12 flt=1 fltcut=300 fltkey=0.6 mcut=2 md=0.3 noise=0.08 ntone=6 ndecay=0.02 level=0.61" },
    { "PICKED BASS", "wave=4 m1=44 m1a=0.6 pos=1 oct=-2 a=0.002 d=2.5 s=0 r=0.1 flt=1 fltcut=450 fltkey=0.6 mcut=3 md=0.2 noise=0.15 ntone=8 ndecay=0.015 level=0.633" },
    { "UPRIGHT BASS", "wave=14 oct=-2 a=0.003 d=1.2 s=0 r=0.15 flt=1 fltcut=250 fltkey=0.6 mcut=1.5 md=0.1 noise=0.1 ntone=2 ndecay=0.05 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.681" },
    { "MOOG BASS", "wave=4 voices=2 spread=0 cdetl=0.05 cdetr=0.05 oct=-2 a=0.003 d=1 s=0.8 r=0.1 flt=1 fltcut=250 fltres=0.5 fltkey=0.5 mcut=3 md=0.3 sat=1 satdrive=0.3 level=1" },
    { "GROWL BASS", "wave=4 m1=20 m1a=0.8 m2=26 m2a=0.5 pos=0.5 lfopos=0.95 lforate=4 oct=-2 dst=1 dstdrive=0.3 dstmix=0.4 level=0.68" },
    { "AMBIENT DRONE", "wave=25 m1=43 m1a=0.7 m2=9 m2a=0.45 pos=0.3 envpos=0.15 lfopos=0.25 lforate=0.07 voices=8 spread=0.85 cdetl=0.55 cdetc=0.2 cdetr=0.55 centl=0.35 centc=0.25 centr=0.35 caggl=0.15 caggr=0.15 cposl=0.38 cposr=0.62 clvll=0.8 clvlr=0.8 phase=1 a=1.2 d=2.5 s=0.75 r=5 flt=1 fltcut=5000 fltenv=0.2 fltres=0.15 cho=1 chomix=0.35 dly=1 dlytime=0.42 dlyfb=0.45 dlyping=0.6 dlytone=0.45 dlymix=0.25 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=1" },
    { "WARM PAD", "wave=9 voices=6 spread=0.8 cdetl=0.3 cdetc=0.1 cdetr=0.3 phase=1 a=1.2 r=3 flt=1 fltcut=1200 fltkey=0.3 cho=1 chomix=0.3 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.29" },
    { "GLASS PAD", "wave=33 voices=4 spread=1 cdetl=0.1 cdetr=0.1 phase=1 a=1 r=4 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.461" },
    { "CHOIR PAD", "wave=31 m1=56 m1a=0.6 pos=0.4 lfopos=0.3 lforate=0.08 voices=8 spread=1 cdetl=0.2 cdetc=0.1 cdetr=0.2 phase=1 vibdepth=0.08 vibrate=5 a=1.5 r=4 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.32" },
    { "STRING PAD", "wave=25 voices=8 spread=1 cdetl=0.25 cdetc=0.1 cdetr=0.25 centc=0.2 phase=1 vibdepth=0.08 vibrate=5.5 a=1 r=3 flt=1 fltcut=4000 cho=1 chomix=0.3 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.321" },
    { "DARK PAD", "wave=9 oct=-1 voices=4 spread=0.9 cdetl=0.15 cdetr=0.15 phase=1 a=2 r=5 flt=1 fltcut=450 fltkey=0.3 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.401" },
    { "SHIMMER PAD", "wave=16 m1=15 m1a=0.6 pos=1 oct=1 voices=6 spread=1 cdetl=0.1 cdetr=0.1 phase=1 a=2 r=6 dly=1 dlyping=1 dlyfb=0.6 dlymix=0.35 dlytime=0.45 plt=1 pltmix=0.5 pltdecay=0.9 level=0.358" },
    { "EVOLVING PAD", "wave=46 m1=53 m1a=0.6 m2=17 m2a=0.5 pos=0.5 lfopos=1 lforate=0.3 voices=6 spread=1 cdetl=0.2 cdetr=0.2 cposl=0.3 cposr=0.7 phase=1 a=1.5 r=4 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.362" },
    { "VOWEL PAD", "wave=26 m1=56 m1a=1 pos=0.5 lfopos=1 lforate=0.25 voices=6 spread=1 cdetl=0.15 cdetr=0.15 phase=1 a=1.2 r=4 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.362" },
    { "WIND PAD", "wave=0 noise=0.9 ntone=3 a=2 r=4 voices=2 spread=1 cho=1 chomix=0.5 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.149" },
    { "ANALOG PAD", "wave=4 voices=4 spread=0.9 cdetl=0.3 cdetr=0.3 phase=1 a=0.8 r=3 flt=1 fltcut=1500 fltres=0.2 fltkey=0.3 cho=1 chomix=0.5 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.458" },
    { "PWM PAD", "wave=2 m1=5 m1a=0.6 pos=0.4 lfopos=0.7 lforate=0.2 voices=4 spread=0.9 cdetl=0.15 cdetr=0.15 phase=1 a=1 r=3 flt=1 fltcut=3000 cho=1 chomix=0.4 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.432" },
    { "FM PAD", "wave=0 m1=17 m1a=0.5 pos=0.3 envpos=0.4 voices=4 spread=1 cdetl=0.15 cdetr=0.15 phase=1 a=1.5 r=3 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.242" },
    { "SPACE PAD", "wave=9 m1=40 m1a=0.5 pos=1 voices=6 spread=1 cdetl=0.2 cdetr=0.2 phase=1 a=1.5 r=5 cho=1 chomix=0.3 dly=1 dlyping=1 dlyfb=0.55 dlymix=0.4 dlytime=0.6 plt=1 pltmix=0.5 pltdecay=0.9 level=0.37" },
    { "CRYSTAL PAD", "wave=32 oct=1 voices=6 spread=1 cdetl=0.1 cdetr=0.1 phase=1 a=0.8 r=5 trem=0.2 vibrate=3 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.376" },
    { "SWELL PAD", "wave=9 voices=4 spread=0.8 cdetl=0.2 cdetr=0.2 phase=1 a=3 s=0.8 d=2 r=4 flt=1 fltcut=400 fltenv=0.7 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.306" },
    { "FLUTE", "wave=21 oct=1 a=0.08 d=0.5 s=0.85 r=0.15 noise=0.12 ntone=4 vibdepth=0.12 vibrate=5 vibdelay=0.3 trem=0.1 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.454" },
    { "PICCOLO", "wave=21 oct=2 a=0.06 d=0.5 s=0.85 r=0.12 noise=0.06 ntone=3 vibdepth=0.1 vibrate=5.5 vibdelay=0.3 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.444" },
    { "RECORDER", "wave=21 m1=40 m1a=0.4 pos=1 oct=1 a=0.04 r=0.08 noise=0.08 ntone=3 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.428" },
    { "PAN FLUTE", "wave=0 oct=1 a=0.06 r=0.2 noise=0.35 ntone=1.5 vibdepth=0.08 vibrate=5 vibdelay=0.3 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.234" },
    { "OCARINA", "wave=0 m1=15 m1a=0.1 pos=1 oct=1 a=0.05 r=0.1 noise=0.05 ntone=2 vibdepth=0.1 vibrate=5 vibdelay=0.4 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.461" },
    { "SHAKUHACHI", "wave=21 a=0.15 r=0.25 noise=0.3 ntone=2 vibdepth=0.3 vibrate=4.5 vibdelay=0.5 mpitch=-0.5 md=0.3 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.207" },
    { "CLARINET", "wave=22 a=0.04 s=0.9 d=0.5 r=0.1 flt=1 fltcut=3500 fltkey=0.5 noise=0.03 ntone=6 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.661" },
    { "BASS CLARINET", "wave=22 oct=-1 a=0.05 s=0.9 d=0.5 r=0.12 flt=1 fltcut=1500 fltkey=0.5 noise=0.03 ntone=6 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.574" },
    { "OBOE", "wave=23 a=0.04 s=0.9 r=0.1 eq=1 eqmid=6 eqfreq=1200 vibdepth=0.1 vibrate=5.5 vibdelay=0.3 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.359" },
    { "BASSOON", "wave=4 m1=43 m1a=0.25 pos=1 oct=-1 a=0.05 s=0.9 r=0.12 flt=1 fltcut=2500 fltkey=0.5 vibdepth=0.06 vibdelay=0.4 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.627" },
    { "ALTO SAX", "wave=4 m1=43 m1a=0.4 pos=1 a=0.04 s=0.9 r=0.1 sat=1 satdrive=0.25 satmix=0.5 vibdepth=0.2 vibrate=5 vibdelay=0.4 noise=0.05 ntone=8 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.564" },
    { "TENOR SAX", "wave=4 m1=43 m1a=0.45 pos=1 oct=-1 a=0.05 s=0.9 r=0.12 sat=1 satdrive=0.3 satmix=0.5 vibdepth=0.2 vibrate=5 vibdelay=0.4 noise=0.06 ntone=8 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.567" },
    { "TRUMPET", "wave=24 m1=43 m1a=0.15 pos=1 a=0.05 d=0.3 s=0.8 r=0.1 flt=1 fltcut=2500 fltkey=0.6 fltenv=0.6 mpitch=-0.6 md=0.05 vibdepth=0.08 vibdelay=0.5 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.617" },
    { "TROMBONE", "wave=24 oct=-1 a=0.05 d=0.3 s=0.8 r=0.12 flt=1 fltcut=1000 fltkey=0.6 fltenv=0.5 mpitch=-0.4 md=0.06 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.636" },
    { "FRENCH HORN", "wave=24 m1=40 m1a=0.82 pos=1 oct=-1 a=0.08 d=0.4 s=0.85 r=0.2 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.545" },
    { "TUBA", "wave=24 m1=40 m1a=0.78 pos=1 oct=-2 a=0.06 d=0.4 s=0.85 r=0.15 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.585" },
    { "VIOLIN", "wave=25 a=0.08 d=0.4 s=0.9 r=0.25 eq=1 eqmid=6 eqfreq=2800 eqlow=-3 vibdepth=0.25 vibrate=6 vibdelay=0.3 noise=0.06 ntone=10 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.272" },
    { "VIOLA", "wave=25 a=0.09 d=0.4 s=0.9 r=0.25 eq=1 eqmid=4 eqfreq=1800 vibdepth=0.22 vibrate=5.5 vibdelay=0.3 noise=0.05 ntone=8 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.334" },
    { "CELLO", "wave=25 oct=-1 a=0.1 d=0.4 s=0.9 r=0.3 eq=1 eqmid=4 eqfreq=1500 eqlow=2 vibdepth=0.2 vibrate=5 vibdelay=0.3 noise=0.05 ntone=8 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.363" },
    { "CONTRABASS", "wave=25 oct=-2 a=0.12 d=0.4 s=0.9 r=0.3 flt=1 fltcut=1500 fltkey=0.5 vibdepth=0.15 vibrate=4.8 vibdelay=0.4 noise=0.04 ntone=6 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.538" },
    { "STRING SECTION", "wave=25 voices=8 spread=1 cdetl=0.25 cdetc=0.15 cdetr=0.25 centl=0.3 centc=0.3 centr=0.3 phase=1 a=0.3 d=0.5 s=0.9 r=0.6 vibdepth=0.1 vibrate=5.5 eq=1 eqmid=4 eqfreq=2500 cho=1 chomix=0.25 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.264" },
    { "PIZZICATO", "wave=14 m1=44 m1a=0.5 pos=1 a=0.002 d=0.5 s=0 r=0.2 flt=1 fltcut=800 fltkey=1 mcut=2 md=0.15 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.63" },
    { "NYLON GUITAR", "wave=14 m1=44 m1a=0.5 pos=1 oct=-1 a=0.002 d=3 s=0 r=0.3 flt=1 fltcut=900 fltkey=1 mcut=2 md=0.5 noise=0.06 ntone=5 ndecay=0.015 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.625" },
    { "STEEL GUITAR", "wave=14 m1=44 m1a=0.83 m2=47 m2a=0.3 pos=1 oct=-1 a=0.002 d=4 s=0 r=0.3 flt=1 fltcut=2000 fltkey=1 mcut=2 md=0.6 noise=0.08 ntone=8 ndecay=0.012 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.618" },
    { "ELECTRIC CLEAN", "wave=14 m1=44 m1a=0.37 m2=47 m2a=0.2 pos=1 oct=-1 a=0.002 d=6 s=0 r=0.2 flt=1 fltcut=1500 fltkey=0.8 mcut=1 md=0.8 cho=1 chomix=0.3 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.641" },
    { "ELECTRIC CRUNCH", "wave=4 m1=44 m1a=0.4 pos=1 oct=-1 a=0.002 d=4 s=0.4 r=0.15 dst=1 dsttype=0 dstdrive=0.45 dstmix=1 dsttone=0.5 flt=1 fltcut=3000 fltkey=0.5 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=1" },
    { "DIST GUITAR", "wave=4 voices=2 spread=0.6 cdetl=0.05 cdetr=0.05 oct=-1 a=0.002 d=3 s=0.9 r=0.15 dst=1 dsttype=0 dstdrive=0.75 dstmix=1 dsttone=0.65 sat=1 satdrive=0.5 eq=1 eqmid=-4 eqfreq=800 vibdepth=0.1 vibdelay=0.6 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.928" },
    { "UKULELE", "wave=14 m1=44 m1a=0.5 pos=1 a=0.002 d=1.0 s=0 r=0.15 flt=1 fltcut=1500 fltkey=1 mcut=1.5 md=0.2 noise=0.08 ntone=6 ndecay=0.012 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.615" },
    { "BANJO", "wave=4 m1=44 m1a=1 pos=1 a=0.001 d=0.5 s=0 r=0.1 flt=1 fltcut=2500 fltkey=1 mcut=2 md=0.08 noise=0.12 ntone=10 ndecay=0.01 level=0.644" },
    { "MANDOLIN", "wave=14 m1=44 m1a=0.5 pos=1 oct=1 voices=2 spread=0.3 cdetl=0.05 cdetr=0.05 a=0.001 d=0.7 s=0 r=0.15 flt=1 fltcut=2500 fltkey=1 mcut=2 md=0.1 noise=0.08 ntone=8 ndecay=0.01 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.452" },
    { "HARP", "wave=14 m1=62 m1a=0.4 pos=1 a=0.002 d=4 s=0 r=1.5 flt=1 fltcut=1200 fltkey=1 mcut=1 md=0.3 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.468" },
    { "SITAR", "wave=4 m1=26 m1a=0.4 m2=43 m2a=0.5 pos=1 a=0.002 d=3 s=0 r=0.4 flt=1 fltcut=3000 fltkey=1 mcut=1 md=0.6 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.793" },
    { "PIANO", "wave=14 m1=44 m1a=0.9 m2=47 m2a=0.25 pos=1 a=0.002 d=4 s=0 r=0.35 flt=1 fltcut=1500 fltkey=1 mcut=2 md=0.8 noise=0.05 ntone=6 ndecay=0.01 voices=2 spread=0.3 cdetl=0.02 cdetr=0.02 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.448" },
    { "ELECTRIC PIANO", "wave=0 m1=19 m1a=0.35 pos=0 mpos=1 md=0.25 a=0.002 d=3 s=0 r=0.4 trem=0.25 vibrate=4 cho=1 chomix=0.2 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.671" },
    { "WURLITZER", "wave=11 m1=24 m1a=0.3 m2=47 m2a=0.4 pos=0.4 mpos=0.6 md=0.3 a=0.002 d=2.5 s=0 r=0.3 trem=0.3 vibrate=5.5 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.617" },
    { "CLAVINET", "wave=5 m1=44 m1a=0.7 pos=1 a=0.001 d=0.8 s=0 r=0.08 flt=1 fltcut=2500 fltkey=1 fltres=0.3 mcut=2 md=0.15 level=0.575" },
    { "HARPSICHORD", "wave=4 m1=44 m1a=1 pos=1 a=0.001 d=2.5 s=0 r=0.15 flt=1 fltcut=4000 fltkey=1 mcut=1 md=0.3 noise=0.1 ntone=8 ndecay=0.01 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.695" },
    { "DRAWBAR ORGAN", "wave=19 a=0.005 r=0.05 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.541" },
    { "JAZZ ORGAN", "wave=18 m1=47 m1a=0.6 pos=0 mpos=1 md=0.3 a=0.004 r=0.05 vibdepth=0.05 vibrate=6.7 trem=0.15 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.532" },
    { "CHURCH ORGAN", "wave=19 m1=15 m1a=0.5 pos=1 a=0.08 r=0.4 plt=1 pltmix=0.45 pltdecay=0.9 pltdamp=0.3 level=0.371" },
    { "REED ORGAN", "wave=35 a=0.1 r=0.2 noise=0.03 ntone=6 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.511" },
    { "ACCORDION", "wave=35 voices=3 spread=1 cdetl=0.35 cdetr=0.35 phase=0 a=0.05 r=0.1 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.418" },
    { "FM PIANO", "wave=0 m1=16 m1a=0.6 m2=19 m2a=0.25 pos=0 mpos=1 md=0.4 a=0.002 d=2.5 s=0 r=0.3 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.617" },
    { "TOY PIANO", "wave=32 oct=1 a=0.001 d=0.6 s=0 r=0.15 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.625" },
    { "HONKY TONK", "wave=14 m1=44 m1a=0.9 m2=47 m2a=0.25 pos=1 a=0.002 d=4 s=0 r=0.35 flt=1 fltcut=1500 fltkey=1 mcut=2 md=0.8 noise=0.05 ntone=6 ndecay=0.01 voices=2 spread=1 cdetl=0.2 cdetr=0.2 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.614" },
    { "LOFI KEYS", "wave=0 m1=19 m1a=0.3 m2=32 m2a=0.5 pos=0.5 mpos=0.5 md=0.25 a=0.002 d=2.5 s=0 r=0.3 trem=0.2 vibrate=3 vibdepth=0.08 flt=1 fltcut=2000 sat=1 satdrive=0.3 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=1" },
    { "FELT PIANO", "wave=14 m1=62 m1a=0.5 pos=1 a=0.004 d=3 s=0 r=0.4 flt=1 fltcut=700 fltkey=1 mcut=1 md=0.4 noise=0.04 ntone=2 ndecay=0.02 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.529" },
    { "COMBO ORGAN", "wave=2 m1=15 m1a=0.5 pos=1 a=0.004 r=0.05 vibdepth=0.08 vibrate=6.5 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.473" },
    { "MARIMBA", "wave=64 us=1 um=1 h=ff00005900000000000d00000000000000000000000000000000000000000000 m1=40 m1a=0.85 pos=1 mpos=-1 md=0.15 a=0.001 d=0.8 s=0 r=0.3 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.615" },
    { "XYLOPHONE", "wave=64 us=1 um=1 h=ff007300001a0000000000000000000000000000000000000000000000000000 m1=40 m1a=0.85 pos=1 mpos=-1 md=0.08 oct=1 a=0.001 d=0.35 s=0 r=0.15 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.616" },
    { "VIBRAPHONE", "wave=64 us=1 um=1 h=ff00004000000000000000000000000000000000000000000000000000000000 m1=40 m1a=0.85 pos=1 mpos=-1 md=0.4 a=0.001 d=3 s=0 r=0.8 trem=0.45 vibrate=5 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.596" },
    { "GLOCKENSPIEL", "wave=64 us=1 um=1 h=ff004c002e000000000000000000000000000000000000000000000000000000 m1=40 m1a=0.85 pos=1 mpos=-1 md=0.3 oct=2 a=0.001 d=3 s=0 r=1 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.612" },
    { "CELESTA", "wave=64 us=1 um=1 h=ff1f001400000000000000000000000000000000000000000000000000000000 oct=1 a=0.001 d=1.6 s=0 r=0.5 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.678" },
    { "MUSIC BOX", "wave=64 us=1 um=1 h=ff401f001a000000000000000000000000000000000000000000000000000000 m1=40 m1a=0.85 pos=1 mpos=-1 md=0.2 oct=2 a=0.001 d=1.8 s=0 r=0.6 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.678" },
    { "KALIMBA", "wave=64 us=1 um=1 h=ff0000001f000000000000000000000000000000000000000000000000000000 m1=40 m1a=0.85 pos=1 mpos=-1 md=0.05 a=0.001 d=1.5 s=0 r=0.4 noise=0.2 ntone=1 ndecay=0.02 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.618" },
    { "STEEL DRUM", "wave=64 us=1 um=1 h=ff99592600000000000000000000000000000000000000000000000000000000 m1=40 m1a=0.85 pos=1 mpos=-1 md=0.3 a=0.001 d=1.2 s=0 r=0.3 plt=1 pltmix=0.18 pltdecay=0.5 pltdamp=0.5 level=0.616" },
    { "TUBULAR BELL", "wave=64 us=1 um=1 h=b2ff800059003300000000000000000000000000000000000000000000000000 m1=40 m1a=0.85 pos=1 mpos=-1 md=1 oct=1 a=0.001 d=6 s=0 r=4 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.551" },
    { "HANDBELL", "wave=64 us=1 um=1 h=ff0080001a000000000000000000000000000000000000000000000000000000 m1=40 m1a=0.85 pos=1 mpos=-1 md=0.8 oct=1 a=0.001 d=4 s=0 r=3 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.519" },
    { "CHURCH BELL", "wave=64 us=1 um=1 h=80ff66bf40330000000000000000000000000000000000000000000000000000 m1=40 m1a=0.85 pos=1 mpos=-1 md=1.5 oct=-1 a=0.001 d=8 s=0 r=6 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.532" },
    { "GAMELAN", "wave=64 us=1 um=1 h=ff4c800000000000000000000000000000000000000000000000000000000000 voices=2 spread=1 cdetl=0.4 cdetr=0.4 m1=40 m1a=0.85 pos=1 mpos=-1 md=0.5 a=0.001 d=3 s=0 r=1.5 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.664" },
    { "SINGING BOWL", "wave=64 us=1 um=1 h=ff00400000000000000000000000000000000000000000000000000000000000 voices=2 spread=0.6 cdetl=0.03 cdetr=0.03 a=0.005 d=8 s=0 r=6 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.293" },
    { "FM BELL", "wave=0 m1=19 m1a=0.6 m2=17 m2a=0.3 pos=0.2 mpos=0.8 md=1.5 a=0.001 d=5 s=0 r=2 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.353" },
    { "CRYSTAL BELL", "wave=33 oct=2 a=0.001 d=3 s=0 r=1.5 plt=1 pltmix=0.35 pltdecay=0.85 pltdamp=0.4 pltpre=0.03 level=0.788" },
    { "WOOD BLOCK", "wave=64 us=1 um=1 h=ff004c0000000000000000000000000000000000000000000000000000000000 oct=1 a=0.001 d=0.12 s=0 r=0.05 noise=0.3 ntone=2 ndecay=0.015 level=0.465" },
};
// WV_FACTORY_END

typedef struct { bool used; char name[33]; char *data; } WvUserPreset;
static WvUserPreset wv_users[WV_NPRESET];
static char *wv_strdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); if (d) memcpy(d, s, n); return d; }
static bool wv_usersLoaded;

static char *read_file(const char *path);
static bool  write_file(const char *path, const char *text);
static void  fs_sync(void);
static bool  fs_ready(void);

#if defined(PLATFORM_WEB)
#define WV_USER_DIR "/raya/wavor"
#else
#define WV_USER_DIR "raya-data/wavor"
#endif

static const char *wv_user_path(int slot) { return TextFormat(WV_USER_DIR "/u%03d.txt", slot + 1); }

static void wv_users_scan(void)
{
    if (wv_usersLoaded || !fs_ready()) return;
    wv_usersLoaded = true;
    MakeDirectory(WV_USER_DIR);
    for (int i = 0; i < WV_NPRESET; i++) {
        char *t = read_file(wv_user_path(i));
        if (!t) continue;
        char *nl = strchr(t, '\n');
        if (nl) {
            *nl = 0;
            wv_users[i].used = true;
            snprintf(wv_users[i].name, sizeof wv_users[i].name, "%s", t);
            wv_users[i].data = wv_strdup(nl + 1);
        }
        free(t);
    }
}

static unsigned wv_sound_sig(const WavorSt *s)   // what the sound is made of, to show unsaved edits
{
    unsigned h = 2166136261u;
    const unsigned char *b = (const unsigned char *)s->q;
    for (size_t i = 0; i < sizeof s->q; i++) h = (h ^ b[i]) * 16777619u;
    if ((int)s->q[WQ_WAVE] == WV_USER) {
        b = (const unsigned char *)s->user;
        for (size_t i = 0; i < sizeof s->user; i++) h = (h ^ b[i]) * 16777619u;
        b = (const unsigned char *)s->part;
        for (size_t i = 0; i < sizeof s->part; i++) h = (h ^ b[i]) * 16777619u;
        h = (h ^ (unsigned)s->userMode) * 16777619u;
    }
    return h;
}

static void wv_reset(WavorSt *s)
{
    for (int i = 0; i < WQ_N; i++) s->q[i] = WVP[i].k.def;
    for (int j = 0; j < WV_UPTS; j++) s->user[j] = sinf(TAU * j / WV_UPTS);
    memset(s->part, 0, sizeof s->part);
    s->part[0] = 1;
    s->userMode = 0; s->userSet = false; s->drawMode = false;
    wv_parse(s, WV_INIT);
}

// idx: 0..127 factory, 128..255 user slot, -1 the init sound
static void wv_load_preset(Module *m, int idx)
{
    WavorSt *s = m->st;
    if (idx >= WV_NPRESET && (idx >= 2 * WV_NPRESET || !wv_users[idx - WV_NPRESET].used)) return;
    wv_reset(s);
    if (idx >= 0 && idx < WV_NPRESET) { wv_parse(s, WV_FACTORY[idx].data); snprintf(s->pname, sizeof s->pname, "%s", WV_FACTORY[idx].name); }
    else if (idx >= WV_NPRESET) { wv_parse(s, wv_users[idx - WV_NPRESET].data); snprintf(s->pname, sizeof s->pname, "%s", wv_users[idx - WV_NPRESET].name); }
    else snprintf(s->pname, sizeof s->pname, "INIT");
    s->pidx = idx;
    s->psig = wv_sound_sig(s);
    wv_work(m, 1e9);
}

static void wv_user_store(Module *m, int slot, const char *name)
{
    WavorSt *s = m->st;
    static char data[4096];
    data[0] = 0;
    snprintf(s->pname, sizeof s->pname, "%s", name);
    s->pidx = WV_NPRESET + slot;
    wv_save(m, data, sizeof data);
    WvUserPreset *u = &wv_users[slot];
    free(u->data);
    u->used = true;
    u->data = wv_strdup(data);
    snprintf(u->name, sizeof u->name, "%s", name);
    MakeDirectory(WV_USER_DIR);
    if (write_file(wv_user_path(slot), TextFormat("%s\n%s\n", name, data))) fs_sync();
    s->psig = wv_sound_sig(s);
    toast(TextFormat("WAVOR: SAVED U%03d %s", slot + 1, name));
}

static void wv_user_delete(int slot)
{
    WvUserPreset *u = &wv_users[slot];
    free(u->data);
    memset(u, 0, sizeof *u);
    remove(wv_user_path(slot));
    fs_sync();
}

// Typing a name for a user slot: Enter saves, Escape cancels.
static void wv_naming(Module *m)
{
    WavorSt *s = m->st;
    int len = (int)strlen(s->nameBuf), ch;
    while ((ch = GetCharPressed()) > 0)
        if (ch >= 32 && ch < 127 && ch != '=' && len < 24) { s->nameBuf[len++] = (char)toupper(ch); s->nameBuf[len] = 0; }
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && len) s->nameBuf[--len] = 0;
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        wv_user_store(m, s->naming, s->nameBuf[0] ? s->nameBuf : "UNTITLED");
        s->naming = -1; g_kbd_owner = -1;
    } else if (IsKeyPressed(KEY_ESCAPE)) { s->naming = -1; g_kbd_owner = -1; }
}

static void wv_frame(Module *m)   // every video frame
{
    WavorSt *s = m->st;
    wv_users_scan();
    if (s->naming >= 0 && g_kbd_owner == (int)(m - mods)) wv_naming(m);
    wv_work(m, 0.004);
}

// ---- preset bar and browser -------------------------------------------------

static Rectangle wv_body(Rectangle area) { return (Rectangle){ area.x, area.y + WV_BAR, area.width, area.height - WV_BAR }; }
static Rectangle wv_bar_btn(Rectangle area, int i)   // 0 prev, 1 next, 2 name, 3 save, 4 init
{
    static const float X[5] = { 0, 30, 62, 588, 674 }, W[5] = { 26, 26, 520, 80, 70 };
    return (Rectangle){ area.x + X[i], area.y + 2, W[i], 24 };
}
static Rectangle wv_br_tab(Rectangle a, int i) { return (Rectangle){ a.x + 8 + i * 104, a.y + 6, 100, 20 }; }
static Rectangle wv_br_close(Rectangle a)      { return (Rectangle){ a.x + a.width - 34, a.y + 6, 26, 20 }; }
static Rectangle wv_br_cell(Rectangle a, int i)
{
    float cw = (a.width - 16) / WV_GROUPS, y0 = a.y + 50, ch = (a.height - 58) / 16;
    return (Rectangle){ a.x + 8 + (i / 16) * cw, y0 + (i % 16) * ch, cw - 4, ch - 3 };
}

static void wv_draw_bar(WavorSt *s, Rectangle area, Vector2 mp)
{
    bool naming = s->naming >= 0;
    wv_button(wv_bar_btn(area, 0), "<", mp, false);
    wv_button(wv_bar_btn(area, 1), ">", mp, false);
    Rectangle nb = wv_bar_btn(area, 2);
    bool h = CheckCollisionPointRec(mp, nb);
    DrawRectangleRec(nb, INSET);
    DrawRectangleLinesEx(nb, 1, naming ? ACCENT : h || s->browse ? ACCENT_DIM : PANEL_HI);
    if (naming) {
        const char *t = TextFormat("NAME FOR U%03d:  %s", s->naming + 1, s->nameBuf);
        txt(t, nb.x + 10, nb.y + 7, 10, ACCENT);
        if (fmodf((float)GetTime(), 1.0f) < 0.6f) DrawRectangle((int)(nb.x + 12 + txt_w(t, 10)), (int)nb.y + 5, 2, 14, ACCENT);
        txt("ENTER SAVES, ESC CANCELS", nb.x + nb.width - 10 - txt_w("ENTER SAVES, ESC CANCELS", 10), nb.y + 7, 10, TEXT_DIM);
    } else {
        const char *id = s->pidx < 0 ? "----" : s->pidx < WV_NPRESET ? TextFormat("F%03d", s->pidx + 1) : TextFormat("U%03d", s->pidx - WV_NPRESET + 1);
        const char *grp = s->pidx < 0 ? "" : s->pidx < WV_NPRESET ? WV_GROUP[s->pidx / 16] : "USER";
        bool mod = wv_sound_sig(s) != s->psig;
        txt(id, nb.x + 10, nb.y + 7, 10, TEXT_DIM);
        txt(grp, nb.x + 52, nb.y + 7, 10, TEXT_DIM);
        txt(TextFormat("%s%s", s->pname[0] ? s->pname : "DEFAULT", mod ? "  *" : ""), nb.x + 120, nb.y + 7, 10, h ? ACCENT : TEXT);
        txt("BROWSE", nb.x + nb.width - 10 - txt_w("BROWSE", 10), nb.y + 7, 10, h ? ACCENT : TEXT_DIM);
    }
    wv_button(wv_bar_btn(area, 3), "SAVE", mp, s->browse && s->saveMode);
    wv_button(wv_bar_btn(area, 4), "INIT", mp, false);
    int nu = 0;
    for (int i = 0; i < WV_NPRESET; i++) nu += wv_users[i].used;
    const char *info = TextFormat("%d FACTORY  -  %d / %d USER", WV_NPRESET, nu, WV_NPRESET);
    txt(info, area.x + area.width - txt_w(info, 10), area.y + 9, 10, TEXT_DIM);
}

static void wv_draw_browser(WavorSt *s, Rectangle a, Vector2 mp)
{
    DrawRectangleRec(a, (Color){ 26, 28, 30, 255 });
    DrawRectangleLinesEx(a, 1, ACCENT_DIM);
    wv_button(wv_br_tab(a, 0), "FACTORY", mp, s->browseTab == 0);
    wv_button(wv_br_tab(a, 1), "USER", mp, s->browseTab == 1);
    wv_button(wv_br_close(a), "X", mp, false);
    const char *hint = s->browseTab == 0 ? "CLICK A PRESET TO LOAD IT" :
                       s->saveMode ? "CLICK A SLOT TO SAVE THE CURRENT SOUND THERE, THEN TYPE A NAME" :
                       "CLICK TO LOAD  -  RIGHT-CLICK TWICE TO DELETE";
    txt(hint, a.x + 224, a.y + 11, 10, s->saveMode && s->browseTab == 1 ? ACCENT : TEXT_DIM);
    for (int g = 0; g < WV_GROUPS; g++) {
        Rectangle c = wv_br_cell(a, g * 16);
        txt(s->browseTab == 0 ? WV_GROUP[g] : TextFormat("U%03d - U%03d", g * 16 + 1, g * 16 + 16), c.x + 4, a.y + 34, 10, ACCENT);
    }
    for (int i = 0; i < WV_NPRESET; i++) {
        Rectangle c = wv_br_cell(a, i);
        int idx = s->browseTab == 0 ? i : WV_NPRESET + i;
        bool used = s->browseTab == 0 || wv_users[i].used, h = CheckCollisionPointRec(mp, c), on = idx == s->pidx;
        bool armed = s->browseTab == 1 && s->armDel == i;
        DrawRectangleRec(c, armed ? fade(CABLE_COLORS[1], 0.3f) : on ? fade(ACCENT, 0.18f) : h ? PANEL_HI : (Color){ 36, 38, 40, 255 });
        if (on || h) DrawRectangleLinesEx(c, 1, on ? ACCENT : ACCENT_DIM);
        const char *name = s->browseTab == 0 ? WV_FACTORY[i].name : used ? wv_users[i].name : "EMPTY";
        if (s->browseTab == 1) txt(TextFormat("U%03d", i + 1), c.x + 5, c.y + c.height / 2 - 5, 10, fade(TEXT_DIM, 0.7f));
        txt(name, c.x + (s->browseTab == 1 ? 40 : 6), c.y + c.height / 2 - 5, 10, armed ? CABLE_COLORS[1] : on || h ? ACCENT : used ? TEXT : fade(TEXT_DIM, 0.5f));
    }
}

static void wv_step_preset(Module *m, int d)
{
    WavorSt *s = m->st;
    if (s->pidx >= WV_NPRESET) {   // step through the used user slots
        for (int k = 1; k <= WV_NPRESET; k++) {
            int i = ((s->pidx - WV_NPRESET) + d * k + 2 * WV_NPRESET) % WV_NPRESET;
            if (wv_users[i].used) { wv_load_preset(m, WV_NPRESET + i); return; }
        }
        return;
    }
    wv_load_preset(m, s->pidx < 0 ? (d > 0 ? 0 : WV_NPRESET - 1) : (s->pidx + d + WV_NPRESET) % WV_NPRESET);
}

// ---- entry points: the preset bar on top, the panes below --------------------

static void wv_draw(Module *m, Rectangle area)
{
    WavorSt *s = m->st;
    Vector2 mp = GetMousePosition();
    wv_draw_bar(s, area, mp);
    if (s->browse) wv_draw_browser(s, wv_body(area), mp);
    else wv_draw_body(m, wv_body(area));
}

static bool wv_press(Module *m, Vector2 mp, Rectangle area)
{
    WavorSt *s = m->st;
    Rectangle a = wv_body(area);
    if (s->naming >= 0) { s->naming = -1; g_kbd_owner = -1; }   // a click elsewhere cancels naming
    if (CheckCollisionPointRec(mp, wv_bar_btn(area, 0))) { wv_step_preset(m, -1); return true; }
    if (CheckCollisionPointRec(mp, wv_bar_btn(area, 1))) { wv_step_preset(m, 1); return true; }
    if (CheckCollisionPointRec(mp, wv_bar_btn(area, 2))) {
        s->browse = !s->browse; s->saveMode = false; s->pick = -1;
        s->browseTab = s->pidx >= WV_NPRESET ? 1 : 0;
        return true;
    }
    if (CheckCollisionPointRec(mp, wv_bar_btn(area, 3))) { s->browse = true; s->saveMode = true; s->browseTab = 1; s->pick = -1; return true; }
    if (CheckCollisionPointRec(mp, wv_bar_btn(area, 4))) { wv_load_preset(m, -1); s->browse = false; return true; }
    if (s->browse) {
        if (CheckCollisionPointRec(mp, wv_br_close(a))) { s->browse = false; return true; }
        for (int t = 0; t < 2; t++) if (CheckCollisionPointRec(mp, wv_br_tab(a, t))) { s->browseTab = t; s->armDel = -1; if (!t) s->saveMode = false; return true; }
        for (int i = 0; i < WV_NPRESET; i++)
            if (CheckCollisionPointRec(mp, wv_br_cell(a, i))) {
                s->armDel = -1;
                if (s->browseTab == 0) { wv_load_preset(m, i); s->browse = false; }
                else if (s->saveMode) {   // name it in the bar, Enter saves
                    s->naming = i; g_kbd_owner = (int)(m - mods);
                    snprintf(s->nameBuf, sizeof s->nameBuf, "%s", wv_users[i].used ? wv_users[i].name : s->pname[0] ? s->pname : "MY SOUND");
                    while (GetCharPressed() > 0) {}
                    s->browse = false; s->saveMode = false;
                } else if (wv_users[i].used) { wv_load_preset(m, WV_NPRESET + i); s->browse = false; }
                return true;
            }
        return true;
    }
    if (mp.y < a.y) return false;
    return wv_press_body(m, mp, a);
}

static void wv_drag_main(Module *m, Vector2 mp, Rectangle area) { wv_drag(m, mp, wv_body(area)); }

static bool wv_wheel(Module *m, Vector2 mp, Rectangle area, float w)
{
    WavorSt *s = m->st;
    if (CheckCollisionPointRec(mp, wv_bar_btn(area, 2))) { wv_step_preset(m, w > 0 ? -1 : 1); return true; }
    if (s->browse) return true;
    return wv_wheel_body(m, mp, wv_body(area), w);
}

static bool wv_rpress(Module *m, Vector2 mp, Rectangle area)
{
    WavorSt *s = m->st;
    Rectangle a = wv_body(area);
    if (s->browse) {
        if (s->browseTab == 1)
            for (int i = 0; i < WV_NPRESET; i++)
                if (CheckCollisionPointRec(mp, wv_br_cell(a, i)) && wv_users[i].used) {
                    if (s->armDel == i) {
                        wv_user_delete(i); s->armDel = -1;
                        if (s->pidx == WV_NPRESET + i) s->pidx = -1;
                    } else s->armDel = i;
                    return true;
                }
        return true;
    }
    return wv_rpress_body(m, mp, a);
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
    [M_TAPE] = { "TAPE", 412, 92, 5, 2, 6, { "IN L", "IN R", "REC", "PLAY", "SPEED" }, { "L", "R" },
        { { "TIME", 0.1f, TAPE_MAX_SEC, 6, K_EXP, F_SEC },  { "SPEED", -2, 2, 1, K_KNOB, F_NUM },
          { "FEEDBACK", 0, 1, 0.6f, K_KNOB, F_PCT },        { "WOW", 0, 1, 0.3f, K_KNOB, F_PCT },
          { "MIX", 0, 1, 0.5f, K_KNOB, F_PCT },             { "LEVEL", 0, 1, 0.8f, K_KNOB, F_PCT } },
        tape_init, tape_process, tape_draw, tape_press, tape_drag, tape_save, tape_load_data, tape_fini,
        tape_audio_id, tape_audio_save, tape_audio_load },
    [M_ARP] = { "ARPEGGIATOR", 412, 56, 6, 2, 6, { "N1", "N2", "N3", "N4", "CLOCK", "RESET" }, { "NOTE", "GATE" },
        { { "MODE", 0, ARP_NMODES - 1, ARP_UPDOWN, K_STEP, F_LABEL, ARP_MODES }, { "OCTAVES", 1, 4, 2, K_STEP, F_INT },
          { "DIVIDE", 1, 8, 1, K_STEP, F_INT },   { "GATE", 0.05f, 1, 0.5f, K_KNOB, F_PCT },
          { "CHANCE", 0, 1, 0.9f, K_KNOB, F_PCT }, { "OCTAVE", -2, 2, 0, K_STEP, F_OCT } },
        arp_init, arp_process, arp_draw, NULL, NULL },
    [M_PERF] = { "PERFORMER", 476, 74, 3, 2, 7, { "CLOCK", "RESET", "ROOT" }, { "NOTE", "GATE" },
        { { "KEY", 0, 11, 9, K_STEP, F_LABEL, NOTE_NAMES }, { "SCALE", 0, SC_COUNT - 1, SC_AUTO, K_STEP, F_LABEL, SCALE_LABELS },
          { "PHRASE", 0, NPHRASE - 1, 0, K_STEP, F_PROG },  { "SPACE", 0, 32, 8, K_STEP, F_INT },
          { "VARY", 0, 1, 0.2f, K_KNOB, F_PCT },             { "GATE", 0.1f, 1, 0.85f, K_KNOB, F_PCT },
          { "OCTAVE", -2, 2, 0, K_STEP, F_OCT } },
        perf_init, perf_process, perf_draw, NULL, NULL },
    [M_DRUMS] = { .name = "DRUMS", .w = 600, .customH = 142, .nin = 16, .nout = 2, .nknob = 6,
        .ins = { "BD1", "BD2", "SD1", "SD2", "CP", "RS", "CH", "OH", "CY", "RD", "LT", "MT", "HT", "BL", "SH", "PC" },
        .outs = { "L", "R" },
        .knobs = { { "KIT", 0, N_KITS - 1, 0, K_STEP, F_LABEL, KIT_NAMES }, { "TUNE", -12, 12, 0, K_STEP, F_SEMI },
                   { "DECAY", 0.05f, 1, 1, K_KNOB, F_PCT },  { "TONE", -1, 1, 0, K_KNOB, F_NUM },
                   { "DRIVE", 0, 1, 0, K_KNOB, F_PCT },       { "LEVEL", 0, 1, 0.8f, K_KNOB, F_PCT } },
        .init = drums_init, .process = drums_process, .draw = drums_draw, .press = drums_press },
    [M_SAMPLER] = { .name = "SAMPLER", .w = 600, .customH = 170, .nin = 17, .nout = 2, .nknob = 4,
        .ins = { "PLAY1", "PLAY2", "PLAY3", "PLAY4", "PLAY5", "PLAY6", "PLAY7", "PLAY8",
                 "STOP1", "STOP2", "STOP3", "STOP4", "STOP5", "STOP6", "STOP7", "STOP8", "PITCH" },
        .outs = { "L", "R" },
        .knobs = { { "PITCH", -24, 24, 0, K_STEP, F_SEMI }, { "FADE", 0.001f, 0.25f, 0.005f, K_EXP, F_SEC },
                   { "VOICES", 0, 1, 0, K_SEG, F_INT, VOICE_LABELS }, { "LEVEL", 0, 1, 0.8f, K_KNOB, F_PCT } },
        .init = sampler_init, .process = sampler_process, .draw = sampler_draw, .press = sampler_press, .drag = sampler_drag,
        .save = sampler_save, .load = sampler_load_data, .fini = sampler_fini,
        .audio_id = sampler_audio_id, .audio_save = sampler_audio_save, .audio_load = sampler_audio_load,
        .wheel = sampler_wheel, .rpress = sampler_rpress },
    [M_FILTER] = { .name = "FILTER", .w = 236, .customH = 40, .nin = 4, .nout = 2, .nknob = 4,
        .ins = { "IN L", "IN R", "HIGH CV", "LOW CV" }, .outs = { "L", "R" },
        .knobs = { { "LOW CUT", 20, 4000, 20, K_EXP, F_HZ }, { "HIGH CUT", 80, 20000, 20000, K_EXP, F_HZ },
                   { "RESO", 0, 0.9f, 0.15f, K_KNOB, F_PCT }, { "CV AMT", 0, 1, 0.5f, K_KNOB, F_PCT } },
        .init = filt_init, .process = filt_process, .draw = filt_draw },
    [M_CHORUS] = { .name = "CHORUS", .w = 236, .customH = 0, .nin = 2, .nout = 2, .nknob = 3,
        .ins = { "IN L", "IN R" }, .outs = { "L", "R" },
        .knobs = { { "RATE", 0.05f, 5, 0.4f, K_EXP, F_HZ }, { "DEPTH", 0, 1, 0.5f, K_KNOB, F_PCT }, { "MIX", 0, 1, 0.5f, K_KNOB, F_PCT } },
        .init = chorus_init, .process = chorus_process },
    [M_ARR] = { .name = "ARRANGER", .w = 560, .customH = 194, .nin = 10, .nout = 10, .nknob = 1,
        .ins = { "CLOCK", "RESET", "IN1", "IN2", "IN3", "IN4", "IN5", "IN6", "IN7", "IN8" },
        .outs = { "OUT1", "OUT2", "OUT3", "OUT4", "OUT5", "OUT6", "OUT7", "OUT8", "LEVEL", "RAMP" },
        .knobs = { { "GLIDE", 0, 30, 4, K_KNOB, F_SEC } },
        .init = arr_init, .process = arr_process, .draw = arr_draw, .press = arr_press, .drag = arr_drag,
        .save = arr_save, .load = arr_load, .wheel = arr_wheel, .rpress = arr_rpress },
    [M_WAVOR] = { .name = "WAVOR", .w = WV_W, .customH = WV_H, .nin = 7, .nout = 2, .nknob = 0,
        .ins = { "P1", "P2", "P3", "P4", "GATE", "POS", "CUTOFF" }, .outs = { "L", "R" },
        .init = wv_init, .process = wv_process, .draw = wv_draw, .press = wv_press, .drag = wv_drag_main,
        .save = wv_save, .load = wv_load, .fini = wv_fini, .wheel = wv_wheel, .rpress = wv_rpress },
};

// Stable ids used in preset files.
static const char *MOD_ID[M_COUNT] = {
    [M_CLOCK] = "clock", [M_SEQ] = "seq", [M_CHORD] = "chords", [M_OSC3] = "osc3",
    [M_LFO] = "lfo", [M_REVERB] = "reverb", [M_OUT] = "output", [M_TAPE] = "tape",
    [M_ARP] = "arp", [M_PERF] = "performer", [M_DRUMS] = "drums", [M_SAMPLER] = "sampler",
    [M_FILTER] = "filter", [M_CHORUS] = "chorus", [M_ARR] = "arranger", [M_WAVOR] = "wavor",
};

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

// Transport and master recording. In the browser the audio callback runs on
// the main thread; natively it would need a lock-free hand-over instead of
// growing the recording buffer in place.
#define REC_MAX_SEC (20 * 60)

static volatile bool g_recording;
static short *g_rec;
static int    g_recLen, g_recCap;

static void rec_append(const float *o, unsigned int frames)
{
    if (g_recLen + (int)frames > g_recCap) {
        int cap = g_recCap ? g_recCap * 2 : 60 * (int)SR;
        if (cap > REC_MAX_SEC * (int)SR) cap = REC_MAX_SEC * (int)SR;
        if (cap < g_recLen + (int)frames) { g_recording = false; return; }   // full
        short *b = realloc(g_rec, (size_t)cap * 2 * sizeof(short));
        if (!b) { g_recording = false; return; }
        g_rec = b; g_recCap = cap;
    }
    for (unsigned int i = 0; i < frames * 2; i++) g_rec[2 * (size_t)g_recLen + i] = (short)(o[i] * 32767.0f);
    g_recLen += (int)frames;
}

static void audio_callback(void *buffer, unsigned int frames)
{
    float *o = buffer;
    if (!g_running) { memset(o, 0, frames * 2 * sizeof(float)); return; }
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
    if (g_recording) rec_append(o, frames);
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
    case F_LABEL: return k->labels[clampi((int)roundf(v - k->min), 0, (int)(k->max - k->min))];
    case F_DB:    return TextFormat("%+.1f DB", v);
    case F_RATIO: return TextFormat("%.1f:1", v);
    case F_OCTS:  return TextFormat("%+.1f OCT", v);
    case F_SEMIS: return TextFormat("%+.2f ST", v);
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
        if (m->st && DEFS[m->type].fini) DEFS[m->type].fini(m);
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
    if (DEFS[mods[i].type].fini) DEFS[mods[i].type].fini(&mods[i]);   // e.g. release tape memory now
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

// Serialize modules and the cables running between them. A preset holds the
// selection with positions relative to its top-left corner; a project holds
// every module at its absolute position plus the view. Caller frees.
static char *patch_serialize(const char *name, bool project)
{
    Buf b = { malloc(1024), 0, 1024 };
    int map[MAX_MODULES], n = 0;
    float x0 = project ? 0 : 1e9f, y0 = project ? 0 : 1e9f;
    for (int i = 0; i < MAX_MODULES; i++) {
        map[i] = -1;
        if (!mods[i].alive || !(sel[i] || project)) continue;
        map[i] = n++;
        if (!project) { x0 = fminf(x0, mods[i].pos.x); y0 = fminf(y0, mods[i].pos.y); }
    }
    if (project) bput(&b, "raya-project 1\nname %s\npan %.0f %.0f\n", name, pan.x, pan.y);
    else         bput(&b, "raya-preset 1\nname %s\n", name);
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
            static char data[4096];
            data[0] = 0;
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

static char *preset_serialize(const char *name) { return patch_serialize(name, false); }

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
// A preset's tapes get fresh ids so their audio never collides with another
// tape's; a project keeps its ids to find its stored tape audio.
static int preset_instantiate(const char *text, Vector2 at, bool keepIds)
{
    int map[MAX_MODULES], n = 0, added = 0;
    Module *cur = NULL;
    static char line[8192];
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
    if (!keepIds)
        for (int i = 0; i < MAX_MODULES; i++)
            if (sel[i] && DEFS[mods[i].type].audio_id) *DEFS[mods[i].type].audio_id(&mods[i]) = new_id();
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
    // new factory presets go at the end: existing racks receive them by index
    { "tape echo swells",
      "module clock 0 0\n"
      "knob BPM 70\n"
      "module seq 256 0\n"
      "knob CHANCE 0.6\n"
      "module osc3 0 300\n"
      "knob WAVE 0.3\nknob CUTOFF 1400\nknob ATTACK 0.3\nknob RELEASE 3\nknob MOD_AMT 0.6\n"
      "module tape 436 300\n"
      "knob TIME 2.57\nknob FEEDBACK 0.7\nknob WOW 0.5\nknob MIX 0.45\n"
      "data 1 1\n"
      "module reverb 872 300\n"
      "module output 1132 300\n"
      "cable 0 1 1 0 3\ncable 1 0 2 3 1\ncable 1 1 2 4 2\n"
      "cable 2 0 3 0 4\ncable 2 1 3 1 4\ncable 3 0 4 0 0\ncable 3 1 4 1 0\n"
      "cable 4 0 5 0 2\ncable 4 1 5 1 2\n" },
    { "chord arpeggio",
      "module clock 0 0\nknob BPM 66\n"
      "module chords 256 0\n"
      "module arp 652 0\nknob MODE 2\nknob OCTAVES 2\nknob DIVIDE 2\nknob GATE 0.6\nknob CHANCE 0.85\n"
      "module osc3 0 300\nknob WAVE 0.3\nknob CUTOFF 700\nknob ATTACK 3\nknob LEVEL 0.45\n"
      "module osc3 436 300\nknob WAVE 0.1\nknob DETUNE 0.15\nknob CUTOFF 2200\n"
      "knob ATTACK 0.02\nknob RELEASE 1.8\nknob GLIDE 0.02\nknob LEVEL 0.4\n"
      "module reverb 872 300\nknob SIZE 1.6\nknob DECAY 18\nknob MIX 0.55\n"
      "module output 1132 300\n"
      "cable 0 2 1 0 0\ncable 0 0 2 4 3\n"
      "cable 1 0 2 0 1\ncable 1 1 2 1 1\ncable 1 2 2 2 1\ncable 1 3 2 3 1\n"
      "cable 1 0 3 0 1\ncable 1 1 3 1 1\ncable 1 2 3 2 1\n"
      "cable 2 0 4 0 2\ncable 2 0 4 1 2\ncable 2 0 4 2 2\ncable 2 1 4 3 3\n"
      "cable 3 0 5 0 4\ncable 3 1 5 1 4\ncable 4 0 5 0 4\ncable 4 1 5 1 4\n"
      "cable 5 0 6 0 0\ncable 5 1 6 1 0\n" },
    { "pentatonic performer",
      "module clock 0 0\nknob BPM 60\n"
      "module performer 256 0\nknob KEY 0\nknob PHRASE 0\nknob SPACE 8\nknob VARY 0.25\n"
      "module osc3 756 0\nknob OCTAVE -1\nknob WAVE 0.3\nknob CUTOFF 450\nknob ATTACK 6\nknob LEVEL 0.35\n"
      "module osc3 0 300\nknob WAVE 0.25\nknob DETUNE 0.2\nknob CUTOFF 1800\nknob ATTACK 0.08\n"
      "knob RELEASE 2.5\nknob GLIDE 0.12\nknob MOD_AMT 0.3\nknob LEVEL 0.5\n"
      "module tape 436 300\nknob TIME 2.5\nknob FEEDBACK 0.55\nknob WOW 0.4\nknob MIX 0.4\ndata 1 1\n"
      "module reverb 872 300\nknob SIZE 1.6\nknob DECAY 20\nknob MIX 0.55\n"
      "module output 1132 300\n"
      "cable 0 0 1 0 3\ncable 1 0 3 0 2\ncable 1 0 3 1 2\ncable 1 0 3 2 2\ncable 1 1 3 3 3\n"
      "cable 3 0 4 0 4\ncable 3 1 4 1 4\ncable 4 0 5 0 0\ncable 4 1 5 1 0\n"
      "cable 2 0 5 0 1\ncable 2 1 5 1 1\ncable 5 0 6 0 0\ncable 5 1 6 1 0\n" },
    { "boomer groove",
      "module clock 0 0\nknob BPM 84\n"
      "module seq 256 0\nknob CHANCE 1\nknob GLIDE 0\ndata 1:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500\n"
      "module seq 668 0\nknob CHANCE 1\nknob GLIDE 0\ndata 0:0.500 0:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500\n"
      "module seq 1080 0\nknob CHANCE 0.85\nknob GLIDE 0\ndata 1:0.500 0:0.300 1:0.700 0:0.300 1:0.500 0:0.300 1:0.800 0:0.300 1:0.500 0:0.300 1:0.700 0:0.300 1:0.500 0:0.300 1:0.900 0:0.300\n"
      "module drums 0 300\nknob KIT 0\n"
      "module reverb 624 300\nknob SIZE 1.2\nknob DECAY 6\nknob MIX 0.22\n"
      "module output 884 300\n"
      "cable 0 0 1 0 3\ncable 0 0 2 0 3\ncable 0 0 3 0 3\n"
      "cable 1 0 4 0 1\ncable 2 0 4 2 0\ncable 3 0 4 6 2\n"
      "cable 4 0 5 0 4\ncable 4 1 5 1 4\ncable 5 0 6 0 0\ncable 5 1 6 1 0\n" },
    { "dusty vinyl beat",
      "module clock 0 0\nknob BPM 76\n"
      "module seq 256 0\nknob CHANCE 1\nknob GLIDE 0\ndata 1:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500\n"
      "module seq 668 0\nknob CHANCE 1\nknob GLIDE 0\ndata 0:0.500 0:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500\n"
      "module seq 1080 0\nknob CHANCE 0.75\nknob GLIDE 0\ndata 1:0.500 0:0.300 1:0.700 0:0.300 1:0.500 0:0.300 1:0.800 0:0.300 1:0.500 0:0.300 1:0.700 0:0.300 1:0.500 0:0.300 1:0.900 0:0.300\n"
      "module drums 0 300\nknob KIT 13\nknob TONE -0.15\n"
      "module tape 624 300\nknob TIME 0.789\nknob FEEDBACK 0.45\nknob WOW 0.5\nknob MIX 0.3\ndata 1 1\n"
      "module reverb 1060 300\nknob SIZE 1.1\nknob DECAY 5\nknob MIX 0.25\n"
      "module output 1320 300\n"
      "cable 0 0 1 0 3\ncable 0 0 2 0 3\ncable 0 0 3 0 3\n"
      "cable 1 0 4 0 1\ncable 2 0 4 2 0\ncable 3 0 4 6 2\n"
      "cable 4 0 5 0 4\ncable 4 1 5 1 4\ncable 5 0 6 0 0\ncable 5 1 6 1 0\ncable 6 0 7 0 0\ncable 6 1 7 1 0\n" },
    { "electro 128",
      "module clock 0 0\nknob BPM 128\n"
      "module arranger 256 0\nknob GLIDE 6\ndata 24:0.250:00000000 16:0.400:10010000 16:0.550:11101000 8:0.800:00000100 32:0.350:00000100 16:0.600:00001100 32:0.900:11001101 16:1.000:10011001 32:0.700:10101000 24:0.750:01101010 16:0.250:00010000 0:0.000:00000000\n"
      "module chords 840 0\nknob PROG 92\nknob CHORD_LEN 0\n"
      "module arp 1236 0\nknob MODE 0\nknob OCTAVES 2\nknob DIVIDE 2\nknob GATE 0.4\nknob CHANCE 0.9\n"
      "module seq 0 420\nknob CHANCE 1\nknob GLIDE 0\ndata 0:0.500 0:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500\n"
      "module seq 412 420\nknob CHANCE 0.95\nknob GLIDE 0\ndata 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500\n"
      "module seq 824 420\nknob CHANCE 1\nknob GLIDE 0\ndata 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 0:0.500 1:0.500 0:0.500\n"
      "module seq 1236 420\nknob CHANCE 1\nknob GLIDE 0\ndata 0:0.000 0:0.000 1:0.000 1:0.000 0:0.000 0:0.000 1:0.000 1:0.583 0:0.000 0:0.000 1:0.000 1:0.000 0:0.000 0:0.000 1:0.000 1:1.000\n"
      "module seq 1648 420\nknob CHANCE 1\nknob GLIDE 0\ndata 1:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 0:0.500 1:0.500 0:0.500 0:0.500 1:0.500 0:0.500\n"
      "module seq 2060 420\nknob CHANCE 0.9\nknob GLIDE 0\ndata 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500 0:0.500 1:0.500\n"
      "module drums 0 730\nknob KIT 1\nknob TONE -0.35\nknob LEVEL 0.55\n"
      "module osc3 624 730\nknob OCTAVE -1\nknob WAVE 0.85\nknob DETUNE 0.15\nknob CUTOFF 260\nknob RESO 0.45\nknob ATTACK 0.004\nknob RELEASE 0.14\nknob GLIDE 0\nknob MOD_AMT 0.6\nknob LEVEL 0.9\n"
      "module osc3 1060 730\nknob WAVE 0.75\nknob DETUNE 0.6\nknob CUTOFF 3000\nknob ATTACK 2.5\nknob RELEASE 4\nknob MOD_AMT 0.3\nknob LEVEL 1\n"
      "module osc3 1496 730\nknob OCTAVE 1\nknob WAVE 0.45\nknob DETUNE 0.25\nknob CUTOFF 2600\nknob RESO 0.3\nknob ATTACK 0.005\nknob RELEASE 0.35\nknob GLIDE 0.02\nknob LEVEL 0.65\n"
      "module filter 624 1110\nknob LOW_CUT 150\nknob HIGH_CUT 1300\nknob RESO 0.25\nknob CV_AMT 0.5\n"
      "module chorus 884 1110\nknob RATE 0.35\nknob DEPTH 0.6\nknob MIX 0.55\n"
      "module tape 1144 1110\nknob TIME 0.3516\nknob FEEDBACK 0.45\nknob WOW 0.15\nknob MIX 0.35\ndata 1 1\n"
      "module reverb 1580 1110\nknob SIZE 1.4\nknob DECAY 6\nknob MIX 0.35\n"
      "module output 1840 1110\n"
      "cable 0 0 1 0 3\n"
      "cable 0 0 4 0 3\n"
      "cable 0 0 5 0 3\n"
      "cable 0 0 6 0 3\n"
      "cable 0 0 7 0 3\n"
      "cable 0 0 8 0 3\n"
      "cable 0 0 9 0 3\n"
      "cable 0 0 3 4 3\n"
      "cable 0 1 1 2 1\n"
      "cable 0 2 2 0 0\n"
      "cable 4 0 1 3 0\n"
      "cable 5 0 1 4 2\n"
      "cable 6 0 1 5 2\n"
      "cable 7 0 1 6 1\n"
      "cable 3 1 1 7 4\n"
      "cable 8 0 1 8 1\n"
      "cable 9 0 1 9 2\n"
      "cable 1 0 10 0 1\n"
      "cable 1 1 10 4 0\n"
      "cable 1 2 10 6 2\n"
      "cable 1 3 10 7 2\n"
      "cable 1 4 11 3 1\n"
      "cable 1 5 13 3 4\n"
      "cable 1 6 10 0 1\n"
      "cable 1 7 10 14 2\n"
      "cable 1 8 14 2 3\n"
      "cable 2 0 3 0 1\n"
      "cable 2 1 3 1 1\n"
      "cable 2 2 3 2 1\n"
      "cable 2 3 3 3 1\n"
      "cable 2 0 12 0 1\n"
      "cable 2 1 12 1 1\n"
      "cable 2 2 12 2 1\n"
      "cable 2 4 11 0 0\n"
      "cable 7 1 11 0 3\n"
      "cable 2 4 11 1 0\n"
      "cable 7 1 11 1 3\n"
      "cable 2 4 11 2 0\n"
      "cable 7 1 11 2 3\n"
      "cable 3 0 13 0 4\n"
      "cable 3 0 13 1 4\n"
      "cable 3 0 13 2 4\n"
      "cable 12 0 14 0 4\n"
      "cable 12 1 14 1 4\n"
      "cable 14 0 15 0 4\n"
      "cable 14 1 15 1 4\n"
      "cable 15 0 17 0 4\n"
      "cable 15 1 17 1 4\n"
      "cable 13 0 16 0 2\n"
      "cable 13 1 16 1 2\n"
      "cable 16 0 17 0 2\n"
      "cable 16 1 17 1 2\n"
      "cable 17 0 18 0 0\n"
      "cable 17 1 18 1 0\n"
      "cable 10 0 18 0 1\n"
      "cable 10 1 18 1 1\n"
      "cable 11 0 18 0 3\n"
      "cable 11 1 18 1 3\n" },
    { "wavor chords",
      "module clock 0 0\n"
      "knob BPM 60\n"
      "module chords 252 0\n"
      "knob PROG 5\nknob CHORD_LEN 3\n"
      "module wavor 0 200\n"
      "module output 1240 200\n"
      "cable 0 2 1 0 0\n"
      "cable 1 0 2 0 1\ncable 1 1 2 1 1\ncable 1 2 2 2 1\ncable 1 3 2 3 1\n"
      "cable 2 0 3 0 4\ncable 2 1 3 1 4\n" },
    { "wavor glass arp",
      "module clock 0 0\n"
      "knob BPM 84\n"
      "module chords 252 0\n"
      "knob PROG 12\nknob CHORD_LEN 3\n"
      "module arp 644 0\n"
      "knob MODE 2\nknob OCTAVES 2\nknob GATE 0.4\n"
      "module wavor 0 220\n"
      "data wave=33 m1=43 m1a=0.5 m2=16 m2a=0.35 m3a=0 m4a=0 a=0.004 d=0.7 s=0.12 r=1.4 "
      "pos=0.1 envpos=0.45 lfopos=0.1 voices=6 spread=1 cdetl=0.3 cdetc=0.1 cdetr=0.3 "
      "fltcut=2600 fltenv=0.6 fltres=0.3 dlytime=0.536 dlyfb=0.5 dlymix=0.35 pltmix=0.4 pltdecay=0.8\n"
      "module output 1240 220\n"
      "cable 0 2 1 0 0\ncable 0 0 2 4 2\n"
      "cable 1 0 2 0 1\ncable 1 1 2 1 1\ncable 1 2 2 2 1\ncable 1 3 2 3 1\n"
      "cable 2 0 3 0 3\ncable 2 1 3 4 3\n"
      "cable 3 0 4 0 4\ncable 3 1 4 1 4\n" },
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
    // .seeded records how many factory presets this rack has received, so
    // presets added in later versions arrive without re-adding deleted ones.
    const char *seeded = PRESET_DIR "/.seeded";
    int have = 0;
    char *mark = read_file(seeded);
    if (mark && sscanf(mark, "factory %d", &have) != 1) have = 10;   // first versions wrote no count
    free(mark);
    if (have < N_FACTORY) {
        fs_hold = true;
        for (int i = have; i < N_FACTORY; i++) rack_store(FACTORY[i].name, factory_text(i));
        write_file(seeded, TextFormat("factory %d\n", N_FACTORY));
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
// Projects — the whole patch (modules, knobs, steps, cables, view) plus the
// audio on its tapes. The current project autosaves every couple of seconds,
// so a reload picks up exactly where it was. Layout on disk (IndexedDB in the
// browser, raya-data natively):
//   <root>/projects/<slug>/project.raya     the patch as text
//   <root>/projects/<slug>/tape-<id>.wav    audio of each tape
//   <root>/current                          slug of the open project
// ---------------------------------------------------------------------------

#define PROJECT_DIR   PRESET_ROOT "/projects"
#define PROJECT_MAX   256
#define AUTOSAVE_SEC  2.0

typedef struct { char slug[PRESET_NAME_MAX + 1], name[PRESET_NAME_MAX + 1]; time_t mtime; } ProjectInfo;

static char   proj_slug[PRESET_NAME_MAX + 1], proj_name[PRESET_NAME_MAX + 1];
static bool   proj_ready;
static char  *proj_last;      // text as last written, to skip unchanged saves
static double proj_saveT;
static ProjectInfo proj_list[PROJECT_MAX];
static int    proj_count;

static void proj_file(char *out, int cap, const char *slug, const char *file)
{
    snprintf(out, cap, "%s/%s/%s", PROJECT_DIR, slug, file);
}

static bool proj_exists(const char *slug)
{
    char path[256];
    proj_file(path, sizeof path, slug, "project.raya");
    return FileExists(path);
}

// A slug for `name` that no other project uses yet.
static void proj_unique_slug(const char *name, char *out)
{
    char base[PRESET_NAME_MAX + 1];
    make_slug(name, base);
    snprintf(out, PRESET_NAME_MAX + 1, "%s", base);
    for (int i = 2; proj_exists(out); i++) snprintf(out, PRESET_NAME_MAX + 1, "%.*s-%d", PRESET_NAME_MAX - 5, base, i);
}

static void proj_set_current(void)
{
    write_file(PRESET_ROOT "/current", proj_slug);
}

// Store the audio of tapes and samplers that changed.
static bool proj_save_audio(int how)
{
    bool wrote = false;
    for (int i = 0; i < MAX_MODULES; i++) {
        Module *m = &mods[i];
        const ModDef *d = &DEFS[m->type];
        if (!m->alive || !d->audio_save) continue;
        char path[256];
        proj_file(path, sizeof path, proj_slug, TextFormat("audio-%u.wav", *d->audio_id(m)));
        if (d->audio_save(m, path, how)) wrote = true;
    }
    return wrote;
}

// Remove stored audio that no module in the patch refers to any more.
static void proj_prune_audio(void)
{
    char dir[256];
    snprintf(dir, sizeof dir, "%s/%s", PROJECT_DIR, proj_slug);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        unsigned id;
        if (sscanf(e->d_name, "audio-%u.wav", &id) != 1 && sscanf(e->d_name, "tape-%u.wav", &id) != 1) continue;
        bool used = false;
        for (int i = 0; i < MAX_MODULES && !used; i++)
            used = mods[i].alive && DEFS[mods[i].type].audio_id && *DEFS[mods[i].type].audio_id(&mods[i]) == id;
        if (!used) remove(TextFormat("%s/%s", dir, e->d_name));
    }
    closedir(d);
}

static void project_save(bool force)
{
    if (!proj_slug[0]) return;
    char *text = patch_serialize(proj_name, true);
    bool changed = !proj_last || strcmp(text, proj_last);
    bool wrote = false;
    if (changed || force) {
        char path[256];
        MakeDirectory(TextFormat("%s/%s", PROJECT_DIR, proj_slug));
        proj_file(path, sizeof path, proj_slug, "project.raya");
        if (write_file(path, text)) wrote = true;
        free(proj_last);
        proj_last = text;
        text = NULL;
    }
    free(text);
    if (proj_save_audio(force ? SAVE_NOW : SAVE_LATER)) wrote = true;
    if (changed) proj_prune_audio();
    if (wrote) fs_sync();
}

static void project_tick(void)
{
    if (!proj_ready || GetTime() - proj_saveT < AUTOSAVE_SEC) return;
    proj_saveT = GetTime();
    project_save(false);
}

static void patch_clear(void)
{
    for (int i = 0; i < MAX_MODULES; i++) if (mods[i].alive) remove_module(i);
    ncables = 0;
    rebuild_routing();
    select_none();
}

static bool project_open(const char *slug)
{
    char path[256];
    proj_file(path, sizeof path, slug, "project.raya");
    char *text = read_file(path);
    if (!text || strncmp(text, "raya-project", 12)) { free(text); return false; }
    patch_clear();
    preset_instantiate(text, (Vector2){ 0, 0 }, true);
    select_none();
    snprintf(proj_slug, sizeof proj_slug, "%s", slug);
    snprintf(proj_name, sizeof proj_name, "%s", slug);
    char line[256];
    for (const char *p = text; *p; ) {
        p = next_line(p, line, sizeof line);
        if (!strncmp(line, "name ", 5)) snprintf(proj_name, sizeof proj_name, "%s", line + 5);
        else sscanf(line, "pan %f %f", &pan.x, &pan.y);
    }
    for (int i = 0; i < MAX_MODULES; i++) {   // tapes and samplers get their audio back
        Module *m = &mods[i];
        const ModDef *d = &DEFS[m->type];
        if (!m->alive || !d->audio_load) continue;
        unsigned id = *d->audio_id(m);
        proj_file(path, sizeof path, slug, TextFormat("audio-%u.wav", id));
        if (!FileExists(path)) proj_file(path, sizeof path, slug, TextFormat("tape-%u.wav", id));   // older projects
        if (FileExists(path)) d->audio_load(m, path);
    }
    free(proj_last);
    proj_last = text;
    proj_set_current();
    fs_sync();
    return true;
}

static void project_new(const char *name)
{
    project_save(true);
    patch_clear();
    pan = (Vector2){ 0, 0 };
    snprintf(proj_name, sizeof proj_name, "%s", name);
    proj_unique_slug(name, proj_slug);
    free(proj_last); proj_last = NULL;
    proj_set_current();
    project_save(true);
}

// Continue under a new name, keeping the original as it was.
static void project_duplicate(const char *name)
{
    project_save(true);
    snprintf(proj_name, sizeof proj_name, "%s", name);
    proj_unique_slug(name, proj_slug);
    free(proj_last); proj_last = NULL;
    proj_set_current();
    project_save(true);
    if (proj_save_audio(SAVE_ALL)) fs_sync();   // every tape and sample into the copy
}

static void project_rename(const char *name)
{
    char slug[PRESET_NAME_MAX + 1];
    make_slug(name, slug);
    if (strcmp(slug, proj_slug)) {
        proj_unique_slug(name, slug);
        rename(TextFormat("%s/%s", PROJECT_DIR, proj_slug), TextFormat("%s/%s", PROJECT_DIR, slug));
        snprintf(proj_slug, sizeof proj_slug, "%s", slug);
    }
    snprintf(proj_name, sizeof proj_name, "%s", name);
    free(proj_last); proj_last = NULL;
    proj_set_current();
    project_save(true);
}

static void project_delete(const char *slug)
{
    char dir[256];
    snprintf(dir, sizeof dir, "%s/%s", PROJECT_DIR, slug);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) remove(TextFormat("%s/%s", dir, e->d_name));
        closedir(d);
    }
    remove(dir);
    fs_sync();
}

static int proj_cmp(const void *a, const void *b)
{
    time_t x = ((const ProjectInfo *)a)->mtime, y = ((const ProjectInfo *)b)->mtime;
    return x < y ? 1 : x > y ? -1 : 0;
}

// All projects, most recently changed first.
static void project_scan(void)
{
    proj_count = 0;
    DIR *d = opendir(PROJECT_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && proj_count < PROJECT_MAX) {
        if (e->d_name[0] == '.') continue;
        char path[256];
        struct stat st;
        proj_file(path, sizeof path, e->d_name, "project.raya");
        if (stat(path, &st) != 0) continue;
        ProjectInfo *pi = &proj_list[proj_count++];
        snprintf(pi->slug, sizeof pi->slug, "%s", e->d_name);
        snprintf(pi->name, sizeof pi->name, "%s", e->d_name);
        pi->mtime = st.st_mtime;
        char *text = read_file(path), line[256];
        for (const char *p = text ? text : ""; *p; ) {
            p = next_line(p, line, sizeof line);
            if (!strncmp(line, "name ", 5)) { snprintf(pi->name, sizeof pi->name, "%s", line + 5); break; }
        }
        free(text);
    }
    closedir(d);
    qsort(proj_list, proj_count, sizeof proj_list[0], proj_cmp);
}

// First start: open the last project, or begin one with the starter patch.
static void default_layout(void);
static void project_boot(void)
{
    MakeDirectory(PROJECT_DIR);
    char *cur = read_file(PRESET_ROOT "/current");
    bool ok = cur && cur[0] && project_open(cur);
    free(cur);
    if (!ok) {
        default_layout();
        snprintf(proj_name, sizeof proj_name, "untitled");
        proj_unique_slug(proj_name, proj_slug);
        proj_set_current();
        project_save(true);
    }
    proj_saveT = GetTime();
    proj_ready = true;
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
enum { MODAL_NONE, MODAL_RACK, MODAL_SAVE, MODAL_PROJECTS, MODAL_NAME, MODAL_REC };
enum { NAME_NEW, NAME_RENAME, NAME_DUPLICATE };

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
    // projects, transport, recording
    int     nameAction;
    bool    recOn, video;
    double  videoT;
} ui = { .lastKnobMod = -1, .toastT = -10 };

static const int MENU_ORDER[M_COUNT] = { M_CLOCK, M_ARR, M_LFO, M_SEQ, M_CHORD, M_ARP, M_PERF, M_OSC3, M_WAVOR, M_DRUMS, M_SAMPLER, M_TAPE, M_FILTER, M_CHORUS, M_REVERB, M_OUT };
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
static Rectangle btn_save(void) { Rectangle r = btn_rack(); return (Rectangle){ r.x + r.width - 190, r.y + 30, 190, 26 }; }
static Rectangle btn_project(void) { return (Rectangle){ 150, 14, 200, 26 }; }
static Rectangle btn_run(void)     { return (Rectangle){ 360, 14, 64, 26 }; }
static Rectangle btn_rec(void)     { return (Rectangle){ 430, 14, 110, 26 }; }
static Rectangle btn_video(void)   { return (Rectangle){ 546, 14, 116, 26 }; }

// --- patch overview (minimap) ---------------------------------------------

#define MAP_W 200
#define MAP_H 124

typedef struct { bool show; Rectangle box; float s; Vector2 o; } Minimap;  // map = o + world * s

static Minimap minimap(void)
{
    Minimap m = { .box = { GetScreenWidth() - 18.0f - MAP_W, 78, MAP_W, MAP_H } };
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
    int n = preset_instantiate(rack[slot].text, at, false);
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

// --- projects, transport and recording ----------------------------------------

static const char *clock_text(double sec) { int t = (int)sec; return TextFormat("%02d:%02d", t / 60, t % 60); }

static void open_projects(void)
{
    project_save(true);
    project_scan();
    ui.modal = MODAL_PROJECTS;
    ui.hl = ui.scroll = ui.confirmDel = 0;
    ui.menu = false;
    drain_chars();
}

static void open_name(int action)
{
    ui.modal = MODAL_NAME;
    ui.nameAction = action;
    if (action == NAME_RENAME) snprintf(ui.text, sizeof ui.text, "%s", proj_name);
    else if (action == NAME_DUPLICATE) snprintf(ui.text, sizeof ui.text, "%.*s copy", PRESET_NAME_MAX - 5, proj_name);
    else ui.text[0] = 0;
    drain_chars();
}

static void toggle_run(void) { g_running = !g_running; toast(g_running ? "RUNNING" : "STOPPED"); }

static void toggle_rec(void)
{
    if (g_recording) { g_recording = false; return; }   // frame() then offers to save it
    if (ui.modal == MODAL_REC) return;
    g_recLen = 0;
    g_running = true;
    g_recording = true;
    ui.recOn = true;
    toast("RECORDING THE OUTPUT");
}

static void toggle_video(void)
{
#if defined(PLATFORM_WEB)
    if (!ui.video) {
        if (emscripten_run_script_int("rayaVideoStart()")) { ui.video = true; ui.videoT = GetTime(); g_running = true; toast("RECORDING VIDEO"); }
        else toast("THIS BROWSER CANNOT RECORD VIDEO");
    } else {
        emscripten_run_script("rayaVideoStop()");
        ui.video = false;
        toast(TextFormat("SAVED A %s VIDEO", clock_text(GetTime() - ui.videoT)));
    }
#else
    toast("VIDEO RECORDING WORKS IN THE BROWSER");
#endif
}

static Rectangle proj_btn(int i)   // NEW / DUPLICATE / RENAME above the list
{
    Rectangle r = rack_search();
    float w = (r.width - 16) / 3;
    return (Rectangle){ r.x + i * (w + 8), r.y, w, r.height };
}

static void projects_input(Vector2 mp)
{
    bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    int vis = rack_visible();
    float wheel = GetMouseWheelMove();
    if (wheel != 0) ui.scroll = clampi(ui.scroll - (wheel > 0 ? 2 : -2), 0, proj_count > vis ? proj_count - vis : 0);
    if (IsKeyPressed(KEY_ESCAPE)) { ui.modal = MODAL_NONE; return; }
    if (!click) return;
    if (!CheckCollisionPointRec(mp, rack_box())) { ui.modal = MODAL_NONE; return; }
    for (int i = 0; i < 3; i++)
        if (CheckCollisionPointRec(mp, proj_btn(i))) { open_name(i == 0 ? NAME_NEW : i == 1 ? NAME_DUPLICATE : NAME_RENAME); return; }
    for (int v = 0; v < vis && ui.scroll + v < proj_count; v++) {
        Rectangle row = rack_row(v);
        if (!CheckCollisionPointRec(mp, row)) continue;
        int i = ui.scroll + v;
        bool current = !strcmp(proj_list[i].slug, proj_slug);
        if (!current && CheckCollisionPointRec(mp, rack_del(row))) {
            if (ui.confirmDel == i + 1) {
                toast(TextFormat("DELETED %s", proj_list[i].name));
                project_delete(proj_list[i].slug);
                project_scan();
                ui.confirmDel = 0;
            } else ui.confirmDel = i + 1;
            return;
        }
        if (!current) {
            project_save(true);
            char slug[PRESET_NAME_MAX + 1];
            snprintf(slug, sizeof slug, "%s", proj_list[i].slug);
            toast(project_open(slug) ? TextFormat("OPENED %s", proj_name) : "COULD NOT OPEN THE PROJECT");
        }
        ui.modal = MODAL_NONE;
        return;
    }
    ui.confirmDel = 0;
}

static void name_input(Vector2 mp)
{
    bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    text_input();
    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || (click && CheckCollisionPointRec(mp, save_ok()))) && ui.text[0]) {
        if (ui.nameAction == NAME_NEW) project_new(ui.text);
        else if (ui.nameAction == NAME_RENAME) project_rename(ui.text);
        else project_duplicate(ui.text);
        toast(TextFormat("PROJECT %s", proj_name));
        ui.modal = MODAL_NONE;
    } else if (IsKeyPressed(KEY_ESCAPE) || (click && (CheckCollisionPointRec(mp, save_cancel()) || !CheckCollisionPointRec(mp, save_box()))))
        open_projects();
}

static Rectangle rec_btn(int i)    // SAVE WAV / SAVE MP3 / DISCARD
{
    Rectangle b = save_box();
    float w = (b.width - 32 - 16) / 3;
    return (Rectangle){ b.x + 16 + i * (w + 8), b.y + b.height - 44, w, 28 };
}

static void rec_close(void)
{
    free(g_rec);
    g_rec = NULL;
    g_recLen = g_recCap = 0;
    ui.modal = MODAL_NONE;
}

static void rec_input(Vector2 mp)
{
    bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (click && CheckCollisionPointRec(mp, rec_btn(0))) export_audio(g_rec, g_recLen, false, "patch");
    else if (click && CheckCollisionPointRec(mp, rec_btn(1))) export_audio(g_rec, g_recLen, true, "patch");
    else if (IsKeyPressed(KEY_ESCAPE) || (click && CheckCollisionPointRec(mp, rec_btn(2)))) rec_close();
}

static void modal_input(Vector2 mp)
{
    if (ui.modal == MODAL_PROJECTS) { projects_input(mp); return; }
    if (ui.modal == MODAL_NAME)     { name_input(mp); return; }
    if (ui.modal == MODAL_REC)      { rec_input(mp); return; }
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

    // keyboard (unless a module is taking typed text)
    if (g_kbd_owner < 0) {
    if (IsKeyPressed(KEY_SPACE)) toggle_run();
    if (IsKeyPressed(KEY_R) && !cmd) toggle_rec();
    if (IsKeyPressed(KEY_V) && !cmd) toggle_video();
    if (IsKeyPressed(KEY_O) && !cmd) { open_projects(); return; }
    if (IsKeyPressed(KEY_P) && !cmd) { open_rack(false, mp); return; }
    if (IsKeyPressed(KEY_S) && !cmd) { open_save(); return; }
    if (cmd && IsKeyPressed(KEY_A)) for (int i = 0; i < MAX_MODULES; i++) sel[i] = mods[i].alive;
    if (IsKeyPressed(KEY_ESCAPE)) select_none();
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        int pm, pi; bool po;
        int ki, kk;
        int mi = module_at(mp);
        ui.press = mp;
        if (CheckCollisionPointRec(mp, btn_rack())) { open_rack(false, mp); return; }
        if (selection_count() && CheckCollisionPointRec(mp, btn_save())) { open_save(); return; }
        if (CheckCollisionPointRec(mp, btn_project())) { open_projects(); return; }
        if (CheckCollisionPointRec(mp, btn_run()))     { toggle_run(); return; }
        if (CheckCollisionPointRec(mp, btn_rec()))     { toggle_rec(); return; }
        if (CheckCollisionPointRec(mp, btn_video()))   { toggle_video(); return; }
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
        } else if (module_at(mp) >= 0) {
            Module *m = &mods[module_at(mp)];
            if (DEFS[m->type].rpress) DEFS[m->type].rpress(m, mp, custom_rect(m));
        } else {
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
    int ki, kk, wm = module_at(mp);
    if (wheel != 0 && ui.mode == D_NONE && wm >= 0 && DEFS[mods[wm].type].wheel &&
        CheckCollisionPointRec(mp, custom_rect(&mods[wm])) && DEFS[mods[wm].type].wheel(&mods[wm], mp, custom_rect(&mods[wm]), wheel))
        wheel = 0;
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

static void draw_projects(Vector2 mp)
{
    Rectangle b = rack_box();
    draw_dialog_frame(b, "PROJECTS");
    const char *count = TextFormat("%d SAVED IN THIS BROWSER", proj_count);
    txt(count, b.x + b.width - 16 - txt_w(count, 10), b.y + 10, 10, TEXT_DIM);
    static const char *labels[3] = { "NEW PROJECT", "DUPLICATE", "RENAME" };
    for (int i = 0; i < 3; i++) draw_button(proj_btn(i), labels[i], mp, false);
    Rectangle l = rack_list();
    DrawRectangleRec(l, INSET);
    int vis = rack_visible();
    for (int v = 0; v < vis && ui.scroll + v < proj_count; v++) {
        int i = ui.scroll + v;
        const ProjectInfo *pi = &proj_list[i];
        Rectangle row = rack_row(v);
        bool current = !strcmp(pi->slug, proj_slug), hover = CheckCollisionPointRec(mp, row);
        if (current || hover) DrawRectangleRec(row, fade(ACCENT, current ? 0.12f : 0.06f));
        if (current) DrawRectangle((int)row.x, (int)row.y, 2, (int)row.height, ACCENT);
        txt(pi->name, row.x + 12, row.y + 9, 12, current ? ACCENT : TEXT);
        Rectangle del = rack_del(row);
        bool armed = ui.confirmDel == i + 1;
        char when[40];
        strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&pi->mtime));
        const char *info = current ? "OPEN NOW" : armed ? "CLICK X AGAIN TO DELETE" : when;
        txt(info, del.x - 12 - txt_w(info, 10), row.y + 10, 10, armed ? CABLE_COLORS[1] : TEXT_DIM);
        if (!current && (hover || armed))
            txt_c("x", del.x + del.width / 2, del.y + 3, 10, CheckCollisionPointRec(mp, del) || armed ? CABLE_COLORS[1] : TEXT_DIM);
    }
    txt("CLICK: OPEN    CHANGES SAVE THEMSELVES    ESC: CLOSE", b.x + 16, b.y + b.height - 24, 10, TEXT_DIM);
}

static void draw_name(Vector2 mp)
{
    static const char *titles[3] = { "NEW PROJECT", "RENAME PROJECT", "DUPLICATE PROJECT" };
    Rectangle b = save_box();
    draw_dialog_frame(b, titles[ui.nameAction]);
    txt(ui.nameAction == NAME_NEW ? "STARTS WITH AN EMPTY PATCH; THE CURRENT ONE IS SAVED"
        : ui.nameAction == NAME_DUPLICATE ? "CONTINUE IN A COPY; THE ORIGINAL STAYS AS IT IS" : "GIVE THIS PROJECT A NEW NAME",
        b.x + 16, b.y + 40, 10, TEXT_DIM);
    draw_field(save_field(), ui.text, "PROJECT NAME");
    draw_button(save_cancel(), "CANCEL", mp, false);
    draw_button(save_ok(), ui.nameAction == NAME_RENAME ? "RENAME" : "CREATE", mp, ui.text[0] != 0);
}

static void draw_rec(Vector2 mp)
{
    Rectangle b = save_box();
    draw_dialog_frame(b, "RECORDING");
    txt(clock_text(g_recLen / SR), b.x + 16, b.y + 46, 20, ACCENT);
    txt("48 KHZ STEREO, FROM THE OUTPUT MODULES", b.x + 16, b.y + 78, 10, TEXT_DIM);
    txt("SAVE IT IN ONE OR BOTH FORMATS; CLOSING DISCARDS IT", b.x + 16, b.y + 100, 10, TEXT_DIM);
    draw_button(rec_btn(0), "SAVE WAV", mp, true);
    draw_button(rec_btn(1), "SAVE MP3", mp, true);
    draw_button(rec_btn(2), "CLOSE", mp, false);
}

// Project and transport buttons beside the brand.
static void draw_topbar(Vector2 mp)
{
    draw_button(btn_project(), TextFormat("PROJECT  %s", proj_name[0] ? proj_name : "..."), mp, false);
    bool running = g_running;
    draw_button(btn_run(), running ? "STOP" : "RUN", mp, !running);
    bool blink = fmodf((float)GetTime(), 1.0f) < 0.6f;
    Rectangle r = btn_rec();
    if (g_recording) {
        DrawRectangleRec(r, fade(CABLE_COLORS[1], 0.18f));
        DrawRectangleLinesEx(r, 1, CABLE_COLORS[1]);
        if (blink) DrawCircleV((Vector2){ r.x + 12, r.y + r.height / 2 }, 3.5f, CABLE_COLORS[1]);
        txt_c(TextFormat("REC %s", clock_text(g_recLen / SR)), r.x + r.width / 2 + 6, r.y + 8, 10, CABLE_COLORS[1]);
    } else draw_button(r, "REC AUDIO", mp, false);
    r = btn_video();
    if (ui.video) {
        DrawRectangleRec(r, fade(CABLE_COLORS[1], 0.18f));
        DrawRectangleLinesEx(r, 1, CABLE_COLORS[1]);
        if (blink) DrawCircleV((Vector2){ r.x + 12, r.y + r.height / 2 }, 3.5f, CABLE_COLORS[1]);
        txt_c(TextFormat("VIDEO %s", clock_text(GetTime() - ui.videoT)), r.x + r.width / 2 + 6, r.y + 8, 10, CABLE_COLORS[1]);
    } else draw_button(r, "REC VIDEO", mp, false);
}

// ---------------------------------------------------------------------------
// Audio files arriving from the file picker or by drag and drop
// ---------------------------------------------------------------------------

static void handle_audio_files(Vector2 mp)
{
#if defined(PLATFORM_WEB)
    if (g_pickTarget >= 0) {
        const char *path = emscripten_run_script_string("rayaTakePicked()");
        if (path[0]) {
            Module *m = &mods[g_pickTarget];
            if (m->alive && m->type == M_TAPE) tape_load(m, path);
            if (m->alive && m->type == M_SAMPLER) sampler_load(m, path);
            remove(path);
            g_pickTarget = -1;
        }
    }
#endif
    if (!IsFileDropped()) return;
#if defined(PLATFORM_WEB)
    mp = (Vector2){ (float)emscripten_run_script_int("Module.rayaDropX | 0"), (float)emscripten_run_script_int("Module.rayaDropY | 0") };
#endif
    FilePathList dropped = LoadDroppedFiles();
    int target = module_at(mp);
    if (target >= 0 && mods[target].type != M_TAPE && mods[target].type != M_SAMPLER && mods[target].type != M_WAVOR) target = -1;
    for (unsigned int i = 0; i < dropped.count; i++) {
        int t = target;
        if (t < 0 || i > 0)   // dropped on empty space: a new tape for each file
            t = add_module(M_TAPE, mp.x - pan.x + i * 24, mp.y - pan.y + i * 24);
        if (t < 0) continue;
        if (mods[t].type == M_WAVOR) wv_load_audio(&mods[t], dropped.paths[i]);
        else if (mods[t].type == M_SAMPLER) sampler_load(&mods[t], dropped.paths[i]);
        else tape_load(&mods[t], dropped.paths[i]);
    }
    UnloadDroppedFiles(dropped);
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
    if (!rack_ready && fs_ready()) { rack_load(); project_boot(); }
    project_tick();
    for (int i = 0; i < MAX_MODULES; i++)   // pre-render drum kits a little each frame
        if (mods[i].alive && mods[i].type == M_DRUMS) {
            for (int k = 0; k < N_KITS; k++) if (!kits[k].ready) { kit_work(k, 0.003); break; }
            break;
        }
    for (int i = 0; i < MAX_MODULES; i++)   // build changed wavetables a little each frame
        if (mods[i].alive && mods[i].type == M_WAVOR) wv_frame(&mods[i]);
    handle_audio_files(GetMousePosition());
    handle_input();
    if (ui.recOn && !g_recording) {   // stopped by hand or full: offer to save it
        ui.recOn = false;
        ui.menu = false;
        ui.modal = g_recLen > 0 ? MODAL_REC : MODAL_NONE;
    }
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
    draw_topbar(bmp);
    bool empty = true;
    for (int i = 0; i < MAX_MODULES && empty; i++) empty = !mods[i].alive;
    if (empty && proj_ready) {
        txt_c("AN EMPTY PATCH", GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f - 20, 20, fade(TEXT, 0.7f));
        txt_c("RIGHT-CLICK TO ADD A MODULE, OR PRESS P TO LOAD A PRESET", GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f + 12, 10, TEXT_DIM);
    }
    draw_minimap(ui.modal || ui.menu ? (Vector2){ -1, -1 } : mp);
    int ns = selection_count();
    if (ns) draw_button(btn_save(), TextFormat("SAVE %d MODULE%s AS PRESET", ns, ns == 1 ? "" : "S"), bmp, true);
    txt("SPACE: RUN/STOP   R: REC AUDIO   V: REC VIDEO   O: PROJECTS   P: PRESETS   S: SAVE SELECTION   DRAG JACK TO JACK: PATCH   RIGHT-CLICK JACK: UNPLUG   RIGHT-CLICK SPACE: ADD   SHIFT-DRAG: SELECT   DROP AUDIO FILE: TAPE (ON WAVOR: A CYCLE)",
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
    if (ui.modal == MODAL_PROJECTS) draw_projects(mp);
    if (ui.modal == MODAL_NAME) draw_name(mp);
    if (ui.modal == MODAL_REC) draw_rec(mp);

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
    preset_instantiate(text, (Vector2){ 24, 70 }, false);
    free(text);
    select_none();
}

int main(void)
{
    parse_progressions();
    parse_phrases();
    for (int i = 0; i < N_KITS; i++) KIT_NAMES[i] = KITS[i].name;

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

    rebuild_routing();   // the patch arrives with the project once storage is ready

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
