#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "ota_app.h"

static const char *TAG = "OTA";
// OTA 状态保存在 RAM 中，仅用于当前运行期间的 Web/日志展示。
static ota_status_t s_status = {.state = OTA_IDLE, .percent = 0, .err_msg = ""};

// 返回当前 OTA 状态的快照。
ota_status_t ota_get_status(void) { return s_status; }

// 新固件启动并完成基础服务初始化后，取消 Bootloader 的回滚等待。
void ota_mark_valid(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) ESP_LOGI(TAG, "Current firmware marked valid");
}

// 统一记录 OTA 错误，并同步更新状态页显示的错误信息。
static void set_error(const char *msg)
{
    s_status.state = OTA_ERROR;
    snprintf(s_status.err_msg, sizeof(s_status.err_msg), "%s", msg ? msg : "unknown");
    ESP_LOGE(TAG, "%s", s_status.err_msg);
}

// 选择当前运行分区之外的 OTA 分区，并打开 ESP-IDF OTA 写入句柄。
esp_err_t ota_writer_begin(ota_writer_t *w, int total)
{
    memset(w, 0, sizeof(*w));
    w->total = total;
    // ESP-IDF 根据当前运行分区返回另一个 OTA 分区，避免覆盖正在运行的固件。
    w->part = esp_ota_get_next_update_partition(NULL);
    if (!w->part) {
        set_error("No OTA partition available");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Writing partition %s @ 0x%lx", w->part->label, (unsigned long)w->part->address);
    esp_err_t err = esp_ota_begin(w->part, OTA_SIZE_UNKNOWN, &w->handle);
    if (err != ESP_OK) {
        set_error(esp_err_to_name(err));
        return err;
    }

    s_status.state = OTA_DOWNLOADING;
    s_status.percent = 0;
    s_status.err_msg[0] = '\0';
    return ESP_OK;
}

// 把网络/HTTP 收到的一块数据写入备用分区，并计算下载百分比。
esp_err_t ota_writer_write(ota_writer_t *w, const void *data, int len)
{
    esp_err_t err = esp_ota_write(w->handle, data, len);
    if (err != ESP_OK) return err;
    w->received += len;
    if (w->total > 0) {
        int pct = (int)((int64_t)w->received * 100 / w->total);
        if (pct > 100) pct = 100;
        s_status.percent = pct;
    }
    return ESP_OK;
}

// 结束写入并验证镜像；验证成功后才登记下次启动分区。
esp_err_t ota_writer_finish(ota_writer_t *w)
{
    esp_err_t err = esp_ota_end(w->handle);
    if (err != ESP_OK) {
        set_error("Firmware validation failed");
        return err;
    }

    err = esp_ota_set_boot_partition(w->part);
    if (err != ESP_OK) {
        set_error("Failed to set boot partition");
        return err;
    }

    s_status.state = OTA_DONE;
    s_status.percent = 100;
    ESP_LOGI(TAG, "OTA complete, bytes=%d", w->received);
    return ESP_OK;
}

// 下载或写入中断时释放 OTA 句柄，但不改变当前启动分区。
void ota_writer_abort(ota_writer_t *w, const char *reason)
{
    if (w && w->handle) esp_ota_abort(w->handle);
    set_error(reason);
}

// URL OTA 后台任务：下载固件、分块写入备用分区，成功后自动重启。
static void ota_url_task(void *arg)
{
    char *url = (char *)arg;
    ESP_LOGI(TAG, "Downloading %s", url);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    // HTTPS 使用 ESP-IDF 证书包校验服务器证书；HTTP 不需要证书配置。
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        set_error("HTTP client init failed");
        goto out;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        set_error("HTTP connection failed");
        esp_http_client_cleanup(client);
        goto out;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    // 只有 2xx 响应才认为服务器返回了可下载的固件资源。
    if (status_code < 200 || status_code >= 300) {
        char msg[64];
        snprintf(msg, sizeof(msg), "HTTP status %d", status_code);
        set_error(msg);
        esp_http_client_cleanup(client);
        goto out;
    }

    ota_writer_t writer;
    if (ota_writer_begin(&writer, content_len) != ESP_OK) {
        esp_http_client_cleanup(client);
        goto out;
    }

    char *buf = malloc(2048);
    if (!buf) {
        ota_writer_abort(&writer, "Out of memory");
        esp_http_client_cleanup(client);
        goto out;
    }

    bool failed = false;
    while (1) {
        int n = esp_http_client_read(client, buf, 2048);
        if (n < 0) {
            ota_writer_abort(&writer, "HTTP read failed");
            failed = true;
            break;
        }
        if (n == 0) break;
        err = ota_writer_write(&writer, buf, n);
        if (err != ESP_OK) {
            ota_writer_abort(&writer, "Flash write failed");
            failed = true;
            break;
        }
    }

    free(buf);
    esp_http_client_cleanup(client);

    // ota_writer_finish() 成功意味着镜像已验证且启动分区已切换。
    if (!failed && ota_writer_finish(&writer) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1200));
        esp_restart();
    }

out:
    free(url);
    vTaskDelete(NULL);
}

// 校验 URL、创建独立任务，并防止同时启动两个 URL OTA。
esp_err_t ota_start_url(const char *url)
{
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_status.state == OTA_DOWNLOADING) return ESP_ERR_INVALID_STATE;

    char *copy = strdup(url);
    if (!copy) return ESP_ERR_NO_MEM;

    s_status.state = OTA_DOWNLOADING;
    s_status.percent = 0;
    s_status.err_msg[0] = '\0';

    if (xTaskCreate(ota_url_task, "ota_url", 8192, copy, 5, NULL) != pdPASS) {
        free(copy);
        s_status.state = OTA_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
