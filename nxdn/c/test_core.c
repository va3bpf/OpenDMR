/* PC test of src/services/nxdn_core.c on the radio's FM recordings: raw 16-bit little-endian 8 kHz samples (from
   the .af.npy files, nxdn/c/run.py) in blocks of 256 as the radio gives them; per frame the LICH, SACCH, VCH. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxdn_core.h"

static const uint8_t TONE[9] = {0xce, 0xa8, 0xfe, 0x83, 0xac, 0xc4, 0x58, 0x20, 0x0a}; // the 1031 Hz test vector
static int frames, lich_ok, sacch_ok, vch_n, vch_tone, facch_ok, messages;
static nxdn_sacch_super super;
static int verbose;

static void on_frame(void *ctx, const nxdn_frame_t *f)
{
    (void)ctx;
    frames++;
    lich_ok += f->lich_ok;
    sacch_ok += f->sacch_ok;
    static FILE *vch_out;
    if (!vch_out && getenv("NXDN_VCH_OUT")) {
        vch_out = fopen(getenv("NXDN_VCH_OUT"), "ab");
    }
    for (int h = 0; h < 2; h++) {
        if (vch_out && (f->halves & (1 << h))) {
            fwrite(f->vch[2 * h], 9, 2, vch_out);
            fflush(vch_out);
        }
        if (f->halves & (1 << h)) {
            for (int k = 0; k < 2; k++) {
                vch_n++;
                vch_tone += memcmp(f->vch[2 * h + k], TONE, 9) == 0;
            }
        }
        if (f->facch_ok & (1 << h)) {
            facch_ok++;
        }
    }
    uint8_t msg[9];
    if (nxdn_sacch_add(&super, f, msg)) {
        uint8_t type;
        uint16_t src, dst;
        messages++;
        if (verbose && nxdn_msg_call(msg, 9, &type, &src, &dst)) {
            printf("  SACCH message: type %d, %u -> %u\n", type, src, dst);
        }
    }
    if (verbose) {
        printf("  frame: corr %.2f level %.0f LICH %02X %s SACCH SR %02X %s halves %d\n", f->fsw_corr / 4096.0, (double)f->level, f->lich,
               f->lich_ok ? "ok" : "BAD", f->sacch_sr, f->sacch_ok ? "ok" : "BAD", f->halves);
    }
}

int main(int argc, char **argv)
{
    if (getenv("NXDN_OPT")) {
        nxdn_rx_opt = atoi(getenv("NXDN_OPT"));
    }
    if (getenv("NXDN_LP")) {
        nxdn_rx_lp_hz = atoi(getenv("NXDN_LP"));
    }
    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "-v") == 0) {
            verbose = 1;
            continue;
        }
        FILE *fp = fopen(argv[a], "rb");
        if (!fp) {
            perror(argv[a]);
            return 1;
        }
        static int16_t buf[1 << 20];
        int n = (int)fread(buf, 2, sizeof(buf) / 2, fp);
        fclose(fp);
        static nxdn_rx rx;
        nxdn_rx_init(&rx, on_frame, NULL);
        frames = lich_ok = sacch_ok = vch_n = vch_tone = facch_ok = messages = 0;
        memset(&super, 0, sizeof(super));
        for (int i = 0; i < n; i += 256) {
            nxdn_rx_samples(&rx, &buf[i], (n - i) < 256 ? (n - i) : 256);
        }
        printf("%s: %d samples, frames %d, LICH ok %d, SACCH ok %d, VCH %d (1031 Hz %d), FACCH1 ok %d, SACCH messages %d,"
               " hunts %u, lost %u, next t0 %.2f, period %.4f\n", argv[a], n, frames, lich_ok, sacch_ok, vch_n, vch_tone, facch_ok, messages,
               rx.hunts, rx.lost_sync, rx.t0 / 4096.0, rx.period / 4096.0);
    }
    return 0;
}
