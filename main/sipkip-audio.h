#ifndef SIPKIP_AUDIO_H
#define SIPKIP_AUDIO_H

#include "sdkconfig.h"

#define DEVICE_NAME "SipKip"

/*The frame size is hardcoded for this sample code but it doesn't have to be*/
#define OPUS_FRAME_SIZE 960
#define OPUS_SAMPLE_RATE 48000
#define OPUS_BITRATE 24000

#define OPUS_MAX_FRAME_SIZE 6*960
#define OPUS_MAX_PACKET_SIZE (3*1276)

/* These are all the buttons on the tail. */
#define GPIO_INPUT_STAR_L          GPIO_NUM_4
#define GPIO_INPUT_TRIANGLE_L      GPIO_NUM_5
#define GPIO_INPUT_SQUARE_L        GPIO_NUM_12
#define GPIO_INPUT_HEART_L         GPIO_NUM_13
#define GPIO_INPUT_HEART_R         GPIO_NUM_14
#define GPIO_INPUT_SQUARE_R        GPIO_NUM_15
#define GPIO_INPUT_TRIANGLE_R      GPIO_NUM_16
#define GPIO_INPUT_STAR_R          GPIO_NUM_32
/* This is the beak button. */
#define GPIO_INPUT_BEAK            GPIO_NUM_33
/* These are the inputs for the switch. */
#define GPIO_INPUT_SWITCH_LEARN    GPIO_NUM_34
#define GPIO_INPUT_SWITCH_MUSIC    GPIO_NUM_35
/* These are the outputs for the LEDs. */
#define GPIO_OUTPUT_LED_STAR_L     GPIO_NUM_17
#define GPIO_OUTPUT_LED_TRIANGLE_L GPIO_NUM_18
#define GPIO_OUTPUT_LED_SQUARE_L   GPIO_NUM_19
#define GPIO_OUTPUT_LED_HEART_L    GPIO_NUM_21
#define GPIO_OUTPUT_LED_HEART_R    GPIO_NUM_22
#define GPIO_OUTPUT_LED_SQUARE_R   GPIO_NUM_23
#define GPIO_OUTPUT_LED_TRIANGLE_R GPIO_NUM_26
#define GPIO_OUTPUT_LED_STAR_R     GPIO_NUM_27

#define GPIO_INPUT_PIN_SEL  ((1ULL << GPIO_INPUT_STAR_L)       | (1ULL << GPIO_INPUT_TRIANGLE_L)   |       \
                             (1ULL << GPIO_INPUT_SQUARE_L)     | (1ULL << GPIO_INPUT_HEART_L)      |       \
                             (1ULL << GPIO_INPUT_HEART_R)      | (1ULL << GPIO_INPUT_SQUARE_R)     |       \
                             (1ULL << GPIO_INPUT_TRIANGLE_R)   | (1ULL << GPIO_INPUT_STAR_R)       |       \
                             (1ULL << GPIO_INPUT_BEAK)         | (1ULL << GPIO_INPUT_SWITCH_LEARN) |       \
                             (1ULL << GPIO_INPUT_SWITCH_MUSIC))

#define GPIO_OUTPUT_PIN_SEL ((1ULL << GPIO_OUTPUT_LED_STAR_L)     | (1ULL << GPIO_OUTPUT_LED_TRIANGLE_L) | \
                             (1ULL << GPIO_OUTPUT_LED_SQUARE_L)   | (1ULL << GPIO_OUTPUT_LED_HEART_L)    | \
                             (1ULL << GPIO_OUTPUT_LED_HEART_R)    | (1ULL << GPIO_OUTPUT_LED_SQUARE_R)   | \
                             (1ULL << GPIO_OUTPUT_LED_TRIANGLE_R) | (1ULL << GPIO_OUTPUT_LED_STAR_R))

#define ESP_INTR_FLAG_DEFAULT 0

#define DAC_WRITE_OPUS(opus, pcm_bytes, decoder, dac_data) \
    dac_write_opus(opus, opus##_packets, opus##_packets_len, pcm_bytes, decoder, dac_data)

#define SPIFFS_CHECK_AT_BOOT 0
#define SPIFFS_BASE_PATH "/spiffs"

#endif /* SIPKIP_AUDIO_H */
