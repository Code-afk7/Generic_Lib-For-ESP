#include "PCF8563.h"

#include "esp_sntp.h"
#include "driver/i2c.h"
#include "i2cdev/i2cdev.h"
#include "pcf8563/pcf8563.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

#include "nvs_flash.h"
#include "esp_netif.h"

static const char *TAG = "RTC";

int* RTC_data = NULL;

#define RTC_READ_INTERVAL_MS    1000   // how often to read the RTC
#define RTC_TASK_STACK_SIZE     4096   // dedicated task stack (safe for I2C)

/* ── Globals ─────────────────────────────────────────────────────────────── */
static i2c_dev_t    dev;
static TaskHandle_t rtc_task_handle = NULL;  // used by timer to notify task

/* =========================================================================
 * Timer callback – runs in Tmr Svc context
 * ONLY sends a task notification. No I2C, no logging, no blocking.
 * ========================================================================= */
void set_rtc_time(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now  = 0;
    struct tm t = {};
    int retries = 0;
    const int max = 20;

    while (t.tm_year < (2020 - 1900) && retries < max) {
        ESP_LOGI(TAG, "Waiting for SNTP sync... (%d/%d)", retries + 1, max);
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
        localtime_r(&now, &t);
        retries++;
    }

    if (t.tm_year < (2020 - 1900)) {
        ESP_LOGE(TAG, "SNTP sync failed – RTC not updated");
        esp_sntp_stop();
        return;
    }

    // ✅ Apply UTC offset BEFORE writing to RTC
    time_t local_epoch = now + (5 * 3600) + (30 * 60);
    struct tm local_t  = {};
    gmtime_r(&local_epoch, &local_t);

    esp_err_t err = pcf8563_set_time(&dev, &local_t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcf8563_set_time failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PCF8563 synced: %04d-%02d-%02d %02d:%02d:%02d",
                 local_t.tm_year + 1900, local_t.tm_mon + 1, local_t.tm_mday,
                 local_t.tm_hour, local_t.tm_min, local_t.tm_sec);
    }

    esp_sntp_stop();
}

int* read_rtc_time(void)
{
    struct tm rtc_time = {0};
    bool valid = false;

    RTC_data[0] = RTC_data[1] = RTC_data[2] = 0;

    esp_err_t err = pcf8563_get_time(&dev, &rtc_time, &valid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcf8563_get_time failed: %s", esp_err_to_name(err));
        return NULL;
    }

    if (!valid) {
        ESP_LOGW(TAG, "VL flag set — add backup battery!");
        return NULL;
    }

    RTC_data[0] = rtc_time.tm_hour;
    RTC_data[1] = rtc_time.tm_min;
    RTC_data[2] = rtc_time.tm_sec;
    return RTC_data;
}

void setup_clkout(void)
{
    ESP_ERROR_CHECK(pcf8563_set_clkout(&dev, PCF8563_32768HZ));
    ESP_LOGI(TAG, "CLKOUT: 32 kHz square wave");
}

void __RTC_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 PCF8563 RTC ===");

    RTC_data = (int*)malloc(sizeof(int) * 3);
    if (!RTC_data) {
        ESP_LOGE(TAG, "Failed to allocate RTC_data");
        return;
    }

    ESP_ERROR_CHECK(i2cdev_init());
    ESP_ERROR_CHECK(pcf8563_init_desc(&dev, I2C_PORT, PIN_SDA, PIN_SCL));

    // ✅ Check VL first, then sync
    struct tm __time = {};
    bool valid = false;
    ESP_ERROR_CHECK(pcf8563_get_time(&dev, &__time, &valid));

    if (!valid) {
        ESP_LOGW(TAG, "VL flag set — add backup battery!");
    }

    set_rtc_time();   // always sync on boot
    setup_clkout();
    read_rtc_time();  // verify and log
}
