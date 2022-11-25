#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <pthread.h>

/* Needed for KDevelop code parser. */
#define SOC_DAC_SUPPORTED 1

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_pthread.h"
#include "esp_task_wdt.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/dac_continuous.h"

#include "opus.h"
#include "sipkip-audio.h"

static const char *TAG = "sipkip-audio";

struct dac_data {
    dac_continuous_handle_t handle;
    uint8_t *data;
    size_t data_size;
    int buffer_full;
    int exit;
};

static void *dac_write_data_synchronously(void *data) {
    struct dac_data *dac_data = data;
    ESP_LOGI(TAG, "Audio size %lu bytes, played at frequency %d Hz synchronously", dac_data->data_size, SAMPLE_RATE);
    uint32_t cnt = 1;
    while (!dac_data->exit) {
        ESP_LOGI(TAG, "Play count: %"PRIu32"\n", cnt++);
        ESP_ERROR_CHECK(dac_continuous_write(dac_data->handle, dac_data->data, dac_data->data_size, NULL, -1));
        dac_data->buffer_full = 0;
    }
    return NULL;
}

static int dac_write_opus(const unsigned char *opus, const unsigned char *opus_packets, unsigned int opus_packets_len, 
                          unsigned char *pcm_bytes, OpusDecoder *decoder, struct dac_data *dac_data) {
    opus_int16 out[MAX_FRAME_SIZE];
    pthread_t thread = 0;
    
    dac_data->exit = 0;
    dac_data->buffer_full = 1;
    
    for (int packet_size_index = 0, packet_size_total = 0, packet_size = 0;
         packet_size_index <= opus_packets_len / sizeof(short);
         packet_size = ((short *)opus_packets)[packet_size_index++]) {
        int iret;
        int frame_size;
    
        if (packet_size <= 0)
            continue;

        /**
         * Decode the data. In this case, frame_size will be constant because the encoder is using
         * a constant frame size. However, that may not be the case for all encoders,so the decoder must always check 
         * the frame size returned.
         */
        frame_size = opus_decode(decoder, opus + packet_size_total, packet_size, out, MAX_FRAME_SIZE, 0);
        if (frame_size < 0) {
            ESP_LOGE(TAG, "Decoder failed: %s\n", opus_strerror(frame_size));
            return -1;
        }
       
        while (thread && dac_data->buffer_full)
            vTaskDelay(1);
        /* Convert to 8-bit. */
        for (int i = 0; i < frame_size; i++)
            pcm_bytes[i] = ((out[i] + 32768) >> 8) & 0xFF;
        
        dac_data->data_size = sizeof(*pcm_bytes) * frame_size;
        dac_data->buffer_full = 1;
        
        if (!thread) {
            iret = pthread_create(&thread, NULL, &dac_write_data_synchronously, dac_data);
            if (iret) {
                ESP_LOGE(TAG, "Failed to create pthread for dac writing: %d\n", iret);
                return -1;
            }
        }
        
        packet_size_total += packet_size;
    }
    dac_data->exit = 1;
    pthread_join(thread, NULL);
    
    return 0;
}

void app_main(void) {
    esp_pthread_cfg_t cfg;
   
    OpusDecoder *decoder;
    int err;

    unsigned char *pcm_bytes;

    dac_continuous_handle_t dac_handle;
    dac_continuous_config_t cont_cfg = {
        .chan_mask = DAC_CHANNEL_MASK_ALL,
        .desc_num = 4,
        .buf_size = 2048,
        .freq_hz = SAMPLE_RATE,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_APLL,   /* Using APLL as clock source to get a wider frequency range */
        /**
         * Assume the data in buffer is 'A B C D E F'
         * DAC_CHANNEL_MODE_SIMUL:
         *      - channel 0: A B C D E F
         *      - channel 1: A B C D E F
         * DAC_CHANNEL_MODE_ALTER:
         *      - channel 0: A C E
         *      - channel 1: B D F
         */
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    /* Allocate continuous channels */
    ESP_ERROR_CHECK(dac_continuous_new_channels(&cont_cfg, &dac_handle));
    /* Enable the continuous channels */
    ESP_ERROR_CHECK(dac_continuous_enable(dac_handle));
    ESP_LOGI(TAG, "DAC initialized success, DAC DMA is ready");

    /* Create a new decoder state. */
    decoder = opus_decoder_create(SAMPLE_RATE, 1, &err);
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to create decoder: %s\n", opus_strerror(err));
        return;
    }
    
    pcm_bytes = malloc(MAX_FRAME_SIZE);
    if (!pcm_bytes) {
        ESP_LOGE(TAG, "Failed to allocate memory for dac output buffer\n");
        return;
    }
    
    cfg = esp_pthread_get_default_config();
    cfg.pin_to_core = 1;
    ESP_ERROR_CHECK(esp_pthread_set_cfg(&cfg));

    struct dac_data dac_data = {
        .handle = dac_handle,
        .data = pcm_bytes,
    };
    
    DAC_WRITE_OPUS(__muziek_snavel_knop_het_is_tijd_om_te_zingen___muziekje_9_opus, pcm_bytes, decoder, &dac_data);
    DAC_WRITE_OPUS(__muziek_snavel_knop_wil_je_mij_horen_zingen___muziekje_10_opus, pcm_bytes, decoder, &dac_data);
    
    free(pcm_bytes);
   
    printf("Done!\n");
    return;
}
