#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>

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
#include "esp_spiffs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_vfs.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/dac_continuous.h"

#include "audio.h"
#include "opus.h"
#include "spp-task.h"
#include "vfs-acceptor.h"
#include "sipkip-audio.h"

static const char *const TAG = "sipkip-audio";

static TaskHandle_t gpio_update_states_task_handle = NULL;

static volatile bool gpio_states[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static volatile bool gpio_output_states[] = {1, 0, 0, 0, 0, 0, 0, 0};

static const int io_num_to_gpio_states_index[] = {
    [GPIO_INPUT_STAR_L] = 0, [GPIO_INPUT_TRIANGLE_L] = 1, [GPIO_INPUT_SQUARE_L] = 2, [GPIO_INPUT_HEART_L] = 3,
    [GPIO_INPUT_HEART_R] = 4, [GPIO_INPUT_SQUARE_R] = 5, [GPIO_INPUT_TRIANGLE_R] = 6, [GPIO_INPUT_STAR_R] = 7,
    /* 8..15 are virtual inputs for the clips, that can be read if gpio_set_mux_clips is set to true. */
    [GPIO_INPUT_BEAK] = 16
};
static const int gpio_states_index_to_io_num[] = {
    [0] = GPIO_INPUT_STAR_L, [1] = GPIO_INPUT_TRIANGLE_L, [2] = GPIO_INPUT_SQUARE_L, [3] = GPIO_INPUT_HEART_L,
    [4] = GPIO_INPUT_HEART_R, [5] = GPIO_INPUT_SQUARE_R, [6] = GPIO_INPUT_TRIANGLE_R, [7] = GPIO_INPUT_STAR_R,
    /* 8..15 are virtual inputs for the clips, that can be read if gpio_set_mux_clips is set to true. */
    [16] = GPIO_INPUT_BEAK
};

static const esp_spp_mode_t esp_spp_mode = ESP_SPP_MODE_VFS;

struct dac_data {
    dac_continuous_handle_t handle;
    uint8_t *data;
    size_t data_size;
    volatile int buffer_full;
    volatile int exit;
};

static void *dac_write_data_synchronously(void *data) {
    struct dac_data *dac_data = data;
    ESP_LOGI(TAG, "Audio size %lu bytes, played at frequency %d Hz synchronously", dac_data->data_size, 
             OPUS_SAMPLE_RATE);
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
    opus_int16 out[OPUS_MAX_FRAME_SIZE];
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
        frame_size = opus_decode(decoder, opus + packet_size_total, packet_size, out, OPUS_MAX_FRAME_SIZE, 0);
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

static void gpio_update_states(void* arg) {
    bool input = false, gpio_mux_clips = false;
    
    for (;;) {
        if (input) {
            for (int i = 0; i < 8; i++) {
                int io_num = gpio_states_index_to_io_num[i];
                gpio_set_level(io_num, 1);
            }
            /* If we're reading the inputs; alternate between buttons and clips. */
            gpio_mux_clips = !gpio_mux_clips;
            gpio_set_level(GPIO_MUX_BUTTONS, !gpio_mux_clips);
            gpio_set_level(GPIO_MUX_CLIPS, gpio_mux_clips);
            
            gpio_set_level(GPIO_OUTPUT_LED_LEFT, 0);
            gpio_set_level(GPIO_OUTPUT_LED_MIDDLE, 0);
            gpio_set_level(GPIO_OUTPUT_LED_RIGHT, 0);
            
            for (int i = 0; i < 8; i++) {
                int io_num = gpio_states_index_to_io_num[i];
                gpio_pulldown_en(io_num);
                gpio_states[i + ((uint8_t)gpio_mux_clips << 3)] = gpio_get_level(io_num);
                gpio_pulldown_dis(io_num);
            }
        } else {
            gpio_set_level(GPIO_OUTPUT_LED_LEFT, 1);
            gpio_set_level(GPIO_OUTPUT_LED_MIDDLE, 1);
            gpio_set_level(GPIO_OUTPUT_LED_RIGHT, 1);
            
            gpio_set_level(GPIO_MUX_BUTTONS, 0);
            gpio_set_level(GPIO_MUX_CLIPS, 0);
            
            for (int i = 0; i < 8; i++)
                gpio_set_level(gpio_states_index_to_io_num[i], !gpio_output_states[i]);
        }
        
        /* Alternate reading inputs and turning on the LEDs. */
        input = !input;
        vTaskDelay(1);
    }
}

static inline char *bda2str(uint8_t *bda, char *str, size_t size) {
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

static inline char *readable_file_size(size_t size /* in bytes */, char *buf) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"};
    int i = 0;
    size *= 1000;
    while (size >= 1024000) {
        size >>= 10;
        i++;
    }
    sprintf(buf, "%lu.%lu %s", size / 1000, size % 1000, units[i]);
    return buf;
}

/**
 * Lists all files and sub-directories at given path.
 */
static void list_files(const char *const path) {
    struct dirent *dp;
    DIR *dir = opendir(path);

    /* Unable to open directory stream. */
    if (!dir) 
        return; 

    while ((dp = readdir(dir)) != NULL) {
        char d_path[CONFIG_SPIFFS_OBJ_NAME_LEN]; /* Here I am using sprintf which is safer than strcat. */
        sprintf(d_path, "%s/%s", path, dp->d_name);
        
        if (dp->d_type != DT_DIR) {
            struct stat st;
            char buf[10];
            
            if (stat(d_path, &st)) {
                ESP_LOGE(TAG, "Failed to stat file %s: %s", d_path, strerror(errno));
                continue;
            }

            ESP_LOGI(TAG, LOG_COLOR(LOG_COLOR_BLUE)"%s,\t%s"LOG_RESET_COLOR, d_path, 
                     readable_file_size(st.st_size, buf));
        } else if (strcmp(dp->d_name, ".") && strcmp(dp->d_name, "..")) {
            ESP_LOGI(TAG, LOG_COLOR(LOG_COLOR_GREEN)"%s\n"LOG_RESET_COLOR, d_path);
            list_files(d_path); /* Recall with the new path. */
        }
    }

    /* Close directory stream. */
    closedir(dir);
}

void app_main(void) {
    esp_pthread_cfg_t cfg;
   
    OpusDecoder *decoder;
    int err;

    unsigned char *pcm_bytes;

    dac_continuous_handle_t dac_handle;
    dac_continuous_config_t cont_cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0,
        .desc_num = 4,
        .buf_size = 2048,
        .freq_hz = OPUS_SAMPLE_RATE,
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
    
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    
    ESP_LOGI(TAG, "Initializing SPIFFS");
    
    /**
     * Use settings defined above to initialize and mount SPIFFS filesystem.
     * NOTE: esp_vfs_spiffs_register is an all-in-one convenience function.
     */
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

#if (SPIFFS_CHECK_AT_BOOT == true)
    ESP_LOGI(TAG, "Performing SPIFFS_check().");
    ret = esp_spiffs_check(conf.partition_label);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS_check() failed (%s)", esp_err_to_name(ret));
        return;
    } else {
        ESP_LOGI(TAG, "SPIFFS_check() successful");
    }
#endif

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s). Formatting...", esp_err_to_name(ret));
        esp_spiffs_format(conf.partition_label);
        return;
    } else {
        ESP_LOGI(TAG, "Partition size: total: %lu, used: %lu", total, used);
    }
    
    list_files(SPIFFS_BASE_PATH);
    
    char bda_str[18] = {0};
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(TAG,"%s initialize controller failed", __func__);
        return;
    }

    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(TAG,"%s enable controller failed", __func__);
        return;
    }

    if (esp_bluedroid_init() != ESP_OK) {
        ESP_LOGE(TAG,"%s initialize bluedroid failed", __func__);
        return;
    }

    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(TAG,"%s enable bluedroid failed", __func__);
        return;
    }

    if (esp_bt_gap_register_callback(&esp_bt_gap_cb) != ESP_OK) {
        ESP_LOGE(TAG,"%s gap register failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if (esp_spp_register_callback(&esp_spp_stack_cb) != ESP_OK) {
        ESP_LOGE(TAG,"%s spp register failed", __func__);
        return;
    }

    spp_task_task_start_up();

    if (esp_spp_init(esp_spp_mode) != ESP_OK) {
        ESP_LOGE(TAG,"%s spp init failed", __func__);
        return;
    }

#if (CONFIG_BT_SSP_ENABLED == true)
    /* Set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

    /**
     * Set default parameters for Legacy Pairing
     * Use variable pin, input pin code when pairing
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    ESP_LOGI(TAG,"Own address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
    
    /* Allocate continuous channels */
    ESP_ERROR_CHECK(dac_continuous_new_channels(&cont_cfg, &dac_handle));
    /* Enable the continuous channels */
    ESP_ERROR_CHECK(dac_continuous_enable(dac_handle));
    ESP_LOGI(TAG, "DAC initialized success, DAC DMA is ready");

    /* Create a new decoder state. */
    decoder = opus_decoder_create(OPUS_SAMPLE_RATE, 1, &err);
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to create decoder: %s\n", opus_strerror(err));
        return;
    }
    
    pcm_bytes = malloc(OPUS_MAX_FRAME_SIZE);
    if (!pcm_bytes) {
        ESP_LOGE(TAG, "Failed to allocate memory for dac output buffer\n");
        return;
    }
    
    cfg = esp_pthread_get_default_config();
    cfg.pin_to_core = 1; /* Pin to core 1, since the main app will run on core 0. */
    cfg.prio = 0;
    ESP_ERROR_CHECK(esp_pthread_set_cfg(&cfg));

    struct dac_data dac_data = {
        .handle = dac_handle,
        .data = pcm_bytes,
    };
    
    bool even = 0;
    DAC_WRITE_OPUS(__muziek______tijd_voor_muziek__druk_op_een_toets_om_naar_muziek_te_luisteren_opus, pcm_bytes, 
                   decoder, &dac_data);
    
    /* Zero-initialize the config structure. */
    gpio_config_t io_conf = {};
   
    /* Set as output mode. */
    io_conf.mode = GPIO_MODE_OUTPUT;
    /* Bit mask of the pins that you want to set as outputs. */
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    /* Configure GPIO with the given settings. */
    gpio_config(&io_conf);
   
    /* Bit mask of the pins, use inputs here. */
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    /* Set as input mode. */
    io_conf.mode = GPIO_MODE_INPUT;
    /* Enable pull-down mode. */
    io_conf.pull_down_en = 1;
    /* Configure GPIO with the given settings. */
    gpio_config(&io_conf);
    
    /* Bit mask of the pins, use input/open drain output here. */
    io_conf.pin_bit_mask = GPIO_INPUT_OUTPUT_PIN_SEL;
    /* Set as input/output mode. */
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    /* Configure GPIO with the given settings. */
    gpio_config(&io_conf);

    /* Start gpio task. */
    xTaskCreate(&gpio_update_states, "GPIO update states", 2048, NULL, 10, &gpio_update_states_task_handle);
    
    for (;;) {
        if (gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_HEART_L]] || 
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_HEART_R]]) {
            DAC_WRITE_OPUS(__muziek_ik_ben_zo_blij__opus, pcm_bytes, decoder, &dac_data);
            if (even)
                DAC_WRITE_OPUS(__muziek_blije_muziekjes_muziekje_5_opus, pcm_bytes, decoder, 
                               &dac_data);
            else
                DAC_WRITE_OPUS(__muziek_blije_muziekjes_muziekje_6_opus, pcm_bytes, decoder, 
                               &dac_data);
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_HEART_L]] = 0;
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_HEART_R]] = 0;
            even = !even;
        }
        if (gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_SQUARE_L]] || 
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_SQUARE_R]]) {
            DAC_WRITE_OPUS(__muziek_ik_voel_me_een_beetje_verdrietig_opus, pcm_bytes, decoder, &dac_data);
            if (even)
                DAC_WRITE_OPUS(__muziek_verdrietige_muziekjes_muziekje_7_opus, pcm_bytes, decoder, 
                               &dac_data);
            else
                DAC_WRITE_OPUS(__muziek_verdrietige_muziekjes_muziekje_8_opus, pcm_bytes, decoder, 
                               &dac_data);
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_SQUARE_L]] = 0;
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_SQUARE_R]] = 0;
            even = !even;
        }
        if (gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_TRIANGLE_L]] || 
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_TRIANGLE_R]]) {
            DAC_WRITE_OPUS(__muziek_ik_ben_boos__opus, pcm_bytes, decoder, &dac_data);
            if (even)
                DAC_WRITE_OPUS(__muziek_boze_muziekjes_muziekje_3_opus, pcm_bytes, decoder, 
                               &dac_data);
            else
                DAC_WRITE_OPUS(__muziek_boze_muziekjes_muziekje_4_opus, pcm_bytes, decoder, 
                               &dac_data);
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_TRIANGLE_L]] = 0;
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_TRIANGLE_R]] = 0;
            even = !even;
        }
        if (gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_STAR_L]] || 
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_STAR_R]]) {
            DAC_WRITE_OPUS(__muziek_wat_een_verassing__opus, pcm_bytes, decoder, &dac_data);
            if (even)
                DAC_WRITE_OPUS(__muziek_verbaasde_muziekjes_muziekje_1_opus, pcm_bytes, decoder, 
                               &dac_data);
            else
                DAC_WRITE_OPUS(__muziek_verbaasde_muziekjes_muziekje_2_opus, pcm_bytes, decoder, 
                               &dac_data);
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_STAR_L]] = 0;
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_STAR_R]] = 0;
            even = !even;
        }
        if (gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_BEAK]]) {
            if (even)
                DAC_WRITE_OPUS(__muziek_snavel_knop_het_is_tijd_om_te_zingen___muziekje_9_opus, pcm_bytes, decoder, 
                               &dac_data);
            else
                DAC_WRITE_OPUS(__muziek_snavel_knop_wil_je_mij_horen_zingen___muziekje_10_opus, pcm_bytes, decoder, 
                               &dac_data);
            gpio_states[io_num_to_gpio_states_index[GPIO_INPUT_BEAK]] = 0;
            even = !even;
        }
        
        for (int i = 0; i < 8; i++)
            if (gpio_output_states[i]) {
                gpio_output_states[i] = 0;
                gpio_output_states[(i + 1) & 7] = 1;
                break;
            }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    /* NOTE: We should never reach this code. */
    /* All done, unmount partition and disable SPIFFS. */
    esp_vfs_spiffs_unregister(conf.partition_label);
    ESP_LOGI(TAG, "SPIFFS unmounted");
    
    free(pcm_bytes);
  
    ESP_LOGI(TAG, "Done!\n");
    return;
}
