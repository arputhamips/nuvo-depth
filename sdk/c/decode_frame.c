/* Decode a single raw NUV1 file through the installed public ABI.
 * Build: cc decode_frame.c $(pkg-config --cflags --libs nuvo-depth) -o decode_frame
 * Usage: ./decode_frame sample.nuv
 * NVR1 and the minimal CLI's length-prefixed stream are different containers.
 */
#include <nuvo/depth.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "Usage: %s single-frame.nuv\n", argv[0]); return 2; }
    FILE *input = fopen(argv[1], "rb");
    if (!input) { perror("Open frame"); return 2; }
    nuvo_stream_decoder *decoder = nuvo_stream_decoder_create(65536, 64ULL * 1024 * 1024);
    if (!decoder) { fclose(input); fprintf(stderr, "Cannot allocate bounded decoder\n"); return 2; }
    unsigned char buffer[4096]; size_t count; int result = 1;
    while ((count = fread(buffer, 1, sizeof buffer, input)) != 0) {
        size_t consumed = nuvo_stream_decoder_feed(decoder, buffer, count);
        if (consumed != count || nuvo_stream_decoder_last_error(decoder)[0]) {
            fprintf(stderr, "Invalid frame or trailing data: %s\n", nuvo_stream_decoder_last_error(decoder)); goto done;
        }
    }
    if (ferror(input) || !nuvo_stream_decoder_ready(decoder)) { fprintf(stderr, "Incomplete frame\n"); goto done; }
    size_t size = 0;
    const uint8_t *metadata = nuvo_stream_decoder_metadata(decoder, &size);
    printf("ABI %u; kind %u; capture %" PRIu64 "\n", nuvo_depth_abi_version(),
           (unsigned)nuvo_stream_decoder_kind(decoder), nuvo_stream_decoder_sequence(decoder));
    fwrite(metadata, 1, size, stdout); putchar('\n');
    puts("Inspect products, float32_le metres, validity, calibration authority and run/capture identity in this metadata before interpreting payload bytes.");
    result = 0;
done:
    nuvo_stream_decoder_destroy(decoder); fclose(input); return result;
}
