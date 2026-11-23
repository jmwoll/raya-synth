// SPDX-License-Identifier: LGPL-2.1-or-later
// Render every DRUMS kit of raya.c to WAV files: <out>/<kit>/<nn>-<pad>.wav
// (48 kHz, 16-bit, mono). Used by `make drum-samples`.
//
// The sounds are synthesized clean-room approximations created by Claude
// Opus 5.5 from rough descriptions of each machine's hardware; nothing comes
// from real hardware or existing recordings. See README.md ("About these sounds").
//
// It compiles raya.c natively with its main() renamed and only calls the
// drum synthesis, so raylib is never linked; the linker is told to leave
// raylib's symbols unresolved.

#define main raya_main
#include "../raya.c"
#undef main

static void put32(FILE *f, unsigned v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, unsigned short v) { fwrite(&v, 2, 1, f); }

static void slug(const char *in, char *out)
{
    int n = 0;
    for (; *in && n < 63; in++) out[n++] = isalnum((unsigned char)*in) ? (char)tolower((unsigned char)*in) : '-';
    out[n] = 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "drum-samples";
    char path[512], kit[64], pad[64];
    for (int k = 0; k < N_KITS; k++) {
        slug(KITS[k].name, kit);
        snprintf(path, sizeof path, "mkdir -p '%s/%s'", dir, kit);
        if (system(path) != 0) return 1;
        for (int s = 0; s < 16; s++) {
            kit_render_slot(k, s);
            const Sample *smp = &kits[k].s[s];
            slug(drum_pad_name(k, s), pad);
            snprintf(path, sizeof path, "%s/%s/%02d-%s.wav", dir, kit, s + 1, pad);
            FILE *f = fopen(path, "wb");
            if (!f) return 1;
            fwrite("RIFF", 1, 4, f); put32(f, 36 + smp->frames * 2); fwrite("WAVEfmt ", 1, 8, f);
            put32(f, 16); put16(f, 1); put16(f, 1); put32(f, 48000); put32(f, 96000); put16(f, 2); put16(f, 16);
            fwrite("data", 1, 4, f); put32(f, smp->frames * 2);
            fwrite(smp->data, 2, smp->frames, f);
            fclose(f);
        }
        printf("%-11s -> %s/%s\n", KITS[k].name, dir, kit);
    }
    return 0;
}
