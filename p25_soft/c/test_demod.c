// src/services/p25_demod.c on the PC: a recording (16-bit 8 kHz raw) in, the dibits out (a byte each, 0 - 3; 255 the
// signal lost), the counters on stdout.
//   test_demod in.raw out.dibits
#include <stdio.h>
#include <stdlib.h>

#include "p25_demod.h"

static p25_demod d;
static FILE *out;

static void cb(void *ctx, int dibit)
{
    (void)ctx;
    fputc((dibit < 0) ? 255 : dibit, out);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "test_demod in.raw out.dibits\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    out = fopen(argv[2], "wb");
    if (!in || !out) {
        return 1;
    }
    p25_demod_init(&d, cb, NULL);
    int16_t x[128];
    size_t n;
    while ((n = fread(x, 2, 128, in)) > 0) {
        p25_demod_samples(&d, x, (int)n);
    }
    printf("syncs %u, lost %u, relocks %u, symbols %u\n", (unsigned)d.syncs, (unsigned)d.lost, (unsigned)d.relocks,
           (unsigned)d.symbols);
    fclose(out);
    return 0;
}
