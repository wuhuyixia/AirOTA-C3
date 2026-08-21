#pragma once

#include <stdbool.h>
#include "esp_err.h"

// 读取 NVS 配置并启动 OneNET MQTT 客户端。
esp_err_t mqtt_app_start(void);
// 停止并销毁 MQTT 客户端。
void mqtt_app_stop(void);
// 查询 MQTT 是否已经收到 CONNECTED 事件。
bool mqtt_app_is_connected(void);
// 获取用于 Web 页面显示的连接状态字符串。
const char *mqtt_app_status(void);
