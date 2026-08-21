#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    WIFI_RUN_NONE = 0,
    WIFI_RUN_AP,
    WIFI_RUN_STA,
} wifi_run_mode_t;

// 初始化 WiFi，并根据配置或 force_ap 选择 STA/AP 模式。
esp_err_t wifi_app_start(bool force_ap);
// 获取当前 WiFi 运行模式。
wifi_run_mode_t wifi_app_get_mode(void);
// 获取当前接口 IP 字符串。
const char *wifi_app_get_ip(void);
// 获取自动生成的 AP SSID。
const char *wifi_app_get_ap_ssid(void);
// 获取 STA 模式下的 RSSI；非 STA 模式返回 0。
int wifi_app_get_rssi(void);
