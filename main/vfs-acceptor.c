#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/unistd.h>
#include <dirent.h>
#include <ctype.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "driver/uart.h"

#include "vfs-acceptor.h"
#include "spp-task.h"
#include "sipkip-audio.h"
#include "xmodem.h"
#include "utils.h"

#define PRINT_PROMPT()                                                                                             \
    dprintf(spp_fd, "%d@%s > ", spp_fd, DEVICE_NAME)

static const char *const TAG = "vfs-acceptor";

static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t role_slave = ESP_SPP_ROLE_SLAVE;

DECL_COMMAND(help) DECL_COMMAND(rx) DECL_COMMAND(rm) DECL_COMMAND(mv) DECL_COMMAND(cp) DECL_COMMAND(speak)
DECL_COMMAND(mkdir) DECL_COMMAND(rmdir) DECL_COMMAND(ls) DECL_COMMAND(cwd) DECL_COMMAND(pwd)

static int spp_fd = -1;

static const struct vfs_commands commands[] = {
    DEF_COMMAND(help, "[command_name]", "Prints help information about command [command_name].")
    DEF_COMMAND(rx, "[filename]", "Starts receiving file [filename] with protocol XMODEM.")
    DEF_COMMAND(rm, "[filename]", "Removes file [filename].")
    DEF_COMMAND(mv, "[src_name] [dst_name]", "Moves file or directory [src_name] to [dst_name].")
    DEF_COMMAND(cp, "[src_filename] [dst_filename]", "Copies file [src_filename] to [dst_filename].")
    DEF_COMMAND(speak, "[opus_filename] [opus_packets_filename]", "Plays the opus stream contained in [opus_filename]")
    DEF_COMMAND(mkdir, "[dirname]", "Creates directory [dirname].")
    DEF_COMMAND(rmdir, "[dirname]", "Removes directory [dirname] (only if it is empty).")
    DEF_COMMAND(ls, "[name]", "Lists files and directories in [name], or only [name] if [name] is a file.")
    DEF_COMMAND(cwd, "[dirname]", "Change current working directory to [dirname]")
    DEF_COMMAND(pwd, "", "Prints current working directory.")
};

static inline const struct vfs_commands *find_command(const char *name) {
    const struct vfs_commands *command;
    for (command = commands; command != commands + sizeof(commands) / sizeof(*commands) &&
         strcmp(name, command->name); command++);

    if (command == commands + sizeof(commands) / sizeof(*commands))
        return NULL;
    return command;
}

IMPL_COMMAND(help) {
    /* General help. */
    if (argc == 1) {
        dprintf(spp_fd, "Available commands:\n");
        for (const struct vfs_commands *command = commands;
             command != commands + sizeof(commands) / sizeof(*commands); command++)
             dprintf(spp_fd, "\t* %s\n", command->name);
        dprintf(spp_fd, "Type `help [command_name]' for more information about a specific command.\n");
        return ESP_OK;
    /* Help for specific command. */
    } else if (argc == 2) {
        const struct vfs_commands *command = find_command(argv[1]);
        if (!command) {
            dprintf(spp_fd, "Unknown command: %s!\n", argv[1]);
            return ESP_OK;
        }

        dprintf(spp_fd, "%s", command->usage);
        return ESP_OK;
    }
    /* Wrong amount of arguments. */
    return ESP_ERR_INVALID_ARG;
}

IMPL_COMMAND(rx) {
    int littlefs_fd;
    bool remove_file = false;
    
    if (argc != 2)
        /* Wrong amount of arguments, or first argument isn't a path. */
        return ESP_ERR_INVALID_ARG;

    if (strstr(argv[1], LITTLEFS_BASE_PATH"/") != argv[1]) {
        dprintf(spp_fd, "Invalid file name: %s, doesn't start with %s\n", argv[1], LITTLEFS_BASE_PATH"/");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Detected valid filename: %s", argv[1]);

    littlefs_fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL);
    if (littlefs_fd < 0) {
        dprintf(spp_fd, "Failed to open file %s: %s\n", argv[1], strerror(errno));
        return ESP_OK;
    }

    if (xmodem_receiver_start(spp_fd, littlefs_fd) != ESP_OK) {
        dprintf(spp_fd, "Failed to receive file %s using XMODEM\n", argv[1]);
        remove_file = true;
    }

    /* Close file, and unlink if the transfer failed. */
    close(littlefs_fd);
    if (remove_file)
        command_rm(argc, argv);
    return ESP_OK;
}

