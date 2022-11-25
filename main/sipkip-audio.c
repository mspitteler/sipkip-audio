#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <pthread.h>

/* Needed for KDevelop code parser. */
#define SOC_DAC_SUPPORTED 1

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

static QueueHandle_t gpio_evt_queue = NULL;
static int gpio_states[] = {0, 0, 0, 0, 0, 0, 0, 0};
static const int io_num_to_gpio_states_index[] = {
    [GPIO_INPUT_IO_0] = 0, [GPIO_INPUT_IO_1] = 1, [GPIO_INPUT_IO_2] = 2, [GPIO_INPUT_IO_3] = 3,
    [GPIO_INPUT_IO_4] = 4, [GPIO_INPUT_IO_5] = 5, [GPIO_INPUT_IO_6] = 6, [GPIO_INPUT_IO_7] = 7
};

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

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (ptrdiff_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_get_states(void* arg) {
    uint32_t io_num;
    for (;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            int level = gpio_get_level(io_num);
            ESP_LOGI(TAG, "GPIO[%"PRIu32"] intr, val: %d\n", io_num, level);
            gpio_states[io_num_to_gpio_states_index[io_num]] = !level;
        }
    }
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
    /* Zero-initialize the config structure. */
    gpio_config_t io_conf = {};
    
    /* Interrupt of rising edge. */
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    /* Bit mask of the pins, use GPIO4/5 here. */
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    /* Set as input mode. */
    io_conf.mode = GPIO_MODE_INPUT;
    /* Enable pull-up mode. */
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);
    
    /* Create a queue to handle gpio event from isr. */
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    /* Start gpio task. */
    xTaskCreate(&gpio_get_states, "GPIO get states", 2048, NULL, 10, NULL);

    /* Install gpio isr service. */
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    /* Hook isr handler for specific gpio pins. */
    gpio_isr_handler_add(GPIO_INPUT_IO_0, &gpio_isr_handler, (void *)GPIO_INPUT_IO_0);
    gpio_isr_handler_add(GPIO_INPUT_IO_1, &gpio_isr_handler, (void *)GPIO_INPUT_IO_1);
    gpio_isr_handler_add(GPIO_INPUT_IO_2, &gpio_isr_handler, (void *)GPIO_INPUT_IO_2);
    gpio_isr_handler_add(GPIO_INPUT_IO_3, &gpio_isr_handler, (void *)GPIO_INPUT_IO_3);
    gpio_isr_handler_add(GPIO_INPUT_IO_4, &gpio_isr_handler, (void *)GPIO_INPUT_IO_4);
    gpio_isr_handler_add(GPIO_INPUT_IO_5, &gpio_isr_handler, (void *)GPIO_INPUT_IO_5);
    gpio_isr_handler_add(GPIO_INPUT_IO_6, &gpio_isr_handler, (void *)GPIO_INPUT_IO_6);
    gpio_isr_handler_add(GPIO_INPUT_IO_7, &gpio_isr_handler, (void *)GPIO_INPUT_IO_7);
    
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
    
    for (;;) {
        if (gpio_states[0]) {
            DAC_WRITE_OPUS(__muziek_snavel_knop_het_is_tijd_om_te_zingen___muziekje_9_opus, pcm_bytes, decoder, 
                           &dac_data);
            gpio_states[0] = 0;
        }
        if (gpio_states[1]) {
            DAC_WRITE_OPUS(__muziek_snavel_knop_wil_je_mij_horen_zingen___muziekje_10_opus, pcm_bytes, decoder, 
                           &dac_data);
            gpio_states[1] = 0;
        }
        if (gpio_states[2]) {
            DAC_WRITE_OPUS(__muziek_ik_ben_boos__opus, pcm_bytes, decoder, 
                           &dac_data);
            gpio_states[2] = 0;
        }
        if (gpio_states[3]) {
            DAC_WRITE_OPUS(__muziek_ik_voel_me_een_beetje_verdrietig_opus, pcm_bytes, decoder, 
                           &dac_data);
            gpio_states[3] = 0;
        }
        if (gpio_states[4]) {
            DAC_WRITE_OPUS(__muziek_wat_een_verassing__opus, pcm_bytes, decoder, 
                           &dac_data);
            gpio_states[4] = 0;
        }
        if (gpio_states[5]) {
            DAC_WRITE_OPUS(__muziek______tijd_voor_muziek__druk_op_een_toets_om_naar_muziek_te_luisteren_opus, 
                           pcm_bytes, decoder, &dac_data);
            gpio_states[5] = 0;
        }
        if (gpio_states[6]) {
            DAC_WRITE_OPUS(__leren_hmm__ik_ben_een_beetje_sip_opus, pcm_bytes, decoder, 
                           &dac_data);
            gpio_states[6] = 0;
        }
        if (gpio_states[7]) {
            DAC_WRITE_OPUS(__muziek_verdrietige_muziekjes_muziekje_7_opus, pcm_bytes, decoder, 
                           &dac_data);
            gpio_states[7] = 0;
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    free(pcm_bytes);
   
    printf("Done!\n");
    return;
}