#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "esp_log.h"
#include "config_store.h"

static const char *TAG = "CONFIG";
// NVS 命名空间和 blob key；整个 app_config_t 使用一个 key 保存。
static const char *NAMESPACE = "app";
static const char *KEY = "cfg";

// 设置第一次启动时使用的默认配置。
void config_set_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->mqtt_host, sizeof(cfg->mqtt_host), "%s", "mqtts.heclouds.com");
    cfg->mqtt_port = 1883;
}

// 先装载默认值，再尝试从 NVS 读取已保存的完整配置。
esp_err_t config_load(app_config_t *cfg)
{
    config_set_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(*cfg);
    err = nvs_get_blob(h, KEY, cfg, &len);
    nvs_close(h);

    if (err == ESP_OK && len == sizeof(*cfg)) {
        if (cfg->mqtt_port == 0) cfg->mqtt_port = 1883;
        ESP_LOGI(TAG, "Configuration loaded, SSID='%s'", cfg->wifi_ssid);
        return ESP_OK;
    }

    config_set_defaults(cfg);
    return err;
}

// 以 blob 形式保存整个配置结构，并通过 nvs_commit() 确保持久化。
esp_err_t config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Configuration save: %s", esp_err_to_name(err));
    return err;
}

// 删除配置 blob；Web 的“清空配置”功能会调用此函数。
esp_err_t config_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_erase_key(h, KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
