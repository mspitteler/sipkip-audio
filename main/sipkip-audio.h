#ifndef SIPKIP_AUDIO_H
#define SIPKIP_AUDIO_H

#include "sdkconfig.h"

#include "audio.h"

/*The frame size is hardcoded for this sample code but it doesn't have to be*/
#define FRAME_SIZE 960
#define SAMPLE_RATE 48000
#define APPLICATION OPUS_APPLICATION_AUDIO
#define BITRATE 24000

#define MAX_FRAME_SIZE 6*960
#define MAX_PACKET_SIZE (3*1276)

#define GPIO_INPUT_IO_0     GPIO_NUM_4
#define GPIO_INPUT_IO_1     GPIO_NUM_5
#define GPIO_INPUT_IO_2     GPIO_NUM_13
#define GPIO_INPUT_IO_3     GPIO_NUM_14
#define GPIO_INPUT_IO_4     GPIO_NUM_15
#define GPIO_INPUT_IO_5     GPIO_NUM_16
#define GPIO_INPUT_IO_6     GPIO_NUM_17
#define GPIO_INPUT_IO_7     GPIO_NUM_18
#define GPIO_INPUT_PIN_SEL  ((1ULL<<GPIO_INPUT_IO_0) | (1ULL<<GPIO_INPUT_IO_1) | \
                             (1ULL<<GPIO_INPUT_IO_2) | (1ULL<<GPIO_INPUT_IO_3) | \
                             (1ULL<<GPIO_INPUT_IO_4) | (1ULL<<GPIO_INPUT_IO_5) | \
                             (1ULL<<GPIO_INPUT_IO_6) | (1ULL<<GPIO_INPUT_IO_7))

#define ESP_INTR_FLAG_DEFAULT 0

#define DAC_WRITE_OPUS(opus, pcm_bytes, decoder, dac_data) \
    dac_write_opus(opus, opus##_packets, opus##_packets_len, pcm_bytes, decoder, dac_data)

#endif /* SIPKIP_AUDIO_H */
