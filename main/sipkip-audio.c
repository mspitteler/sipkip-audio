#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Needed for KDevelop code parser. */
#define SOC_DAC_SUPPORTED 1

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_task_wdt.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_vfs.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "esp_random.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/dac_continuous.h"

#include "audio.h"
#include "opus.h"
#include "glob.h"
#include "spp-task.h"
#include "vfs-acceptor.h"
#include "sipkip-audio.h"
#include "muxed-gpio.h"
#include "utils.h"

static const char *const TAG = "sipkip-audio";

static TaskHandle_t dac_write_data_task_handle = NULL;

static volatile bool *gpio_states_changed = NULL;
static volatile bool gpio_states[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static bool gpio_output_states[] = {1, 0, 0, 0, 0, 0, 0, 0};

struct dac_data {
    dac_continuous_handle_t handle;
    uint8_t *data;
    size_t data_size;
    volatile bool buffer_full;
    volatile bool exit;
};

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

static void dac_write_data_synchronously(void *data) {
    struct dac_data *dac_data = data;
    ESP_LOGI(TAG, "Audio size %lu bytes, played at frequency %d Hz synchronously", dac_data->data_size, 
             OPUS_SAMPLE_RATE);
    for (;;) {
        if (dac_data->buffer_full) {
            ESP_ERROR_CHECK(dac_continuous_write(dac_data->handle, dac_data->data, dac_data->data_size, NULL, -1));
            dac_data->buffer_full = 0;
        } else {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

static esp_err_t dac_write_opus(struct opus_mem_or_file opus_mem_or_file, OpusDecoder *decoder,
                                struct dac_data *dac_data) {
    opus_int16 out[OPUS_MAX_FRAME_SIZE];
    bool suspended = true;

    dac_data->buffer_full = 1;

    for (int packet_size_index = 0, packet_size_total = 0, packet_size = 0;
         packet_size_index <= opus_mem_or_file.opus_packets_len / sizeof(short) && !dac_data->exit;
         packet_size = (opus_mem_or_file.is_mem ? ((short *)opus_mem_or_file.mem.opus_packets)[packet_size_index] : 
            (fgetc(opus_mem_or_file.file.opus_packets) & 0xFF) | (fgetc(opus_mem_or_file.file.opus_packets) << 8)), 
            packet_size_index++) {
        int frame_size;
        void *buf;
        bool free_after_decode;
    
        if (packet_size <= 0)
            continue;

        if (opus_mem_or_file.is_mem) {
            buf = (void *)(opus_mem_or_file.mem.opus + packet_size_total);
            free_after_decode = false;
        } else {
            size_t bytes_read;
            buf = malloc(packet_size);
            if (!buf) {
                ESP_LOGE(TAG, "Failed to allocate memory for opus input buffer\n");
                return ESP_FAIL;
            }
            if ((bytes_read = fread(buf, 1, packet_size, opus_mem_or_file.file.opus)) < packet_size) {
                ESP_LOGE(TAG, "Opus file is too short, read %lu, while expecting %d", bytes_read, packet_size);
                free(buf);
                return ESP_FAIL;
            }
                
            free_after_decode = true;
        }
        
        /**
         * Decode the data. In this case, frame_size will be constant because the encoder is using
         * a constant frame size. However, that may not be the case for all encoders,so the decoder must always check 
         * the frame size returned.
         */
        frame_size = opus_decode(decoder, buf, packet_size, out, OPUS_MAX_FRAME_SIZE, 0);
        
        /* Free memory if apliccable. */
        if (free_after_decode)
            free(buf);
        
        if (frame_size < 0) {
            ESP_LOGE(TAG, "Decoder failed: %s\n", opus_strerror(frame_size));
            return ESP_FAIL;
        }
       
        while (dac_write_data_task_handle && !suspended && dac_data->buffer_full)
            vTaskDelay(10 / portTICK_PERIOD_MS);
        /* Convert to 8-bit. */
        for (int i = 0; i < frame_size; i++)
            dac_data->data[i] = ((out[i] + 32768) >> 8) & 0xFF;
        
        dac_data->data_size = sizeof(*dac_data->data) * frame_size;
        dac_data->buffer_full = 1;
        
        if (!dac_write_data_task_handle) {
            xTaskCreate(&dac_write_data_synchronously, "DAC write data", 2048, dac_data, 10, 
                        &dac_write_data_task_handle);
        } else if (suspended) {
            vTaskResume(dac_write_data_task_handle);
            suspended = false;
        }
        
        packet_size_total += packet_size;
    }

    vTaskSuspend(dac_write_data_task_handle);
    
    return ESP_OK;
}

static void on_gpio_states_changed(volatile bool (*states)[19]) {
    for (int i = 0; i < sizeof(*states); i++) {
        if ((*states)[i])
            gpio_states[i] = true;
        (*states)[i] = false;
    }
    if (gpio_states_changed)
        *gpio_states_changed = true;
}

/**
 * Lists all files and sub-directories at given path.
 */
static void list_file_tree(const char *const path, int depth) {
    struct dirent *dp;
    DIR *dir = opendir(path);
    static bool last_printed[LITTLEFS_MAX_DEPTH] = {false};

    /* Unable to open directory stream. */
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory %s: %s!", path, strerror(errno));
        return; 
    }
    
    if (depth >= LITTLEFS_MAX_DEPTH) {
        ESP_LOGW(TAG, "Max directory nesting depth reached: %d!", LITTLEFS_MAX_DEPTH);
        return;
    }

    while ((dp = readdir(dir)) != NULL) {
        /* Accomodate for leading slashes, length of the path argument and trailing NUL character. */
        char d_path[(CONFIG_LITTLEFS_OBJ_NAME_LEN + 1) + strlen(path) + 1];
        long dir_loc = telldir(dir);
        struct dirent *tmp_dp;
        do {
            tmp_dp = readdir(dir);
        } while (tmp_dp && (!strcmp(tmp_dp->d_name, ".") || !strcmp(tmp_dp->d_name, "..")));
        last_printed[depth] = !tmp_dp;
        seekdir(dir, dir_loc);
        
        /* Here I am using sprintf which is safer than strcat. */
        snprintf(d_path, sizeof(d_path) / sizeof(*d_path), "%s/%s", path, dp->d_name);
        
        if (dp->d_type != DT_DIR) {
            struct stat st;
            char buf[10];
            
            if (stat(d_path, &st)) {
                ESP_LOGE(TAG, "Failed to stat file %s: %s", d_path, strerror(errno));
                continue;
            }

            for (int i = 0; i < depth; i++)
                printf("%c   ", last_printed[i] ? ' ' : '|');
            printf("%c-"LOG_COLOR(LOG_COLOR_CYAN)"%s"LOG_RESET_COLOR", %s\n", tmp_dp ? '|' : '`',
                   dp->d_name, readable_file_size(st.st_size, buf));
        } else if (strcmp(dp->d_name, ".") && strcmp(dp->d_name, "..")) {
            for (int i = 0; i < depth; i++)
                printf("%c   ", last_printed[i] ? ' ' : '|');
            printf("%c-"LOG_COLOR(LOG_COLOR_BLUE)"%s"LOG_RESET_COLOR"\n", tmp_dp ? '|' : '`', dp->d_name);
            list_file_tree(d_path, depth + 1); /* Recall with the new path. */
        }
    }

    /* Close directory stream. */
    closedir(dir);
}

static int play_littlefs_opus_file(OpusDecoder *decoder, struct dac_data *dac_data, const char *starts_with) {
    glob_t glob_buf;
    char *glob_path;
    char opus_path[(CONFIG_LITTLEFS_OBJ_NAME_LEN + 1) * LITTLEFS_MAX_DEPTH];
    char opus_packets_path[(CONFIG_LITTLEFS_OBJ_NAME_LEN + 1) * LITTLEFS_MAX_DEPTH];
    
    sprintf(opus_path, "%s-*.opus", starts_with);
    switch (g_glob(opus_path, GLOB_ERR, NULL, &glob_buf)) {
        case GLOB_NOMATCH:
            ESP_LOGW(TAG, "No files matching pattern \"%s\" present on LITTLEFS partition", opus_path);
            sprintf(opus_path, "%s.opus", starts_with);
            ESP_LOGW(TAG, "Falling back to \"%s\"", opus_path);
            break;
        /* Fatal errors; return. */
        case GLOB_NOSPACE: ESP_LOGE(TAG, "No memory left for executing glob()"); return -1;
        case GLOB_ABORTED: ESP_LOGE(TAG, "Read error in glob() function"); return -1;
    }
    if (glob_buf.gl_pathc)
        glob_path = glob_buf.gl_pathv[esp_random() % glob_buf.gl_pathc]; /* Take a random entry from gl_pathv. */
    else /* When no files matching the specified pattern were found. */
        glob_path = opus_path;
    
    sprintf(opus_packets_path, "%s_packets", glob_path);
    FILE *littlefs_file_opus = fopen(glob_path, "r");
    FILE *littlefs_file_opus_packets = fopen(opus_packets_path, "r");
    g_globfree(&glob_buf);
    unsigned int littlefs_file_opus_packets_len;
    
    if (littlefs_file_opus && littlefs_file_opus_packets) {
        fseek(littlefs_file_opus_packets, 0, SEEK_END);
        littlefs_file_opus_packets_len = ftell(littlefs_file_opus_packets);
        fseek(littlefs_file_opus_packets, 0, SEEK_SET);
        DAC_WRITE_OPUS(littlefs_file_opus, file, decoder, dac_data);
    } else {
        ESP_LOGW(TAG, "Failed to open \"%s\" or \"%s\"", glob_path, opus_packets_path);
    }
    
    if (littlefs_file_opus)
        fclose(littlefs_file_opus);
    if (littlefs_file_opus_packets)
        fclose(littlefs_file_opus_packets);
    
    return 0;
}

static void play_opus_files(OpusDecoder *decoder, struct dac_data *dac_data) {
    static bool even = false;
    
    if (gpio_states[MUXED_INPUT_HEART_L_BUTTON] || 
        gpio_states[MUXED_INPUT_HEART_R_BUTTON]) {
        DAC_WRITE_OPUS(__muziek_ik_ben_zo_blij__opus, mem, decoder, dac_data);
        if (even)
            DAC_WRITE_OPUS(__muziek_blije_muziekjes_muziekje_5_opus, mem, decoder, dac_data);
        else
            DAC_WRITE_OPUS(__muziek_blije_muziekjes_muziekje_6_opus, mem, decoder, dac_data);
        gpio_states[MUXED_INPUT_HEART_L_BUTTON] = 0;
        gpio_states[MUXED_INPUT_HEART_R_BUTTON] = 0;
        even = !even;
    }
    if (gpio_states[MUXED_INPUT_HEART_L_CLIP] || 
        gpio_states[MUXED_INPUT_HEART_R_CLIP]) {
        play_littlefs_opus_file(decoder, dac_data, "/littlefs/1");
        gpio_states[MUXED_INPUT_HEART_L_CLIP] = 0;
        gpio_states[MUXED_INPUT_HEART_R_CLIP] = 0;
    }
    if (gpio_states[MUXED_INPUT_SQUARE_L_BUTTON] || 
        gpio_states[MUXED_INPUT_SQUARE_R_BUTTON]) {
        DAC_WRITE_OPUS(__muziek_ik_voel_me_een_beetje_verdrietig_opus, mem, decoder, dac_data);
        if (even)
            DAC_WRITE_OPUS(__muziek_verdrietige_muziekjes_muziekje_7_opus, mem, decoder, dac_data);
        else
            DAC_WRITE_OPUS(__muziek_verdrietige_muziekjes_muziekje_8_opus, mem, decoder, dac_data);
        gpio_states[MUXED_INPUT_SQUARE_L_BUTTON] = 0;
        gpio_states[MUXED_INPUT_SQUARE_R_BUTTON] = 0;
        even = !even;
    }
    if (gpio_states[MUXED_INPUT_SQUARE_L_CLIP] || 
        gpio_states[MUXED_INPUT_SQUARE_R_CLIP]) {
        play_littlefs_opus_file(decoder, dac_data, "/littlefs/2");
        gpio_states[MUXED_INPUT_SQUARE_L_CLIP] = 0;
        gpio_states[MUXED_INPUT_SQUARE_R_CLIP] = 0;
    }
    if (gpio_states[MUXED_INPUT_TRIANGLE_L_BUTTON] || 
        gpio_states[MUXED_INPUT_TRIANGLE_R_BUTTON]) {
        DAC_WRITE_OPUS(__muziek_ik_ben_boos__opus, mem, decoder, dac_data);
        if (even)
            DAC_WRITE_OPUS(__muziek_boze_muziekjes_muziekje_3_opus, mem, decoder, dac_data);
        else
            DAC_WRITE_OPUS(__muziek_boze_muziekjes_muziekje_4_opus, mem, decoder, dac_data);
        gpio_states[MUXED_INPUT_TRIANGLE_L_BUTTON] = 0;
        gpio_states[MUXED_INPUT_TRIANGLE_R_BUTTON] = 0;
        even = !even;
    }
    if (gpio_states[MUXED_INPUT_TRIANGLE_L_CLIP] || 
        gpio_states[MUXED_INPUT_TRIANGLE_R_CLIP]) {
        play_littlefs_opus_file(decoder, dac_data, "/littlefs/3");
        gpio_states[MUXED_INPUT_TRIANGLE_L_CLIP] = 0;
        gpio_states[MUXED_INPUT_TRIANGLE_R_CLIP] = 0;
    }
    if (gpio_states[MUXED_INPUT_STAR_L_BUTTON] || 
        gpio_states[MUXED_INPUT_STAR_R_BUTTON]) {
        DAC_WRITE_OPUS(__muziek_wat_een_verassing__opus, mem, decoder, dac_data);
        if (even)
            DAC_WRITE_OPUS(__muziek_verbaasde_muziekjes_muziekje_1_opus, mem, decoder, dac_data);
        else
            DAC_WRITE_OPUS(__muziek_verbaasde_muziekjes_muziekje_2_opus, mem, decoder, dac_data);
        gpio_states[MUXED_INPUT_STAR_L_BUTTON] = 0;
        gpio_states[MUXED_INPUT_STAR_R_BUTTON] = 0;
        even = !even;
    }
    if (gpio_states[MUXED_INPUT_STAR_L_CLIP] || 
        gpio_states[MUXED_INPUT_STAR_R_CLIP]) {
        play_littlefs_opus_file(decoder, dac_data, "/littlefs/4");
        gpio_states[MUXED_INPUT_STAR_L_CLIP] = 0;
        gpio_states[MUXED_INPUT_STAR_R_CLIP] = 0;
    }
    if (gpio_states[MUXED_INPUT_BEAK_SWITCH]) {
        play_littlefs_opus_file(decoder, dac_data, "/littlefs/b");
        if (even)
            DAC_WRITE_OPUS(__muziek_snavel_knop_het_is_tijd_om_te_zingen___muziekje_9_opus, mem, 
                            decoder, dac_data);
        else
            DAC_WRITE_OPUS(__muziek_snavel_knop_wil_je_mij_horen_zingen___muziekje_10_opus, mem, 
                            decoder, dac_data);
        gpio_states[MUXED_INPUT_BEAK_SWITCH] = 0;
        even = !even;
    }
}

void app_main(void) {
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
    
    esp_vfs_littlefs_conf_t conf = {
        .base_path = LITTLEFS_BASE_PATH,
        .partition_label = "storage", /* See `partitions.csv' */
        .format_if_mount_failed = true
    };
    
    ESP_LOGI(TAG, "Initializing LITTLEFS");
    
    /**
     * Use settings defined above to initialize and mount LITTLEFS filesystem.
     * NOTE: esp_vfs_littlefs_register is an all-in-one convenience function.
     */
    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LITTLEFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LITTLEFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

#if (LITTLEFS_CHECK_AT_BOOT == true)
    ESP_LOGI(TAG, "Performing LITTLEFS_check().");
    ret = esp_littlefs_check(conf.partition_label);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LITTLEFS_check() failed (%s)", esp_err_to_name(ret));
        return;
    } else {
        ESP_LOGI(TAG, "LITTLEFS_check() successful");
    }
#endif

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LITTLEFS partition information (%s). Formatting...", esp_err_to_name(ret));
        esp_littlefs_format(conf.partition_label);
        return;
    } else {
        ESP_LOGI(TAG, "Partition size: total: %lu, used: %lu", total, used);
    }
    
    ESP_LOGI(TAG, "LITTLEFS file tree:");
    list_file_tree(LITTLEFS_BASE_PATH, 0);
    
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
        ESP_LOGE(TAG, "%s initialize controller failed", __func__);
        return;
    }

    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(TAG, "%s enable controller failed", __func__);
        return;
    }

    if (esp_bluedroid_init() != ESP_OK) {
        ESP_LOGE(TAG, "%s initialize bluedroid failed", __func__);
        return;
    }

    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(TAG, "%s enable bluedroid failed", __func__);
        return;
    }

    if (esp_bt_gap_register_callback(&esp_bt_gap_cb) != ESP_OK) {
        ESP_LOGE(TAG, "%s gap register failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if (esp_spp_register_callback(&esp_spp_stack_cb) != ESP_OK) {
        ESP_LOGE(TAG, "%s spp register failed", __func__);
        return;
    }

    spp_task_task_start_up();

    esp_spp_cfg_t bt_spp_cfg = BT_SPP_DEFAULT_CONFIG();
    if (esp_spp_enhanced_init(&bt_spp_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "%s spp init failed", __func__);
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

    ESP_LOGI(TAG,"Own address:[%s]", bd_address_to_string((uint8_t *)esp_bt_dev_get_address(),
                                                          bda_str, sizeof(bda_str)));
    
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

    struct dac_data dac_data = {
        .handle = dac_handle,
        .data = pcm_bytes,
    };
    /* Set dac_data->exit to true when one of the gpio states has changed. */
    gpio_states_changed = &dac_data.exit;
   
    DAC_WRITE_OPUS(__muziek______tijd_voor_muziek__druk_op_een_toets_om_naar_muziek_te_luisteren_opus, mem, 
                   decoder, &dac_data);
    
    muxed_gpio_setup(&on_gpio_states_changed);

    /* Format the littlefs partition if the beak button is pressed for 5 seconds. */
    if (gpio_get_level(GPIO_NUM_15)) {
        vTaskDelay(LITTLEFS_FORMAT_BEAK_PRESSED_TIMEOUT / portTICK_PERIOD_MS);
        if (gpio_get_level(GPIO_NUM_15)) {
            ESP_LOGW(TAG, "Pressed the beak button for %d seconds. Formatting...",
                     LITTLEFS_FORMAT_BEAK_PRESSED_TIMEOUT / 1000);
            esp_littlefs_format(conf.partition_label);
        }
    }
    
    for (;;) {
        *gpio_states_changed = false;
        play_opus_files(decoder, &dac_data);
        
        for (int i = 0; i < 8; i++)
            if (gpio_output_states[i]) {
                gpio_output_states[i] = 0;
                gpio_output_states[(i + 1) & 7] = 1;
                break;
            }
        
        muxed_gpio_set_output_levels(&gpio_output_states);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    /* NOTE: We should never reach this code. */
    /* All done, unmount partition and disable LITTLEFS. */
    esp_vfs_littlefs_unregister(conf.partition_label);
    ESP_LOGI(TAG, "LITTLEFS unmounted");
    
    free(pcm_bytes);
  
    ESP_LOGI(TAG, "Done!\n");
    return;
}
