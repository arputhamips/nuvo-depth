#ifndef NUVO_DEPTH_H
#define NUVO_DEPTH_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(NUVO_DEPTH_BUILD)
#define NUVO_API __declspec(dllexport)
#elif defined(_WIN32)
#define NUVO_API __declspec(dllimport)
#else
#define NUVO_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define NUVO_DEPTH_ABI_VERSION 1U

typedef struct nuvo_stream_decoder nuvo_stream_decoder;

NUVO_API uint32_t nuvo_depth_abi_version(void);
NUVO_API nuvo_stream_decoder* nuvo_stream_decoder_create(
    uint32_t maximum_metadata_bytes, uint64_t maximum_payload_bytes);
NUVO_API void nuvo_stream_decoder_destroy(nuvo_stream_decoder* decoder);

/* Returns bytes consumed. A zero result means the message is complete or an
 * error occurred; distinguish these with ready() and last_error(). */
NUVO_API size_t nuvo_stream_decoder_feed(nuvo_stream_decoder* decoder,
                                         const uint8_t* bytes, size_t size);
NUVO_API int nuvo_stream_decoder_ready(const nuvo_stream_decoder* decoder);
NUVO_API uint16_t nuvo_stream_decoder_kind(const nuvo_stream_decoder* decoder);
NUVO_API uint64_t nuvo_stream_decoder_sequence(
    const nuvo_stream_decoder* decoder);
NUVO_API const uint8_t* nuvo_stream_decoder_metadata(
    const nuvo_stream_decoder* decoder, size_t* size);
NUVO_API const uint8_t* nuvo_stream_decoder_payload(
    const nuvo_stream_decoder* decoder, size_t* size);
NUVO_API void nuvo_stream_decoder_reset(nuvo_stream_decoder* decoder);
NUVO_API const char* nuvo_stream_decoder_last_error(
    const nuvo_stream_decoder* decoder);

#ifdef __cplusplus
}
#endif

#endif
