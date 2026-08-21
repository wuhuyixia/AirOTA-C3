#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "config_store.h"
#include "wifi_app.h"
#include "mqtt_app.h"
#include "led_app.h"
#include "ota_app.h"
#include "web_app.h"

// 本模块提供设备管理网页：配置、LED 控制、本地上传 OTA 和 URL OTA。
static const char *TAG = "WEB";
static httpd_handle_t s_server;

// 对 application/x-www-form-urlencoded 中的 %XX 和 '+' 进行解码。
static void url_decode(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t i = 0, j = 0;
    while (i < src_len && j + 1 < dst_len) {
        if (src[i] == '%' && i + 2 < src_len && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

// 从表单 body 中查找指定 key，并把解码后的值写入 out。
static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    const size_t key_len = strlen(key);
    const char *p = body;
    out[0] = '\0';

    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            url_decode(out, out_len, eq + 1, (size_t)(end - eq - 1));
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

// 向固定长度配置字段复制字符串，超长内容会被截断并保持 '\0' 结尾。
static void copy_string(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) return;

    size_t len = strlen(src);
    if (len >= dst_len) len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// 读取 HTTP 请求体；限制为 4096 字节，避免异常请求消耗过多内存。
static char *read_body(httpd_req_t *req)
{
    if (req->content_len > 4096) return NULL;
    char *buf = calloc(1, req->content_len + 1);
    if (!buf) return NULL;

    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, buf + total, req->content_len - total);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            free(buf);
            return NULL;
        }
        total += n;
    }
    buf[total] = '\0';
    return buf;
}

// 将 HTML 属性中可能有特殊意义的字符替换为 '_'，避免配置值破坏页面结构。
static void html_escape(char *dst, size_t dst_len, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < dst_len; ++i) {
        char c = src[i];
        if (c == '<' || c == '>' || c == '"' || c == '\'' || c == '&') c = '_';
        dst[j++] = c;
    }
    dst[j] = '\0';
}

// 统一设置 HTML 响应类型并发送页面。
static esp_err_t send_html(httpd_req_t *req, const char *html)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// 返回 303 重定向响应，用于表单提交后的页面跳转。
static esp_err_t redirect(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_sendstr(req, "Redirecting");
}

