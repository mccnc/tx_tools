/** @file
    tx_tools - sdr_mix, SDR I/Q sample file mixer.

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
    fprintf(stderr, "sdr_mix version 0.1\n");
    fprintf(stderr, "Use -h for usage help and see https://triq.org/ for documentation.\n");
}

__attribute__((noreturn))
static void usage(int exitcode)
{
    fprintf(stderr,
            "\nsdr_mix, SDR I/Q sample file mixer\n\n"
            "Usage:"
            "\t[-h] Output this usage help and exit\n"
            "\t[-V] Output the version string and exit\n"
            "\t[-v] Increase verbosity (can be used multiple times).\n"
            "\t[-b block_size (default: 16 * 16384) bytes]\n"
            "\t[-r file] add a file to read samples from ('-' reads from stdin)\n"
            "\t[-g signal gain dBFS or multiplier] set attenuation for current file\n"
            "\t Gain level < 0 for attenuation in dBFS, otherwise amplitude multiplier,\n"
            "\t 1 is 0 dBFS, 0 is -inf dBFS.\n"
            "\t[-w file] write samples to file ('-' writes to stdout)\n\n");
    exit(exitcode);
}

typedef struct input {
    int fd;
    char *path;
    double gain;
} input_t;

int main(int argc, char **argv)
{
    int verbosity = 0;

    size_t frame_size = 0x40000;

    input_t inputs[32] = {0};
    unsigned inputs_idx = 0;

    char *wr_path = NULL;

    print_version();

    int opt;
    while ((opt = getopt(argc, argv, "hVvs:b:r:g:w:")) != -1) {
        switch (opt) {
        case 'h':
            usage(0);
        case 'V':
            exit(0); // we already printed the version
        case 'v':
            verbosity++;
            break;
        case 'b':
            frame_size = atou_metric(optarg, "-b: ");
            break;
        case 'r':
            if (inputs[inputs_idx].path)
                inputs_idx += 1;
            inputs[inputs_idx].path = optarg;
            inputs[inputs_idx].gain = 1.0;
            break;
        case 'g':
            inputs[inputs_idx].gain = atod_metric(optarg, "-g: ");
            break;
        case 'w':
            wr_path = optarg;
            break;
        default:
            usage(1);
        }
    }

    if (argc > optind) {
        fprintf(stderr, "\nExtra arguments? \"%s\"...\n", argv[optind]);
        usage(1);
    }

    if (!inputs[0].path) {
        fprintf(stderr, "No inputs.\n");
        exit(1);
    }

    if (!wr_path) {
        fprintf(stderr, "Output to stdout.\n");
        wr_path = "-";
    }

    for (unsigned i = 0; i <= inputs_idx; ++i) {
        int fd = open(inputs[i].path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "Failed to open input \"%s\" (%d).\n", inputs[i].path, fd);
            exit(1);
        }
        inputs[i].fd = fd;
    }

    int w_fd = open(wr_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (w_fd < 0) {
        fprintf(stderr, "Failed to open output \"%s\" (%d).\n", wr_path, w_fd);
        exit(1);
    }

    uint8_t *blk_cu8 = malloc(frame_size);
    int8_t *blk_cs8  = malloc(frame_size);

    while (1) {
        // read first input
        ssize_t r1s = read(inputs[0].fd, blk_cu8, frame_size);
        if (r1s < 0) {
            fprintf(stderr, "Failed to read input \"%s\" of %zu bytes (%zd).\n", inputs[0].path, frame_size, r1s);
            break;
        }
        size_t r1 = (size_t)r1s;
        size_t write_size = r1;

        // convert and attenuate
        if (inputs[0].gain == 1.0) {
            for (size_t k = 0; k < r1; ++k) {
                blk_cs8[k] = (int8_t)(blk_cu8[k] - 128);
            }
        }
        else {
            for (size_t k = 0; k < r1; ++k) {
                blk_cs8[k] = (int8_t)((blk_cu8[k] - 128) * inputs[0].gain);
            }
        }

        // zero remainder
        memset(&blk_cs8[r1], 0, frame_size - r1);

        // read next inputs
        for (unsigned i = 1; i <= inputs_idx; ++i) {
            ssize_t rns = read(inputs[i].fd, blk_cu8, frame_size);
            if (rns < 0) {
                fprintf(stderr, "Failed to read input \"%s\" of %zu bytes (%zd).\n", inputs[i].path, frame_size, rns);
                break;
            }
            size_t rn = (size_t)rns;
            if (rn > write_size) write_size = rn;

            // mix
            for (size_t k = 0; k < rn; ++k) {
                //blk_cs8[k] = abs(blk1[k]-128) > abs(blk2[k]-128) ? blk1[k] : blk2[k];
                blk_cs8[k] += (int8_t)((blk_cu8[k] - 128) * inputs[i].gain);
                //blk_cs8[k] = (blk_cs8[k] * (blk_cu8[k] - 128)) / 128;
            }
        }

        // convert
        for (size_t k = 0; k < write_size; ++k) {
            blk_cu8[k] = (uint8_t)(blk_cs8[k] + 128);
        }

        // write
        ssize_t w = write(w_fd, blk_cu8, write_size);
        if (w != (ssize_t)write_size) {
            fprintf(stderr, "Failed to write output \"%s\" of %zu bytes (%zd).\n", wr_path, write_size, w);
            break;
        }

        if (write_size != frame_size) {
            fprintf(stderr, "Done.\n");
            break;
        }
    }

    for (unsigned i = 0; i <= inputs_idx; ++i) {
        close(inputs[i].fd);
    }

    close(w_fd);

    free(blk_cu8);
    free(blk_cs8);
}
