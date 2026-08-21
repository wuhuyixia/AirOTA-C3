#pragma once

#include <stdint.h>
#include "esp_err.h"

#define WIFI_SSID_LEN       33 // 包含结尾的 '\0' 字符。
#define WIFI_PASS_LEN       65
#define MQTT_HOST_LEN       96
#define MQTT_CLIENT_ID_LEN  64
#define MQTT_USER_LEN       64
#define MQTT_PASS_LEN       320
#define OTA_URL_LEN         320

// 所有可持久化保存的设备配置；整个结构体作为一个 NVS blob 保存。
typedef struct {
    char wifi_ssid[WIFI_SSID_LEN];
    char wifi_pass[WIFI_PASS_LEN];

    char mqtt_host[MQTT_HOST_LEN];
    uint16_t mqtt_port;
    char mqtt_client_id[MQTT_CLIENT_ID_LEN];
    char mqtt_user[MQTT_USER_LEN];
    char mqtt_pass[MQTT_PASS_LEN];

    char ota_url[OTA_URL_LEN];
} app_config_t;

// 用默认值初始化配置，不访问 NVS。
void config_set_defaults(app_config_t *cfg);
// 读取 NVS 配置；读取失败时 cfg 仍保留默认值。
esp_err_t config_load(app_config_t *cfg);
// 将完整配置写入 NVS 并提交。
esp_err_t config_save(const app_config_t *cfg);
// 删除 NVS 配置，使下次启动进入配网模式。
esp_err_t config_reset(void);
