#ifndef XMODEM_H
#define XMODEM_H

#include "esp_err.h"

#define XMODEM_SOH  0x01
#define XMODEM_STX  0x02
#define XMODEM_EOT  0x04
#define XMODEM_ACK  0x06
#define XMODEM_NAK  0x15
#define XMODEM_CAN  0x18
#define XMODEM_CTRLZ 0x1A
#define XMODEM_READ_TIMEOUT_MS 1000
#define XMODEM_MAX_RETRANSMIT 25

esp_err_t xmodem_receiver_start(int spp_fd, const char *spiffs_filename);

#endif /* XMODEM_H */
