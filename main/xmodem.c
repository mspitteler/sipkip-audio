#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "xmodem.h"

static const char *const TAG = "xmodem";

static inline int xmodem_read_byte(int fd, TickType_t ticks) {
    unsigned char c;
    while (!read(fd, &c, 1)) {
        vTaskDelay(1);
        if (ticks-- <= 0)
            return -1;
    }
    return c;
}

static uint16_t xmodem_crc16_ccitt(const unsigned char *buf, ssize_t buf_size) {
    uint16_t crc = 0;
    while (--buf_size >= 0) {
        int i;
        crc ^= (uint16_t) *buf++ << 8;
        for (i = 0; i < 8; i++)
            if (crc & 0x8000)
                crc = crc << 1 ^ 0x1021;
            else
                crc <<= 1;
    }
    return crc;
}

static bool xmodem_check_buffer(int crc, const unsigned char *buf, ssize_t buf_size) {
    if (crc) {
        uint16_t real_crc = xmodem_crc16_ccitt(buf, buf_size);
        uint16_t xmodem_crc = ((uint16_t)buf[buf_size] << 8) | buf[buf_size + 1];
        if (real_crc == xmodem_crc)
            return true;
        else
            ESP_LOGE(TAG, "CRC doesn't match: %d vs %d", real_crc, xmodem_crc);
    } else {
        unsigned char checksum = 0;
        for (ssize_t i = 0; i < buf_size; ++i)
            checksum += buf[i];

        if (checksum == buf[buf_size])
            return true;
        else
            ESP_LOGE(TAG, "Checksum doesn't match: %d vs %d", checksum, buf[buf_size]);
    }
    return false;
}

static inline void xmodem_flush_input(int fd) {
    while (xmodem_read_byte(fd, ((XMODEM_READ_TIMEOUT_MS / portTICK_PERIOD_MS) * 3) >> 1) >= 0);
}

esp_err_t xmodem_receiver_start(int spp_fd, int spiffs_fd) {
    unsigned char *xmodem_buf = NULL;
    unsigned char *p;
    int xmodem_buf_size = 0, crc = 0;
    unsigned char trychar = 'C';
    unsigned char packet_number = 1;
    int c;
    esp_err_t err = ESP_OK;
    int retry, retransmit = XMODEM_MAX_RETRANSMIT;
   
    for (;;) {
        for (retry = 0; retry < 16; ++retry) {
            ESP_LOGI(TAG, "Trying to receive for the %dth time", retry);
            if (trychar)
                write(spp_fd, &trychar, 1);
            
            if ((c = xmodem_read_byte(spp_fd, (XMODEM_READ_TIMEOUT_MS / portTICK_PERIOD_MS) << 1)) >= 0) {
                switch (c) {
                case XMODEM_SOH:
                    if (xmodem_buf_size != 0 && xmodem_buf_size != 128) {
                        ESP_LOGE(TAG, "Cannot switch to classic XMODEM, when already initialized as XMODEM 1K!");
                        err = ESP_FAIL;
                        goto exit;
                    }
                    ESP_LOGI(TAG, "Detected classic XMODEM transmitter");
                    xmodem_buf_size = 128;
                    goto start_receive;
                case XMODEM_STX:
                    if (xmodem_buf_size != 0 && xmodem_buf_size != 1024) {
                        ESP_LOGE(TAG, "Cannot switch to XMODEM 1K, when already initialized as classic XMODEM!");
                        err = ESP_FAIL;
                        goto exit;
                    }
                    ESP_LOGI(TAG, "Detected XMODEM 1K transmitter");
                    xmodem_buf_size = 1024;
                    goto start_receive;
                case XMODEM_EOT:
                    ESP_LOGI(TAG, "Received file with fd %d successfully", spiffs_fd);
                    xmodem_flush_input(spp_fd);
                    write(spp_fd, (char []) {XMODEM_ACK}, 1);
                    goto exit; /* normal end */
                case XMODEM_CAN:
                    if ((c = xmodem_read_byte(spp_fd, XMODEM_READ_TIMEOUT_MS / portTICK_PERIOD_MS)) == XMODEM_CAN) {
                        ESP_LOGW(TAG, "Transmitter canceled the file transfer");
                        xmodem_flush_input(spp_fd);
                        write(spp_fd, (char []) {XMODEM_ACK}, 1);
                        /* canceled by remote */
                        err = ESP_FAIL;
                        goto exit;
                    }
                    break;
                default:
                    break;
                }
            }
        }
        /* Try different trychar, if after 16 retries, the transmitter still didn't respond. */
        if (trychar == 'C') {
            trychar = XMODEM_NAK;
            continue;
        }
        
        ESP_LOGE(TAG, "Sync error, aborting");
        xmodem_flush_input(spp_fd);
        write(spp_fd, (char []) {XMODEM_CAN}, 1);
        write(spp_fd, (char []) {XMODEM_CAN}, 1);
        write(spp_fd, (char []) {XMODEM_CAN}, 1);
        /* Sync error. */
        err = ESP_FAIL;
        goto exit;
    start_receive:
        if (!xmodem_buf)
            xmodem_buf = malloc(xmodem_buf_size + 3 + 2 + 1); /* 1024 for XMODEM 1K + 3 head chars + 2 crc + nul */

        if (!xmodem_buf) {
            ESP_LOGE(TAG, "Failed to allocate memory for xmodem_buffer: %s", strerror(errno));
            xmodem_flush_input(spp_fd);
            write(spp_fd, (char []) {XMODEM_CAN}, 1);
            write(spp_fd, (char []) {XMODEM_CAN}, 1);
            write(spp_fd, (char []) {XMODEM_CAN}, 1);
            /* Failed to allocate memory. */
            err = ESP_FAIL;
            goto exit;
        }
        if (trychar == 'C')
            crc = 1;
        
        trychar = 0;
        p = xmodem_buf;
        *p++ = c;
        
        for (int i = 0;  i < (xmodem_buf_size + (crc ? 1 : 0) + 3); ++i) {
            if ((c = xmodem_read_byte(spp_fd, XMODEM_READ_TIMEOUT_MS / portTICK_PERIOD_MS)) < 0)
                goto reject;
            *p++ = c;
        }
        
        if (xmodem_buf[1] == (unsigned char)(~xmodem_buf[2]) &&
            (xmodem_buf[1] == packet_number || xmodem_buf[1] == (unsigned char)packet_number - 1) &&
            xmodem_check_buffer(crc, &xmodem_buf[3], xmodem_buf_size)) {
            if (xmodem_buf[1] == packet_number) {
                write(spiffs_fd, &xmodem_buf[3], xmodem_buf_size);
                ++packet_number;
                retransmit = XMODEM_MAX_RETRANSMIT + 1;
            }
            if (--retransmit <= 0) {
                ESP_LOGE(TAG, "Too many retries");
                xmodem_flush_input(spp_fd);
                write(spp_fd, (char []) {XMODEM_CAN}, 1);
                write(spp_fd, (char []) {XMODEM_CAN}, 1);
                write(spp_fd, (char []) {XMODEM_CAN}, 1);
                /* Too many retry error. */
                err = ESP_FAIL;
                goto exit;
            }
            write(spp_fd, (char []) {XMODEM_ACK}, 1);
            continue;
        }
    reject:
        ESP_LOGW(TAG, "Rejecting packet, because of incorrect CRC/checksum or short read");
        xmodem_flush_input(spp_fd);
        write(spp_fd, (char []) {XMODEM_NAK}, 1);
    }
exit:
    /* Free memory if apliccable. */
    if (xmodem_buf)
        free(xmodem_buf);
    
    return err;
}
