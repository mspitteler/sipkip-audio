#ifndef VFS_ACCEPTOR_H
#define VFS_ACCEPTOR_H

#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"

#define SPP_SERVER_NAME "SPP_SERVER"
#define SPP_MAX_ARGC 16
#define SPP_MAX_ARG_LEN 256

#define DECL_COMMAND(name)                                                                                         \
    static esp_err_t command_##name(int argc, char **argv);

/**
 * @brief     read data from file descriptor
 */
void spp_read_handle(void *param);

/**
 * @brief     callback for SPP event
 */
void esp_spp_cb(uint16_t e, void *p);

void esp_spp_stack_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);

/**
 * @brief     callback for GAP event
 */
void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

#endif /* VFS_ACCEPTOR_H */
