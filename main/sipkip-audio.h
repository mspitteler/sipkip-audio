#ifndef SIPKIP_AUDIO_H
#define SIPKIP_AUDIO_H

#include "audio.h"

/*The frame size is hardcoded for this sample code but it doesn't have to be*/
#define FRAME_SIZE 960
#define SAMPLE_RATE 48000
#define APPLICATION OPUS_APPLICATION_AUDIO
#define BITRATE 24000

#define MAX_FRAME_SIZE 6*960
#define MAX_PACKET_SIZE (3*1276)

#define DAC_WRITE_OPUS(opus, pcm_bytes, decoder, dac_data) \
    dac_write_opus(opus, opus##_packets, opus##_packets_len, pcm_bytes, decoder, dac_data)

#endif /* SIPKIP_AUDIO_H */
