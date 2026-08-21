#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_app.h"

// LuatOS CORE ESP32-C3: D5 = GPIO13, active high.
#define LED_GPIO GPIO_NUM_13

static const char *TAG = "LED";
// 初始闪烁周期为 1 秒；MQTT 和 Web 命令都可以动态修改它。
static volatile uint32_t s_period_ms = 200;

// 设置 LED 完整闪烁周期，并限制在 100 ms 到 10000 ms 范围内。
void led_set_period_ms(uint32_t period_ms)
{
    if (period_ms < 100) period_ms = 100;
    if (period_ms > 10000) period_ms = 10000;
    s_period_ms = period_ms;
    ESP_LOGI(TAG, "Blink period = %lu ms", (unsigned long)period_ms);
}

// 返回当前 LED 完整闪烁周期，供 Web 页面显示。
uint32_t led_get_period_ms(void)
{
    return s_period_ms;
}

// LED 后台任务：亮、灭各持续半个周期。
static void led_task(void *arg)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    while (1) {
        uint32_t half = s_period_ms / 2;
        if (half < 50) half = 50;
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(half));
        gpio_set_level(LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(half));
    }
}

// 创建 LED 任务；GPIO 初始化放在任务中完成。
void led_app_start(void)
{
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
}
