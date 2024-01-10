/******************************************************************************
 *
 *  Copyright 2022 Google LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define _POSIX_C_SOURCE 199309L

#include <stdalign.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>

#include <lc3.h>
#include "lc3bin.h"
#include "wave.h"

#define MAX_CHANNELS 2

#ifndef MIN
#define MIN(a, b)  ( (a) < (b) ? (a) : (b) )
#endif

#ifndef MAX
#define MAX(a, b)  ( (a) > (b) ? (a) : (b) )
#endif


/**
 * Error handling
 */

static void error(int status, const char *format, ...)
{
    va_list args;

    fflush(stdout);

    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, status ? ": %s\n" : "\n", strerror(status));
    exit(status);
}


/**
 * Parameters
 */

struct parameters {
    const char *fname_in;
    const char *fname_out;
    int bitdepth;
    int srate_hz;
};

static struct parameters parse_args(int argc, char *argv[])
{
    static const char *usage =
        "Usage: %s [in_file] [wav_file]\n"
        "\n"
        "wav_file\t"  "Input wave file, stdin if omitted\n"
        "out_file\t"  "Output bitstream file, stdout if omitted\n"
        "\n"
        "Options:\n"
        "\t-h\t"     "Display help\n"
        "\t-b\t"     "Output bitdepth, 16 bits (default) or 24 bits\n"
        "\t-r\t"     "Output samplerate, default is LC3 stream samplerate\n"
        "\n";

    struct parameters p = { .bitdepth = 16 };

    for (int iarg = 1; iarg < argc; ) {
        const char *arg = argv[iarg++];

        if (arg[0] == '-') {
            if (arg[2] != '\0')
                error(EINVAL, "Option %s", arg);

            char opt = arg[1];
            const char *optarg = NULL;

            switch (opt) {
                case 'b': case 'r':
                    if (iarg >= argc)
                        error(EINVAL, "Argument %s", arg);
                    optarg = argv[iarg++];
            }

            switch (opt) {
                case 'h': fprintf(stderr, usage, argv[0]); exit(0);
                case 'b': p.bitdepth = atoi(optarg); break;
                case 'r': p.srate_hz = atoi(optarg); break;
                default:
                    error(EINVAL, "Option %s", arg);
            }

        } else {

            if (!p.fname_in)
                p.fname_in = arg;
            else if (!p.fname_out)
                p.fname_out = arg;
            else
                error(EINVAL, "Argument %s", arg);
        }
    }

    return p;
}

/**
 * Return time in (us) from unspecified point in the past
 */
static unsigned clock_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);

    return (unsigned)(ts.tv_sec * 1000*1000) + (unsigned)(ts.tv_nsec / 1000);
}

/**
 * Entry point
 */
