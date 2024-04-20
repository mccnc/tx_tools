/** @file
    tx_tools - pulse_beep, beep pulse I/Q waveform generator.

    Copyright (C) 2021 by Christian Zuckschwerdt <zany@triq.net>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pulse_text.h"
#include "iq_render.h"
#include "sample.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#ifdef _MSC_VER
#include "getopt/getopt.h"
#define F_OK 0
#endif
#endif
#ifndef _MSC_VER
#include <unistd.h>
#include <getopt.h>
#endif

#include <time.h>

#include "optparse.h"

static void print_version(void)
{
    fprintf(stderr, "pulse_beep version 0.1\n");
    fprintf(stderr, "Use -h for usage help and see https://triq.org/ for documentation.\n");
}

__attribute__((noreturn))
static void usage(int exitcode)
{
    fprintf(stderr,
            "\npulse_beep, beep pulse I/Q waveform generator\n\n"
            "Usage:"
            "\t[-h] Output this usage help and exit\n"
            "\t[-V] Output the version string and exit\n"
            "\t[-v] Increase verbosity (can be used multiple times).\n"
            "\t[-s sample_rate (default: 2048000 Hz)]\n"
            "\t[-f frequency Hz] add new beep frequency\n"
            "\t[-a attenuation dB] set beep attenuation\n"
            "\t[-l time ms] set beep length\n"
            "\t[-i time ms] set beep interval\n"
            "\t[-n noise floor dBFS or multiplier]\n"
            "\t[-N noise on signal dBFS or multiplier]\n"
            "\t Noise level < 0 for attenuation in dBFS, otherwise amplitude multiplier, 0 is off.\n"
            "\t[-g signal gain dBFS or multiplier]\n"
            "\t Gain level < 0 for attenuation in dBFS, otherwise amplitude multiplier, 0 is 0 dBFS.\n"
            "\t Levels as dbFS or multiplier are peak values, e.g. 0 dB or 1.0 x are equivalent to -3 dB RMS.\n"
            "\t[-W filter ratio]\n"
            "\t[-G step width in us]\n"
            "\t[-b output_block_size (default: 16 * 16384) bytes]\n"
            "\t[-S rand_seed] set random seed for reproducible output\n"
            "\t[-M full_scale] limit the output full scale, e.g. use -F 2048 with CS16\n"
            "\t[-w file] write samples to file ('-' writes to stdout)\n\n");
    exit(exitcode);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
    if (CTRL_C_EVENT == signum) {
        fprintf(stderr, "Signal caught, exiting!\n");
        abort_render = 1;
        return TRUE;
    }
    return FALSE;
}
#else
static void sighandler(int signum)
{
    fprintf(stderr, "Signal caught, exiting!\n");
    abort_render = 1;
}
#endif

typedef struct beep {
    int freq;
    int att;
    int len;
    int intv;
    int next;
} beep_t;

int main(int argc, char **argv)
{
    int verbosity = 0;

    char *wr_filename = NULL;

    iq_render_t spec = {0};
    iq_render_defaults(&spec);

    beep_t beeps[32] = {0};
    unsigned beeps_idx = 0;

    unsigned rand_seed = 1;

    print_version();

    int opt;
    while ((opt = getopt(argc, argv, "hVvs:f:a:l:i:n:N:g:W:G:b:r:w:t:M:S:")) != -1) {
        switch (opt) {
        case 'h':
            usage(0);
        case 'V':
            exit(0); // we already printed the version
        case 'v':
            verbosity++;
            break;
        case 's':
            spec.sample_rate = atodu_metric(optarg, "-s: ");
            break;
        case 'f':
            if (beeps[beeps_idx].freq)
                beeps_idx += 1;
            beeps[beeps_idx].freq = atoi_metric(optarg, "-f: ");
            break;
        case 'a':
            beeps[beeps_idx].att = atoi_metric(optarg, "-a: ");
            break;
        case 'l':
            beeps[beeps_idx].len = atoi_metric(optarg, "-l: ");
            break;
        case 'i':
            beeps[beeps_idx].intv = atoi_metric(optarg, "-i: ");
            break;
        case 'n':
            spec.noise_floor = atod_metric(optarg, "-n: ");
            break;
        case 'N':
            spec.noise_signal = atod_metric(optarg, "-N: ");
            break;
        case 'g':
            spec.gain = atod_metric(optarg, "-g: ");
            break;
        case 'W':
            spec.filter_wc = atodu_metric(optarg, "-W: ");
            break;
        case 'G':
            spec.step_width = atou_metric(optarg, "-G: ");
            break;
        case 'b':
            spec.frame_size = atou_metric(optarg, "-b: ");
            break;
        case 'w':
            wr_filename = optarg;
            break;
        case 'M':
            spec.full_scale = atof(optarg);
            break;
        case 'S':
            rand_seed = (unsigned)atoi(optarg);
            break;
        default:
            usage(1);
        }
    }

    if (argc > optind) {
        fprintf(stderr, "\nExtra arguments? \"%s\"...\n", argv[optind]);
        usage(1);
    }

    if (!wr_filename) {
        fprintf(stderr, "Output to stdout.\n");
        wr_filename = "-";
    }

    spec.sample_format = file_info(&wr_filename);
    if (verbosity)
        fprintf(stderr, "Output format %s.\n", sample_format_str(spec.sample_format));

    if (spec.frame_size < MINIMAL_BUF_LENGTH ||
            spec.frame_size > MAXIMAL_BUF_LENGTH) {
        fprintf(stderr, "Output block size wrong value, falling back to default\n");
        fprintf(stderr, "Minimal length: %u\n", MINIMAL_BUF_LENGTH);
        fprintf(stderr, "Maximal length: %u\n", MAXIMAL_BUF_LENGTH);
        spec.frame_size = DEFAULT_BUF_LENGTH;
    }

#ifndef _WIN32
    struct sigaction sigact;
    sigact.sa_handler = sighandler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGQUIT, &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);
#else
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)sighandler, TRUE);
#endif

    srand(rand_seed);

    fprintf(stderr, "Beeps: ");
    for (unsigned i = 0; i <= beeps_idx; ++i) {
        beep_t *p = &beeps[i];
        fprintf(stderr, "%i Hz at %i dB %i ms / %i ms; ", p->freq, p->att, p->len, p->intv);
    }
    fprintf(stderr, "\n");

    // gen_beeps(beeps);
    tone_t tones[30] = {0};
    unsigned tones_idx = 0;

    // start silence
    tones[tones_idx++] = (tone_t){
            .hz = 0,
            .db = -99,
            .us = 500 * 1000,
    };

    for (unsigned i = 0; i <= beeps_idx; ++i) {
        beep_t *p = &beeps[i];
        p->next = (int)((long long)p->intv * rand() / RAND_MAX) + 1;
    }

    while (tones_idx < 30 - 2) {
        // find next beep
        int gap = INT_MAX;
        beep_t *beep = NULL;
        for (unsigned i = 0; i <= beeps_idx; ++i) {
            beep_t *p = &beeps[i];
            if (p->next < gap) {
                gap = p->next;
                beep = p;
            }
        }

        // add silence
        tones[tones_idx++] = (tone_t){
                .hz = 0,
                .db = -99,
                .us = gap * 1000,
        };

        // add beep
        tones[tones_idx++] = (tone_t){
                .hz = beep->freq,
                .db = beep->att,
                .us = beep->len * 1000,
        };

        // advance
        for (unsigned i = 0; i <= beeps_idx; ++i) {
            beep_t *p = &beeps[i];
            if (p->next <= gap) {
                p->next = p->intv;
            } else {
                p->next -= gap;
            }
        }
    }

    if (verbosity > 1)
        output_tones(tones);

    if (verbosity) {
        size_t length_us = iq_render_length_us(tones);
        size_t length_smp = iq_render_length_smp(&spec, tones);
        fprintf(stderr, "Signal length: %zu us, %zu smp\n\n", length_us, length_smp);
    }

    iq_render_file(wr_filename, &spec, tones);
    // void *buf;
    // size_t len;
    // iq_render_buf(&spec, tones, &buf, &len);
    // free(buf);

    // free(tones);
}

/*
 ./pulse_beep -vv -s 2048k -W 0.4 -g -20 -w beeps_158.7560M_2048k.cu8 -f 159M -l 14 -i 4000 -f 159.5M -l 14 -i 3800 -f 158M -l 14 -i 3500
*/
