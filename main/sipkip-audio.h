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

#define GPIO_MUX_BUTTONS           GPIO_NUM_4
#define GPIO_MUX_CLIPS             GPIO_NUM_5        

/* These are all the buttons on the tail. */
#define GPIO_INPUT_STAR_L          GPIO_NUM_21
#define GPIO_INPUT_TRIANGLE_L      GPIO_NUM_19
#define GPIO_INPUT_SQUARE_L        GPIO_NUM_27
#define GPIO_INPUT_HEART_L         GPIO_NUM_14
#define GPIO_INPUT_HEART_R         GPIO_NUM_12
#define GPIO_INPUT_SQUARE_R        GPIO_NUM_13
#define GPIO_INPUT_TRIANGLE_R      GPIO_NUM_18
#define GPIO_INPUT_STAR_R          GPIO_NUM_16
/* This is the beak button. */
#define GPIO_INPUT_BEAK            GPIO_NUM_15
/* These are the inputs for the switch. */
#define GPIO_INPUT_SWITCH_LEARN    GPIO_NUM_34
#define GPIO_INPUT_SWITCH_MUSIC    GPIO_NUM_35
/* These are the outputs for the LEDs. */
#define GPIO_OUTPUT_LED_LEFT       GPIO_NUM_22
#define GPIO_OUTPUT_LED_MIDDLE     GPIO_NUM_26
#define GPIO_OUTPUT_LED_RIGHT      GPIO_NUM_17

#define GPIO_OUTPUT_PIN_SEL         ((1ULL << GPIO_OUTPUT_LED_LEFT)    | (1ULL << GPIO_OUTPUT_LED_MIDDLE)  |       \
                                     (1ULL << GPIO_OUTPUT_LED_RIGHT)   | (1ULL << GPIO_MUX_BUTTONS)        |       \
                                     (1ULL << GPIO_MUX_CLIPS))
#define GPIO_INPUT_OUTPUT_PIN_SEL   ((1ULL << GPIO_INPUT_STAR_L)       | (1ULL << GPIO_INPUT_TRIANGLE_L)   |       \
                                     (1ULL << GPIO_INPUT_SQUARE_L)     | (1ULL << GPIO_INPUT_HEART_L)      |       \
                                     (1ULL << GPIO_INPUT_HEART_R)      | (1ULL << GPIO_INPUT_SQUARE_R)     |       \
                                     (1ULL << GPIO_INPUT_TRIANGLE_R)   | (1ULL << GPIO_INPUT_STAR_R))
#define GPIO_INPUT_PIN_SEL          ((1ULL << GPIO_INPUT_BEAK)         | (1ULL << GPIO_INPUT_SWITCH_LEARN) |       \
                                     (1ULL << GPIO_INPUT_SWITCH_MUSIC))
                                     

#define ESP_INTR_FLAG_DEFAULT 0

#define DAC_WRITE_OPUS(opus_name, mem_or_file, decoder, dac_data)                                           \
    dac_write_opus((struct opus_mem_or_file) {                                                              \
        .is_##mem_or_file = true,                                                                           \
        .mem_or_file.opus = opus_name,                                                                      \
        .mem_or_file.opus_packets = opus_name##_packets,                                                    \
        .mem_or_file.opus_packets_len = opus_name##_packets_len                                             \
    }, decoder, dac_data)

#define SPIFFS_CHECK_AT_BOOT 0
#define SPIFFS_BASE_PATH "/spiffs"
#define SPIFFS_FORMAT_BEAK_PRESSED_TIMEOUT 5000

#endif /* SIPKIP_AUDIO_H */
