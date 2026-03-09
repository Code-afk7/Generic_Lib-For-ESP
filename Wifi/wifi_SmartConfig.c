// Local Header files 
#include "wifi_SmartConfig.h"

// Wifi Header files
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"  
#include "esp_smartconfig.h"

// freeRTOS Header files
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// NVS Header files 
// It helps Store SSID and Password in the flash memory of the ESP32, so that we don't have to hardcode them in the code.
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "Wifi";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;

esp_err_t nvs_save_wifi(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_str(handle, NVS_KEY_PASS, password);
    if (err != ESP_OK) goto cleanup;

    err = nvs_commit(handle);

cleanup:
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t nvs_load_wifi(char *ssid, size_t ssid_len,
                                char *password, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found (first boot?)");
        return err;
    }

    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK) goto cleanup;

    err = nvs_get_str(handle, NVS_KEY_PASS, password, &pass_len);

cleanup:
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded SSID from NVS: %s", ssid);
    }
    return err;
}

void nvs_erase_wifi(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "NVS WiFi credentials erased");
    }
}

void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc =
                (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Disconnected, reason: %d", disc->reason);

            if (s_retry_count < MAX_RETRY) {
                s_retry_count++;
                ESP_LOGI(TAG, "Retrying (%d/%d)...", s_retry_count, MAX_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "Max retries reached");
                /* Erase bad credentials so next boot starts SmartConfig */
                nvs_erase_wifi();
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    } else if (event_base == SC_EVENT) {
        switch (event_id) {
        case SC_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "SmartConfig: scan done");
            break;

        case SC_EVENT_FOUND_CHANNEL:
            ESP_LOGI(TAG, "SmartConfig: found channel");
            break;

        case SC_EVENT_GOT_SSID_PSWD: {
            smartconfig_event_got_ssid_pswd_t *sc_data = 
                (smartconfig_event_got_ssid_pswd_t *)event_data;

            wifi_config_t wifi_config = { 0 };
            memcpy(wifi_config.sta.ssid,     sc_data->ssid,     sizeof(wifi_config.sta.ssid));
            memcpy(wifi_config.sta.password, sc_data->password, sizeof(wifi_config.sta.password));

            ESP_LOGI(TAG, "SmartConfig SSID: %s", sc_data->ssid);
            ESP_LOGI(TAG, "SmartConfig Password: %s", sc_data->password);
            
            /* Save credentials to NVS */
            nvs_save_wifi((char *)sc_data->ssid, (char *)sc_data->password);

            /* Connect with the new credentials */
            ESP_ERROR_CHECK(esp_wifi_disconnect());
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            s_retry_count = 0;
            esp_wifi_connect();
            break;
        }

        case SC_EVENT_SEND_ACK_DONE:
            ESP_LOGI(TAG, "SmartConfig: ACK sent to phone");
            xEventGroupSetBits(s_wifi_event_group, SC_DONE_BIT);
            break;

        default:
            break;
        }
    }
}

void smartconfig_task(void *param)
{
    ESP_LOGI(TAG, "Starting SmartConfig...");

    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));

    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));

    /* Wait until SmartConfig signals completion or timeout (15 s) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           SC_DONE_BIT | WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (!(bits & (SC_DONE_BIT | WIFI_CONNECTED_BIT))) {
        ESP_LOGW(TAG, "SmartConfig timed out");
    }

    esp_smartconfig_stop();
    vTaskDelete(NULL);
}

void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}

void __Wifi_Start(void)
{
    /* Initialize NVS flash */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash and reinitialising...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();

    /* Try to load saved credentials */
    char ssid[32]     = { 0 };
    char password[64] = { 0 };

    bool has_credentials = (nvs_load_wifi(ssid, sizeof(ssid),
                                          password, sizeof(password)) == ESP_OK);

    if (has_credentials) {
        /* Connect using stored credentials */
        ESP_LOGI(TAG, "Connecting with stored credentials...");
        wifi_config_t wifi_config = { 0 };
        strlcpy((char *)wifi_config.sta.ssid,     ssid,     sizeof(wifi_config.sta.ssid));
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        /* Wait for connection result */
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(15000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected using stored credentials!");
            return; /* Done — no SmartConfig needed */
        }

        /* Connection failed; fall through to SmartConfig */
        ESP_LOGW(TAG, "Stored credentials failed, starting SmartConfig");
        /* WiFi was already started, just reset retry counter */
        s_retry_count = 0;
    } else {
        /* No stored credentials — start WiFi then SmartConfig */
        ESP_LOGI(TAG, "No stored credentials, starting SmartConfig");
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    /* Launch SmartConfig task */
    xTaskCreate(smartconfig_task, "smartconfig_task", 4000, NULL, 1, NULL);

    /* Wait for SmartConfig + connection */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected via SmartConfig!");
    } else {
        ESP_LOGE(TAG, "WiFi connection ultimately failed");
    }
}


