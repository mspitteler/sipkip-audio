/* Copyright (c) 2013 Jean-Marc Valin */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* This is meant to be a simple example of encoding and decoding audio
   using Opus. It should make it easy to understand how the Opus API
   works. For more information, see the full API documentation at:
   https://www.opus-codec.org/docs/ */

/**
 * Needs libopus-dev
 */

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <opus/opus.h>
#include <stdio.h>

/*The frame size is hardcoded for this sample code but it doesn't have to be*/
#define FRAME_SIZE 960
#define SAMPLE_RATE 48000
#define CHANNELS 1
#define APPLICATION OPUS_APPLICATION_AUDIO
#define BITRATE 16000

#define MAX_FRAME_SIZE 6*960
#define MAX_PACKET_SIZE (3*1276)

int main(int argc, char **argv) {
    char *in_file;
    FILE *fin;
    char *in_file_packets;
    FILE *fin_packets;
    char *out_file;
    FILE *fout;
    opus_int16 in[FRAME_SIZE * CHANNELS];
    opus_int16 out[MAX_FRAME_SIZE * CHANNELS];
    unsigned char encoded_bytes[MAX_PACKET_SIZE];
    short n_encoded_bytes;
    /*Holds the state of the decoder */
    OpusDecoder *decoder;
    int err;

    if (argc != 4) {
        fprintf(stderr, "usage: %s input.opus input.opus_packets output.pcm\n", argv[0]);
        fprintf(stderr, "output is a 16-bit little-endian raw file\n");
        return EXIT_FAILURE;
    }

    in_file = argv[1];
    fin = fopen(in_file, "r");
    if (fin == NULL) {
        fprintf(stderr, "failed to open input file: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    in_file_packets = argv[2];
    fin_packets = fopen(in_file_packets, "r");
    if (fin_packets == NULL) {
        fprintf(stderr, "failed to open input file: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    /* Create a new decoder state. */
    decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
    if (err < 0) {
        fprintf(stderr, "failed to create decoder: %s\n", opus_strerror(err));
        return EXIT_FAILURE;
    }
    out_file = argv[3];
    fout = fopen(out_file, "w");
    if (fout == NULL) {
        fprintf(stderr, "failed to open output file: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    while (1) {
        int i;
        unsigned char pcm_bytes[MAX_FRAME_SIZE * CHANNELS * 2];
        int frame_size;
        /* Read a 16 bits/sample audio frame. */
        fread(&n_encoded_bytes, sizeof(short), 1, fin_packets);
        if (feof(fin_packets))
            break;
        fread(encoded_bytes, 1, n_encoded_bytes, fin);
        /* Decode the data. In this example, frame_size will be constant because
           the encoder is using a constant frame size. However, that may not
           be the case for all encoders, so the decoder must always check
           the frame size returned. */
        frame_size = opus_decode(decoder, encoded_bytes, n_encoded_bytes, out, MAX_FRAME_SIZE, 0);
        if (frame_size < 0) {
            fprintf(stderr, "decoder failed: %s\n", opus_strerror(frame_size));
            return EXIT_FAILURE;
        }

        /* Convert to little-endian ordering. */
        for (i = 0; i < CHANNELS * frame_size; i++) {
            pcm_bytes[2 * i] = out[i] & 0xFF;
            pcm_bytes[2 * i + 1] = (out[i] >> 8) & 0xFF;
        }
        /* Write the decoded audio to file. */
        fwrite(pcm_bytes, sizeof(short), frame_size * CHANNELS, fout);
    }

    /*Destroy the encoder state*/
    opus_decoder_destroy(decoder);
    fclose(fin);
    fclose(fin_packets);
    fclose(fout);
    return EXIT_SUCCESS;
}
