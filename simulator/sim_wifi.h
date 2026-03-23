#ifndef SIM_WIFI_H
#define SIM_WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include "../src/os/os.h"

void wifi_poll(void);
wifi_status_t wifi_get_status(void);
const char *wifi_get_ip(void);
const char *wifi_get_ssid(void);
void wifi_set_http_required(bool required);
bool wifi_get_http_required(void);

#endif // SIM_WIFI_H
