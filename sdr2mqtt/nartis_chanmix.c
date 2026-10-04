/*
 * nartis_chanmix: cut several narrow channels out of a wideband rtl_sdr stream,
 * shift each to 0 Hz, squelch-gate them and pass the strongest active one
 * as a single 256 kS/s cu8 stream for rtl_433.
 *
 * Nartis D101 display and meter use 4 channels spread over ~1.4 MHz, while
 * rtl_433's FSK demodulator only works well on a narrow (~250k) band around
 * the tuned frequency. Frames never overlap in time, so one output stream
 * carries all of them, centered. A strong burst raises the other channels
 * too (ADC intermodulation), hence "strongest wins" instead of a sum.
 *
 *   rtl_sdr -f 434000000 -s 2048000 - | nartis_chanmix 434000000 \
 *       433814000 433293600 434253400 434693600 433950000:aux | \
 *   rtl_433 -r cu8:- -s 256k ...
 *
 * Channel "freq" is a narrow (+-25 kHz) channel, "freq:aux" a wide (+-80 kHz)
 * low priority channel (e.g. for other 433 MHz sensors): it is muted while
 * any narrow channel is active.
 *
 * Input must be 2048 kS/s (decimation 8 -> 256 kS/s).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS_IN 2048000.0
#define D1 4        // boxcar 2048k -> 512k
#define NTAPS 48    // FIR at 512k, decimate 2 -> 256k
#define BLK 256     // output samples per squelch block (1 ms)
#define MAXCH 8
#define OPEN_RATIO 10.0f // 10 dB above noise floor
#define HANG 3      // blocks to keep gate open
#define FLOOR_RISE 1.00115f // per block: +10 dB in 2 s

typedef struct {
    double freq;
    int aux;
    float h[NTAPS];
    // NCO
    double ph_re, ph_im, w_re, w_im;
    // FIR history at 512k (double length ring for contiguous reads)
    float hist_re[2 * NTAPS], hist_im[2 * NTAPS];
    int hpos;
    // squelch
    float floor;
    int hang;
    int open_prev;
    float ratio;
    float prev_re[BLK], prev_im[BLK];
    float cur_re[BLK], cur_im[BLK];
} chan_t;

static void design_lowpass(float *h, double cutoff, double fs)
{
    double sum = 0;
    for (int i = 0; i < NTAPS; ++i) {
        double k  = i - (NTAPS - 1) / 2.0;
        double x  = 2 * cutoff / fs * k;
        double s  = x == 0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
        double wn = 0.54 - 0.46 * cos(2 * M_PI * i / (NTAPS - 1));
        h[i]      = (float)(s * wn);
        sum += h[i];
    }
    for (int i = 0; i < NTAPS; ++i) {
        h[i] /= (float)sum;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <center_hz> <ch_hz>[:aux] ...  (stdin cu8 @2048k -> stdout cu8 @256k)\n", argv[0]);
        return 1;
    }
    double center = atof(argv[1]);
    int debug     = getenv("CHANMIX_DEBUG") != NULL;
    int nch       = 0;
    static chan_t ch[MAXCH];
    for (int a = 2; a < argc && nch < MAXCH; ++a, ++nch) {
        chan_t *c = &ch[nch];
        memset(c, 0, sizeof(*c));
        c->freq = atof(argv[a]);
        c->aux  = strstr(argv[a], ":aux") != NULL;
        design_lowpass(c->h, c->aux ? 80e3 : 25e3, FS_IN / D1);
        double w = -2 * M_PI * (c->freq - center) / FS_IN;
        c->w_re = cos(w), c->w_im = sin(w);
        c->ph_re = 1, c->ph_im = 0;
        c->floor = -1;
        fprintf(stderr, "nartis_chanmix: channel %.4f MHz%s (offset %+.1f kHz)\n",
                c->freq / 1e6, c->aux ? " aux" : "", (c->freq - center) / 1e3);
    }

    static uint8_t in[BLK * 8 * 2];
    static uint8_t out[BLK * 2];
    static float lut[256];
    for (int i = 0; i < 256; ++i) {
        lut[i] = (i - 127.5f) / 128.0f;
    }

    setvbuf(stdout, NULL, _IOFBF, sizeof(out));
    while (fread(in, 1, sizeof(in), stdin) == sizeof(in)) {
        static int active = -1;
        for (int k = 0; k < nch; ++k) {
            chan_t *c = &ch[k];
            double pr = c->ph_re, pi = c->ph_im;
            float power = 0;
            for (int o = 0; o < BLK; ++o) {
                // two 512k samples per output sample
                for (int half = 0; half < 2; ++half) {
                    float acc_re = 0, acc_im = 0;
                    uint8_t const *p = &in[(o * 8 + half * 4) * 2];
                    for (int j = 0; j < D1; ++j) {
                        float xr = lut[p[2 * j]], xi = lut[p[2 * j + 1]];
                        acc_re += (float)(xr * pr - xi * pi);
                        acc_im += (float)(xr * pi + xi * pr);
                        double t = pr * c->w_re - pi * c->w_im;
                        pi       = pr * c->w_im + pi * c->w_re;
                        pr       = t;
                    }
                    c->hpos = (c->hpos + 1) % NTAPS;
                    c->hist_re[c->hpos] = c->hist_re[c->hpos + NTAPS] = acc_re / D1;
                    c->hist_im[c->hpos] = c->hist_im[c->hpos + NTAPS] = acc_im / D1;
                }
                float yr = 0, yi = 0;
                float const *hr = &c->hist_re[c->hpos + 1];
                float const *hi = &c->hist_im[c->hpos + 1];
                for (int t = 0; t < NTAPS; ++t) {
                    yr += c->h[t] * hr[t];
                    yi += c->h[t] * hi[t];
                }
                c->cur_re[o] = yr;
                c->cur_im[o] = yi;
                power += yr * yr + yi * yi;
            }
            // renormalize NCO
            double mag = sqrt(pr * pr + pi * pi);
            c->ph_re = pr / mag, c->ph_im = pi / mag;

            power /= BLK;
            // noise floor: follows minima at once, rises slowly (+10 dB in ~2 s,
            // longer than any frame) so a stuck carrier eventually closes
            if (c->floor < 0 || power < c->floor) {
                c->floor = power;
            }
            else {
                c->floor *= FLOOR_RISE;
            }
            c->ratio  = c->floor > 0 ? power / c->floor : 0;
            int above = c->ratio > OPEN_RATIO;
            if (above) {
                c->hang = HANG;
            }
            else if (c->hang > 0) {
                c->hang--;
            }
            int open = above || c->hang > 0;
            // emit previous block if it or the current one is open (1 block pre-roll)
            c->open_prev = (c->open_prev << 1 | open) & 3;
        }

        // a narrow channel 10 dB stronger than the active one takes over
        if (active >= 0 && ch[active].open_prev) {
            for (int k = 0; k < nch; ++k) {
                if (!ch[k].aux && ch[k].open_prev && ch[k].ratio > OPEN_RATIO * ch[active].ratio) {
                    active = k;
                }
            }
        }

        // keep the active channel while it stays open, else pick the strongest
        // open one (narrow channels take precedence over aux ones)
        if (active < 0 || !ch[active].open_prev || (ch[active].aux && !ch[active].hang)) {
            int best = -1;
            for (int k = 0; k < nch; ++k) {
                if (!ch[k].open_prev) {
                    continue;
                }
                if (best < 0 || (ch[best].aux && !ch[k].aux) ||
                        (ch[best].aux == ch[k].aux && ch[k].ratio > ch[best].ratio)) {
                    best = k;
                }
            }
            active = best;
        }
        else if (ch[active].aux) {
            // a stronger narrow channel preempts an aux one (not mere intermodulation)
            for (int k = 0; k < nch; ++k) {
                if (!ch[k].aux && ch[k].open_prev && ch[k].ratio > ch[active].ratio) {
                    active = k;
                    break;
                }
            }
        }

        static int last_active = -2;
        static long nblk = 0;
        nblk++;
        if (debug && active != last_active) {
            fprintf(stderr, "%8.3fs active=%d (%s) ratio=%.1f\n", nblk * BLK / 256000.0, active,
                    active >= 0 ? argv[2 + active] : "-", active >= 0 ? ch[active].ratio : 0.0f);
        }
        last_active = active;

        for (int o = 0; o < BLK; ++o) {
            float sr = 0, si = 0;
            if (active >= 0) {
                sr = ch[active].prev_re[o];
                si = ch[active].prev_im[o];
            }
            int vr = (int)lrintf(sr * 128.0f + 127.5f);
            int vi = (int)lrintf(si * 128.0f + 127.5f);
            out[2 * o]     = (uint8_t)(vr < 0 ? 0 : vr > 255 ? 255 : vr);
            out[2 * o + 1] = (uint8_t)(vi < 0 ? 0 : vi > 255 ? 255 : vi);
        }
        for (int k = 0; k < nch; ++k) {
            memcpy(ch[k].prev_re, ch[k].cur_re, sizeof(ch[k].cur_re));
            memcpy(ch[k].prev_im, ch[k].cur_im, sizeof(ch[k].cur_im));
        }
        if (fwrite(out, 1, sizeof(out), stdout) != sizeof(out)) {
            return 0;
        }
    }
    return 0;
}
