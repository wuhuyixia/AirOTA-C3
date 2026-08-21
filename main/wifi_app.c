#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "config_store.h"
#include "wifi_app.h"

static const char *TAG = "WIFI";
#define CONNECTED_BIT BIT0
#define FAILED_BIT    BIT1
#define MAX_RETRY     15

static EventGroupHandle_t s_events;
static wifi_run_mode_t s_mode = WIFI_RUN_NONE;
static int s_retry = 0;
static char s_ip[16] = "";
static char s_ap_ssid[24] = "";
static bool s_handlers_registered = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

// 将字符串复制到固定长度的 WiFi 配置字段，始终保证结尾有 '\0'。
static void copy_string(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) return;

    size_t len = strlen(src);
    if (len >= dst_len) len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// 统一处理 WiFi/IP 事件：STA 启动后连接，断线时重试，拿到 IP 后通知等待者。
static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_mode == WIFI_RUN_AP) return;
        if (s_retry < MAX_RETRY) {
            ++s_retry;
            ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_retry, MAX_RETRY);
            esp_wifi_connect();
        } else if (s_events) {
            xEventGroupSetBits(s_events, FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        ESP_LOGI(TAG, "STA connected, IP=%s", s_ip);
        if (s_events) xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

// 事件处理器只注册一次，避免重复初始化时收到重复事件。
static void ensure_handlers(void)
{
    if (s_handlers_registered) return;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    s_handlers_registered = true;
}

// 把 SoftAP 网关固定为 192.168.100.1，并重新启动 DHCP 服务。
static void set_ap_ip(void)
{
    if (!s_ap_netif) return;
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ESP_IP4TOADDR(192, 168, 100, 1);
    ip.gw.addr = ESP_IP4TOADDR(192, 168, 100, 1);
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
}

// 启动开放的配网 AP。SSID 使用 SoftAP MAC 后两字节生成，便于区分多块板子。
static esp_err_t start_ap(void)
{
    // 先切换内部状态，避免模式切换期间的 STA 断线事件再次触发自动重连。
    s_mode = WIFI_RUN_AP;
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) return ESP_FAIL;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "esp32c3_%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_cfg = {0};
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", s_ap_ssid);
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_ERROR_CHECK(stop_err);
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    set_ap_ip();

    s_mode = WIFI_RUN_AP;
    snprintf(s_ip, sizeof(s_ip), "%s", "192.168.100.1");
    ESP_LOGI(TAG, "AP ready: SSID=%s, URL=http://192.168.100.1", s_ap_ssid);
    return ESP_OK;
}

// 使用 NVS 中的配置启动 STA，并等待最多 30 秒获取 IP；失败则回退到 AP。
static esp_err_t start_sta(const app_config_t *cfg)
{
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) return ESP_FAIL;

    wifi_config_t sta_cfg = {0};
    copy_string((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), cfg->wifi_ssid);
    copy_string((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), cfg->wifi_pass);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    s_retry = 0;
    s_mode = WIFI_RUN_NONE;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_events, CONNECTED_BIT | FAILED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & CONNECTED_BIT) {
        s_mode = WIFI_RUN_STA;
        vEventGroupDelete(s_events);
        s_events = NULL;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "STA connection failed/timeout, fallback to AP");
    vEventGroupDelete(s_events);
    s_events = NULL;
    return start_ap();
}

// 初始化网络栈并选择启动模式：强制 AP、无配置时 AP，否则尝试 STA。
esp_err_t wifi_app_start(bool force_ap)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ensure_handlers();

    app_config_t cfg;
    bool has_cfg = (config_load(&cfg) == ESP_OK && cfg.wifi_ssid[0] != '\0');
    if (force_ap || !has_cfg) return start_ap();
    return start_sta(&cfg);
}

// 以下访问器供 main/Web 页面读取当前网络状态。
wifi_run_mode_t wifi_app_get_mode(void) { return s_mode; }
const char *wifi_app_get_ip(void) { return s_ip; }
const char *wifi_app_get_ap_ssid(void) { return s_ap_ssid; }

int wifi_app_get_rssi(void)
{
    wifi_ap_record_t info;
    if (s_mode == WIFI_RUN_STA && esp_wifi_sta_get_ap_info(&info) == ESP_OK) return info.rssi;
    return 0;
}
