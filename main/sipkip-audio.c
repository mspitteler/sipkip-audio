#include <stdio.h>
#include <stdbool.h>
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
#include "freertos/semphr.h"
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
SemaphoreHandle_t dac_write_opus_mutex = NULL;

enum mode {
    LEARN,
    PLAY,
    MUSIC,
};

static volatile enum mode mode = MUSIC;
static volatile bool mode_changed = true;

volatile bool exit_dac_write_opus_loop = false;
static volatile bool gpio_states[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static bool gpio_output_states[] = {1, 0, 0, 0, 0, 0, 0, 0};

static OpusDecoder *decoder = NULL;
static struct dac_data {
    dac_continuous_handle_t handle;
    uint8_t *data_front, *data_back;
    size_t data_front_size;
} *dac_data = NULL;

static void dac_write_data_synchronously(void *data) {
    struct dac_data *dac_data = data;
    ESP_LOGI(TAG, "Audio size %lu bytes, playing at frequency %d Hz synchronously", dac_data->data_front_size, 
             OPUS_SAMPLE_RATE);
    for (;;) {
        if (dac_data->data_front_size > 0) {
            ESP_ERROR_CHECK(dac_continuous_write(dac_data->handle, dac_data->data_front, dac_data->data_front_size,
                                                 NULL, -1));
            dac_data->data_front_size = 0;
        } else {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

esp_err_t dac_write_opus(struct opus_mem_or_file opus_mem_or_file) {
    opus_int16 out[OPUS_MAX_FRAME_SIZE];
    bool suspended = true;
    esp_err_t ret = ESP_OK;
   
    xSemaphoreTake(dac_write_opus_mutex, portMAX_DELAY);
    exit_dac_write_opus_loop = false;

    for (int packet_size_index = 0, packet_size_total = 0, packet_size = 0;
         packet_size_index < opus_mem_or_file.opus_packets_len / sizeof(short);
         packet_size_total += packet_size, packet_size = opus_mem_or_file.is_mem ?
            ((const short *)opus_mem_or_file.mem.opus_packets)[packet_size_index] : 
            (fgetc(opus_mem_or_file.file.opus_packets) & 0xFF) | (fgetc(opus_mem_or_file.file.opus_packets) << 8),
         packet_size_index++) {
        int frame_size;
        void *in;
        bool free_after_decode;
    
        if (packet_size <= 0)
            continue;

        if (opus_mem_or_file.is_mem) {
            in = (void *)(opus_mem_or_file.mem.opus + packet_size_total);
            free_after_decode = false;
        } else {
            size_t bytes_read;
            in = malloc(packet_size);
            if (!in) {
                ESP_LOGE(TAG, "Failed to allocate memory for opus input buffer\n");
                ret = ESP_FAIL;
                break;
            }
            if ((bytes_read = fread(in, 1, packet_size, opus_mem_or_file.file.opus)) < packet_size) {
                ESP_LOGE(TAG, "Opus file is too short, read %lu, while expecting %d", bytes_read, packet_size);
                free(in);
                ret = ESP_FAIL;
                break;
            }
                
            free_after_decode = true;
        }
        
        /**
         * Decode the data. In this case, frame_size will be constant because the encoder is using
         * a constant frame size. However, that may not be the case for all encoders,so the decoder must always check 
         * the frame size returned.
         */
        frame_size = opus_decode(decoder, in, packet_size, out, OPUS_MAX_FRAME_SIZE, 0);
        
        /* Free memory if apliccable. */
        if (free_after_decode)
            free(in);
        
        if (frame_size < 0) {
            ESP_LOGE(TAG, "Decoder failed: %s\n", opus_strerror(frame_size));
            ret = ESP_FAIL;
            break;
        }
      
        /* Convert to 8-bit. */
        for (int i = 0; i < frame_size; i++)
            dac_data->data_back[i] = ((out[i] + 32768) >> 8) & 0xFF;
        
        while (dac_write_data_task_handle && !suspended && dac_data->data_front_size)
            vTaskDelay(10 / portTICK_PERIOD_MS);
        
        if (exit_dac_write_opus_loop) {
            ret = ESP_ERR_NOT_FINISHED;
            break;
        }
        
        /* Swap front and back buffer. */
        uint8_t *data_tmp = dac_data->data_front;
        
        dac_data->data_front = dac_data->data_back;
        dac_data->data_front_size = sizeof(*dac_data->data_front) * frame_size;
        dac_data->data_back = data_tmp;
        
        if (!dac_write_data_task_handle) {
            xTaskCreate(&dac_write_data_synchronously, "DAC write data", 2048, dac_data, 10, 
                        &dac_write_data_task_handle);
        } else if (suspended) {
            vTaskResume(dac_write_data_task_handle);
            suspended = false;
        }
    }

    vTaskSuspend(dac_write_data_task_handle);
    xSemaphoreGive(dac_write_opus_mutex);
    
    return ret;
}


static void on_gpio_states_changed(volatile bool (*states)[19]) {
    bool input_switch_levels[19];
    enum mode new_mode;
    
    for (int i = 0; i < sizeof(*states); i++) {
        if ((*states)[i]) {
            gpio_states[i] = 1;
            exit_dac_write_opus_loop = true;
        }
    }
    muxed_gpio_get_input_switch_levels(&input_switch_levels);
    if (input_switch_levels[MUXED_INPUT_LEARN_SWITCH])
        new_mode = LEARN;
    else if (input_switch_levels[MUXED_INPUT_PLAY_SWITCH])
        new_mode = PLAY;
    else
        new_mode = MUSIC;
    
    if (new_mode != mode)
        mode_changed = true;
    mode = new_mode;
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

static esp_err_t play_littlefs_opus_file(const char *starts_with) {
    FILE *littlefs_file_opus = NULL, *littlefs_file_opus_packets = NULL;
    glob_t glob_buf = {0};
    char *glob_path;
    char opus_path[(CONFIG_LITTLEFS_OBJ_NAME_LEN + 1) * LITTLEFS_MAX_DEPTH];
    char opus_packets_path[(CONFIG_LITTLEFS_OBJ_NAME_LEN + 1) * LITTLEFS_MAX_DEPTH];
    esp_err_t ret = ESP_OK;
    
    sprintf(opus_path, "%s*.opus", starts_with);
    switch (g_glob(opus_path, GLOB_ERR, NULL, &glob_buf)) {
        case GLOB_NOMATCH:
            ESP_LOGW(TAG, "No files matching pattern \"%s\" present on LITTLEFS partition", opus_path);
            sprintf(opus_path, "%s.opus", starts_with);
            ESP_LOGW(TAG, "Falling back to \"%s\"", opus_path);
            break;
        /* Fatal errors; return. */
        case GLOB_NOSPACE:
            ESP_LOGE(TAG, "No memory left for executing glob()");
            return ESP_ERR_NO_MEM;
        case GLOB_ABORTED:
            ESP_LOGE(TAG, "Read error in glob() function");
            ret = ESP_FAIL;
            goto exit;
    }
    if (glob_buf.gl_pathc)
        glob_path = glob_buf.gl_pathv[esp_random() % glob_buf.gl_pathc]; /* Take a random entry from gl_pathv. */
    else /* When no files matching the specified pattern were found. */
        glob_path = opus_path;
    
    littlefs_file_opus = fopen(glob_path, "r");
    if (!littlefs_file_opus) {
        ESP_LOGW(TAG, "Failed to open file %s: %s\n", glob_path, strerror(errno));
        ret = ESP_FAIL;
        goto exit;
    }
    
    sprintf(opus_packets_path, "%s_packets", glob_path);
    littlefs_file_opus_packets = fopen(opus_packets_path, "r");
    if (!littlefs_file_opus_packets) {
        ESP_LOGW(TAG, "Failed to open file %s: %s\n", opus_packets_path, strerror(errno));
        ret = ESP_FAIL;
        goto exit;
    }
    
    fseek(littlefs_file_opus_packets, 0, SEEK_END);
    unsigned int littlefs_file_opus_packets_len = ftell(littlefs_file_opus_packets);
    fseek(littlefs_file_opus_packets, 0, SEEK_SET);
    ret = DAC_WRITE_OPUS(littlefs_file_opus, file);
    
exit:
    g_globfree(&glob_buf);
    if (littlefs_file_opus)
        fclose(littlefs_file_opus);
    if (littlefs_file_opus_packets)
        fclose(littlefs_file_opus_packets);
    
    return ret;
}

static void mode_learn(void) {
    if (mode_changed) {
        switch (esp_random() % 2) {
            case 0:
                DAC_WRITE_OPUS(__leren_laten_we_ontdekken_en_leren__met_mijn_prachtige_veren__opus, mem);
                break;
            case 1:
                DAC_WRITE_OPUS(__leren_laten_we_eens_kijken_of_je_deze_vragen_kunt_beantwoorden_opus, mem);
                break;
        }
        mode_changed = false;
    }
}

static void mode_play(void) {
    if (mode_changed) {
        switch (esp_random() % 3) {
            case 0:
                DAC_WRITE_OPUS(__spelen_groep_1_druk_op_een_toets_of_plaats_een_knijper_om_te_spelen_opus, mem);
                break;
            case 1:
                DAC_WRITE_OPUS(__spelen_groep_1_hoi__ik_ben_een_sierlijke_pauw__laten_we_spelen__hoeraa___opus, mem);
                break;
            case 2:
                DAC_WRITE_OPUS(__spelen_groep_1_laten_we_ontdekken_en_leren__met_mijn_prachtige_veren__opus, mem);
                break;
        }
        mode_changed = false;
    }
}

static void mode_music(void) {
    static bool even = false;
    
    if (mode_changed) {
        DAC_WRITE_OPUS(__muziek______tijd_voor_muziek__druk_op_een_toets_om_naar_muziek_te_luisteren_opus, mem);
        mode_changed = false;
    }
    
    if (gpio_states[MUXED_INPUT_HEART_L_BUTTON] || 
        gpio_states[MUXED_INPUT_HEART_R_BUTTON]) {
        gpio_states[MUXED_INPUT_HEART_L_BUTTON] = 0;
        gpio_states[MUXED_INPUT_HEART_R_BUTTON] = 0;
        if (DAC_WRITE_OPUS(__muziek_ik_ben_zo_blij__opus, mem) == ESP_ERR_NOT_FINISHED)
            goto exit;
        if (even)
            DAC_WRITE_OPUS(__muziek_blije_muziekjes_muziekje_5_opus, mem);
        else
            DAC_WRITE_OPUS(__muziek_blije_muziekjes_muziekje_6_opus, mem);
        even = !even;
    }
    if (gpio_states[MUXED_INPUT_HEART_L_CLIP] || 
        gpio_states[MUXED_INPUT_HEART_R_CLIP]) {
        gpio_states[MUXED_INPUT_HEART_L_CLIP] = 0;
        gpio_states[MUXED_INPUT_HEART_R_CLIP] = 0;
        play_littlefs_opus_file("/littlefs/music/heart_clip/");
    }
    if (gpio_states[MUXED_INPUT_SQUARE_L_BUTTON] || 
        gpio_states[MUXED_INPUT_SQUARE_R_BUTTON]) {
        gpio_states[MUXED_INPUT_SQUARE_L_BUTTON] = 0;
        gpio_states[MUXED_INPUT_SQUARE_R_BUTTON] = 0;
        if (DAC_WRITE_OPUS(__muziek_ik_voel_me_een_beetje_verdrietig_opus, mem) == ESP_ERR_NOT_FINISHED)
            goto exit;
        if (even)
            DAC_WRITE_OPUS(__muziek_verdrietige_muziekjes_muziekje_7_opus, mem);
        else
            DAC_WRITE_OPUS(__muziek_verdrietige_muziekjes_muziekje_8_opus, mem);
        even = !even;
    }
    if (gpio_states[MUXED_INPUT_SQUARE_L_CLIP] || 
        gpio_states[MUXED_INPUT_SQUARE_R_CLIP]) {
        gpio_states[MUXED_INPUT_SQUARE_L_CLIP] = 0;
        gpio_states[MUXED_INPUT_SQUARE_R_CLIP] = 0;
        play_littlefs_opus_file("/littlefs/music/square_clip/");
    }
    if (gpio_states[MUXED_INPUT_TRIANGLE_L_BUTTON] || 
        gpio_states[MUXED_INPUT_TRIANGLE_R_BUTTON]) {
        gpio_states[MUXED_INPUT_TRIANGLE_L_BUTTON] = 0;
        gpio_states[MUXED_INPUT_TRIANGLE_R_BUTTON] = 0;
        if (DAC_WRITE_OPUS(__muziek_ik_ben_boos__opus, mem) == ESP_ERR_NOT_FINISHED)
            goto exit;
        if (even)
            DAC_WRITE_OPUS(__muziek_boze_muziekjes_muziekje_3_opus, mem);
        else
            DAC_WRITE_OPUS(__muziek_boze_muziekjes_muziekje_4_opus, mem);
        even = !even;
    }
    if (gpio_states[MUXED_INPUT_TRIANGLE_L_CLIP] || 
        gpio_states[MUXED_INPUT_TRIANGLE_R_CLIP]) {
        gpio_states[MUXED_INPUT_TRIANGLE_L_CLIP] = 0;
        gpio_states[MUXED_INPUT_TRIANGLE_R_CLIP] = 0;
        play_littlefs_opus_file("/littlefs/music/triangle_clip/");
    }
    if (gpio_states[MUXED_INPUT_STAR_L_BUTTON] || 
        gpio_states[MUXED_INPUT_STAR_R_BUTTON]) {
        gpio_states[MUXED_INPUT_STAR_L_BUTTON] = 0;
        gpio_states[MUXED_INPUT_STAR_R_BUTTON] = 0;
        if (DAC_WRITE_OPUS(__muziek_wat_een_verassing__opus, mem) == ESP_ERR_NOT_FINISHED)
            goto exit;
        if (even)
            DAC_WRITE_OPUS(__muziek_verbaasde_muziekjes_muziekje_1_opus, mem);
        else
            DAC_WRITE_OPUS(__muziek_verbaasde_muziekjes_muziekje_2_opus, mem);
        even = !even;
    }
    if (gpio_states[MUXED_INPUT_STAR_L_CLIP] || 
        gpio_states[MUXED_INPUT_STAR_R_CLIP]) {
        gpio_states[MUXED_INPUT_STAR_L_CLIP] = 0;
        gpio_states[MUXED_INPUT_STAR_R_CLIP] = 0;
        play_littlefs_opus_file("/littlefs/music/star_clip/");
    }
    if (gpio_states[MUXED_INPUT_BEAK_SWITCH]) {
        gpio_states[MUXED_INPUT_BEAK_SWITCH] = 0;
        if (play_littlefs_opus_file("/littlefs/music/beak_switch/") == ESP_ERR_NOT_FINISHED)
            goto exit;
        if (even)
            DAC_WRITE_OPUS(__muziek_snavel_knop_het_is_tijd_om_te_zingen___muziekje_9_opus, mem);
        else
            DAC_WRITE_OPUS(__muziek_snavel_knop_wil_je_mij_horen_zingen___muziekje_10_opus, mem);
        even = !even;
    }
    
exit:
    for (int i = 0; i < 8; i++)
        if (gpio_output_states[i]) {
            gpio_output_states[i] = 0;
            gpio_output_states[(i + 1) & 7] = 1;
            break;
        }
}

void app_main(void) {
    int err;

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
    
    dac_data = &(struct dac_data) {
        .handle = dac_handle,
        .data_front = malloc(OPUS_MAX_FRAME_SIZE),
        .data_back = malloc(OPUS_MAX_FRAME_SIZE),
        .data_front_size = 0
    };

    if (!dac_data->data_front || !dac_data->data_back) {
        ESP_LOGE(TAG, "Failed to allocate memory for dac output buffers\n");
        return;
    }
    
    dac_write_opus_mutex = xSemaphoreCreateMutex();
    if (!dac_write_opus_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex for the dac writing function\n");
        return;
    }
   
    DAC_WRITE_OPUS(__pauw_opstart_geluid_opus, mem);
    DAC_WRITE_OPUS(
        _______hallo_ik_ben_een_pauw__kom_speel_je_mee_met_mij_want_samen_zijn_met_jou__dat_maakt_me_reuze_blij_opus, 
        mem);
    
    muxed_gpio_setup(&on_gpio_states_changed);

    /* Format the littlefs partition if the beak button is pressed for 5 seconds. */
    bool input_switch_levels[19];
    muxed_gpio_get_input_switch_levels(&input_switch_levels);
    if (input_switch_levels[MUXED_INPUT_BEAK_SWITCH]) {
        vTaskDelay(LITTLEFS_FORMAT_BEAK_PRESSED_TIMEOUT / portTICK_PERIOD_MS);
        muxed_gpio_get_input_switch_levels(&input_switch_levels);
        if (input_switch_levels[MUXED_INPUT_BEAK_SWITCH]) {
            ESP_LOGW(TAG, "Pressed the beak button for %d seconds. Formatting...",
                     LITTLEFS_FORMAT_BEAK_PRESSED_TIMEOUT / 1000);
            esp_littlefs_format(conf.partition_label);
        }
    }
    
    if (input_switch_levels[MUXED_INPUT_LEARN_SWITCH])
        mode = LEARN;
    else if (input_switch_levels[MUXED_INPUT_PLAY_SWITCH])
        mode = PLAY;
    else
        mode = MUSIC;
    
    for (;;) {
        switch (mode) {
            case LEARN:
                mode_learn();
                break;
            case PLAY:
                mode_play();
                break;
            case MUSIC:
                mode_music();
                break;
        }
        
        muxed_gpio_set_output_levels(&gpio_output_states);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    /* NOTE: We should never reach this code. */
    /* All done, unmount partition and disable LITTLEFS. */
    esp_vfs_littlefs_unregister(conf.partition_label);
    ESP_LOGI(TAG, "LITTLEFS unmounted");
    
    opus_decoder_destroy(decoder);
    
    free(dac_data->data_front);
    free(dac_data->data_back);
    
    spp_task_task_shut_down();
    
    vTaskDelete(dac_write_data_task_handle);
    dac_write_data_task_handle = NULL;
    vSemaphoreDelete(dac_write_opus_mutex);
  
    ESP_LOGI(TAG, "Done!\n");
    return;
}
