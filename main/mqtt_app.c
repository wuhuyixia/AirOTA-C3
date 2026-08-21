#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "config_store.h"
#include "led_app.h"
#include "ota_app.h"
#include "mqtt_app.h"

static const char *TAG = "MQTT";
static esp_mqtt_client_handle_t s_client = NULL;
static app_config_t s_cfg;
static char s_uri[160];
static char s_status[24] = "stopped";
static bool s_connected = false;

// 更新 Web 页面显示的 MQTT 状态。
static void set_status(const char *s)
{
    snprintf(s_status, sizeof(s_status), "%s", s);
}

bool mqtt_app_is_connected(void) { return s_connected; }
const char *mqtt_app_status(void) { return s_status; }

// 将命令回复发送到与请求 cmdid 对应的 response topic。
static void reply_command(esp_mqtt_client_handle_t client, const char *request_topic,
                          const char *payload)
{
    const char *mark = "/cmd/request/";
    const char *p = strstr(request_topic, mark);
    if (!p || !p[strlen(mark)]) return;

    const char *cmdid = p + strlen(mark);
    char topic[256];
    snprintf(topic, sizeof(topic), "$sys/%s/%s/cmd/response/%s",
             s_cfg.mqtt_user, s_cfg.mqtt_client_id, cmdid);
    esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
}

// 解析 OneNET 命令 JSON，分发 LED 控制或 URL OTA 请求。
static void handle_command(esp_mqtt_client_handle_t client, const char *topic, const char *data)
{
    cJSON *root = cJSON_Parse(data);
    if (!root) {
        ESP_LOGW(TAG, "Command is not valid JSON: %s", data);
        reply_command(client, topic, "{\"ok\":false,\"msg\":\"invalid json\"}");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd)) {
        reply_command(client, topic, "{\"ok\":false,\"msg\":\"missing cmd\"}");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd->valuestring, "led") == 0) {
        // LED 命令只修改运行时周期，不写入 NVS。
        cJSON *period = cJSON_GetObjectItemCaseSensitive(root, "period");
        if (cJSON_IsNumber(period)) {
            led_set_period_ms((uint32_t)period->valuedouble);
            reply_command(client, topic, "{\"ok\":true,\"msg\":\"led updated\"}");
        } else {
            reply_command(client, topic, "{\"ok\":false,\"msg\":\"missing period\"}");
        }
    } else if (strcmp(cmd->valuestring, "ota") == 0) {
        // MQTT 只负责触发 OTA，实际固件由设备通过 URL 下载。
        cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
        if (cJSON_IsString(url) && url->valuestring[0]) {
            esp_err_t err = ota_start_url(url->valuestring);
            if (err == ESP_OK) {
                reply_command(client, topic, "{\"ok\":true,\"msg\":\"ota started\"}");
            } else {
                reply_command(client, topic, "{\"ok\":false,\"msg\":\"ota start failed\"}");
            }
        } else {
            reply_command(client, topic, "{\"ok\":false,\"msg\":\"missing url\"}");
        }
    } else {
        reply_command(client, topic, "{\"ok\":false,\"msg\":\"unknown cmd\"}");
    }

    cJSON_Delete(root);
}

// MQTT 事件回调：维护连接状态、订阅命令主题并接收命令数据。
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_BEFORE_CONNECT:
            set_status("connecting");
            ESP_LOGI(TAG, "Connecting to %s", s_uri);
            break;

        case MQTT_EVENT_CONNECTED: {
            // 连接成功后订阅当前设备的命令请求通配主题。
            s_connected = true;
            set_status("connected");
            ESP_LOGI(TAG, "OneNET MQTT connected");

            char topic[192];
            snprintf(topic, sizeof(topic), "$sys/%s/%s/cmd/request/+",
                     s_cfg.mqtt_user, s_cfg.mqtt_client_id);
            int msg_id = esp_mqtt_client_subscribe(event->client, topic, 0);
            ESP_LOGI(TAG, "Subscribe %s, msg_id=%d", topic, msg_id);
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            set_status("disconnected");
            ESP_LOGW(TAG, "MQTT disconnected");
            break;

        case MQTT_EVENT_DATA: {
            // MQTT 事件中的 topic/data 不保证以 '\0' 结尾，先复制到本地缓冲区再解析。
            if (event->topic_len <= 0 || event->data_len < 0) break;
            if (event->topic_len >= 255 || event->data_len >= 1023) {
                ESP_LOGW(TAG, "MQTT message too large");
                break;
            }

            char topic[256];
            char data[1024];
            memcpy(topic, event->topic, event->topic_len);
            topic[event->topic_len] = '\0';
            memcpy(data, event->data, event->data_len);
            data[event->data_len] = '\0';

            ESP_LOGI(TAG, "RX topic=%s payload=%s", topic, data);
            if (strstr(topic, "/cmd/request/") != NULL) {
                handle_command(event->client, topic, data);
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            s_connected = false;
            set_status("error");
            ESP_LOGE(TAG, "MQTT error");
            break;

        default:
            break;
    }
}

// 创建并启动 MQTT 客户端；缺少必要配置时返回 ESP_ERR_INVALID_STATE。
esp_err_t mqtt_app_start(void)
{
    if (s_client) return ESP_OK;

    config_load(&s_cfg);
    if (s_cfg.mqtt_host[0] == '\0' || s_cfg.mqtt_client_id[0] == '\0' ||
        s_cfg.mqtt_user[0] == '\0' || s_cfg.mqtt_pass[0] == '\0') {
        set_status("not configured");
        ESP_LOGW(TAG, "MQTT not configured; set it from the Web page first");
        return ESP_ERR_INVALID_STATE;
    }

    // Host 可以填写完整 URI；否则按 mqtt://host:port 组合地址。
    if (strstr(s_cfg.mqtt_host, "://")) {
        snprintf(s_uri, sizeof(s_uri), "%s", s_cfg.mqtt_host);
    } else {
        snprintf(s_uri, sizeof(s_uri), "mqtt://%s:%u", s_cfg.mqtt_host, s_cfg.mqtt_port);
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_uri,
        .credentials.client_id = s_cfg.mqtt_client_id,
        .credentials.username = s_cfg.mqtt_user,
        .credentials.authentication.password = s_cfg.mqtt_pass,
        .session.keepalive = 60,
        .session.disable_clean_session = false,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        set_status("init failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));

    set_status("starting");
    return esp_mqtt_client_start(s_client);
}

// 停止并释放 MQTT 客户端资源，允许之后再次启动。
void mqtt_app_stop(void)
{
    if (!s_client) return;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
    set_status("stopped");
}
