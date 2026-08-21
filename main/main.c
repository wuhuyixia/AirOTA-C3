#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "led_app.h"
#include "wifi_app.h"
#include "web_app.h"
#include "mqtt_app.h"
#include "ota_app.h"

static const char *TAG = "APP";

// LuatOS CORE ESP32-C3 BOOT button = GPIO9, active low.
#define BOOT_BUTTON_GPIO GPIO_NUM_9

// 在应用启动最初的一小段时间内检查 BOOT 按键。
// 按住约 1.5 秒会强制进入 AP 配网；不要在复位前就一直按住该键。
static bool boot_button_held_after_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    // Do not hold BOOT before reset: GPIO9 is a strapping pin.
    int held_ms = 0;
    for (int i = 0; i < 20; ++i) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= 1500) return true;
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

// 初始化 NVS；版本不兼容或空间不足时，先擦除再重新初始化。
static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// 应用主入口：初始化基础服务，启动 WiFi/Web/MQTT，最后确认 OTA 固件有效。
void app_main(void)
{
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-C3 Web + WiFi + OneNET + OTA");
    ESP_LOGI(TAG, "IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Flash: %lu MB", (unsigned long)(flash_size / 1024 / 1024));
    ESP_LOGI(TAG, "Running partition: %s", esp_ota_get_running_partition()->label);
    ESP_LOGI(TAG, "========================================");

    init_nvs();
    led_app_start();

    bool force_ap = boot_button_held_after_start();
    if (force_ap) ESP_LOGW(TAG, "BOOT held after startup: force AP provisioning mode");

    // 根据 NVS 配置选择 STA 或 AP 模式；两种模式下都会启动 Web 服务。
    ESP_ERROR_CHECK(wifi_app_start(force_ap));
    ESP_ERROR_CHECK(web_app_start());

    // 只有 STA 模式才尝试连接 MQTT；没有 MQTT 配置不会阻止 Web/OTA 工作。
    if (wifi_app_get_mode() == WIFI_RUN_STA) {
        esp_err_t err = mqtt_app_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "MQTT start failed: %s", esp_err_to_name(err));
        }
    }

    // 到达基础服务稳定状态后，向 Bootloader 确认当前 OTA 固件有效。
    ota_mark_valid();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