IMPL_COMMAND(rm) {
    int ret;
    
    if (argc != 2)
        /* Wrong amount of arguments, or first argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Removing file: %s", argv[1]);
    
    ret = remove(argv[1]);
    if (ret) {
        dprintf(spp_fd, "Failed to remove file %s: %s\n", argv[1], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(mv) {
    int ret;
    
    if (argc != 3)
        /* Wrong amount of arguments, or first and second argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Moving file: %s to %s", argv[1], argv[2]);
    
    ret = rename(argv[1], argv[2]);
    if (ret) {
        dprintf(spp_fd, "Failed to move file %s to %s: %s\n", argv[1], argv[2], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(cp) {
    int ret;
    
    if (argc != 3)
        /* Wrong amount of arguments, or first and second argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Copying file: %s to %s", argv[1], argv[2]);
    
    ret = copy_file(argv[1], argv[2]);
    if (ret) {
        dprintf(spp_fd, "Failed to copy file %s to %s: %s\n", argv[1], argv[2], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(speak) {
    unsigned int file_opus_packets_len;
    
    if (argc != 3)
        /* Wrong amount of arguments, or first and second argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Playing opus file: %s, with opus packets file: %s", argv[1], argv[2]);
    
    FILE *file_opus = fopen(argv[1], "rb");
    if (!file_opus) {
        dprintf(spp_fd, "Failed to open file %s: %s", argv[1], strerror(errno));
        return ESP_OK;
    }
    
    FILE *file_opus_packets = fopen(argv[2], "rb");
    if (!file_opus_packets) {
        dprintf(spp_fd, "Failed to open file %s: %s", argv[2], strerror(errno));
        goto exit;
    }
    
    fseek(file_opus_packets, 0, SEEK_END);
    file_opus_packets_len = ftell(file_opus_packets);
    fseek(file_opus_packets, 0, SEEK_SET);
    
    exit_dac_write_opus_loop = true;    
    DAC_WRITE_OPUS(file_opus, file);
    
exit:
    if (file_opus)
        fclose(file_opus);
    if (file_opus_packets)
        fclose(file_opus_packets);
    
    return ESP_OK;
}

IMPL_COMMAND(mkdir) {
    int ret;
    
    if (argc != 2)
        /* Wrong amount of arguments, or first argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Creating directory: %s", argv[1]);
    
    ret = mkdir(argv[1], 0777);
    if (ret) {
        dprintf(spp_fd, "Failed to create directory %s: %s\n", argv[1], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(rmdir) {
    int ret;
    
    if (argc != 2)
        /* Wrong amount of arguments, or first argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Removing directory: %s\n", argv[1]);
    
    ret = rmdir(argv[1]);
    if (ret) {
        dprintf(spp_fd, "Failed to remove directory %s: %s\n", argv[1], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(ls) {
    char buffer[(CONFIG_LITTLEFS_OBJ_NAME_LEN + 1) * LITTLEFS_MAX_DEPTH];
    if (argc == 1) {
        if (!getcwd(buffer, sizeof(buffer) / sizeof(*buffer))) {
            dprintf(spp_fd, "Couldn't determine current working directory: %s!\n", strerror(errno));
            return ESP_OK;
        }
    } else if (argc == 2) {
        strncpy(buffer, argv[1], sizeof(buffer) / sizeof(*buffer));
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    DIR *dir = opendir(buffer);
    if (!dir) {
        dprintf(spp_fd, "Couldn't open directory %s: %s!\n", buffer, strerror(errno));
        return ESP_OK;
    }

    for (;;) {
        struct dirent* de = readdir(dir);
        if (!de)
            break;
        
        dprintf(spp_fd, "%s%s\n", de->d_name, &"/"[de->d_type != DT_DIR]); /* Print trailing `/' if it's a directory. */
    }

    closedir(dir);
    return ESP_OK;
}

