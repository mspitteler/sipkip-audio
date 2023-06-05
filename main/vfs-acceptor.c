#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/unistd.h>
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
#include "commands.h"
#include "utils.h"

#define PRINT_PROMPT()                                                                                             \
    dprintf(spp_fd, "%d@%s > ", spp_fd, DEVICE_NAME)

static const char *const TAG = "vfs-acceptor";

static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t role_slave = ESP_SPP_ROLE_SLAVE;

int spp_fd = -1;

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