// 生成管理首页：展示运行状态，并提供配置、LED 和 OTA 控件。
static esp_err_t handle_root(httpd_req_t *req)
{
    app_config_t cfg;
    config_load(&cfg);
    const esp_app_desc_t *app = esp_app_get_description();

    char ssid[80], host[128], cid[96], user[96], ota_url[384];
    html_escape(ssid, sizeof(ssid), cfg.wifi_ssid);
    html_escape(host, sizeof(host), cfg.mqtt_host);
    html_escape(cid, sizeof(cid), cfg.mqtt_client_id);
    html_escape(user, sizeof(user), cfg.mqtt_user);
    html_escape(ota_url, sizeof(ota_url), cfg.ota_url);

    const char *mode = wifi_app_get_mode() == WIFI_RUN_AP ? "AP 配网" :
                       wifi_app_get_mode() == WIFI_RUN_STA ? "STA" : "未就绪";

    char *page = malloc(12000);
    if (!page) return ESP_ERR_NO_MEM;

    snprintf(page, 12000,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32-C3 管理</title><style>"
        "body{font-family:Arial,sans-serif;max-width:720px;margin:auto;padding:18px;background:#f4f6f8;color:#222}"
        ".card{background:#fff;padding:16px;margin:12px 0;border-radius:10px;box-shadow:0 1px 5px #ccd}"
        "input{width:100%%;box-sizing:border-box;padding:9px;margin:4px 0 10px;border:1px solid #bbb;border-radius:6px}"
        "button{padding:10px 16px;border:0;border-radius:6px;background:#1677ff;color:white;cursor:pointer}"
        ".danger{background:#c43}.ok{color:#087}.small{color:#667;font-size:13px}"
        "</style></head><body><h2>ESP32-C3 Web 管理</h2>"
        "<div class='card'><b>状态</b><p>模式: %s<br>IP: %s<br>RSSI: %d dBm<br>"
        "固件: %s<br>运行分区: %s<br>LED 周期: %lu ms<br>MQTT: %s</p></div>"
        "<div class='card'><b>WiFi + OneNET MQTT 配置</b>"
        "<form method='POST' action='/config'>"
        "<label>WiFi SSID</label><input name='wifi_ssid' value='%s' required>"
        "<label>WiFi 密码（留空表示不修改）</label><input name='wifi_pass' type='password'>"
        "<label>MQTT Host</label><input name='mqtt_host' value='%s' placeholder='mqtts.heclouds.com'>"
        "<label>MQTT Port</label><input name='mqtt_port' type='number' value='%u'>"
        "<label>Client ID / OneNET Device Name</label><input name='mqtt_client_id' value='%s'>"
        "<label>Username / OneNET Product ID</label><input name='mqtt_user' value='%s'>"
        "<label>Password / OneNET Token（留空表示不修改）</label><input name='mqtt_pass' type='password'>"
        "<button type='submit'>保存并重启</button></form>"
        "<p class='small'>OneNET 默认：Host=mqtts.heclouds.com，Port=1883，Client ID=设备名，Username=产品ID，Password=Token。</p></div>"
        "<div class='card'><b>LED</b><form method='POST' action='/led'>"
        "<label>闪烁完整周期 ms</label><input name='period' type='number' min='100' max='10000' value='%lu'>"
        "<button type='submit'>修改 LED 周期</button></form></div>"
        "<div class='card'><b>OTA - URL</b><form method='POST' action='/ota'>"
        "<label>固件 URL</label><input name='url' value='%s' placeholder='http://192.168.31.10:8000/firmware.bin'>"
        "<button type='submit'>开始 URL OTA</button></form><p><a href='/ota-status'>查看 OTA 状态</a></p></div>"
        "<div class='card'><b>OTA - 本地 .bin 上传</b>"
        "<input id='fw' type='file' accept='.bin'><button onclick='uploadFw()'>上传并升级</button>"
        "<p id='upmsg' class='small'></p><script>async function uploadFw(){let f=document.getElementById('fw').files[0];"
        "if(!f){alert('请选择 .bin');return;}document.getElementById('upmsg').innerText='上传中...';"
        "let r=await fetch('/ota-upload',{method:'POST',body:f});document.getElementById('upmsg').innerText=await r.text();}</script></div>"
        "<div class='card'><form method='POST' action='/reset'><button class='danger' type='submit'>清空配置并进入 AP 配网</button></form></div>"
        "</body></html>",
        mode, wifi_app_get_ip(), wifi_app_get_rssi(), app->version,
        esp_ota_get_running_partition()->label, (unsigned long)led_get_period_ms(), mqtt_app_status(),
        ssid, host, cfg.mqtt_port, cid, user, (unsigned long)led_get_period_ms(), ota_url);

    esp_err_t err = send_html(req, page);
    free(page);
    return err;
}

// 保存 WiFi/MQTT 配置；保存成功后重启，使新配置生效。
static esp_err_t handle_config(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");

    app_config_t cfg;
    config_load(&cfg);
    char val[MQTT_PASS_LEN];

    if (form_get(body, "wifi_ssid", val, sizeof(val)) && val[0])
        copy_string(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), val);
    if (form_get(body, "wifi_pass", val, sizeof(val)) && val[0])
        copy_string(cfg.wifi_pass, sizeof(cfg.wifi_pass), val);
    if (form_get(body, "mqtt_host", val, sizeof(val)) && val[0])
        copy_string(cfg.mqtt_host, sizeof(cfg.mqtt_host), val);
    if (form_get(body, "mqtt_port", val, sizeof(val)) && val[0]) {
        int port = atoi(val);
        if (port > 0 && port <= 65535) cfg.mqtt_port = (uint16_t)port;
    }
    if (form_get(body, "mqtt_client_id", val, sizeof(val)))
        copy_string(cfg.mqtt_client_id, sizeof(cfg.mqtt_client_id), val);
    if (form_get(body, "mqtt_user", val, sizeof(val)))
        copy_string(cfg.mqtt_user, sizeof(cfg.mqtt_user), val);
    if (form_get(body, "mqtt_pass", val, sizeof(val)) && val[0])
        copy_string(cfg.mqtt_pass, sizeof(cfg.mqtt_pass), val);

    free(body);
    esp_err_t err = config_save(&cfg);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");

    send_html(req, "<html><meta charset='utf-8'><body><h3>配置已保存，设备即将重启...</h3></body></html>");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// 处理网页提交的 LED 闪烁周期，修改仅作用于当前运行周期。