IMPL_COMMAND(cwd) {
    if (argc != 2)
        /* Wrong amount of arguments, or first argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    if (chdir(argv[1])) {
        dprintf(spp_fd, "Couldn't change current working directory to %s: %s!\n", argv[1], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(pwd) {
    char buffer[(CONFIG_LITTLEFS_OBJ_NAME_LEN + 1) * LITTLEFS_MAX_DEPTH];
    
    if (argc != 1)
        return ESP_ERR_INVALID_ARG;
    
    if (!getcwd(buffer, sizeof(buffer) / sizeof(*buffer))) {
        dprintf(spp_fd, "Couldn't determine current working directory: %s!\n", strerror(errno));
        return ESP_OK;
    }
    dprintf(spp_fd, "%s\n", buffer);
    
    return ESP_OK;
}

void spp_read_handle(void *param) {
    char dummy, buf[SPP_MAX_ARG_LEN], *argv[SPP_MAX_ARGC];
    esp_err_t err;
    spp_fd = (ptrdiff_t)param;
   
    PRINT_PROMPT();

    for (;;) {
        const struct vfs_commands *command;
        static const char *const delim = " \t\n\r";
        int argc = 0;
        
        ssize_t ret = read(spp_fd, buf, sizeof(buf) / sizeof(*buf));
        if (ret < 0) {
            ESP_LOGE(TAG, "Couldn't read from vfs fd %d: %s!", spp_fd, strerror(errno));
            goto exit;
        } else if (!ret) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }
        
        while (read(spp_fd, &dummy, sizeof(dummy)) > 0);
       
        buf[MIN(ret, sizeof(buf) / sizeof(*buf) - 1)] = '\0';
        char *pch = strtok(buf, delim);
        /* User just pressed enter probably if pch is NULL. */
        if (pch) {
            while (pch && argc < SPP_MAX_ARGC) {
                argv[argc] = strdup(pch);
                if (!argv[argc]) {
                    ESP_LOGE(TAG, "strdup failed: %s!", strerror(errno));
                    goto exit;
                }
                
                ESP_LOGI(TAG, "Argument argv[%d]=\"%s\"", argc, argv[argc]);
                
                pch = strtok(NULL, delim);
                argc++;
            }
            
            command = find_command(argv[0]);
            if (!command) {
                dprintf(spp_fd, "Unknown command: %s!\n", argv[0]);
            } else if ((err = command->fn(argc, argv))) {
                dprintf(spp_fd, "Command %s error: %s!\n", command->name, esp_err_to_name(err));
                dprintf(spp_fd, "%s", command->usage);
            }
            
            for (int i = 0; i < argc; i++)
                free(argv[i]);
        }
        
        PRINT_PROMPT();
    }
    
exit:
    spp_fd = -1;
    spp_wr_task_shut_down();
}

void esp_spp_cb(uint16_t e, void *p) {
    esp_spp_cb_event_t event = e;
    esp_spp_cb_param_t *param = p;
    char bda_str[18] = {0};

    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(TAG, "ESP_SPP_INIT_EVT");
            /* Enable SPP VFS mode */
            esp_spp_vfs_register();
            esp_spp_start_srv(sec_mask, role_slave, 0, SPP_SERVER_NAME);
        } else {
            ESP_LOGE(TAG, "ESP_SPP_INIT_EVT status:%d", param->init.status);
        }
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        ESP_LOGI(TAG, "ESP_SPP_DISCOVERY_COMP_EVT");
        break;
    case ESP_SPP_OPEN_EVT:
        ESP_LOGI(TAG, "ESP_SPP_OPEN_EVT");
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(TAG, "ESP_SPP_CLOSE_EVT status:%d handle:%d close_by_remote:%d", param->close.status,
                 param->close.handle, param->close.async);
        break;
    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(TAG, "ESP_SPP_START_EVT handle:%d sec_id:%d scn:%d", param->start.handle, param->start.sec_id,
                     param->start.scn);
            esp_bt_dev_set_device_name(DEVICE_NAME);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        } else {
            ESP_LOGE(TAG, "ESP_SPP_START_EVT status:%d", param->start.status);
        }
        break;
    case ESP_SPP_CL_INIT_EVT:
        ESP_LOGI(TAG, "ESP_SPP_CL_INIT_EVT");
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        ESP_LOGI(TAG, "ESP_SPP_SRV_OPEN_EVT status:%d handle:%d, rem_bda:[%s]", param->srv_open.status,
                 param->srv_open.handle, bd_address_to_string(param->srv_open.rem_bda, bda_str, sizeof(bda_str)));
        if (param->srv_open.status == ESP_SPP_SUCCESS) {
            spp_wr_task_start_up(&spp_read_handle, param->srv_open.fd);
        }
        break;
    default:
        break;
    }
}

void esp_spp_stack_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    /* To avoid stucking Bluetooth stack, we dispatch the SPP callback event to the other lower priority task */
    spp_task_work_dispatch(&esp_spp_cb, event, param, sizeof(esp_spp_cb_param_t), NULL);
}

void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "authentication success: %s", param->auth_cmpl.device_name);
            esp_log_buffer_hex(TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            ESP_LOGI(TAG, "Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            ESP_LOGI(TAG, "Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

#if (CONFIG_BT_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %d", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%d", param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_MODE_CHG_EVT mode:%d", param->mode_chg.mode);
        break;

    default: {
        ESP_LOGI(TAG, "event: %d", event);
        break;
    }
    }
    return;
}
