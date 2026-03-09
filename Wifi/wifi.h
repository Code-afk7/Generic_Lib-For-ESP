#ifndef __WIFI_H
#define __WIFI_H


#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#define NVS_NAMESPACE       "wifi_cfg"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASS        "password"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define SC_DONE_BIT         BIT2

#define MAX_RETRY           5

void wifi_event_handler(void *arg, esp_event_base_t event_base,int32_t event_id, void *event_data);
void wifi_init(void);
esp_err_t nvs_save_wifi(const char *ssid, const char *password);
esp_err_t nvs_load_wifi(char *ssid, size_t ssid_len, char *password, size_t pass_len);
void nvs_erase_wifi(void);
void smartconfig_task(void *param);
void __Wifi_Start(void);

#endif /* __WIFI_H__ */