int main(int argc, char *argv[])
{
    /* --- Read parameters --- */

    struct parameters p = parse_args(argc, argv);
    FILE *fp_in = stdin, *fp_out = stdout;

    if (p.fname_in && (fp_in = fopen(p.fname_in, "rb")) == NULL)
        error(errno, "%s", p.fname_in);

    if (p.fname_out && (fp_out = fopen(p.fname_out, "wb")) == NULL)
        error(errno, "%s", p.fname_out);

    if (p.bitdepth && p.bitdepth != 16 && p.bitdepth != 24)
        error(EINVAL, "Bitdepth %d", p.bitdepth);

    /* --- Check parameters --- */

    int frame_us, srate_hz, nchannels, nsamples;
    bool hrmode;

    if (lc3bin_read_header(fp_in,
            &frame_us, &srate_hz, &hrmode, &nchannels, &nsamples) < 0)
        error(EINVAL, "LC3 binary input file");

    if (nchannels < 1 || nchannels > MAX_CHANNELS)
        error(EINVAL, "Number of channels %d", nchannels);

    if (!LC3_CHECK_DT_US(frame_us))
        error(EINVAL, "Frame duration");

    if (!LC3_HR_CHECK_SR_HZ(hrmode, srate_hz))
         error(EINVAL, "Samplerate %d Hz", srate_hz);

    if (p.srate_hz && (!LC3_HR_CHECK_SR_HZ(hrmode, p.srate_hz) ||
                       p.srate_hz < srate_hz                     ))
         error(EINVAL, "Output samplerate %d Hz", p.srate_hz);

    int pcm_sbits = p.bitdepth;
    int pcm_sbytes = pcm_sbits / 8;

    int pcm_srate_hz = !p.srate_hz ? srate_hz : p.srate_hz;
    int pcm_samples = !p.srate_hz ? nsamples :
        ((int64_t)nsamples * pcm_srate_hz) / srate_hz;

    wave_write_header(fp_out,
          pcm_sbits, pcm_sbytes, pcm_srate_hz, nchannels, pcm_samples);

    /* --- Setup decoding --- */

    uint8_t in[2 * LC3_HR_MAX_FRAME_BYTES];
    int8_t alignas(int32_t) pcm[2 * LC3_HR_MAX_FRAME_SAMPLES*4];
    lc3_decoder_t dec[2];

    int frame_samples = lc3_hr_frame_samples(hrmode, frame_us, pcm_srate_hz);
    int encode_samples = pcm_samples +
        lc3_hr_delay_samples(hrmode, frame_us, pcm_srate_hz);
    enum lc3_pcm_format pcm_fmt =
        pcm_sbits == 24 ? LC3_PCM_FORMAT_S24_3LE : LC3_PCM_FORMAT_S16;

    for (int ich = 0; ich < nchannels; ich++) {
        dec[ich] = lc3_hr_setup_decoder(
            hrmode, frame_us, srate_hz, p.srate_hz,
            malloc(lc3_hr_decoder_size(hrmode, frame_us, pcm_srate_hz)));

        if (!dec[ich])
            error(EINVAL, "Decoder initialization failed");
    }

    /* --- Decoding loop --- */

    static const char *dash_line = "========================================";

    int nsec = 0;
    int nerr = 0;
    unsigned t0 = clock_us();

    for (int i = 0; i * frame_samples < encode_samples; i++) {

        int block_bytes = lc3bin_read_data(fp_in, nchannels, in);

        if (floorf(i * frame_us * 1e-6) > nsec) {

            float progress = fminf((float)i * frame_samples / pcm_samples, 1);

            fprintf(stderr, "%02d:%02d [%-40s]\r",
                    nsec / 60, nsec % 60,
                    dash_line + (int)floorf((1 - progress) * 40));

            nsec = rint(i * frame_us * 1e-6);
        }

        if (block_bytes <= 0)
            memset(pcm, 0, nchannels * frame_samples * pcm_sbytes);
        else {
            const uint8_t *in_ptr = in;
            for (int ich = 0; ich < nchannels; ich++) {
                int frame_bytes = block_bytes / nchannels
                    + (ich < block_bytes % nchannels);

                int res = lc3_decode(dec[ich], in_ptr, frame_bytes,
                    pcm_fmt, pcm + ich * pcm_sbytes, nchannels);

                nerr += (res != 0);
                in_ptr += frame_bytes;
            }
        }

        int pcm_offset = i > 0 ? 0 : encode_samples - pcm_samples;
        int pcm_nwrite = MIN(frame_samples - pcm_offset,
            encode_samples - i*frame_samples);

        wave_write_pcm(fp_out,
            pcm_sbytes, pcm, nchannels, pcm_offset, pcm_nwrite);
    }

    unsigned t = (clock_us() - t0) / 1000;
    nsec = nsamples / srate_hz;

    fprintf(stderr, "%02d:%02d Decoded in %d.%03d seconds %20s\n",
        nsec / 60, nsec % 60, t / 1000, t % 1000, "");

    if (nerr)
        fprintf(stderr, "Warning: Decoding of %d frames failed!\n", nerr);

    /* --- Cleanup --- */

    for (int ich = 0; ich < nchannels; ich++)
        free(dec[ich]);

    if (fp_in != stdin)
        fclose(fp_in);

    if (fp_out != stdout)
        fclose(fp_out);
}
