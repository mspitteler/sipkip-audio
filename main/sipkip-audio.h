#ifndef SIPKIP_AUDIO_H
#define SIPKIP_AUDIO_H

#include "sdkconfig.h"

#define DEVICE_NAME "SipKip"

/*The frame size is hardcoded for this sample code but it doesn't have to be*/
#define OPUS_FRAME_SIZE 960
#define OPUS_SAMPLE_RATE 48000
#define OPUS_BITRATE 16000

#define OPUS_MAX_FRAME_SIZE 6*960
#define OPUS_MAX_PACKET_SIZE (3*1276)
                                     

#define ESP_INTR_FLAG_DEFAULT 0

#define DAC_WRITE_OPUS(opus_name, mem_or_file, decoder, dac_data)                                           \
    dac_write_opus((struct opus_mem_or_file) {                                                              \
        .is_##mem_or_file = true,                                                                           \
        .mem_or_file.opus = opus_name,                                                                      \
        .mem_or_file.opus_packets = opus_name##_packets,                                                    \
        .mem_or_file.opus_packets_len = opus_name##_packets_len                                             \
    }, decoder, dac_data)

#define LITTLEFS_CHECK_AT_BOOT 0
#define LITTLEFS_MAX_DEPTH 8
#define LITTLEFS_BASE_PATH "/littlefs"
#define LITTLEFS_FORMAT_BEAK_PRESSED_TIMEOUT 5000

#endif /* SIPKIP_AUDIO_H */