static esp_err_t handle_led(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    char val[32];
    if (form_get(body, "period", val, sizeof(val))) led_set_period_ms((uint32_t)atoi(val));
    free(body);
    return redirect(req, "/");
}

// 接收 OTA URL，保存为最近使用的地址并启动后台 URL OTA 任务。
static esp_err_t handle_ota(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");

    char url[OTA_URL_LEN];
    if (!form_get(body, "url", url, sizeof(url)) || !url[0]) {
        free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
    }
    free(body);

    app_config_t cfg;
    config_load(&cfg);
    snprintf(cfg.ota_url, sizeof(cfg.ota_url), "%s", url);
    config_save(&cfg);

    esp_err_t err = ota_start_url(url);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    return redirect(req, "/ota-status");
}

// 返回当前 OTA 状态；页面自身每 2 秒刷新一次。
static esp_err_t handle_ota_status(httpd_req_t *req)
{
    ota_status_t st = ota_get_status();
    const char *name = st.state == OTA_IDLE ? "IDLE" : st.state == OTA_DOWNLOADING ? "DOWNLOADING" :
                       st.state == OTA_DONE ? "DONE" : "ERROR";
    char page[768];
    snprintf(page, sizeof(page),
             "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='2'></head>"
             "<body><h3>OTA 状态</h3><p>State: %s<br>Progress: %d%%<br>Error: %s</p>"
             "<a href='/'>返回</a></body></html>", name, st.percent, st.err_msg);
    return send_html(req, page);
}

// 接收浏览器直接上传的原始 .bin 文件，并写入备用 OTA 分区。
static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    if (req->content_len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty file");

    ota_writer_t writer;
    // req->content_len 用于进度统计；实际写入仍由 ota_writer_* 统一处理。
    esp_err_t err = ota_writer_begin(&writer, req->content_len);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));

    char *buf = malloc(2048);
    if (!buf) {
        ota_writer_abort(&writer, "out of memory");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    int remain = req->content_len;
    while (remain > 0) {
        int n = httpd_req_recv(req, buf, remain > 2048 ? 2048 : remain);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            free(buf);
            ota_writer_abort(&writer, "upload interrupted");
            return ESP_FAIL;
        }
        if (ota_writer_write(&writer, buf, n) != ESP_OK) {
            free(buf);
            ota_writer_abort(&writer, "flash write failed");
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
        }
        remain -= n;
    }
    free(buf);

    err = ota_writer_finish(&writer);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image invalid");

    httpd_resp_sendstr(req, "OTA success, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// 删除 NVS 配置并重启；下次启动会自动进入 AP 配网模式。
static esp_err_t handle_reset(httpd_req_t *req)
{
    config_reset();
    httpd_resp_sendstr(req, "Configuration cleared. Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

// 启动 HTTP Server 并注册所有管理路由。
esp_err_t web_app_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 10;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    // 路由表把浏览器请求分发到上面的处理函数。
    const httpd_uri_t uris[] = {
        {.uri = "/",           .method = HTTP_GET,  .handler = handle_root,       .user_ctx = NULL},
        {.uri = "/config",     .method = HTTP_POST, .handler = handle_config,     .user_ctx = NULL},
        {.uri = "/led",        .method = HTTP_POST, .handler = handle_led,        .user_ctx = NULL},
        {.uri = "/ota",        .method = HTTP_POST, .handler = handle_ota,        .user_ctx = NULL},
        {.uri = "/ota-status", .method = HTTP_GET,  .handler = handle_ota_status, .user_ctx = NULL},
        {.uri = "/ota-upload", .method = HTTP_POST, .handler = handle_ota_upload, .user_ctx = NULL},
        {.uri = "/reset",      .method = HTTP_POST, .handler = handle_reset,      .user_ctx = NULL},
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));
    }

    ESP_LOGI(TAG, "Web server: http://%s", wifi_app_get_ip());
    return ESP_OK;
}
