#ifndef SIPKIP_AUDIO_H
#define SIPKIP_AUDIO_H

#include "sdkconfig.h"

#include <stdio.h>
#include <stdbool.h>

#include "freertos/semphr.h"
#include "esp_err.h"

#define DEVICE_NAME "SipKip"

/*The frame size is hardcoded for this sample code but it doesn't have to be*/
#define OPUS_FRAME_SIZE 960
#define OPUS_SAMPLE_RATE 48000
#define OPUS_BITRATE 16000

#define OPUS_MAX_FRAME_SIZE 6*960
#define OPUS_MAX_PACKET_SIZE (3*1276)
                                     

#define ESP_INTR_FLAG_DEFAULT 0

#define DAC_WRITE_OPUS(opus_name, mem_or_file)                                                              \
    dac_write_opus((struct opus_mem_or_file) {                                                              \
        .is_##mem_or_file = true,                                                                           \
        .mem_or_file.opus = opus_name,                                                                      \
        .mem_or_file.opus_packets = opus_name##_packets,                                                    \
        .mem_or_file.opus_packets_len = opus_name##_packets_len                                             \
    })
    
struct opus_mem_or_file {
    bool is_mem;
    bool is_file;
    union {
        unsigned int opus_packets_len;
        struct {
            unsigned int opus_packets_len;
            const unsigned char *opus;
            const unsigned char *opus_packets;
        } mem;
        struct {
            unsigned int opus_packets_len;
            FILE *opus;
            FILE *opus_packets;
        } file;
    };
};

extern SemaphoreHandle_t dac_write_opus_mutex;
extern volatile bool exit_dac_write_opus_loop;

esp_err_t dac_write_opus(struct opus_mem_or_file opus_mem_or_file);

#define LITTLEFS_CHECK_AT_BOOT 0
#define LITTLEFS_MAX_DEPTH 8
#define LITTLEFS_BASE_PATH "/littlefs"
#define LITTLEFS_FORMAT_BEAK_PRESSED_TIMEOUT 5000

#endif /* SIPKIP_AUDIO_H */
